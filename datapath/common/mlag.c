/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MLAG: classic peer-link multi-chassis link aggregation. switchapi 1.5.
 *
 * Two switches, joined by a peer-link, each have a LAG with the same MLAG id.
 * A device cabled to both sees ONE LACP partner -- the same system id and key
 * on every link -- and bundles them as one LAG. Each switch forwards to its
 * own half; the peer-link carries what one of them cannot deliver itself.
 *
 * WHAT THIS FILE DOES
 *
 *   The pair.   Control frames over the peer-link every second carry this
 *               switch's priority and MAC, the shared LACP system id, and which
 *               MLAG ids have members distributing here. Lower (priority, MAC)
 *               is the primary. A UDP heartbeat over the management network,
 *               if the peer's address is configured, tells a dead peer from a
 *               dead peer-link.
 *
 *   LACP.       lag.c asks nosaic_mlag_lacp() what an MLAG interface presents:
 *               the shared system id -- the primary's MAC, sticky, so a peer
 *               lost does not make the partner renegotiate -- a key of 0x100 +
 *               id on both, and port numbers offset by 0x800 on the secondary
 *               so no two links of the pair share one.
 *
 *   No copies.  A frame flooded to the peer across the peer-link must not leave
 *               by an MLAG interface the peer has already delivered it to: the
 *               device on the far end would get it twice. So while an MLAG id
 *               is up on BOTH sides, flooding from the peer-link's ports to its
 *               members is blocked. When the peer's half is down, the block
 *               comes off, and this side delivers for both.
 *
 *   MAC sync.   MACs learned on an MLAG interface are advertised to the peer
 *               every second, and the peer installs them on its own half of the
 *               same interface -- or on the peer-link, if its half is down -- so
 *               a frame for them is forwarded locally instead of flooded.
 *
 *   Split brain. If the peer-link fails but the heartbeat still hears the
 *               peer, both switches are alive and cannot coordinate: the
 *               secondary disables its MLAG members, and the partner carries
 *               on over the primary's. If the heartbeat is silent too, the peer
 *               is gone, and this switch carries on alone.
 *
 * WHAT IT DOES NOT
 *
 *   Spanning tree does not run on MLAG interfaces or the peer-link: they
 *   forward (nosaic_mlag_stp_excluded). There is no shared gateway address:
 *   each peer's SVI has its own.
 *
 * LOCKING: mlag_lock over everything here. Nothing is called into vlan.c.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <bcm/error.h>
#include <bcm/l2.h>
#include <bcm/pkt.h>
#include <bcm/port.h>
#include <bcm/rx.h>
#include <bcm/stack.h>

#include "lag.h"
#include "mlag.h"
#include "tapbridge.h"

#define RX_PRIORITY    108            /* ahead of tapbridge; LACP 110, STP 105 */
#define TICK_MS        200
#define HELLO_MS       1000
#define DEAD_MS        3500           /* three hellos missed, and a half */
#define HB_PORT        47101
#define ETYPE          0x88b5         /* IEEE local experimental */
#define MAX_SYNC       4096
#define PER_FRAME      120
#define SETTLE_MS      2500           /* see evaluate(): how long a half must stay put */
#define MAX_PORT       256

static const unsigned char ctl_da[6] = { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x0e };

enum { ROLE_NONE, ROLE_PRIMARY, ROLE_SECONDARY };
static const char *role_name[] = { "none", "primary", "secondary" };

struct smac { unsigned char mac[6]; unsigned vid, id; };

static pthread_mutex_t mlag_lock = PTHREAD_MUTEX_INITIALIZER;
static int mlag_unit = -1;

/* Configuration. */
static volatile int enabled;
static char peer_link[32];
static char peer_addr[64];
static int priority = 32768;
static int mlag_of[NOSAIC_MAX_LAGS + 1];         /* LAG -> MLAG id */
static volatile int excluded[NOSAIC_MAX_TAPS + NOSAIC_MAX_LAGS + 1];

/* This switch and its peer. */
static unsigned char my_mac[6], sys_id[6];
static int sys_set;
static int role;
static struct {
	int           heard;
	long long     last_link, last_hb;
	unsigned char mac[6], sys[6];
	int           prio;
	unsigned char up[NOSAIC_MLAG_MAX_ID + 1];
	unsigned char known[NOSAIC_MLAG_MAX_ID + 1];   /* the peer has reported it */
	/* MAC sync being received, and the last complete set */
	unsigned      gen;
	int           nrx;
	struct smac   rx[MAX_SYNC];
} peer;
static int peer_alive, hb_alive, link_up, shut;

/* What this switch has done in the chip. */
static struct smac installed[MAX_SYNC];
static int ninstalled;
static bcm_pbmp_t blocked_p[NOSAIC_MLAG_MAX_ID + 1], blocked_m[NOSAIC_MLAG_MAX_ID + 1];
static int hb_fd = -1;
static unsigned tx_gen;
static long long peer_down_since[NOSAIC_MLAG_MAX_ID + 1];   /* 0: not reported down */
static long long local_up_since[NOSAIC_MLAG_MAX_ID + 1];    /* 0: not up here */
static unsigned char local_up[NOSAIC_MLAG_MAX_ID + 1];
static int trigger;                                          /* a hello, now */

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
static unsigned get16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }

/* ---------------------------------------------------------------------- */
/* Interfaces */

/* The peer-link's key (tap index, or NOSAIC_MAX_TAPS + LAG), its ports and
 * whether it has link. -1 if it does not exist. */
static int peer_link_key(bcm_pbmp_t *pbm)
{
	int i, key, svc;

	BCM_PBMP_CLEAR(*pbm);
	for (i = 0; i < nosaic_tap_count(); i++) {
		const char *n = NULL;
		int p = -1;

		if (nosaic_tap_info(i, &n, &p, NULL, NULL, NULL) == 0 && n != NULL &&
		    strcmp(n, peer_link) == 0) {
			BCM_PBMP_PORT_ADD(*pbm, p);
			return i;
		}
	}
	if (nosaic_lag_iface(peer_link, &key, pbm, &svc) == 0)
		return NOSAIC_MAX_TAPS + key;
	return -1;
}

static int key_link(int key)
{
	int up = 0, p = -1;

	if (key < 0)
		return 0;
	if (key >= NOSAIC_MAX_TAPS)
		return nosaic_lag_active(key - NOSAIC_MAX_TAPS) > 0;
	if (nosaic_tap_info(key, NULL, &p, NULL, NULL, NULL) != 0)
		return 0;
	return bcm_port_link_status_get(mlag_unit, p, &up) == BCM_E_NONE &&
	       up == BCM_PORT_LINK_STATUS_UP;
}

static int key_xmit_port(int key)
{
	int p = -1;

	if (key < 0)
		return -1;
	if (key >= NOSAIC_MAX_TAPS)
		return nosaic_lag_pick(key - NOSAIC_MAX_TAPS, 0);
	nosaic_tap_info(key, NULL, &p, NULL, NULL, NULL);
	return p;
}

static int lag_of_id(int id)
{
	int k;

	for (k = 1; k <= NOSAIC_MAX_LAGS; k++)
		if (mlag_of[k] == id && nosaic_lag_tid(k) >= 0)
			return k;
	return 0;
}

static void lag_pbm(int lag, bcm_pbmp_t *pbm)
{
	int key, svc;

	BCM_PBMP_CLEAR(*pbm);
	nosaic_lag_iface(nosaic_lag_name(lag), &key, pbm, &svc);
}

/* ---------------------------------------------------------------------- */
/* Control frames */

static int collect(struct smac *out, int max);

static void send_frames(int plk)
{
	static struct smac mine[MAX_SYNC];
	unsigned char f[1500];
	int n, k, port = key_xmit_port(plk), chunk, chunks;
	unsigned id;

	if (port < 0)
		return;

	/* The MACs this switch learned on its MLAG interfaces. Static entries
	 * are skipped: those are the peer's, installed here. */
	n = collect(mine, MAX_SYNC);

	tx_gen++;
	chunks = n == 0 ? 1 : (n + PER_FRAME - 1) / PER_FRAME;
	for (chunk = 0; chunk < chunks; chunk++) {
		int len = 0, i, nifs = 0, first = chunk * PER_FRAME;
		int cnt = n - first < PER_FRAME ? n - first : PER_FRAME;

		if (cnt < 0)
			cnt = 0;
		memset(f, 0, sizeof(f));
		memcpy(f, ctl_da, 6);
		memcpy(f + 6, my_mac, 6);
		put16(f + 12, ETYPE);
		len = 14;
		memcpy(f + len, "NMLG", 4); len += 4;
		f[len++] = 1;                                     /* version */
		f[len++] = 0;
		put16(f + len, (unsigned)priority); len += 2;
		memcpy(f + len, my_mac, 6); len += 6;
		memcpy(f + len, sys_id, 6); len += 6;
		/* which MLAG ids are up here */
		{
			int at = len;

			len += 2;
			for (k = 1; k <= NOSAIC_MAX_LAGS; k++) {
				if (!mlag_of[k] || nosaic_lag_tid(k) < 0)
					continue;
				put16(f + len, (unsigned)mlag_of[k]);
				f[len + 2] = !shut && nosaic_lag_active(k) > 0;
				len += 3;
				nifs++;
			}
			put16(f + at, (unsigned)nifs);
		}
		/* MAC sync: generation, chunk of chunks, entries */
		put16(f + len, tx_gen & 0xffff); len += 2;
		f[len++] = (unsigned char)chunk;
		f[len++] = (unsigned char)chunks;
		put16(f + len, (unsigned)cnt); len += 2;
		for (i = 0; i < cnt; i++) {
			const struct smac *m = &mine[first + i];

			memcpy(f + len, m->mac, 6);
			put16(f + len + 6, m->vid);
			id = m->id;
			put16(f + len + 8, id);
			len += 10;
		}
		nosaic_tap_xmit(port, 1, f, len);
	}
}

static int cb_collect(int unit, bcm_l2_addr_t *l2, void *arg)
{
	struct { struct smac *out; int n, max; } *c = arg;
	int k;

	(void)unit;
	if (c->n >= c->max || (l2->flags & BCM_L2_STATIC) || !(l2->flags & BCM_L2_TRUNK_MEMBER))
		return BCM_E_NONE;
	k = nosaic_lag_of_tid(l2->tgid);
	if (k == 0 || mlag_of[k] == 0)
		return BCM_E_NONE;
	memcpy(c->out[c->n].mac, l2->mac, 6);
	c->out[c->n].vid = l2->vid;
	c->out[c->n].id = (unsigned)mlag_of[k];
	c->n++;
	return BCM_E_NONE;
}

static int collect(struct smac *out, int max)
{
	struct { struct smac *out; int n, max; } c = { out, 0, max };

	bcm_l2_traverse(mlag_unit, cb_collect, &c);
	return c.n;
}

static int rx_port(int unit, bcm_pkt_t *pkt)
{
	bcm_gport_t gp;
	bcm_port_t local;

	BCM_GPORT_MODPORT_SET(gp, pkt->src_mod, pkt->src_port);
	if (bcm_port_local_get(unit, gp, &local) != BCM_E_NONE)
		return pkt->src_port;
	return local;
}

static bcm_rx_t mlag_rx(int unit, bcm_pkt_t *pkt, void *cookie)
{
	const unsigned char *p = pkt->pkt_data[0].data;
	int len = (int)pkt->pkt_data[0].len, off = 12, n, i, nifs;
	bcm_pbmp_t plp;
	unsigned gen, chunk, chunks;

	(void)cookie;
	if (len < 20 || memcmp(p, ctl_da, 6) != 0)
		return BCM_RX_NOT_HANDLED;
	if (p[12] == 0x81 && p[13] == 0x00)
		off = 16;
	if (get16(p + off) != ETYPE)
		return BCM_RX_NOT_HANDLED;
	p += off + 2;
	len -= off + 2;
	if (len < 26 || memcmp(p, "NMLG", 4) != 0 || p[4] != 1 || !enabled)
		return BCM_RX_HANDLED;

	pthread_mutex_lock(&mlag_lock);
	/* Only from the peer-link: the same frame arriving anywhere else is a
	 * cabling mistake, not a peer. */
	if (peer_link_key(&plp) < 0 || !BCM_PBMP_MEMBER(plp, rx_port(unit, pkt))) {
		pthread_mutex_unlock(&mlag_lock);
		return BCM_RX_HANDLED;
	}
	peer.prio = (int)get16(p + 6);
	memcpy(peer.mac, p + 8, 6);
	memcpy(peer.sys, p + 14, 6);
	peer.heard = 1;
	peer.last_link = now_ms();
	nifs = (int)get16(p + 20);
	p += 22;
	len -= 22;
	if (len < nifs * 3 + 6) {
		pthread_mutex_unlock(&mlag_lock);
		return BCM_RX_HANDLED;
	}
	memset(peer.up, 0, sizeof(peer.up));
	memset(peer.known, 0, sizeof(peer.known));
	for (i = 0; i < nifs; i++) {
		unsigned id = get16(p);

		if (id >= 1 && id <= NOSAIC_MLAG_MAX_ID) {
			peer.up[id] = p[2];
			peer.known[id] = 1;
		}
		p += 3;
	}
	for (i = 1; i <= NOSAIC_MLAG_MAX_ID; i++) {
		if (peer.known[i] && !peer.up[i]) {
			if (!peer_down_since[i])
				peer_down_since[i] = peer.last_link;
		} else {
			peer_down_since[i] = 0;
		}
	}
	gen = get16(p);
	chunk = p[2];
	chunks = p[3];
	n = (int)get16(p + 4);
	p += 6;
	len -= nifs * 3 + 6;
	if (chunk == 0) {
		peer.gen = gen;
		peer.nrx = 0;
	}
	if (gen == peer.gen && len >= n * 10) {
		for (i = 0; i < n && peer.nrx < MAX_SYNC; i++) {
			memcpy(peer.rx[peer.nrx].mac, p, 6);
			peer.rx[peer.nrx].vid = get16(p + 6);
			peer.rx[peer.nrx].id = get16(p + 8);
			peer.nrx++;
			p += 10;
		}
		if (chunk + 1 == chunks)
			peer.gen |= 0x10000;             /* complete: apply it */
	}
	pthread_mutex_unlock(&mlag_lock);
	return BCM_RX_HANDLED;
}

/* ---------------------------------------------------------------------- */
/* The heartbeat, over the management network */

static void hb_open(void)
{
	struct sockaddr_in a;
	int one = 1;

	if (hb_fd >= 0)
		return;
	if ((hb_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
		return;
	/* In the management VRF, where the peer's management address is. */
	if (if_nametoindex("mgmt") != 0)
		setsockopt(hb_fd, SOL_SOCKET, SO_BINDTODEVICE, "mgmt", 5);
	setsockopt(hb_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	fcntl(hb_fd, F_SETFL, O_NONBLOCK);
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(HB_PORT);
	if (bind(hb_fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
		fprintf(stderr, "mlag: heartbeat bind: %s\n", strerror(errno));
		close(hb_fd);
		hb_fd = -1;
	}
}

static void hb_tick(long long now)
{
	struct sockaddr_in a;
	unsigned char b[64];
	ssize_t r;

	if (peer_addr[0] == '\0')
		return;
	hb_open();
	if (hb_fd < 0)
		return;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(HB_PORT);
	if (inet_pton(AF_INET, peer_addr, &a.sin_addr) == 1) {
		memcpy(b, "NMHB", 4);
		memcpy(b + 4, my_mac, 6);
		sendto(hb_fd, b, 10, 0, (struct sockaddr *)&a, sizeof(a));
	}
	while ((r = recv(hb_fd, b, sizeof(b), 0)) > 0) {
		if (r >= 10 && memcmp(b, "NMHB", 4) == 0 && memcmp(b + 4, my_mac, 6) != 0)
			peer.last_hb = now;
	}
}

/* ---------------------------------------------------------------------- */
/* The chip */

/*
 * Block flooding from the peer-link's ports to an MLAG id's members, or
 * unblock it; only the pairs that changed are rewritten.
 *
 * ⚠ FLOODING ONLY, NOT BCM_PORT_FLOOD_BLOCK_ALL. "ALL" is not "every kind of
 * flood": it is the ingress port's egress mask, ING_EGRMSKBMAP, which stops
 * known unicast too. The first version used it, and a frame the peer had to
 * send across the peer-link for this side's half of an MLAG interface -- a
 * reply to the dual-homed device, addressed to a MAC learned over the
 * peer-link -- was dropped at the wall. Pings from the device to either peer
 * failed while pings to it worked.
 */
#define FLOODS (BCM_PORT_FLOOD_BLOCK_BCAST | BCM_PORT_FLOOD_BLOCK_UNKNOWN_UCAST | \
		BCM_PORT_FLOOD_BLOCK_UNKNOWN_MCAST | BCM_PORT_FLOOD_BLOCK_KNOWN_MCAST)
static const char *block_why = "";

static void set_block(int id, int on, bcm_pbmp_t plp, bcm_pbmp_t mem)
{
	bcm_port_t p, m;

	if (!on) {
		BCM_PBMP_CLEAR(plp);
		BCM_PBMP_CLEAR(mem);
	}
	if (BCM_PBMP_EQ(plp, blocked_p[id]) && BCM_PBMP_EQ(mem, blocked_m[id]))
		return;
	BCM_PBMP_ITER(blocked_p[id], p)
		BCM_PBMP_ITER(blocked_m[id], m)
			bcm_port_flood_block_set(mlag_unit, p, m, 0);
	BCM_PBMP_ITER(plp, p)
		BCM_PBMP_ITER(mem, m)
			bcm_port_flood_block_set(mlag_unit, p, m, FLOODS);
	BCM_PBMP_ASSIGN(blocked_p[id], plp);
	BCM_PBMP_ASSIGN(blocked_m[id], mem);
	printf("mlag: %d: flooding from the peer-link to it %s%s\n", id,
	       BCM_PBMP_IS_NULL(plp) ? "allowed" : "blocked", block_why);
	fflush(stdout);
}

static void set_shut(int on)
{
	int k;

	if (on == shut)
		return;
	for (k = 1; k <= NOSAIC_MAX_LAGS; k++) {
		bcm_pbmp_t pbm;
		bcm_port_t p;

		if (!mlag_of[k])
			continue;
		lag_pbm(k, &pbm);
		BCM_PBMP_ITER(pbm, p)
			bcm_port_enable_set(mlag_unit, p, !on);
	}
	shut = on;
	printf("mlag: %s\n", on ? "peer-link lost but the peer is alive: this secondary "
	       "disables its MLAG members" : "MLAG members enabled again");
	fflush(stdout);
}

static int smac_find(const struct smac *set, int n, const struct smac *m)
{
	int i;

	for (i = 0; i < n; i++)
		if (set[i].vid == m->vid && memcmp(set[i].mac, m->mac, 6) == 0)
			return i;
	return -1;
}

static void unsync_all(void)
{
	int i;

	for (i = 0; i < ninstalled; i++)
		bcm_l2_addr_delete(mlag_unit, installed[i].mac, (bcm_vlan_t)installed[i].vid);
	ninstalled = 0;
}

/* The peer's MACs: on this switch's half of the same MLAG interface if it has
 * one up, otherwise on the peer-link. Static, so learning here does not move
 * them and this switch does not advertise them back. */
static void apply_sync(int plk)
{
	static struct smac want[MAX_SYNC];
	int i, n = peer.nrx;

	if (!(peer.gen & 0x10000))
		return;
	peer.gen &= 0xffff;
	memcpy(want, peer.rx, sizeof(struct smac) * (size_t)n);

	for (i = 0; i < ninstalled; ) {
		if (smac_find(want, n, &installed[i]) < 0) {
			bcm_l2_addr_delete(mlag_unit, installed[i].mac, (bcm_vlan_t)installed[i].vid);
			installed[i] = installed[--ninstalled];
		} else {
			i++;
		}
	}
	for (i = 0; i < n; i++) {
		bcm_l2_addr_t l2;
		int lag = lag_of_id((int)want[i].id), tid;

		bcm_l2_addr_t_init(&l2, want[i].mac, (bcm_vlan_t)want[i].vid);
		l2.flags = BCM_L2_STATIC;
		if (lag && !shut && nosaic_lag_active(lag) > 0 && (tid = nosaic_lag_tid(lag)) >= 0) {
			l2.flags |= BCM_L2_TRUNK_MEMBER;
			l2.tgid = tid;
		} else if (plk >= NOSAIC_MAX_TAPS && nosaic_lag_tid(plk - NOSAIC_MAX_TAPS) >= 0) {
			l2.flags |= BCM_L2_TRUNK_MEMBER;
			l2.tgid = nosaic_lag_tid(plk - NOSAIC_MAX_TAPS);
		} else {
			bcm_gport_t gp;
			int port = key_xmit_port(plk);

			if (port < 0 || bcm_port_gport_get(mlag_unit, port, &gp) != BCM_E_NONE)
				continue;
			l2.port = gp;
		}
		if (bcm_l2_addr_add(mlag_unit, &l2) != BCM_E_NONE)
			continue;
		if (smac_find(installed, ninstalled, &want[i]) < 0 && ninstalled < MAX_SYNC)
			installed[ninstalled++] = want[i];
	}
}

/* ---------------------------------------------------------------------- */

static void evaluate(long long now)
{
	bcm_pbmp_t plp;
	int plk = peer_link_key(&plp), k, was_alive = peer_alive, old_role = role;

	link_up = key_link(plk);
	peer_alive = peer.heard && now - peer.last_link < DEAD_MS && link_up;
	hb_alive = peer_addr[0] && now - peer.last_hb < DEAD_MS;

	/* Spanning tree leaves the peer-link and the MLAG interfaces alone. */
	for (k = 0; k < NOSAIC_MAX_TAPS + NOSAIC_MAX_LAGS + 1; k++)
		excluded[k] = enabled && (k == plk ||
			(k >= NOSAIC_MAX_TAPS && mlag_of[k - NOSAIC_MAX_TAPS]));

	if (!sys_set) {
		memcpy(sys_id, my_mac, 6);
		sys_set = 1;
	}
	if (peer_alive) {
		int mine_first = priority < peer.prio ||
			(priority == peer.prio && memcmp(my_mac, peer.mac, 6) < 0);

		role = mine_first ? ROLE_PRIMARY : ROLE_SECONDARY;
		/* The primary's MAC is the pair's LACP system. */
		if (memcmp(sys_id, mine_first ? my_mac : peer.mac, 6) != 0) {
			memcpy(sys_id, mine_first ? my_mac : peer.mac, 6);
			printf("mlag: lacp system %02x%02x%02x%02x%02x%02x\n", sys_id[0], sys_id[1],
			       sys_id[2], sys_id[3], sys_id[4], sys_id[5]);
		}
	} else if (!hb_alive) {
		role = ROLE_NONE;                   /* no peer: on our own */
	}
	if (role != old_role || peer_alive != was_alive) {
		printf("mlag: %s, peer %s over the peer-link%s\n", role_name[role],
		       peer_alive ? "heard" : "not heard",
		       hb_alive ? ", heartbeat heard" : "");
		fflush(stdout);
	}

	/* Split brain: the peer is alive, the peer-link is not. */
	set_shut(!peer_alive && hb_alive && role == ROLE_SECONDARY);

	for (k = 1; k <= NOSAIC_MAX_LAGS; k++) {
		bcm_pbmp_t mem;
		int id = mlag_of[k];

		if (!id)
			continue;
		lag_pbm(k, &mem);
		/*
		 * ⚠ BLOCKED UNLESS BOTH HALVES HAVE STAYED PUT.
		 *
		 * A flood the peer has delivered to the device, let through here
		 * as well, reaches the device twice -- the second time as its own
		 * frame back, so it learns its own MAC on the LAG, and every ARP
		 * reply to it is dropped from then on as going back where it came
		 * from. A Nexus 3172TQ did exactly that, twice:
		 *
		 *   - blocking only once this side's half was distributing left a
		 *     200 ms window behind LACP;
		 *   - blocking unless the peer had reported its half down left a
		 *     window when both halves came up together, each still
		 *     believing the other's was down.
		 *
		 * So the block comes off only when the peer has reported its half
		 * down for SETTLE_MS AND this side's half has been up for as long:
		 * the single-homed case, which is the only one that needs it off.
		 * Known unicast crosses the peer-link either way; only floods wait.
		 */
		{
			int up = nosaic_lag_active(k) > 0;

			int single;

			if (up != local_up[id]) {
				local_up[id] = (unsigned char)up;
				local_up_since[id] = up ? now : 0;
				trigger = 1;             /* tell the peer now, not in a second */
				/*
				 * This half gone: what was learned on it points at a
				 * trunk with no members, and would swallow traffic for
				 * the device until it aged out. Forgotten, it floods,
				 * crosses the peer-link and is learned again there.
				 */
				if (!up && nosaic_lag_tid(k) >= 0)
					bcm_l2_addr_delete_by_trunk(mlag_unit, nosaic_lag_tid(k), 0);
			}
			single = peer_down_since[id] && now - peer_down_since[id] >= SETTLE_MS &&
				 local_up[id] && now - local_up_since[id] >= SETTLE_MS;
			block_why = !peer_alive ? " (no peer)" : shut ? " (shut)" :
				    single ? " (the peer's half is down: this side delivers for both)" :
				    " (the peer's half is up, or has not settled)";
			set_block(id, peer_alive && !shut && !single, plp, mem);
		}
	}
	if (peer_alive)
		apply_sync(plk);
	else if (!was_alive || ninstalled)
		unsync_all();
}

static void *mlag_thread(void *arg)
{
	long long next_hello = 0;

	(void)arg;
	for (;;) {
		long long now;

		usleep(TICK_MS * 1000);
		now = now_ms();
		pthread_mutex_lock(&mlag_lock);
		if (enabled) {
			bcm_pbmp_t plp;
			int plk = peer_link_key(&plp);

			if (now >= next_hello || trigger) {
				trigger = 0;
				hb_tick(now);
				if (plk >= 0)
					send_frames(plk);
				next_hello = now + HELLO_MS;
			}
			evaluate(now);
		}
		pthread_mutex_unlock(&mlag_lock);
	}
	return NULL;
}

/* ---------------------------------------------------------------------- */
/* The contract */

int nosaic_mlag_supported(void)
{
	return mlag_unit >= 0;
}

int nosaic_mlag_set(int on, const char *plink, const char *paddr, int prio,
		    char *err, size_t n)
{
	bcm_pbmp_t pbm;
	int k;

	if (!nosaic_mlag_supported())
		return -2;
	pthread_mutex_lock(&mlag_lock);
	if (!on) {
		int id;

		BCM_PBMP_CLEAR(pbm);
		set_shut(0);
		block_why = " (mlag off)";
		for (id = 1; id <= NOSAIC_MLAG_MAX_ID; id++)
			set_block(id, 0, pbm, pbm);
		unsync_all();
		enabled = 0;
		memset((void *)excluded, 0, sizeof(excluded));
		peer_link[0] = peer_addr[0] = '\0';
		peer.heard = 0;
		role = ROLE_NONE;
		sys_set = 0;
		pthread_mutex_unlock(&mlag_lock);
		return 0;
	}
	snprintf(peer_link, sizeof(peer_link), "%s", plink ? plink : "");
	if (peer_link_key(&pbm) < 0) {
		if (err != NULL)
			snprintf(err, n, "no such port %s", peer_link);
		peer_link[0] = '\0';
		pthread_mutex_unlock(&mlag_lock);
		return -1;
	}
	for (k = 1; k <= NOSAIC_MAX_LAGS; k++) {
		if (mlag_of[k] && strcmp(nosaic_lag_name(k), peer_link) == 0) {
			if (err != NULL)
				snprintf(err, n, "%s is MLAG interface %d; the peer-link cannot be one",
					 peer_link, mlag_of[k]);
			peer_link[0] = '\0';
			pthread_mutex_unlock(&mlag_lock);
			return -1;
		}
	}
	{
		int p = -1;

		for (k = 0; k < nosaic_tap_count(); k++) {
			const char *nm = NULL;

			if (nosaic_tap_info(k, &nm, &p, NULL, NULL, NULL) == 0 && nm &&
			    strcmp(nm, peer_link) == 0 && nosaic_lag_of_port(p)) {
				if (err != NULL)
					snprintf(err, n, "%s is a member of %s; make %s the peer-link instead",
						 peer_link, nosaic_lag_name(nosaic_lag_of_port(p)),
						 nosaic_lag_name(nosaic_lag_of_port(p)));
				peer_link[0] = '\0';
				pthread_mutex_unlock(&mlag_lock);
				return -1;
			}
		}
	}
	if (paddr && paddr[0]) {
		struct in_addr a;

		if (inet_pton(AF_INET, paddr, &a) != 1) {
			if (err != NULL)
				snprintf(err, n, "mlag peer-address %s: an IPv4 address is needed "
					 "for the heartbeat", paddr);
			peer_link[0] = '\0';
			pthread_mutex_unlock(&mlag_lock);
			return -1;
		}
	}
	if (prio < 0 || prio > 65535) {
		if (err != NULL)
			snprintf(err, n, "mlag priority %d: must be 0 to 65535", prio);
		pthread_mutex_unlock(&mlag_lock);
		return -1;
	}
	snprintf(peer_addr, sizeof(peer_addr), "%s", paddr ? paddr : "");
	priority = prio;
	if (!enabled) {
		printf("mlag: on, peer-link %s%s%s\n", peer_link,
		       peer_addr[0] ? ", heartbeat to " : "", peer_addr);
		fflush(stdout);
	}
	enabled = 1;
	pthread_mutex_unlock(&mlag_lock);
	return 0;
}

int nosaic_mlag_set_lag(const char *name, int id, char *err, size_t n)
{
	int key, svc, k;
	bcm_pbmp_t pbm;

	if (!nosaic_mlag_supported())
		return -2;
	if (id < 0 || id > NOSAIC_MLAG_MAX_ID) {
		if (err != NULL)
			snprintf(err, n, "mlag id %d: must be 1 to %d, or 0 for none", id,
				 NOSAIC_MLAG_MAX_ID);
		return -1;
	}
	if (nosaic_lag_iface(name, &key, &pbm, &svc) != 0) {
		if (err != NULL)
			snprintf(err, n, "%s does not exist", name);
		return -1;
	}
	pthread_mutex_lock(&mlag_lock);
	if (id && enabled && strcmp(name, peer_link) == 0) {
		pthread_mutex_unlock(&mlag_lock);
		if (err != NULL)
			snprintf(err, n, "%s is the peer-link; it cannot be an MLAG interface", name);
		return -1;
	}
	for (k = 1; k <= NOSAIC_MAX_LAGS; k++) {
		if (id && k != key && mlag_of[k] == id) {
			pthread_mutex_unlock(&mlag_lock);
			if (err != NULL)
				snprintf(err, n, "mlag id %d is already %s", id, nosaic_lag_name(k));
			return -1;
		}
	}
	if (mlag_of[key] && mlag_of[key] != id) {
		bcm_pbmp_t none;

		BCM_PBMP_CLEAR(none);
		block_why = " (no longer that mlag id)";
		set_block(mlag_of[key], 0, none, none);
	}
	if (mlag_of[key] != id) {
		printf("mlag: %s is %s%d\n", name, id ? "mlag " : "no longer an mlag interface ", id);
		fflush(stdout);
	}
	mlag_of[key] = id;
	pthread_mutex_unlock(&mlag_lock);
	return 0;
}

void nosaic_mlag_query(FILE *out)
{
	int k, first = 1;

	pthread_mutex_lock(&mlag_lock);
	fprintf(out, "{\"ok\":true,\"result\":{\"Enabled\":%s,\"Role\":\"%s\",\"PeerLink\":\"%s\","
		"\"PeerLinkUp\":%s,\"PeerAlive\":%s,\"Heartbeat\":%s,\"Peer\":\"",
		enabled ? "true" : "false", role_name[enabled ? role : ROLE_NONE], peer_link,
		link_up ? "true" : "false", peer_alive ? "true" : "false",
		hb_alive ? "true" : "false");
	if (enabled && peer.heard)
		fprintf(out, "%02x%02x%02x%02x%02x%02x", peer.mac[0], peer.mac[1], peer.mac[2],
			peer.mac[3], peer.mac[4], peer.mac[5]);
	fprintf(out, "\",\"SystemID\":\"%02x%02x%02x%02x%02x%02x\",\"SyncedMACs\":%d,"
		"\"Interfaces\":[", sys_id[0], sys_id[1], sys_id[2], sys_id[3], sys_id[4],
		sys_id[5], ninstalled);
	for (k = 1; k <= NOSAIC_MAX_LAGS; k++) {
		int id = mlag_of[k], local, rem;
		const char *st;

		if (!id || nosaic_lag_tid(k) < 0)
			continue;
		local = nosaic_lag_active(k) > 0 && !shut;
		rem = peer_alive && peer.up[id];
		st = shut ? "disabled" : local && rem ? "active" : local ? "local" :
		     rem ? "peer" : "down";
		fprintf(out, "%s{\"LAG\":\"%s\",\"ID\":%d,\"Local\":%s,\"Peer\":%s,\"State\":\"%s\"}",
			first ? "" : ",", nosaic_lag_name(k), id, local ? "true" : "false",
			rem ? "true" : "false", st);
		first = 0;
	}
	fprintf(out, "]}}\n");
	pthread_mutex_unlock(&mlag_lock);
}

/* ---------------------------------------------------------------------- */
/* For lag.c and rstp.c */

int nosaic_mlag_id(int lag)
{
	return lag >= 1 && lag <= NOSAIC_MAX_LAGS ? mlag_of[lag] : 0;
}

int nosaic_mlag_lacp(int lag, unsigned char sys[6], unsigned *key, unsigned *port_offset)
{
	int id = nosaic_mlag_id(lag);

	if (!enabled || !id)
		return 0;
	memcpy(sys, sys_set ? sys_id : my_mac, 6);
	*key = 0x100u + (unsigned)id;
	*port_offset = role == ROLE_SECONDARY ? 0x800u : 0u;
	return 1;
}

void nosaic_mlag_lag_gone(int lag)
{
	if (lag < 1 || lag > NOSAIC_MAX_LAGS)
		return;
	pthread_mutex_lock(&mlag_lock);
	if (mlag_of[lag]) {
		bcm_pbmp_t none;

		BCM_PBMP_CLEAR(none);
		block_why = " (the LAG is gone)";
		set_block(mlag_of[lag], 0, none, none);
		mlag_of[lag] = 0;
	}
	pthread_mutex_unlock(&mlag_lock);
}

int nosaic_mlag_stp_excluded(int key)
{
	return key >= 0 && key < NOSAIC_MAX_TAPS + NOSAIC_MAX_LAGS + 1 && excluded[key];
}

/* ---------------------------------------------------------------------- */

int nosaic_mlag_start(int unit)
{
	pthread_t th;
	int rv;

	if (nosaic_tap_count() > 0)
		nosaic_tap_info(0, NULL, NULL, NULL, NULL, my_mac);
	rv = bcm_rx_register(unit, "nosaic-mlag", mlag_rx, RX_PRIORITY, NULL,
			     BCM_RCO_F_ALL_COS);
	if (rv != BCM_E_NONE) {
		fprintf(stderr, "mlag: bcm_rx_register: %s; no mlag\n", bcm_errmsg(rv));
		return -1;
	}
	if (pthread_create(&th, NULL, mlag_thread, NULL) != 0) {
		fprintf(stderr, "mlag: no thread: %s; no mlag\n", strerror(errno));
		return -1;
	}
	pthread_detach(th);
	mlag_unit = unit;
	return 0;
}
