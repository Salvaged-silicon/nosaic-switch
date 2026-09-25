/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Link aggregation: port-channels in the chip's trunk table, static or
 * negotiated by LACP (IEEE 802.1AX). switchapi 1.3.
 *
 * WHAT THE CHIP DOES AND WHAT THIS DOES
 *
 * The chip forwards: a next hop or an L2 entry that names the trunk is hashed
 * across whichever members the trunk table holds, and a frame received on any
 * member is taken as received on the trunk. What the chip does not do is
 * decide which members those are. That is this file's job, and it is the
 * whole of LACP: a member is in the trunk table only while it is
 * DISTRIBUTING -- link up, and for LACP, agreed with the partner.
 *
 * ⚠ THE CHIP'S OWN LINKSCAN IS NOT ENOUGH. On the Tridents a hardware
 * linkscan takes a member whose link drops out of the trunk, but never puts
 * it back; the AS5610 scans in software and removes nothing. Membership is
 * therefore re-stated here from what this file knows, every time it changes.
 *
 * ROUTED AND SWITCHED
 *
 * A LAG is an interface, and like a port it is routed until it joins a VLAN
 * (vlan.c). Routed, its members sit untagged in the LAG's own service VLAN,
 * NOSAIC_LAG_SVC_BASE + N, instead of their own, so what they receive is
 * punted tagged with it and reaches the tap po<N>; the router interface on
 * that VLAN sends its next hops to the trunk. Switched, vlan.c puts the
 * members in the LAG's VLANs as a set.
 *
 * Either way a member's own tap, swpN, falls silent while it belongs to a LAG
 * (nosaic_lag_port_claimed): the port is not an interface of its own any more.
 *
 * LOCKING
 *
 * The configuration -- which LAGs exist and which ports are in them -- is
 * changed only by the query server, one request at a time, and read by
 * vlan.c from inside those same requests, so it takes no lock. What the LACP
 * thread and the receive callback share with the query server -- partner
 * information, member state, the trunk table -- is under lag_lock. The rule
 * that keeps it deadlock-free is that lag_lock is never held across a call
 * into vlan.c, which takes its own.
 *
 * What the packet paths read -- which LAG claims a port, and each LAG's
 * distributing members -- is published in plain arrays that are written
 * whole-word and read without a lock. A reader that races a change sends one
 * frame by a member that has just stopped distributing, which a LAG has to
 * tolerate anyway.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <bcm/error.h>
#include <bcm/l2.h>
#include <bcm/link.h>
#include <bcm/pkt.h>
#include <bcm/port.h>
#include <bcm/rx.h>
#include <bcm/stat.h>
#include <bcm/stg.h>
#include <bcm/trunk.h>
#include <bcm/vlan.h>

#include "l3sync.h"
#include "lag.h"
#include "tapbridge.h"
#include "vlan.h"

#define LAG_L3_MTU     9216
#define RX_PRIORITY    110          /* ahead of tapbridge's 100 */
#define TICK_MS        200
#define FAST_PERIOD_MS 1000         /* LACPDUs, short timeout */
#define SLOW_PERIOD_MS 30000        /* to a partner that asked for long */
#define SHORT_TIMEOUT_MS 3000       /* three missed fast LACPDUs */
#define LONG_TIMEOUT_MS  90000
#define MAX_PORT       256

/* Actor and partner state bits, 802.1AX 6.4.2.3. */
#define ST_ACTIVITY    0x01
#define ST_TIMEOUT     0x02         /* short */
#define ST_AGGREGATION 0x04
#define ST_SYNC        0x08
#define ST_COLLECTING  0x10
#define ST_DISTRIBUTING 0x20
#define ST_DEFAULTED   0x40
#define ST_EXPIRED     0x80

struct peer {
	unsigned char  sys[6];
	unsigned short sys_pri, key, port_pri, port;
	unsigned char  state;
};

struct member {
	int           port;          /* logical port */
	int           tap;           /* its own tap, for its name and MAC */
	int           svc;           /* its own service VLAN, while routed */
	int           link;
	int           dist;          /* in the trunk table */
	uint32        learn;         /* bcm_port_learn_get, before it joined */
	/* LACP */
	unsigned char actor;         /* our state on this link, as last sent */
	struct peer   partner;
	struct peer   partner_view;  /* what the partner says we are */
	int           partner_valid;
	long long     expires, next_tx;
	int           ntt;           /* something changed: send now */
	unsigned long rx, tx, rx_bad;
};

struct lag {
	int           used;
	int           lacp;
	bcm_trunk_t   tid;
	int           svc;
	char          name[8];
	unsigned char mac[6];
	int           l3;            /* has its router interface */
	int           n;
	struct member m[NOSAIC_MAX_LAGM];
};

static struct lag lags[NOSAIC_MAX_LAGS + 1];         /* 1-based */
static pthread_mutex_t lag_lock = PTHREAD_MUTEX_INITIALIZER;
static int lag_unit = -1;
static bcm_pbmp_t cpu_pbm;
static unsigned char sys_mac[6];

/* Published for the packet paths; see LOCKING. */
static volatile int claimed[MAX_PORT];                /* port -> LAG, 0 none */
static volatile int dports[NOSAIC_MAX_LAGS + 1][NOSAIC_MAX_LAGM];
static volatile int ndist[NOSAIC_MAX_LAGS + 1];
static volatile int tid_lag[1024];                    /* trunk id -> LAG */
static volatile int relink[NOSAIC_MAX_LAGS + 1];      /* a member's link moved */

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int lag_key(const char *name)
{
	int n = 0;
	char tail;

	if (name == NULL || sscanf(name, "po%d%c", &n, &tail) != 1 ||
	    n < 1 || n > NOSAIC_MAX_LAGS)
		return 0;
	{
		char canon[8];

		snprintf(canon, sizeof(canon), "po%d", n);
		if (strcmp(canon, name) != 0)
			return 0;               /* po01, po+1 */
	}
	return n;
}

static int tap_of(const char *name, int *port, int *svc)
{
	int i;

	for (i = 0; i < nosaic_tap_count(); i++) {
		const char *n = NULL;
		int p = -1, v = 0;

		if (nosaic_tap_info(i, &n, &p, &v, NULL, NULL) == 0 && n != NULL &&
		    strcmp(n, name) == 0) {
			*port = p;
			*svc = v;
			return i;
		}
	}
	return -1;
}

static const char *tap_name(int tap)
{
	const char *n = NULL;

	if (nosaic_tap_info(tap, &n, NULL, NULL, NULL, NULL) != 0 || n == NULL)
		return "?";
	return n;
}

static void stg_forward(int vid, int port)
{
	bcm_stg_t stg;

	if (bcm_vlan_stg_get(lag_unit, (bcm_vlan_t)vid, &stg) == BCM_E_NONE)
		bcm_stg_stp_set(lag_unit, stg, port, BCM_STG_STP_FORWARD);
}

/* ---------------------------------------------------------------------- */
/* The trunk table: exactly the distributing members. Caller holds lag_lock. */

static void publish(int k)
{
	struct lag *g = &lags[k];
	bcm_trunk_member_t mem[NOSAIC_MAX_LAGM];
	bcm_trunk_info_t info;
	int i, n = 0, rv;

	memset(mem, 0, sizeof(mem));
	for (i = 0; i < g->n; i++) {
		bcm_gport_t gp;

		if (!g->m[i].dist)
			continue;
		if (bcm_port_gport_get(lag_unit, g->m[i].port, &gp) != BCM_E_NONE)
			continue;
		bcm_trunk_member_t_init(&mem[n]);
		mem[n].gport = gp;
		dports[k][n] = g->m[i].port;
		n++;
	}
	ndist[k] = n;

	bcm_trunk_info_t_init(&info);
	info.psc = BCM_TRUNK_PSC_SRCDSTIP;
	/*
	 * ⚠ -1, NOT 0. These name the member that floods, multicasts and
	 * multicast-routes for the trunk, and zero is a member index: a zeroed
	 * structure sends every flooded frame out of member 0, whether or not it
	 * is the one that is up. -1 means "hash it like the rest".
	 */
	info.dlf_index = -1;
	info.mc_index = -1;
	info.ipmc_index = -1;
	rv = bcm_trunk_set(lag_unit, g->tid, &info, n, n ? mem : NULL);
	if (rv != BCM_E_NONE)
		fprintf(stderr, "lag: %s: bcm_trunk_set %d members: %s\n",
			g->name, n, bcm_errmsg(rv));
}

/* ---------------------------------------------------------------------- */
/* LACP */

static void put16(unsigned char *p, unsigned v)
{
	p[0] = (unsigned char)(v >> 8);
	p[1] = (unsigned char)v;
}

static unsigned get16(const unsigned char *p)
{
	return ((unsigned)p[0] << 8) | p[1];
}

static void put_peer(unsigned char *p, int type, const struct peer *x)
{
	p[0] = (unsigned char)type;
	p[1] = 20;
	put16(p + 2, x->sys_pri);
	memcpy(p + 4, x->sys, 6);
	put16(p + 10, x->key);
	put16(p + 12, x->port_pri);
	put16(p + 14, x->port);
	p[16] = x->state;
	/* p[17..19] reserved */
}

static void get_peer(const unsigned char *p, struct peer *x)
{
	x->sys_pri = (unsigned short)get16(p + 2);
	memcpy(x->sys, p + 4, 6);
	x->key = (unsigned short)get16(p + 10);
	x->port_pri = (unsigned short)get16(p + 12);
	x->port = (unsigned short)get16(p + 14);
	x->state = p[16];
}

static void actor_of(int k, const struct member *m, struct peer *a)
{
	memset(a, 0, sizeof(*a));
	a->sys_pri = 32768;
	memcpy(a->sys, sys_mac, 6);
	a->key = (unsigned short)k;
	a->port_pri = 32768;
	a->port = (unsigned short)(m->port + 1);
	a->state = m->actor;
}

/* One LACPDU out of one member. Caller holds lag_lock. */
static void send_pdu(int k, struct member *m)
{
	unsigned char f[128], mac[6];
	struct peer a, p;
	int vid;

	memset(f, 0, sizeof(f));
	f[0] = 0x01; f[1] = 0x80; f[2] = 0xc2; f[5] = 0x02;
	if (nosaic_tap_info(m->tap, NULL, NULL, NULL, NULL, mac) != 0)
		memcpy(mac, sys_mac, 6);
	memcpy(f + 6, mac, 6);
	f[12] = 0x88; f[13] = 0x09;
	f[14] = 1;                              /* subtype LACP */
	f[15] = 1;                              /* version */
	actor_of(k, m, &a);
	put_peer(f + 16, 1, &a);
	if (m->partner_valid)
		p = m->partner;
	else
		memset(&p, 0, sizeof(p));
	put_peer(f + 36, 2, &p);
	f[56] = 3;                              /* collector */
	f[57] = 16;
	/* max delay 0, reserved 12; terminator 0/0 at 72, reserved to 124 */
	vid = nosaic_vlan_lag_switched(k) ? m->svc : lags[k].svc;
	if (nosaic_tap_xmit(m->port, vid, f, 124) == 0)
		m->tx++;
	m->ntt = 0;
}

/*
 * Selection and the mux, 802.1AX 6.4.14-15, reduced to what a switch with
 * one aggregator per LAG needs: a member is SELECTED if its link is up and
 * its partner is aggregatable and is the same system and key as the LAG's
 * other selected members. It COLLECTS once the partner says it is in sync,
 * and DISTRIBUTES once the partner is collecting too. Caller holds lag_lock.
 */
static int lacp_eval(int k, long long now)
{
	struct lag *g = &lags[k];
	const struct peer *agg = NULL;
	int i, changed = 0;

	for (i = 0; i < g->n; i++) {
		struct member *m = &g->m[i];

		if (m->partner_valid && now >= m->expires) {
			m->partner_valid = 0;          /* three LACPDUs missed */
			m->ntt = 1;
		}
		if (!m->link && m->partner_valid) {
			m->partner_valid = 0;
			m->ntt = 1;
		}
	}
	for (i = 0; i < g->n; i++) {
		struct member *m = &g->m[i];
		unsigned char st = ST_ACTIVITY | ST_TIMEOUT | ST_AGGREGATION;
		int sel, dist;

		sel = m->link && m->partner_valid &&
		      (m->partner.state & ST_AGGREGATION);
		if (sel && agg == NULL)
			agg = &m->partner;
		if (sel && (memcmp(agg->sys, m->partner.sys, 6) != 0 ||
			    agg->key != m->partner.key))
			sel = 0;                     /* a different partner */
		if (!m->partner_valid)
			st |= ST_DEFAULTED | (m->link ? ST_EXPIRED : 0);
		if (sel) {
			st |= ST_SYNC;
			if (m->partner.state & ST_SYNC)
				st |= ST_COLLECTING;
		}
		dist = (st & ST_COLLECTING) && (m->partner.state & ST_COLLECTING);
		if (dist)
			st |= ST_DISTRIBUTING;
		if (st != m->actor) {
			m->actor = st;
			m->ntt = 1;
		}
		if (dist != m->dist) {
			m->dist = dist;
			changed = 1;
			printf("lag: %s %s %s\n", g->name, tap_name(m->tap),
			       dist ? "distributing" : "stopped distributing");
			fflush(stdout);
		}
	}
	return changed;
}

static void *lag_thread(void *arg)
{
	(void)arg;
	for (;;) {
		long long now;
		int k, i;

		usleep(TICK_MS * 1000);
		now = now_ms();
		pthread_mutex_lock(&lag_lock);
		for (k = 1; k <= NOSAIC_MAX_LAGS; k++) {
			struct lag *g = &lags[k];
			int changed = 0;

			if (!g->used)
				continue;
			for (i = 0; i < g->n; i++) {
				struct member *m = &g->m[i];
				int up = 0;

				if (bcm_port_link_status_get(lag_unit, m->port, &up) != BCM_E_NONE)
					up = 0;
				up = up == BCM_PORT_LINK_STATUS_UP;
				if (up != m->link) {
					m->link = up;
					m->ntt = 1;
					printf("lag: %s %s link %s\n", g->name,
					       tap_name(m->tap), up ? "up" : "down");
					fflush(stdout);
				}
				if (!g->lacp && m->dist != m->link) {
					m->dist = m->link;
					changed = 1;
				}
			}
			/* The chip's linkscan may have taken a member out behind
			 * our back, and a flap shorter than a tick leaves nothing
			 * else to show for it: state the table again. */
			if (relink[k]) {
				relink[k] = 0;
				changed = 1;
			}
			if (g->lacp) {
				changed |= lacp_eval(k, now);
				for (i = 0; i < g->n; i++) {
					struct member *m = &g->m[i];
					int period = (m->partner_valid &&
						      !(m->partner.state & ST_TIMEOUT)) ?
						     SLOW_PERIOD_MS : FAST_PERIOD_MS;

					if (!m->link)
						continue;
					if (m->ntt || now >= m->next_tx) {
						send_pdu(k, m);
						m->next_tx = now + period;
					}
				}
			}
			if (changed)
				publish(k);
		}
		pthread_mutex_unlock(&lag_lock);
	}
	return NULL;
}

/* The SDK's linkscan: a member's link changed, however briefly. */
static void lag_linkscan(int unit, bcm_port_t port, bcm_port_info_t *info)
{
	int k;

	(void)unit;
	(void)info;
	if ((k = nosaic_lag_of_port(port)) != 0)
		relink[k] = 1;
}

/* A frame's receiving port. src_port is relative to its module: on a chip
 * with two module ids the second half comes back as port - 32 (vlan.h). */
static int rx_port(int unit, bcm_pkt_t *pkt)
{
	bcm_gport_t gp;
	bcm_port_t local;

	BCM_GPORT_MODPORT_SET(gp, pkt->src_mod, pkt->src_port);
	if (bcm_port_local_get(unit, gp, &local) != BCM_E_NONE)
		return pkt->src_port;
	return local;
}

static bcm_rx_t lag_rx(int unit, bcm_pkt_t *pkt, void *cookie)
{
	const unsigned char *p = pkt->pkt_data[0].data;
	int len = (int)pkt->pkt_data[0].len, off = 12, port, k, i;

	(void)cookie;
	if (len < 16)
		return BCM_RX_NOT_HANDLED;
	if (p[12] == 0x81 && p[13] == 0x00)
		off = 16;                       /* the punt tag */
	if (len < off + 2 || p[off] != 0x88 || p[off + 1] != 0x09)
		return BCM_RX_NOT_HANDLED;

	/* Slow protocols are ours whatever they are: a marker PDU (subtype 2)
	 * handed to a routed tap would only confuse the kernel. */
	port = rx_port(unit, pkt);
	if (port < 0 || port >= MAX_PORT || (k = claimed[port]) == 0)
		return BCM_RX_HANDLED;
	p += off + 2;
	len -= off + 2;
	if (len < 58 || p[0] != 1 || p[2] != 1 || p[3] != 20 || p[22] != 2 || p[23] != 20)
		return BCM_RX_HANDLED;

	pthread_mutex_lock(&lag_lock);
	for (i = 0; i < lags[k].n; i++) {
		struct member *m = &lags[k].m[i];
		struct peer old = m->partner;
		struct peer a;

		if (m->port != port)
			continue;
		m->rx++;
		if (!lags[k].lacp)
			break;                      /* a static LAG ignores LACP */
		get_peer(p + 2, &m->partner);
		get_peer(p + 22, &m->partner_view);
		m->expires = now_ms() + ((m->actor & ST_TIMEOUT) ?
					 SHORT_TIMEOUT_MS : LONG_TIMEOUT_MS);
		if (!m->partner_valid || memcmp(&old, &m->partner, sizeof(old)) != 0)
			m->ntt = 1;
		m->partner_valid = 1;
		/* The partner has us wrong: tell it again now, not in a second. */
		actor_of(k, m, &a);
		if (memcmp(a.sys, m->partner_view.sys, 6) != 0 ||
		    a.key != m->partner_view.key || a.port != m->partner_view.port ||
		    a.state != m->partner_view.state)
			m->ntt = 1;
		break;
	}
	pthread_mutex_unlock(&lag_lock);
	return BCM_RX_HANDLED;
}

/* ---------------------------------------------------------------------- */
/* Members joining and leaving. Called without lag_lock (vlan.c takes its own). */

/*
 * ⚠ NOTHING FLOODED IN ON ONE MEMBER MAY LEAVE BY ANOTHER.
 *
 * The chip floods a broadcast to one member of each trunk in the VLAN, and
 * that includes the trunk it came in on: proven between a 7050SX2 and a
 * 7050TX-64, where the TX sent the SX2's own ARP broadcasts straight back to
 * it over the other link. The SX2 then learned its OWN MAC on the trunk, and
 * every unicast frame for it -- the ARP replies it was waiting for -- was
 * dropped as going back out of the port it arrived on. Pings that had worked
 * stopped, and the adjacency sat in ExStart.
 *
 * So every pair of members blocks flooding to each other, both ways.
 */
static void flood_block(struct lag *g, const struct member *m, int on)
{
	uint32 f = on ? BCM_PORT_FLOOD_BLOCK_ALL : 0;
	int i;

	for (i = 0; i < g->n; i++) {
		if (g->m[i].port == m->port)
			continue;
		bcm_port_flood_block_set(lag_unit, m->port, g->m[i].port, f);
		bcm_port_flood_block_set(lag_unit, g->m[i].port, m->port, f);
	}
}

/*
 * Learning, for a member of a routed LAG: off. Its next hops name the trunk
 * and its transmit picks a member itself, so the L2 table has nothing to
 * tell it, and a learned entry can only be wrong. Switched, vlan.c turns it
 * on, because an SVI's next hops come from the L2 table.
 */
static void routed_learning(int port)
{
	bcm_port_learn_set(lag_unit, port, BCM_PORT_LEARN_FWD);
}

static void member_join(int k, struct member *m)
{
	bcm_pbmp_t pbm, none;
	struct lag *g = &lags[k];

	BCM_PBMP_CLEAR(pbm);
	BCM_PBMP_PORT_ADD(pbm, m->port);
	BCM_PBMP_CLEAR(none);

	/* Out of its own service VLAN: it is not an interface of its own. */
	if (m->svc > 0)
		bcm_vlan_port_remove(lag_unit, (bcm_vlan_t)m->svc, pbm);
	claimed[m->port] = k;
	flood_block(g, m, 1);
	if (bcm_port_learn_get(lag_unit, m->port, &m->learn) != BCM_E_NONE)
		m->learn = BCM_PORT_LEARN_ARL | BCM_PORT_LEARN_FWD;
	if (!nosaic_vlan_lag_join(k, m->port)) {
		/* Routed: into the LAG's service VLAN, untagged, as its PVID. */
		routed_learning(m->port);
		bcm_vlan_port_add(lag_unit, (bcm_vlan_t)g->svc, pbm, pbm);
		bcm_port_untagged_vlan_set(lag_unit, m->port, (bcm_vlan_t)g->svc);
		stg_forward(g->svc, m->port);
	}
	bcm_l2_addr_delete_by_port(lag_unit, -1, m->port, 0);
	printf("lag: %s joins %s\n", tap_name(m->tap), g->name);
	fflush(stdout);
}

static void member_leave(int k, struct member *m)
{
	bcm_pbmp_t pbm;
	struct lag *g = &lags[k];

	BCM_PBMP_CLEAR(pbm);
	BCM_PBMP_PORT_ADD(pbm, m->port);
	if (!nosaic_vlan_lag_leave(k, m->port))
		bcm_vlan_port_remove(lag_unit, (bcm_vlan_t)g->svc, pbm);
	flood_block(g, m, 0);
	bcm_port_learn_set(lag_unit, m->port, m->learn);
	/* Routed again, in its own service VLAN, as tapbridge made it. */
	if (m->svc > 0) {
		bcm_vlan_port_add(lag_unit, (bcm_vlan_t)m->svc, pbm, pbm);
		bcm_port_untagged_vlan_set(lag_unit, m->port, (bcm_vlan_t)m->svc);
		stg_forward(m->svc, m->port);
	}
	bcm_l2_addr_delete_by_port(lag_unit, -1, m->port, 0);
	claimed[m->port] = 0;
	printf("lag: %s leaves %s\n", tap_name(m->tap), g->name);
	fflush(stdout);
}

/* ---------------------------------------------------------------------- */
/* The contract */

static void say(char *err, size_t n, const char *fmt, const char *a, const char *b)
{
	if (err != NULL)
		snprintf(err, n, fmt, a, b);
}

int nosaic_lag_add(const char *name, int lacp, char *err, size_t n)
{
	struct lag *g;
	bcm_pbmp_t none, pbm;
	int k = lag_key(name), rv, i;

	if (lag_unit < 0)
		return -2;
	if (k == 0) {
		say(err, n, "%s is not a LAG name: po1 ... po64%s", name, "");
		return -1;
	}
	g = &lags[k];
	if (g->used) {
		if (g->lacp != !!lacp) {
			pthread_mutex_lock(&lag_lock);
			g->lacp = !!lacp;
			for (i = 0; i < g->n; i++) {
				g->m[i].partner_valid = 0;
				g->m[i].dist = 0;
				g->m[i].actor = 0;
				g->m[i].ntt = 1;
			}
			publish(k);
			pthread_mutex_unlock(&lag_lock);
			printf("lag: %s is now %s\n", g->name, lacp ? "LACP" : "static");
			fflush(stdout);
		}
		return 0;
	}

	memset(g, 0, sizeof(*g));
	snprintf(g->name, sizeof(g->name), "po%d", k);
	g->svc = NOSAIC_LAG_SVC_BASE + k;
	g->lacp = !!lacp;

	rv = bcm_trunk_create(lag_unit, 0, &g->tid);
	if (rv != BCM_E_NONE) {
		say(err, n, "%s: bcm_trunk_create: %s", name, bcm_errmsg(rv));
		return -1;
	}
	if (g->tid >= 0 && g->tid < (int)(sizeof(tid_lag) / sizeof(tid_lag[0])))
		tid_lag[g->tid] = k;

	/* Its service VLAN, with the CPU tagged, like a port's (tapbridge.c). */
	rv = bcm_vlan_create(lag_unit, (bcm_vlan_t)g->svc);
	if (rv != BCM_E_NONE && rv != BCM_E_EXISTS) {
		bcm_trunk_destroy(lag_unit, g->tid);
		say(err, n, "%s: bcm_vlan_create: %s", name, bcm_errmsg(rv));
		return -1;
	}
	BCM_PBMP_CLEAR(none);
	BCM_PBMP_ASSIGN(pbm, cpu_pbm);
	bcm_vlan_port_add(lag_unit, (bcm_vlan_t)g->svc, pbm, none);
	{
		bcm_stg_t stg;
		bcm_port_t p;

		if (bcm_vlan_stg_get(lag_unit, (bcm_vlan_t)g->svc, &stg) == BCM_E_NONE)
			BCM_PBMP_ITER(cpu_pbm, p)
				bcm_stg_stp_set(lag_unit, stg, p, BCM_STG_STP_FORWARD);
	}

	if (nosaic_tap_lag_add(g->svc, g->name, g->mac, k) != 0) {
		bcm_trunk_destroy(lag_unit, g->tid);
		say(err, n, "%s: could not make its tap%s", name, "");
		return -1;
	}
	g->l3 = nosaic_l3_add_lag_intf(lag_unit, g->name, g->tid, g->svc, g->mac,
				       LAG_L3_MTU) == 0;
	if (!g->l3)
		fprintf(stderr, "lag: %s has no router interface; it will answer "
			"but the chip will not route through it\n", g->name);
	pthread_mutex_lock(&lag_lock);
	publish(k);
	g->used = 1;
	pthread_mutex_unlock(&lag_lock);
	printf("lag: %s created, %s, trunk %d, service vlan %d, "
	       "mac %02x:%02x:%02x:%02x:%02x:%02x\n", g->name,
	       lacp ? "LACP" : "static", (int)g->tid, g->svc,
	       g->mac[0], g->mac[1], g->mac[2], g->mac[3], g->mac[4], g->mac[5]);
	fflush(stdout);
	return 0;
}

int nosaic_lag_members(const char *name, const char **ports, int np,
		       char *err, size_t n)
{
	struct member want[NOSAIC_MAX_LAGM];
	struct lag *g;
	int k = lag_key(name), i, j;

	if (lag_unit < 0)
		return -2;
	if (k == 0 || !lags[k].used) {
		say(err, n, "%s does not exist%s", name, "");
		return -1;
	}
	g = &lags[k];
	if (np > NOSAIC_MAX_LAGM) {
		if (err != NULL)
			snprintf(err, n, "a LAG takes at most %d members", NOSAIC_MAX_LAGM);
		return -1;
	}

	/* Every port checked before anything changes. */
	memset(want, 0, sizeof(want));
	for (i = 0; i < np; i++) {
		struct member *m = &want[i];
		int other;

		m->tap = tap_of(ports[i], &m->port, &m->svc);
		if (m->tap < 0 || m->port < 0 || m->port >= MAX_PORT) {
			say(err, n, "no such port %s%s", ports[i], "");
			return -1;
		}
		for (j = 0; j < i; j++)
			if (want[j].port == m->port) {
				say(err, n, "%s is listed twice%s", ports[i], "");
				return -1;
			}
		other = claimed[m->port];
		if (other != 0 && other != k) {
			say(err, n, "%s is already a member of %s", ports[i],
			    lags[other].name);
			return -1;
		}
		if (other == 0 && nosaic_vlan_port_switched(m->port)) {
			say(err, n, "%s is a switched port; take it out of its "
			    "VLANs first%s", ports[i], "");
			return -1;
		}
	}

	/* Leavers first, then joiners; members that stay keep their state. */
	for (i = 0; i < g->n; i++) {
		struct member gone;

		for (j = 0; j < np; j++)
			if (want[j].port == g->m[i].port)
				break;
		if (j < np)
			continue;
		pthread_mutex_lock(&lag_lock);
		gone = g->m[i];
		g->m[i] = g->m[--g->n];
		publish(k);
		pthread_mutex_unlock(&lag_lock);
		member_leave(k, &gone);
		i--;
	}
	for (j = 0; j < np; j++) {
		for (i = 0; i < g->n; i++)
			if (g->m[i].port == want[j].port)
				break;
		if (i < g->n)
			continue;
		member_join(k, &want[j]);
		pthread_mutex_lock(&lag_lock);
		want[j].ntt = 1;
		g->m[g->n++] = want[j];
		pthread_mutex_unlock(&lag_lock);
	}
	return 0;
}

int nosaic_lag_del(const char *name, char *err, size_t n)
{
	struct lag *g;
	int k = lag_key(name), i;

	if (lag_unit < 0)
		return -2;
	if (k == 0 || !lags[k].used)
		return 0;
	g = &lags[k];
	nosaic_vlan_lag_forget(k);
	while (g->n > 0) {
		struct member gone;

		pthread_mutex_lock(&lag_lock);
		gone = g->m[--g->n];
		pthread_mutex_unlock(&lag_lock);
		member_leave(k, &gone);
	}
	pthread_mutex_lock(&lag_lock);
	g->used = 0;
	ndist[k] = 0;
	pthread_mutex_unlock(&lag_lock);

	/*
	 * The router interface goes, keeping its slot for the next po<N>; the
	 * service VLAN stays, empty but for the CPU, because next hops that
	 * named the interface may still hold it for a moment.
	 */
	nosaic_l3_del_intf(g->name);
	nosaic_tap_svi_del(g->svc);
	for (i = 0; i < (int)(sizeof(tid_lag) / sizeof(tid_lag[0])); i++)
		if (tid_lag[i] == k)
			tid_lag[i] = 0;
	bcm_l2_addr_delete_by_trunk(lag_unit, g->tid, 0);
	if (bcm_trunk_destroy(lag_unit, g->tid) != BCM_E_NONE)
		fprintf(stderr, "lag: %s: trunk %d not destroyed\n", g->name,
			(int)g->tid);
	(void)err;
	(void)n;
	printf("lag: %s removed\n", g->name);
	fflush(stdout);
	return 0;
}

void nosaic_lag_query(FILE *out)
{
	int k, i, first = 1;

	pthread_mutex_lock(&lag_lock);
	fprintf(out, "{\"ok\":true,\"result\":[");
	for (k = 1; k <= NOSAIC_MAX_LAGS; k++) {
		struct lag *g = &lags[k];

		if (!g->used)
			continue;
		fprintf(out, "%s{\"Name\":\"%s\",\"LACP\":%s,\"Trunk\":%d,"
			"\"SVI\":%s,\"Members\":[", first ? "" : ",", g->name,
			g->lacp ? "true" : "false", (int)g->tid,
			nosaic_vlan_lag_switched(k) ? "false" : "true");
		first = 0;
		for (i = 0; i < g->n; i++) {
			const struct member *m = &g->m[i];
			const unsigned char *s = m->partner.sys;
			uint64 in = 0, outp = 0;

			/* Frames through the member, from the chip: how the
			 * trunk hash is actually spreading the load. */
			bcm_stat_get(lag_unit, m->port, snmpIfHCInUcastPkts, &in);
			bcm_stat_get(lag_unit, m->port, snmpIfHCOutUcastPkts, &outp);

			fprintf(out, "%s{\"Port\":\"%s\",\"Active\":%s,\"Link\":%s,"
				"\"Actor\":%d,\"Partner\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
				"\"PartnerKey\":%d,\"PartnerState\":%d,\"PartnerValid\":%s,"
				"\"RxPDU\":%lu,\"TxPDU\":%lu,"
				"\"InUcast\":%llu,\"OutUcast\":%llu}",
				i ? "," : "", tap_name(m->tap),
				m->dist ? "true" : "false", m->link ? "true" : "false",
				m->actor, s[0], s[1], s[2], s[3], s[4], s[5],
				m->partner.key, m->partner.state,
				m->partner_valid ? "true" : "false", m->rx, m->tx,
				(unsigned long long)in, (unsigned long long)outp);
		}
		fprintf(out, "]}");
	}
	fprintf(out, "]}\n");
	pthread_mutex_unlock(&lag_lock);
}

void nosaic_lag_capability(int *max_lags, int *max_members)
{
	*max_lags = lag_unit >= 0 ? NOSAIC_MAX_LAGS : 0;
	*max_members = NOSAIC_MAX_LAGM;
}

/* ---------------------------------------------------------------------- */
/* For vlan.c */

int nosaic_lag_iface(const char *name, int *key, bcm_pbmp_t *ports, int *svc)
{
	int k = lag_key(name), i;

	if (k == 0 || !lags[k].used)
		return -1;
	*key = k;
	BCM_PBMP_CLEAR(*ports);
	for (i = 0; i < lags[k].n; i++)
		BCM_PBMP_PORT_ADD(*ports, lags[k].m[i].port);
	*svc = lags[k].svc;
	return 0;
}

int nosaic_lag_of_port(int port)
{
	return port >= 0 && port < MAX_PORT ? claimed[port] : 0;
}

const char *nosaic_lag_name(int key)
{
	static char names[NOSAIC_MAX_LAGS + 1][8];

	if (key < 1 || key > NOSAIC_MAX_LAGS)
		return "?";
	if (names[key][0] == '\0')
		snprintf(names[key], sizeof(names[key]), "po%d", key);
	return names[key];
}

/* ---------------------------------------------------------------------- */
/* For the packet paths: lock-free */

int nosaic_lag_port_claimed(int port)
{
	return nosaic_lag_of_port(port) != 0;
}

int nosaic_lag_pick(int key, unsigned hash)
{
	int n;

	if (key < 1 || key > NOSAIC_MAX_LAGS || (n = ndist[key]) <= 0)
		return -1;
	return dports[key][hash % (unsigned)n];
}

void nosaic_lag_flood_mask(bcm_pbmp_t *pbm, unsigned hash)
{
	bcm_pbmp_t in;
	bcm_port_t p;
	int k, seen[NOSAIC_MAX_LAGS + 1];

	/*
	 * Every member out first, then one back per LAG. Doing both in one walk
	 * put the chosen member back and then, when the walk reached it, took it
	 * out again: a broadcast whose hash chose the LAG's later port went
	 * nowhere, and an SVI's ARP failed while its unicast worked.
	 */
	memset(seen, 0, sizeof(seen));
	BCM_PBMP_ASSIGN(in, *pbm);
	BCM_PBMP_ITER(in, p) {
		if ((k = nosaic_lag_of_port(p)) != 0) {
			BCM_PBMP_PORT_REMOVE(*pbm, p);
			seen[k] = 1;
		}
	}
	for (k = 1; k <= NOSAIC_MAX_LAGS; k++) {
		int pick;

		if (seen[k] && (pick = nosaic_lag_pick(k, hash)) >= 0 &&
		    BCM_PBMP_MEMBER(in, pick))
			BCM_PBMP_PORT_ADD(*pbm, pick);
	}
}

int nosaic_lag_of_tid(int tid)
{
	if (tid < 0 || tid >= (int)(sizeof(tid_lag) / sizeof(tid_lag[0])))
		return 0;
	return tid_lag[tid];
}

/* ---------------------------------------------------------------------- */

int nosaic_lag_start(int unit)
{
	bcm_port_config_t cfg;
	pthread_t th;
	int rv;

	BCM_PBMP_CLEAR(cpu_pbm);
	if (bcm_port_config_get(unit, &cfg) == BCM_E_NONE)
		BCM_PBMP_ASSIGN(cpu_pbm, cfg.cpu);
	/* The system id: one address for every LAG on the switch, the first
	 * port's, which is derived from the switch's own (switch-mac.sh). */
	if (nosaic_tap_count() > 0)
		nosaic_tap_info(0, NULL, NULL, NULL, NULL, sys_mac);
	/*
	 * LACPDUs go to 01:80:c2:00:00:02, which the SDK's own L2 cache
	 * defaults already send to the CPU (01:80:c2:00:00:0x).
	 */
	rv = bcm_rx_register(unit, "nosaic-lacp", lag_rx, RX_PRIORITY, NULL,
			     BCM_RCO_F_ALL_COS);
	if (rv != BCM_E_NONE) {
		fprintf(stderr, "lag: bcm_rx_register: %s; no LAGs\n", bcm_errmsg(rv));
		return -1;
	}
	if (bcm_linkscan_register(unit, lag_linkscan) != BCM_E_NONE)
		fprintf(stderr, "lag: no linkscan callback; a member's link flap "
			"shorter than %d ms may leave it out of its trunk\n", TICK_MS);
	if (pthread_create(&th, NULL, lag_thread, NULL) != 0) {
		fprintf(stderr, "lag: no thread: %s; no LAGs\n", strerror(errno));
		return -1;
	}
	pthread_detach(th);
	lag_unit = unit;
	return 0;
}
