/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Rapid spanning tree: IEEE 802.1D-2004 clause 17 (802.1w), one instance for
 * every user VLAN. switchapi 1.4.
 *
 * WHERE IT RUNS
 *
 * On switched interfaces only: ports and LAGs that are members of a VLAN. A
 * routed port sits alone in its own service VLAN and has no L2 neighbour to
 * loop through, so it never takes part, and turning spanning tree on cannot
 * disturb a routed link or the OSPF on it.
 *
 * Every user VLAN is moved into one spanning-tree group of the chip's, made
 * at start-up, and an interface's state is written into that group for each
 * of its ports -- all the members of a LAG get the LAG's state. The service
 * VLANs stay in the default group, forwarding, as before.
 *
 * WHAT IT IMPLEMENTS
 *
 * The parts of clause 17 a switch needs, organised around one run() that is
 * called on every received BPDU and every tick, rather than as the standard's
 * eight separate state machines:
 *
 *   - role selection from priority vectors (17.21.25 updtRolesTree): root,
 *     designated, alternate, backup, disabled;
 *   - proposal and agreement (17.29-17.30): a designated port proposes, the
 *     bridge below syncs -- every designated port of its own discards until it
 *     has agreed downstream -- and answers with an agreement, so a
 *     point-to-point link forwards in one round trip, not two forward delays;
 *   - edge ports, configured or found by the absence of any BPDU for the
 *     migrate time (17.25, the bridge detection machine's AutoEdge);
 *   - topology change (17.31): a non-edge port that starts forwarding sets
 *     tcWhile on every root and designated port, their BPDUs carry TC, and
 *     learned MACs are flushed everywhere else;
 *   - disputes (17.21.10): a designated port that hears an inferior
 *     designated BPDU from a port that is learning or forwarding discards,
 *     which is the one-way-link protection;
 *   - 802.1D compatibility: a port that hears a configuration BPDU talks
 *     configuration BPDUs back, and treats TCNs as topology changes.
 *
 * LOCKING
 *
 * rstp_lock over everything below. vlan.c calls in holding vlan_lock, so the
 * order is vlan -> rstp, and nothing here calls into vlan.c. The per-port
 * state the packet paths read is published in pstate[] and read without it.
 */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <bcm/error.h>
#include <bcm/l2.h>
#include <bcm/pkt.h>
#include <bcm/port.h>
#include <bcm/rx.h>
#include <bcm/stg.h>
#include <bcm/vlan.h>

#include "lag.h"
#include "rstp.h"
#include "tapbridge.h"

#define NKEY          (NOSAIC_MAX_TAPS + NOSAIC_MAX_LAGS + 1)
#define MAX_PORT      256
#define RX_PRIORITY   105           /* ahead of tapbridge (100); LACP is 110 */
#define TICK_MS       100

/* Bridge times, 802.1D-2004 defaults, in milliseconds. */
#define HELLO_MS      2000
#define MAX_AGE_MS    20000
#define FWD_DELAY_MS  15000
#define MIGRATE_MS    3000
#define TX_HOLD       6             /* BPDUs per port per second */

enum { INFO_DISABLED, INFO_AGED, INFO_MINE, INFO_RECEIVED };
enum { R_DISABLED, R_ROOT, R_DESIGNATED, R_ALTERNATE, R_BACKUP };
enum { S_DISCARDING, S_LEARNING, S_FORWARDING };

static const char *role_name[] = { "disabled", "root", "designated", "alternate", "backup" };
static const char *state_name[] = { "discarding", "learning", "forwarding" };

/* BPDU flags, 802.1D-2004 9.3.3. */
#define F_TC          0x01
#define F_PROPOSAL    0x02
#define F_ROLE_SHIFT  2
#define F_ROLE_MASK   0x0c
#define F_LEARNING    0x10
#define F_FORWARDING  0x20
#define F_AGREEMENT   0x40
#define F_TCACK       0x80
#define ROLE_ALTBACK  1
#define ROLE_ROOT     2
#define ROLE_DESIG    3

/* A priority vector as the BPDU carries it, so memcmp orders it: root id (8),
 * root path cost (4), designated bridge id (8), designated port id (2). */
#define PV_LEN 22
#define PV_ROOT   0
#define PV_RPC    8
#define PV_BRIDGE 12
#define PV_PORT   20

struct times { int msg_age, max_age, hello, fwd; };   /* ms */

struct sport {
	int           used;          /* switched */
	int           enabled;       /* used, and link */
	/* configuration */
	int           admin_edge, cfg_cost;
	/* operation */
	int           oper_edge;
	int           cost;
	unsigned      id;            /* port id: priority 8, 12-bit number */
	int           role, state, info;
	unsigned char pv[PV_LEN];    /* the port priority vector */
	struct times  pt;            /* the port times */
	int           proposing, proposed, agreed, agree;
	int           send_rstp;     /* else configuration BPDUs */
	int           newinfo, tc_ack, heard;
	long long     rcvd_until, fd_until, tc_until, hello_next, edge_until;
	long long     tx_window;
	int           tx_count;
	/* a received BPDU, waiting for run() */
	int           pending;
	unsigned char msg[PV_LEN];
	struct times  mt;
	int           mflags, mtype;
	unsigned long rx, tx;
};

static struct sport ports[NKEY];
static pthread_mutex_t rstp_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rstp_wake = PTHREAD_COND_INITIALIZER;
static int rstp_unit = -1;
static bcm_stg_t user_stg = -1;
static volatile int enabled;
static int priority = 32768;
static unsigned char bridge_id[8];
static unsigned char root_pv[PV_LEN];
static int root_key = -1;
static struct times root_times;
static int reselect;
static unsigned long tc_count;

/* Published for the packet paths, and read without the lock. */
static volatile int kstate[NKEY];                    /* S_*, per interface */
static volatile int pstate[MAX_PORT];                /* BCM_STG_STP_*, per port */

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
static void put32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}
static unsigned get16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }
static uint32_t get32(const unsigned char *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* BPDU time fields are in 1/256 s. */
static int t_from(unsigned v) { return (int)((v * 1000u) / 256u); }
static unsigned t_to(int ms) { return (unsigned)((ms * 256) / 1000); }

/* ---------------------------------------------------------------------- */
/* Interfaces: a port by tap index, a LAG by NOSAIC_MAX_TAPS + its number */

static int is_lag(int k) { return k >= NOSAIC_MAX_TAPS; }
static int lag_of(int k) { return k - NOSAIC_MAX_TAPS; }

static const char *key_name(int k)
{
	const char *n = NULL;

	if (is_lag(k))
		return nosaic_lag_name(lag_of(k));
	if (nosaic_tap_info(k, &n, NULL, NULL, NULL, NULL) != 0 || n == NULL)
		return "?";
	return n;
}

static int key_port(int k)
{
	int p = -1;

	if (is_lag(k) || nosaic_tap_info(k, NULL, &p, NULL, NULL, NULL) != 0)
		return -1;
	return p;
}

/* The interface a port belongs to, -1 for one that is not a tap (the CPU). */
static int key_of_port(int port)
{
	int i, lag = nosaic_lag_of_port(port);

	if (lag != 0)
		return NOSAIC_MAX_TAPS + lag;
	for (i = 0; i < nosaic_tap_count(); i++)
		if (key_port(i) == port)
			return i;
	return -1;
}

static int key_by_name(const char *name)
{
	int i, key, svc;
	bcm_pbmp_t pbm;

	for (i = 0; i < nosaic_tap_count(); i++)
		if (strcmp(key_name(i), name) == 0)
			return i;
	if (nosaic_lag_iface(name, &key, &pbm, &svc) == 0)
		return NOSAIC_MAX_TAPS + key;
	return -1;
}

/* The ports an interface's state applies to. */
static void key_pbm(int k, bcm_pbmp_t *pbm)
{
	int key, svc;

	BCM_PBMP_CLEAR(*pbm);
	if (is_lag(k)) {
		nosaic_lag_iface(nosaic_lag_name(lag_of(k)), &key, pbm, &svc);
	} else if (key_port(k) >= 0) {
		BCM_PBMP_PORT_ADD(*pbm, key_port(k));
	}
}

static int key_link(int k)
{
	int up = 0;

	if (is_lag(k))
		return nosaic_lag_active(lag_of(k)) > 0;
	if (bcm_port_link_status_get(rstp_unit, key_port(k), &up) != BCM_E_NONE)
		return 0;
	return up == BCM_PORT_LINK_STATUS_UP;
}

/* 802.1D-2004 table 17-3: 20000000 / Mb/s. A LAG's speed is its active
 * members' total. */
static int key_cost(int k)
{
	int speed = 0, n = 1;
	bcm_pbmp_t pbm;
	bcm_port_t p;

	if (ports[k].cfg_cost > 0)
		return ports[k].cfg_cost;
	key_pbm(k, &pbm);
	BCM_PBMP_ITER(pbm, p) {
		bcm_port_speed_get(rstp_unit, p, &speed);
		break;
	}
	if (is_lag(k) && nosaic_lag_active(lag_of(k)) > 0)
		n = nosaic_lag_active(lag_of(k));
	if (speed <= 0)
		return 200000000;
	return 20000000 / (speed * n) > 0 ? 20000000 / (speed * n) : 1;
}

/* Port id: priority 128 in the top four bits, then a 12-bit number -- the
 * tap index + 1 for a port, 0x100 + N for po<N>. */
static unsigned key_id(int k)
{
	return 0x8000u | (unsigned)(is_lag(k) ? 0x100 + lag_of(k) : k + 1);
}

/* ---------------------------------------------------------------------- */
/* The chip */

static int bcm_state(int s)
{
	return s == S_FORWARDING ? BCM_STG_STP_FORWARD :
	       s == S_LEARNING ? BCM_STG_STP_LEARN : BCM_STG_STP_BLOCK;
}

static void program(int k, int s)
{
	bcm_pbmp_t pbm;
	bcm_port_t p;

	kstate[k] = s;
	key_pbm(k, &pbm);
	BCM_PBMP_ITER(pbm, p) {
		if (p < MAX_PORT)
			pstate[p] = bcm_state(s);
		if (user_stg >= 0)
			bcm_stg_stp_set(rstp_unit, user_stg, p, bcm_state(s));
	}
}

/* Forget what an interface learned: its ports', or its trunk's. */
static void flush(int k)
{
	bcm_pbmp_t pbm;
	bcm_port_t p;

	if (is_lag(k)) {
		int tid = nosaic_lag_tid(lag_of(k));

		if (tid >= 0)
			bcm_l2_addr_delete_by_trunk(rstp_unit, tid, 0);
		return;
	}
	key_pbm(k, &pbm);
	BCM_PBMP_ITER(pbm, p)
		bcm_l2_addr_delete_by_port(rstp_unit, -1, p, 0);
}

static void set_state(int k, int s)
{
	struct sport *sp = &ports[k];

	if (sp->state == s)
		return;
	if (sp->state == S_FORWARDING && s != S_FORWARDING)
		flush(k);                   /* what it learned is no longer reachable */
	sp->state = s;
	program(k, s);
}

/* ---------------------------------------------------------------------- */
/* BPDUs out */

static void send_bpdu(int k, long long now)
{
	struct sport *sp = &ports[k];
	unsigned char f[64], mac[6];
	int len, flags = 0, port;

	if (now - sp->tx_window >= 1000) {
		sp->tx_window = now;
		sp->tx_count = 0;
	}
	if (sp->tx_count >= TX_HOLD)
		return;                     /* the transmit hold count, 17.13.12 */

	if (is_lag(k))
		port = nosaic_lag_pick(lag_of(k), 0);
	else
		port = key_port(k);
	if (port < 0)
		return;

	memset(f, 0, sizeof(f));
	f[0] = 0x01; f[1] = 0x80; f[2] = 0xc2;           /* 01:80:c2:00:00:00 */
	if (is_lag(k) || nosaic_tap_info(k, NULL, NULL, NULL, NULL, mac) != 0)
		memcpy(mac, bridge_id + 2, 6);
	memcpy(f + 6, mac, 6);
	f[14] = 0x42; f[15] = 0x42; f[16] = 0x03;        /* LLC */
	/* f[17..18] protocol id 0 */
	if (now < sp->tc_until)
		flags |= F_TC;
	if (sp->send_rstp) {
		int code = sp->role == R_ROOT ? ROLE_ROOT :
			   sp->role == R_DESIGNATED ? ROLE_DESIG : ROLE_ALTBACK;

		f[19] = 2;                                   /* version */
		f[20] = 2;                                   /* RST BPDU */
		flags |= code << F_ROLE_SHIFT;
		if (sp->proposing && sp->role == R_DESIGNATED)
			flags |= F_PROPOSAL;
		if (sp->state >= S_LEARNING)
			flags |= F_LEARNING;
		if (sp->state == S_FORWARDING)
			flags |= F_FORWARDING;
		if (sp->agree && sp->role != R_DESIGNATED)
			flags |= F_AGREEMENT;
		len = 36;
	} else {
		f[19] = 0;
		f[20] = 0;                                   /* configuration BPDU */
		if (sp->tc_ack)
			flags |= F_TCACK;
		sp->tc_ack = 0;
		len = 35;
	}
	f[21] = (unsigned char)flags;
	/* The designated vector: the root, our cost to it, us, this port. */
	memcpy(f + 22, root_pv + PV_ROOT, 8);
	put32(f + 30, get32(root_pv + PV_RPC));
	memcpy(f + 34, bridge_id, 8);
	put16(f + 42, sp->id);
	put16(f + 44, t_to(root_times.msg_age));
	put16(f + 46, t_to(root_times.max_age));
	put16(f + 48, t_to(HELLO_MS));
	put16(f + 50, t_to(root_times.fwd));
	/* f[52] version 1 length 0, RST only */
	put16(f + 12, (unsigned)(3 + len));              /* 802.3 length */
	if (nosaic_tap_xmit(port, 1, f, 14 + 3 + len) == 0) {
		sp->tx++;
		sp->tx_count++;
	}
	sp->newinfo = 0;
}

/* ---------------------------------------------------------------------- */
/* Roles */

static void designated_vector(int k, unsigned char dv[PV_LEN])
{
	memcpy(dv + PV_ROOT, root_pv + PV_ROOT, 8);
	memcpy(dv + PV_RPC, root_pv + PV_RPC, 4);
	memcpy(dv + PV_BRIDGE, bridge_id, 8);
	put16(dv + PV_PORT, ports[k].id);
}

/* Every designated port that could forward into the old tree discards until
 * it has agreed with the bridge below it (17.21.14 setSyncTree). */
static void sync_all(void)
{
	int k;

	for (k = 0; k < NKEY; k++) {
		struct sport *sp = &ports[k];

		if (!sp->enabled || sp->role != R_DESIGNATED || sp->oper_edge || sp->agreed)
			continue;
		set_state(k, S_DISCARDING);
		sp->fd_until = 0;
	}
}

static int all_synced(int except)
{
	int k;

	for (k = 0; k < NKEY; k++) {
		struct sport *sp = &ports[k];

		if (k == except || !sp->enabled)
			continue;
		if (sp->role == R_DESIGNATED && !sp->oper_edge && !sp->agreed &&
		    sp->state != S_DISCARDING)
			return 0;
	}
	return 1;
}

static void select_roles(void)
{
	unsigned char best[PV_LEN], old_root[PV_LEN];
	int k, best_key = -1, old_key = root_key;

	/* This bridge as root: (B, 0, B, 0). */
	memset(best, 0, sizeof(best));
	memcpy(best + PV_ROOT, bridge_id, 8);
	memcpy(best + PV_BRIDGE, bridge_id, 8);
	memcpy(old_root, root_pv, PV_LEN);

	for (k = 0; k < NKEY; k++) {
		struct sport *sp = &ports[k];
		unsigned char cand[PV_LEN];
		int c;

		if (!sp->enabled || sp->info != INFO_RECEIVED)
			continue;
		if (memcmp(sp->pv + PV_BRIDGE, bridge_id, 8) == 0)
			continue;               /* our own BPDU, on another port */
		memcpy(cand, sp->pv, PV_LEN);
		put32(cand + PV_RPC, get32(sp->pv + PV_RPC) + (uint32_t)sp->cost);
		c = memcmp(cand, best, PV_LEN);
		if (c < 0 || (c == 0 && best_key >= 0 && sp->id < ports[best_key].id)) {
			memcpy(best, cand, PV_LEN);
			best_key = k;
		}
	}
	memcpy(root_pv, best, PV_LEN);
	root_key = best_key;
	if (best_key < 0) {
		root_times.msg_age = 0;
		root_times.max_age = MAX_AGE_MS;
		root_times.hello = HELLO_MS;
		root_times.fwd = FWD_DELAY_MS;
	} else {
		root_times = ports[best_key].pt;
		root_times.msg_age += 1000;
	}

	for (k = 0; k < NKEY; k++) {
		struct sport *sp = &ports[k];
		unsigned char dv[PV_LEN];
		int role;

		if (!sp->enabled) {
			role = R_DISABLED;
		} else if (k == best_key) {
			role = R_ROOT;
		} else {
			designated_vector(k, dv);
			if (sp->info == INFO_RECEIVED && memcmp(sp->pv, dv, PV_LEN) < 0) {
				role = memcmp(sp->pv + PV_BRIDGE, bridge_id, 8) == 0 ?
				       R_BACKUP : R_ALTERNATE;
			} else {
				role = R_DESIGNATED;
				if (sp->info != INFO_MINE || memcmp(sp->pv, dv, PV_LEN) != 0) {
					/* updtInfo: ours to send now, and agreed no more */
					memcpy(sp->pv, dv, PV_LEN);
					sp->pt = root_times;
					sp->info = INFO_MINE;
					sp->agreed = 0;
					sp->newinfo = 1;
				}
			}
		}
		if (role != sp->role) {
			sp->role = role;
			sp->proposing = 0;
			sp->fd_until = 0;
			sp->newinfo = 1;
			if (role != R_ROOT)
				sp->agree = 0;
		}
	}

	/* A new root, or a new way to it: sync before anything forwards into it. */
	if (best_key != old_key || memcmp(old_root, root_pv, PV_LEN) != 0) {
		if (best_key >= 0)
			ports[best_key].agree = 0;
		sync_all();
	}
	reselect = 0;
}

/* ---------------------------------------------------------------------- */
/* Topology change */

/*
 * A topology change: flush what every other port learned, and tell the rest
 * of the tree through the TC flag on our root and designated ports.
 *
 * ⚠ Received from a neighbour, it goes out of every port EXCEPT the one it
 * came in on, and a port already announcing one is left to finish (17.31,
 * tcProp and newTcWhile). Without both rules two bridges hand the same change
 * back and forth for ever, flushing their tables each time round.
 */
static void topology_change(int from, int rcvd, long long now)
{
	int k, started = 0;

	for (k = 0; k < NKEY; k++) {
		struct sport *sp = &ports[k];

		if (!sp->enabled || sp->oper_edge)
			continue;
		if (k != from)
			flush(k);
		if (rcvd && k == from)
			continue;
		if ((sp->role == R_ROOT || sp->role == R_DESIGNATED) && now >= sp->tc_until) {
			sp->tc_until = now + HELLO_MS + 1000;
			sp->newinfo = 1;
			started = 1;
		}
	}
	if (started || !rcvd)
		tc_count++;
}

/* ---------------------------------------------------------------------- */
/* A received BPDU (17.21.8 rcvInfo, 17.27 PIM) */

static void receive(int k, long long now)
{
	struct sport *sp = &ports[k];
	int role = (sp->mflags & F_ROLE_MASK) >> F_ROLE_SHIFT;
	int c;

	sp->pending = 0;
	sp->heard = 1;
	if (sp->oper_edge) {
		/* An edge -- configured or found -- that hears a bridge is not
		 * one any more (17.25). */
		sp->oper_edge = 0;
		reselect = 1;
	}
	sp->edge_until = 0;

	if (sp->mtype == 0x80) {                         /* TCN, from an 802.1D bridge */
		sp->send_rstp = 0;
		sp->tc_ack = 1;
		topology_change(k, 1, now);
		return;
	}
	if (sp->mtype == 0x00)                           /* configuration BPDU */
		sp->send_rstp = 0;
	if (sp->mtype == 0x02 && !sp->send_rstp) {
		sp->send_rstp = 1;                       /* it speaks RSTP after all */
		sp->newinfo = 1;
	}
	if (sp->mt.msg_age >= sp->mt.max_age)
		return;                                  /* too old to believe */
	if (sp->mtype == 0x00)
		role = ROLE_DESIG;                       /* a configuration BPDU is designated */

	if (memcmp(sp->msg + PV_BRIDGE, bridge_id, 8) == 0 &&
	    get16(sp->msg + PV_PORT) == sp->id) {
		/* Our own BPDU, looped back to the port that sent it: something
		 * between here and there is a loop spanning tree cannot see. Treat
		 * it as a backup, which discards. */
		memcpy(sp->pv, sp->msg, PV_LEN);
		sp->info = INFO_RECEIVED;
		sp->rcvd_until = now + 3 * HELLO_MS;
		reselect = 1;
		return;
	}

	if (role == ROLE_DESIG) {
		c = memcmp(sp->msg, sp->pv, PV_LEN);
		if (c < 0 || (sp->info == INFO_RECEIVED &&
			      memcmp(sp->msg + PV_BRIDGE, sp->pv + PV_BRIDGE, 10) == 0 && c != 0)) {
			/* Superior designated information -- or new information
			 * from the same designated port, better or worse. */
			memcpy(sp->pv, sp->msg, PV_LEN);
			sp->pt = sp->mt;
			sp->info = INFO_RECEIVED;
			sp->agreed = 0;
			reselect = 1;
		} else if (c == 0 && sp->info == INFO_RECEIVED) {
			/* Repeated: nothing changes but the timer. */
			if (memcmp(&sp->pt, &sp->mt, sizeof(sp->pt)) != 0) {
				sp->pt = sp->mt;
				reselect = 1;
			}
		} else {
			/* Inferior. If that bridge thinks it is designated and is
			 * already learning or forwarding, the link is one-way or
			 * our BPDUs are not arriving: a dispute (17.21.10). */
			if (sp->role == R_DESIGNATED && (sp->mflags & F_LEARNING)) {
				set_state(k, S_DISCARDING);
				sp->agreed = 0;
			}
			sp->newinfo = 1;                 /* tell it what it is missing */
			goto flags;
		}
		sp->rcvd_until = now + 3 * (sp->mt.hello > 0 ? sp->mt.hello : HELLO_MS);
		sp->proposed = (sp->mflags & F_PROPOSAL) != 0;
	} else if (sp->role == R_DESIGNATED && (sp->mflags & F_AGREEMENT) &&
		   memcmp(sp->msg + PV_ROOT, root_pv + PV_ROOT, 8) == 0) {
		/* The bridge below agrees: this port may forward at once. */
		sp->agreed = 1;
		sp->proposing = 0;
	}
flags:
	if (sp->mflags & F_TC)
		topology_change(k, 1, now);
}

/* ---------------------------------------------------------------------- */
/* Per-port transitions (17.29 PRT, 17.30 PST, 17.25 BDM) */

static void transition(int k, long long now)
{
	struct sport *sp = &ports[k];
	int was = sp->state;

	switch (sp->role) {
	case R_DISABLED:
		set_state(k, S_DISCARDING);
		return;

	case R_ALTERNATE:
	case R_BACKUP:
		/*
		 * Discarding, and ANSWERING a proposal (17.29.3): an alternate is
		 * already synced, so it agrees at once. Silence here left the
		 * designated port at the far end with no BPDU at all, and after
		 * the migrate time it decided it had a host on it -- an edge, on a
		 * link to a bridge.
		 */
		set_state(k, S_DISCARDING);
		if (sp->proposed) {
			sp->agree = 1;
			sp->newinfo = 1;
			sp->proposed = 0;
		}
		return;

	case R_ROOT:
		/* The old root port is already discarding (its role changed in
		 * the same selection), so the new one may forward at once. */
		if (sp->proposed) {
			sync_all();
			sp->proposed = 0;
		}
		if (!sp->agree && all_synced(k)) {
			sp->agree = 1;
			sp->newinfo = 1;
		}
		set_state(k, S_FORWARDING);
		break;

	case R_DESIGNATED:
		/* AutoEdge: a port that has heard no BPDU since it came up, for
		 * the migrate time, has a host on it, not a bridge. */
		if (!sp->oper_edge && !sp->heard && sp->edge_until && now >= sp->edge_until)
			sp->oper_edge = 1;
		if (sp->state == S_FORWARDING)
			break;
		if (sp->oper_edge || sp->agreed) {
			set_state(k, S_FORWARDING);
			sp->proposing = 0;
			break;
		}
		if (sp->send_rstp && !sp->proposing) {
			sp->proposing = 1;
			sp->newinfo = 1;
		}
		if (sp->fd_until == 0)
			sp->fd_until = now + root_times.fwd;
		if (now >= sp->fd_until) {
			set_state(k, sp->state == S_DISCARDING ? S_LEARNING : S_FORWARDING);
			sp->fd_until = now + root_times.fwd;
		}
		break;
	}
	if (was != S_FORWARDING && sp->state == S_FORWARDING && !sp->oper_edge) {
		printf("stp: %s %s forwarding\n", key_name(k), role_name[sp->role]);
		fflush(stdout);
		topology_change(k, 0, now);
	}
}

static void run(long long now)
{
	int k;

	if (!enabled)
		return;
	for (k = 0; k < NKEY; k++) {
		struct sport *sp = &ports[k];
		int up = sp->used && key_link(k);

		if (up != sp->enabled) {
			sp->enabled = up;
			reselect = 1;
			if (up) {
				/* A port coming up: nothing known, an edge only if
				 * configured one, and RSTP until told otherwise. */
				sp->info = INFO_AGED;
				sp->oper_edge = sp->admin_edge;
				sp->heard = 0;
				sp->edge_until = now + MIGRATE_MS;
				sp->send_rstp = 1;
				sp->agreed = sp->agree = sp->proposed = sp->proposing = 0;
				sp->hello_next = now;
			} else {
				sp->info = INFO_DISABLED;
				sp->pending = 0;
			}
		}
		if (!sp->enabled)
			continue;
		if (sp->cost != key_cost(k)) {
			sp->cost = key_cost(k);
			reselect = 1;
		}
		if (sp->pending)
			receive(k, now);
		if (sp->info == INFO_RECEIVED && now >= sp->rcvd_until) {
			sp->info = INFO_AGED;            /* three hellos missed */
			reselect = 1;
		}
	}
	if (reselect)
		select_roles();
	/* Down before up, so a port never forwards while the one it replaces
	 * still does. */
	for (k = 0; k < NKEY; k++)
		if (ports[k].enabled && ports[k].role != R_ROOT && ports[k].role != R_DESIGNATED)
			transition(k, now);
		else if (!ports[k].enabled && ports[k].used)
			set_state(k, S_DISCARDING);
	for (k = 0; k < NKEY; k++)
		if (ports[k].enabled && (ports[k].role == R_ROOT || ports[k].role == R_DESIGNATED))
			transition(k, now);
	for (k = 0; k < NKEY; k++) {
		struct sport *sp = &ports[k];

		if (!sp->enabled)
			continue;
		if (sp->role == R_DESIGNATED && (sp->newinfo || now >= sp->hello_next)) {
			send_bpdu(k, now);
			sp->hello_next = now + HELLO_MS;
		} else if (sp->role == R_ROOT && sp->newinfo && sp->send_rstp) {
			send_bpdu(k, now);
		} else if (sp->role == R_ROOT && sp->tc_ack) {
			send_bpdu(k, now);
		} else if ((sp->role == R_ALTERNATE || sp->role == R_BACKUP) &&
			   sp->newinfo && sp->agree && sp->send_rstp) {
			send_bpdu(k, now);
		} else {
			sp->newinfo = 0;
		}
	}
}

static void *rstp_thread(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&rstp_lock);
	for (;;) {
		struct timespec ts;

		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += TICK_MS * 1000000L;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000L;
		}
		pthread_cond_timedwait(&rstp_wake, &rstp_lock, &ts);
		run(now_ms());
	}
	return NULL;
}

/* ---------------------------------------------------------------------- */
/* BPDUs in */

static int rx_port(int unit, bcm_pkt_t *pkt)
{
	bcm_gport_t gp;
	bcm_port_t local;

	BCM_GPORT_MODPORT_SET(gp, pkt->src_mod, pkt->src_port);
	if (bcm_port_local_get(unit, gp, &local) != BCM_E_NONE)
		return pkt->src_port;
	return local;
}

static bcm_rx_t rstp_rx(int unit, bcm_pkt_t *pkt, void *cookie)
{
	static const unsigned char da[6] = { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x00 };
	const unsigned char *p = pkt->pkt_data[0].data;
	int len = (int)pkt->pkt_data[0].len, off = 12, k, port, type;
	struct sport *sp;

	(void)cookie;
	if (len < 20 || memcmp(p, da, 6) != 0)
		return BCM_RX_NOT_HANDLED;
	if (p[12] == 0x81 && p[13] == 0x00)
		off = 16;                                /* the punt tag */
	/* 802.3 length, then LLC 42 42 03 and protocol id 0. Anything else to
	 * this address is not ours, and is dropped rather than bridged. */
	if (len < off + 2 + 3 + 4 || get16(p + off) >= 0x600 ||
	    p[off + 2] != 0x42 || p[off + 3] != 0x42 || p[off + 4] != 0x03 ||
	    get16(p + off + 5) != 0)
		return BCM_RX_HANDLED;
	p += off + 7;                                    /* at the version */
	len -= off + 7;
	type = p[1];
	if (!enabled)
		return BCM_RX_HANDLED;
	if ((type == 0x00 && len < 35) || (type == 0x02 && len < 36) ||
	    (type != 0x00 && type != 0x02 && type != 0x80))
		return BCM_RX_HANDLED;
	port = rx_port(unit, pkt);
	if ((k = key_of_port(port)) < 0)
		return BCM_RX_HANDLED;

	pthread_mutex_lock(&rstp_lock);
	sp = &ports[k];
	if (sp->enabled) {
		sp->rx++;
		sp->mtype = type;
		if (type != 0x80) {
			sp->mflags = p[2];
			memcpy(sp->msg + PV_ROOT, p + 3, 8);
			memcpy(sp->msg + PV_RPC, p + 11, 4);
			memcpy(sp->msg + PV_BRIDGE, p + 15, 8);
			memcpy(sp->msg + PV_PORT, p + 23, 2);
			sp->mt.msg_age = t_from(get16(p + 25));
			sp->mt.max_age = t_from(get16(p + 27));
			sp->mt.hello = t_from(get16(p + 29));
			sp->mt.fwd = t_from(get16(p + 31));
		}
		sp->pending = 1;
		pthread_cond_signal(&rstp_wake);
	}
	pthread_mutex_unlock(&rstp_lock);
	return BCM_RX_HANDLED;
}

/* ---------------------------------------------------------------------- */
/* The contract */

static void set_bridge_id(void)
{
	unsigned char mac[6] = { 0 };

	if (nosaic_tap_count() > 0)
		nosaic_tap_info(0, NULL, NULL, NULL, NULL, mac);
	put16(bridge_id, (unsigned)priority);
	memcpy(bridge_id + 2, mac, 6);
}

int nosaic_rstp_supported(void)
{
	return rstp_unit >= 0 && user_stg >= 0;
}

int nosaic_rstp_set(int on, int prio, char *err, size_t n)
{
	int k;

	if (!nosaic_rstp_supported())
		return -2;
	if (prio < 0 || prio > 61440 || prio % 4096 != 0) {
		if (err != NULL)
			snprintf(err, n, "stp priority %d: must be 0 to 61440 in steps of 4096", prio);
		return -1;
	}
	pthread_mutex_lock(&rstp_lock);
	if (prio != priority || on != enabled) {
		priority = prio;
		set_bridge_id();
		for (k = 0; k < NKEY; k++) {
			struct sport *sp = &ports[k];

			sp->enabled = 0;                 /* run() starts it afresh */
			sp->info = INFO_DISABLED;
			sp->role = R_DISABLED;
			sp->pending = 0;
			if (sp->used) {
				/* On: discarding until the tree says otherwise.
				 * Off: forwarding, as before spanning tree. */
				sp->state = on ? S_DISCARDING : S_FORWARDING;
				program(k, sp->state);
			}
		}
		memset(root_pv, 0, sizeof(root_pv));
		root_key = -1;
		enabled = on;
		reselect = 1;
		printf("stp: %s, bridge %02x%02x.%02x%02x%02x%02x%02x%02x\n",
		       on ? "on" : "off", bridge_id[0], bridge_id[1], bridge_id[2],
		       bridge_id[3], bridge_id[4], bridge_id[5], bridge_id[6], bridge_id[7]);
		fflush(stdout);
		pthread_cond_signal(&rstp_wake);
	}
	pthread_mutex_unlock(&rstp_lock);
	return 0;
}

int nosaic_rstp_port(const char *name, int edge, int cost, char *err, size_t n)
{
	int k;

	if (!nosaic_rstp_supported())
		return -2;
	if (cost < 0 || cost > 200000000) {
		if (err != NULL)
			snprintf(err, n, "stp cost %d: must be 1 to 200000000, or 0 for the speed's default", cost);
		return -1;
	}
	if ((k = key_by_name(name)) < 0) {
		if (err != NULL)
			snprintf(err, n, "no such port %s", name);
		return -1;
	}
	if (!is_lag(k) && nosaic_lag_of_port(key_port(k)) != 0) {
		const char *l = nosaic_lag_name(nosaic_lag_of_port(key_port(k)));

		if (err != NULL)
			snprintf(err, n, "%s is a member of %s; configure %s instead", name, l, l);
		return -1;
	}
	pthread_mutex_lock(&rstp_lock);
	ports[k].admin_edge = edge;
	ports[k].cfg_cost = cost;
	if (!edge)
		ports[k].oper_edge = 0;
	else if (!ports[k].heard)
		ports[k].oper_edge = 1;
	reselect = 1;
	pthread_cond_signal(&rstp_wake);
	pthread_mutex_unlock(&rstp_lock);
	return 0;
}

static void put_id(FILE *out, const unsigned char *id)
{
	fprintf(out, "\"%02x%02x.%02x%02x%02x%02x%02x%02x\"",
		id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
}

void nosaic_rstp_query(FILE *out)
{
	int k, first = 1;

	pthread_mutex_lock(&rstp_lock);
	if (bridge_id[2] == 0 && bridge_id[3] == 0)
		set_bridge_id();
	fprintf(out, "{\"ok\":true,\"result\":{\"Enabled\":%s,\"Priority\":%d,\"BridgeID\":",
		enabled ? "true" : "false", priority);
	put_id(out, bridge_id);
	fprintf(out, ",\"RootID\":");
	put_id(out, enabled && (root_pv[0] | root_pv[2]) ? root_pv + PV_ROOT : bridge_id);
	fprintf(out, ",\"RootCost\":%u,\"RootPort\":\"%s\",\"TopologyChanges\":%lu,\"Ports\":[",
		enabled ? (unsigned)get32(root_pv + PV_RPC) : 0u,
		enabled && root_key >= 0 ? key_name(root_key) : "", tc_count);
	for (k = 0; k < NKEY; k++) {
		const struct sport *sp = &ports[k];
		int on = enabled;

		if (!sp->used)
			continue;
		fprintf(out, "%s{\"Port\":\"%s\",\"Role\":\"%s\",\"State\":\"%s\","
			"\"Edge\":%s,\"Cost\":%d,\"RSTP\":%s,\"RxBPDU\":%lu,\"TxBPDU\":%lu}",
			first ? "" : ",", key_name(k),
			on ? role_name[sp->role] : "designated",
			on ? state_name[sp->state] : "forwarding",
			on && sp->oper_edge ? "true" : "false",
			sp->cfg_cost ? sp->cfg_cost : key_cost(k),
			sp->send_rstp ? "true" : "false", sp->rx, sp->tx);
		first = 0;
	}
	fprintf(out, "]}}\n");
	pthread_mutex_unlock(&rstp_lock);
}

/* ---------------------------------------------------------------------- */
/* For vlan.c */

void nosaic_rstp_iface(int k, int switched)
{
	if (k < 0 || k >= NKEY)
		return;
	pthread_mutex_lock(&rstp_lock);
	ports[k].used = switched;
	ports[k].id = key_id(k);
	if (switched) {
		/* Discarding until the tree has decided -- a port that joined a
		 * VLAN must not forward into a loop for even one tick. */
		ports[k].state = enabled ? S_DISCARDING : S_FORWARDING;
		ports[k].enabled = 0;
		ports[k].role = R_DISABLED;
		kstate[k] = ports[k].state;
	} else {
		ports[k].state = S_FORWARDING;
		kstate[k] = S_FORWARDING;
	}
	reselect = 1;
	pthread_cond_signal(&rstp_wake);
	pthread_mutex_unlock(&rstp_lock);
}

void nosaic_rstp_vlan(int vid)
{
	int rv;

	if (user_stg < 0)
		return;
	rv = bcm_stg_vlan_add(rstp_unit, user_stg, (bcm_vlan_t)vid);
	if (rv != BCM_E_NONE)
		fprintf(stderr, "stp: vlan %d into group %d: %s\n", vid, (int)user_stg,
			bcm_errmsg(rv));
}

int nosaic_rstp_port_state(int port)
{
	int k, s;

	if (!enabled || (k = key_of_port(port)) < 0)
		s = BCM_STG_STP_FORWARD;
	else
		s = bcm_state(kstate[k]);
	if (port >= 0 && port < MAX_PORT)
		pstate[port] = s;
	return s;
}

int nosaic_rstp_forwarding(int port)
{
	return !enabled || port < 0 || port >= MAX_PORT || pstate[port] == BCM_STG_STP_FORWARD;
}

/* ---------------------------------------------------------------------- */

int nosaic_rstp_start(int unit)
{
	bcm_port_config_t cfg;
	pthread_t th;
	bcm_port_t p;
	int i, rv;

	for (i = 0; i < MAX_PORT; i++)
		pstate[i] = BCM_STG_STP_FORWARD;
	for (i = 0; i < NKEY; i++) {
		kstate[i] = S_FORWARDING;
		ports[i].state = S_FORWARDING;
		ports[i].id = key_id(i);
	}
	rstp_unit = unit;
	set_bridge_id();

	/* The user VLANs' own group. The CPU forwards in it always: an SVI
	 * sends and receives through it. */
	rv = bcm_stg_create(unit, &user_stg);
	if (rv != BCM_E_NONE) {
		fprintf(stderr, "stp: bcm_stg_create: %s; no spanning tree\n", bcm_errmsg(rv));
		user_stg = -1;
		return -1;
	}
	if (bcm_port_config_get(unit, &cfg) == BCM_E_NONE) {
		BCM_PBMP_ITER(cfg.cpu, p)
			bcm_stg_stp_set(unit, user_stg, p, BCM_STG_STP_FORWARD);
		BCM_PBMP_ITER(cfg.port, p)
			bcm_stg_stp_set(unit, user_stg, p, BCM_STG_STP_FORWARD);
	}
	rv = bcm_rx_register(unit, "nosaic-stp", rstp_rx, RX_PRIORITY, NULL,
			     BCM_RCO_F_ALL_COS);
	if (rv != BCM_E_NONE) {
		fprintf(stderr, "stp: bcm_rx_register: %s; no spanning tree\n", bcm_errmsg(rv));
		user_stg = -1;
		return -1;
	}
	if (pthread_create(&th, NULL, rstp_thread, NULL) != 0) {
		fprintf(stderr, "stp: no thread: %s; no spanning tree\n", strerror(errno));
		user_stg = -1;
		return -1;
	}
	pthread_detach(th);
	printf("stp: user vlans in spanning-tree group %d; off until configured\n",
	       (int)user_stg);
	fflush(stdout);
	return 0;
}
