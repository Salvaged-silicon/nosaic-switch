/*
 * User VLANs and routed VLAN interfaces, in the chip. switchapi 1.2.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Every front-panel port starts as a ROUTED port: a tap, a private service
 * VLAN the port is the only member of, and a router interface on that VLAN
 * (tapbridge.c, l3sync.c). That stays the default. A port becomes a SWITCHED
 * port the moment it joins a user VLAN, and goes back to routed when it leaves
 * the last one. What changes between the two:
 *
 *   routed                            switched
 *   member of its service VLAN        member of user VLANs only
 *   PVID = the service VLAN           PVID = its native user VLAN, if any
 *   its tap sends and receives        its tap is silent; an SVI speaks for
 *                                     the VLAN instead
 *
 * The chip does the switching. Learning, flooding and forwarding between
 * members of a VLAN happen in silicon, and nothing here touches a frame on
 * that path. What comes to the CPU is only what the chip sends to its own
 * port, and the CPU is a member of a user VLAN only while that VLAN has an
 * SVI -- so a VLAN nobody routes for punts nothing.
 *
 * An SVI is a tap of its own, vlan<VID>, with its own MAC and a router
 * interface on the user VLAN. Its next hops are not tied to a port: the port
 * is whatever the chip's L2 table says the neighbour's MAC is behind, and
 * l3sync looks it up (and looks again when it moves).
 *
 * ⚠ LOCK ORDER. Writers here hold vlan_lock and call into tapbridge (which
 * takes its own lock) and l3sync (likewise). The receive and transmit paths
 * call the three read functions at the bottom from inside tapbridge's lock,
 * so those must never take vlan_lock -- the two orders would deadlock the
 * first time a frame arrived while an SVI was being made. They read state
 * that writers publish with plain stores; the worst a torn read can do is
 * flood one frame to a slightly stale member set while the set is changing.
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <bcm/error.h>
#include <bcm/l2.h>
#include <bcm/port.h>
#include <bcm/stg.h>
#include <bcm/vlan.h>

#include "l3sync.h"
#include "lag.h"
#include "rstp.h"
#include "tapbridge.h"
#include "vlan.h"

#define MAX_VID    4096
/* An SVI's router interface MTU in the chip. The kernel enforces the
 * interface MTU on what it sends; this only has to be large enough that the
 * chip does not drop what it routes out of the VLAN, and the ports' own frame
 * size is the real limit. */
#define SVI_L3_MTU 9216

static pthread_mutex_t vlan_lock = PTHREAD_MUTEX_INITIALIZER;
static int vlan_unit = -1;
static bcm_pbmp_t cpu_pbm;

/* Published state, read without the lock -- see the header comment. */
static volatile unsigned char user_vid[MAX_VID];  /* a user VLAN exists */
static volatile unsigned char svi_vid[MAX_VID];   /* and has an SVI */
static bcm_pbmp_t members[MAX_VID];
static bcm_pbmp_t untagged[MAX_VID];

/*
 * Per interface: the front-panel ports, indexed like the taps, then the LAGs
 * at NOSAIC_MAX_TAPS + their number. A LAG is a set of ports that join and
 * leave VLANs together; its own memberships are kept in lagvl, because a LAG
 * can be in a VLAN with no members yet, and a member added later has to find
 * out which VLANs it is joining.
 */
#define NIFACE (NOSAIC_MAX_TAPS + NOSAIC_MAX_LAGS + 1)
static struct {
	int      switched;      /* member of how many user VLANs */
	uint32   member_flags;  /* bcm_port_vlan_member_get, before we changed it */
	int      saved;
} pst[NIFACE];
static unsigned char lagvl[NOSAIC_MAX_LAGS + 1][MAX_VID]; /* 1 untagged, 2 tagged */

/* A port, or a LAG: what joins a VLAN. */
struct iface {
	int         key;        /* index into pst */
	int         lag;        /* LAG number, 0 for a port */
	bcm_pbmp_t  pbm;        /* its ports: one, or the LAG's members */
	int         svc;        /* its service VLAN, where it lives while routed */
	const char *name;
};

static void say(char *err, size_t n, const char *fmt, int a, const char *s, int rv)
{
	if (err != NULL && n > 0)
		snprintf(err, n, fmt, a, s != NULL ? s : "", rv < 0 ? bcm_errmsg(rv) : "");
}

/* A port or a LAG by the name the contract uses. */
static int iface_by_name(const char *name, struct iface *f)
{
	int i;

	memset(f, 0, sizeof(*f));
	for (i = 0; i < nosaic_tap_count(); i++) {
		const char *n = NULL;
		unsigned char mac[6];
		int p = 0, v = 0, mtu = 0;

		if (nosaic_tap_info(i, &n, &p, &v, &mtu, mac) != 0 || n == NULL)
			continue;
		if (strcmp(n, name) == 0) {
			f->key = i;
			BCM_PBMP_PORT_ADD(f->pbm, p);
			f->svc = v;
			f->name = n;
			return 0;
		}
	}
	if (nosaic_lag_iface(name, &f->lag, &f->pbm, &f->svc) == 0) {
		f->key = NOSAIC_MAX_TAPS + f->lag;
		f->name = nosaic_lag_name(f->lag);
		return 0;
	}
	return -1;
}

static int tap_by_port(int port)
{
	int i;

	for (i = 0; i < nosaic_tap_count(); i++) {
		int p = -1;

		if (nosaic_tap_info(i, NULL, &p, NULL, NULL, NULL) == 0 && p == port)
			return i;
	}
	return -1;
}

/* Is this interface in the VLAN, and untagged there? */
static int in_vlan(const struct iface *f, int vid)
{
	bcm_port_t p;

	if (f->lag)
		return lagvl[f->lag][vid] != 0;
	BCM_PBMP_ITER(f->pbm, p)
		return BCM_PBMP_MEMBER(members[vid], p);
	return 0;
}

static int untagged_in(const struct iface *f, int vid)
{
	bcm_port_t p;

	if (f->lag)
		return lagvl[f->lag][vid] == 1;
	BCM_PBMP_ITER(f->pbm, p)
		return BCM_PBMP_MEMBER(untagged[vid], p);
	return 0;
}

/* A VID the datapath already uses for a routed port or a routed LAG. */
static int reserved_vid(int vid)
{
	int i;

	if (vid > NOSAIC_LAG_SVC_BASE && vid <= NOSAIC_LAG_SVC_BASE + NOSAIC_MAX_LAGS)
		return 1;

	for (i = 0; i < nosaic_tap_count(); i++) {
		int v = 0;

		if (nosaic_tap_info(i, NULL, NULL, &v, NULL, NULL) == 0 && v == vid)
			return 1;
	}
	return 0;
}

int nosaic_vlan_start(int unit)
{
	bcm_port_config_t cfg;

	vlan_unit = unit;
	BCM_PBMP_CLEAR(cpu_pbm);
	if (bcm_port_config_get(unit, &cfg) == BCM_E_NONE)
		BCM_PBMP_ASSIGN(cpu_pbm, cfg.cpu);
	return 0;
}

/*
 * The ports' state in the VLAN's own spanning-tree group.
 *
 * The state that decides whether a port forwards in a VLAN is the one in that
 * VLAN's group, not the port's default one -- tapbridge.c has the story of a
 * port that was FORWARD in the default group and BLOCKING where it counted.
 *
 * A service VLAN forwards, always: a routed port is never in the spanning
 * tree. A user VLAN is in the spanning tree's group (rstp.c), and a port
 * there has whatever state its interface has -- FORWARD while spanning tree
 * is off, otherwise what the tree decided, which for a port that has only
 * just joined is discarding.
 */
static void stg_forward(int vid, bcm_pbmp_t pbm)
{
	bcm_stg_t stg;
	bcm_port_t p;

	if (bcm_vlan_stg_get(vlan_unit, (bcm_vlan_t)vid, &stg) != BCM_E_NONE)
		return;
	BCM_PBMP_ITER(pbm, p)
		bcm_stg_stp_set(vlan_unit, stg, p, user_vid[vid] ?
				nosaic_rstp_port_state(p) : BCM_STG_STP_FORWARD);
}

int nosaic_vlan_add(int vid, char *err, size_t n)
{
	int rv;

	if (vlan_unit < 0)
		return -2;
	if (vid < 1 || vid >= MAX_VID - 1) {
		say(err, n, "vlan %d out of range 1-4094%s%s", vid, NULL, 0);
		return -1;
	}
	if (reserved_vid(vid)) {
		say(err, n, "vlan %d is reserved: this datapath uses it for a "
		    "routed port or LAG%s%s", vid, NULL, 0);
		return -1;
	}
	pthread_mutex_lock(&vlan_lock);
	if (user_vid[vid]) {
		pthread_mutex_unlock(&vlan_lock);
		return 0;
	}
	rv = bcm_vlan_create(vlan_unit, (bcm_vlan_t)vid);
	/*
	 * EXISTS on anything but VLAN 1 means the datapath made it -- tdp and
	 * helix4 build a service VLAN for every port on the chip, tapped or
	 * not -- and taking it over would put a user's ports in one.
	 */
	if (rv == BCM_E_EXISTS && vid != 1) {
		pthread_mutex_unlock(&vlan_lock);
		say(err, n, "vlan %d is reserved: it already exists in the chip, "
		    "made by the datapath%s%s", vid, NULL, 0);
		return -1;
	}
	if (rv != BCM_E_NONE && rv != BCM_E_EXISTS) {
		pthread_mutex_unlock(&vlan_lock);
		say(err, n, "vlan %d: bcm_vlan_create: %s%s", vid, NULL, rv);
		return -1;
	}
	BCM_PBMP_CLEAR(members[vid]);
	BCM_PBMP_CLEAR(untagged[vid]);
	user_vid[vid] = 1;
	nosaic_rstp_vlan(vid);          /* into the spanning tree's group */
	pthread_mutex_unlock(&vlan_lock);
	printf("vlan: %d created\n", vid);
	fflush(stdout);
	return 0;
}

/* Per port: switch on, the way a switched port or a LAG member needs it. */
static void port_switching(bcm_port_t port)
{
	bcm_port_vlan_member_set(vlan_unit, port,
				 BCM_PORT_VLAN_MEMBER_INGRESS |
				 BCM_PORT_VLAN_MEMBER_EGRESS);
	bcm_port_learn_set(vlan_unit, port, BCM_PORT_LEARN_ARL | BCM_PORT_LEARN_FWD);
	bcm_l2_addr_delete_by_port(vlan_unit, -1, port, 0);
}

/* Take an interface out of one VLAN, and back to routed if that was its last. */
static int leave(const struct iface *f, int vid, char *err, size_t n)
{
	bcm_port_t port;
	int rv;

	rv = bcm_vlan_port_remove(vlan_unit, (bcm_vlan_t)vid, f->pbm);
	if (rv != BCM_E_NONE && rv != BCM_E_NOT_FOUND) {
		say(err, n, "vlan %d: bcm_vlan_port_remove: %s%s", vid, NULL, rv);
		return -1;
	}
	BCM_PBMP_REMOVE(members[vid], f->pbm);
	BCM_PBMP_REMOVE(untagged[vid], f->pbm);
	if (f->lag)
		lagvl[f->lag][vid] = 0;
	if (pst[f->key].switched > 0)
		pst[f->key].switched--;
	if (pst[f->key].switched > 0)
		return 0;
	nosaic_rstp_iface(f->key, 0);   /* out of the spanning tree */

	/*
	 * The last one: routed again. Back into its service VLAN -- the port's
	 * own, or the LAG's -- as the untagged member, that VLAN as its PVID,
	 * its learned MACs forgotten (they were learned in VLANs it is no longer
	 * in, and a stale entry would send a neighbour's traffic to a port that
	 * no longer switches), and a port's ingress filtering as it was. A LAG's
	 * members keep the filtering lag.c gave them.
	 */
	if (f->svc > 0) {
		rv = bcm_vlan_port_add(vlan_unit, (bcm_vlan_t)f->svc, f->pbm, f->pbm);
		if (rv != BCM_E_NONE)
			fprintf(stderr, "vlan: %s back into service vlan %d: %s\n",
				f->name, f->svc, bcm_errmsg(rv));
		BCM_PBMP_ITER(f->pbm, port)
			bcm_port_untagged_vlan_set(vlan_unit, port, (bcm_vlan_t)f->svc);
		stg_forward(f->svc, f->pbm);
	}
	BCM_PBMP_ITER(f->pbm, port) {
		bcm_l2_addr_delete_by_port(vlan_unit, -1, port, 0);
		if (!f->lag && pst[f->key].saved)
			bcm_port_vlan_member_set(vlan_unit, port, pst[f->key].member_flags);
		if (f->lag)     /* a routed LAG's members learn nothing (lag.c) */
			bcm_port_learn_set(vlan_unit, port, BCM_PORT_LEARN_FWD);
	}
	printf("vlan: %s is routed again (service vlan %d)\n", f->name, f->svc);
	fflush(stdout);
	return 0;
}

int nosaic_vlan_port_set(const char *name, int vid, int tagged, char *err, size_t n)
{
	struct iface f;
	bcm_pbmp_t ubm;
	bcm_port_t port;
	int rv, v, was, lag;

	if (vlan_unit < 0)
		return -2;
	if (iface_by_name(name, &f) != 0) {
		if (err != NULL)
			snprintf(err, n, "no such port %s", name);
		return -1;
	}
	BCM_PBMP_ITER(f.pbm, port)
		break;
	if (!f.lag && (lag = nosaic_lag_of_port(port)) != 0) {
		if (err != NULL)
			snprintf(err, n, "%s is a member of %s; put %s in the VLAN instead",
				 name, nosaic_lag_name(lag), nosaic_lag_name(lag));
		return -1;
	}
	pthread_mutex_lock(&vlan_lock);
	if (vid < 1 || vid >= MAX_VID || !user_vid[vid]) {
		pthread_mutex_unlock(&vlan_lock);
		say(err, n, "vlan %d does not exist%s%s", vid, NULL, 0);
		return -1;
	}
	was = in_vlan(&f, vid);

	/* One native VLAN per interface: an untagged membership replaces the last. */
	if (!tagged) {
		for (v = 1; v < MAX_VID - 1; v++) {
			if (v != vid && user_vid[v] && in_vlan(&f, v) && untagged_in(&f, v)) {
				if (leave(&f, v, err, n) != 0) {
					pthread_mutex_unlock(&vlan_lock);
					return -1;
				}
			}
		}
	}
	/* Changing a membership's tagging: the SDK adds to the untagged set
	 * and never clears it, so go out and come back in. */
	if (was && untagged_in(&f, vid) == !tagged) {
		pthread_mutex_unlock(&vlan_lock);
		return 0;                       /* already exactly this */
	}
	if (was) {
		if (leave(&f, vid, err, n) != 0) {
			pthread_mutex_unlock(&vlan_lock);
			return -1;
		}
	}

	if (pst[f.key].switched == 0) {
		/* Into the spanning tree first, so the port's state in the VLAN
		 * below is the tree's -- discarding, if it is on -- and never a
		 * moment of FORWARD into a loop. */
		nosaic_rstp_iface(f.key, 1);
		/*
		 * Becoming switched. Out of its service VLAN, so its router
		 * interface hears nothing; ingress filtering on, so a frame in a
		 * VLAN it is not a member of is dropped at the port instead of
		 * being taken into the PVID -- on a trunk with no native VLAN
		 * that PVID would still be the service VLAN, and untagged
		 * frames would reach the routed tap; and learning on, because
		 * switching is what it is for now.
		 */
		if (!f.lag) {
			uint32 flags = 0;

			BCM_PBMP_ITER(f.pbm, port) {
				if (bcm_port_vlan_member_get(vlan_unit, port, &flags) == BCM_E_NONE) {
					pst[f.key].member_flags = flags;
					pst[f.key].saved = 1;
				}
			}
		}
		if (f.svc > 0)
			bcm_vlan_port_remove(vlan_unit, (bcm_vlan_t)f.svc, f.pbm);
		BCM_PBMP_ITER(f.pbm, port)
			port_switching(port);
	}

	BCM_PBMP_CLEAR(ubm);
	if (!tagged)
		BCM_PBMP_ASSIGN(ubm, f.pbm);
	rv = bcm_vlan_port_add(vlan_unit, (bcm_vlan_t)vid, f.pbm, ubm);
	if (rv != BCM_E_NONE) {
		pthread_mutex_unlock(&vlan_lock);
		say(err, n, "vlan %d: bcm_vlan_port_add: %s%s", vid, NULL, rv);
		return -1;
	}
	if (!tagged) {
		BCM_PBMP_ITER(f.pbm, port) {
			rv = bcm_port_untagged_vlan_set(vlan_unit, port, (bcm_vlan_t)vid);
			if (rv != BCM_E_NONE)
				fprintf(stderr, "vlan: port %d pvid %d: %s\n",
					port, vid, bcm_errmsg(rv));
		}
	}
	stg_forward(vid, f.pbm);
	BCM_PBMP_OR(members[vid], f.pbm);
	if (!tagged)
		BCM_PBMP_OR(untagged[vid], f.pbm);
	if (f.lag)
		lagvl[f.lag][vid] = tagged ? 2 : 1;
	pst[f.key].switched++;
	pthread_mutex_unlock(&vlan_lock);
	printf("vlan: %s in vlan %d, %s\n", name, vid,
	       tagged ? "tagged" : "untagged (native)");
	fflush(stdout);
	return 0;
}

int nosaic_vlan_port_del(const char *name, int vid, char *err, size_t n)
{
	struct iface f;
	int rv = 0;

	if (vlan_unit < 0)
		return -2;
	if (iface_by_name(name, &f) != 0) {
		if (err != NULL)
			snprintf(err, n, "no such port %s", name);
		return -1;
	}
	pthread_mutex_lock(&vlan_lock);
	if (vid >= 1 && vid < MAX_VID && user_vid[vid] && in_vlan(&f, vid))
		rv = leave(&f, vid, err, n);
	pthread_mutex_unlock(&vlan_lock);
	return rv;
}

int nosaic_vlan_del(int vid, char *err, size_t n)
{
	bcm_port_t port;
	bcm_pbmp_t m;
	int rv, k;

	if (vlan_unit < 0)
		return -2;
	pthread_mutex_lock(&vlan_lock);
	if (vid < 1 || vid >= MAX_VID || !user_vid[vid]) {
		pthread_mutex_unlock(&vlan_lock);
		return 0;
	}
	if (svi_vid[vid]) {
		pthread_mutex_unlock(&vlan_lock);
		if (err != NULL)
			snprintf(err, n, "vlan %d has a routed interface; remove "
				 "vlan%d first", vid, vid);
		return -1;
	}
	/* The LAGs first, then the ports that are left: a LAG member's bit in
	 * members[] belongs to its LAG, not to itself. */
	for (k = 1; k <= NOSAIC_MAX_LAGS; k++) {
		struct iface f;

		if (!lagvl[k][vid] || iface_by_name(nosaic_lag_name(k), &f) != 0)
			continue;
		if (leave(&f, vid, err, n) != 0) {
			pthread_mutex_unlock(&vlan_lock);
			return -1;
		}
	}
	BCM_PBMP_ASSIGN(m, members[vid]);
	BCM_PBMP_ITER(m, port) {
		struct iface f;
		const char *name = NULL;
		int tap = tap_by_port(port);

		if (tap < 0 || nosaic_tap_info(tap, &name, NULL, NULL, NULL, NULL) != 0 ||
		    iface_by_name(name, &f) != 0)
			continue;
		if (leave(&f, vid, err, n) != 0) {
			pthread_mutex_unlock(&vlan_lock);
			return -1;
		}
	}
	user_vid[vid] = 0;
	if (vid != 1) {
		rv = bcm_vlan_destroy(vlan_unit, (bcm_vlan_t)vid);
		if (rv != BCM_E_NONE && rv != BCM_E_NOT_FOUND)
			fprintf(stderr, "vlan: bcm_vlan_destroy %d: %s\n",
				vid, bcm_errmsg(rv));
	}
	pthread_mutex_unlock(&vlan_lock);
	printf("vlan: %d removed\n", vid);
	fflush(stdout);
	return 0;
}

/*
 * A LAG member arriving or leaving while the LAG is switched: it takes on, or
 * gives up, every VLAN the LAG is in. Called by lag.c, which handles the
 * routed case itself. Returns whether the LAG is switched.
 */
int nosaic_vlan_lag_join(int key, int port)
{
	bcm_pbmp_t pbm, none;
	int vid, sw;

	pthread_mutex_lock(&vlan_lock);
	sw = pst[NOSAIC_MAX_TAPS + key].switched > 0;
	if (sw) {
		BCM_PBMP_CLEAR(pbm);
		BCM_PBMP_PORT_ADD(pbm, port);
		BCM_PBMP_CLEAR(none);
		port_switching(port);
		for (vid = 1; vid < MAX_VID - 1; vid++) {
			if (!lagvl[key][vid])
				continue;
			bcm_vlan_port_add(vlan_unit, (bcm_vlan_t)vid, pbm,
					  lagvl[key][vid] == 1 ? pbm : none);
			if (lagvl[key][vid] == 1) {
				bcm_port_untagged_vlan_set(vlan_unit, port, (bcm_vlan_t)vid);
				BCM_PBMP_PORT_ADD(untagged[vid], port);
			}
			stg_forward(vid, pbm);
			BCM_PBMP_PORT_ADD(members[vid], port);
		}
	}
	pthread_mutex_unlock(&vlan_lock);
	return sw;
}

int nosaic_vlan_lag_leave(int key, int port)
{
	bcm_pbmp_t pbm;
	int vid, sw;

	pthread_mutex_lock(&vlan_lock);
	sw = pst[NOSAIC_MAX_TAPS + key].switched > 0;
	if (sw) {
		BCM_PBMP_CLEAR(pbm);
		BCM_PBMP_PORT_ADD(pbm, port);
		for (vid = 1; vid < MAX_VID - 1; vid++) {
			if (!lagvl[key][vid])
				continue;
			bcm_vlan_port_remove(vlan_unit, (bcm_vlan_t)vid, pbm);
			BCM_PBMP_PORT_REMOVE(members[vid], port);
			BCM_PBMP_PORT_REMOVE(untagged[vid], port);
		}
		bcm_l2_addr_delete_by_port(vlan_unit, -1, port, 0);
	}
	pthread_mutex_unlock(&vlan_lock);
	return sw;
}

/* A LAG going away: out of every VLAN it is in. */
void nosaic_vlan_lag_forget(int key)
{
	struct iface f;
	int vid;

	pthread_mutex_lock(&vlan_lock);
	if (iface_by_name(nosaic_lag_name(key), &f) == 0)
		for (vid = 1; vid < MAX_VID - 1; vid++)
			if (lagvl[key][vid])
				leave(&f, vid, NULL, 0);
	memset(lagvl[key], 0, sizeof(lagvl[key]));
	pst[NOSAIC_MAX_TAPS + key].switched = 0;
	pthread_mutex_unlock(&vlan_lock);
}

int nosaic_vlan_lag_switched(int key)
{
	return key > 0 && key <= NOSAIC_MAX_LAGS &&
	       pst[NOSAIC_MAX_TAPS + key].switched > 0;
}

int nosaic_svi_add(int vid, char *err, size_t n)
{
	bcm_pbmp_t none;
	unsigned char mac[6];
	char name[16];
	int rv;

	if (vlan_unit < 0)
		return -2;
	pthread_mutex_lock(&vlan_lock);
	if (vid < 1 || vid >= MAX_VID || !user_vid[vid]) {
		pthread_mutex_unlock(&vlan_lock);
		say(err, n, "vlan %d does not exist%s%s", vid, NULL, 0);
		return -1;
	}
	if (svi_vid[vid]) {
		pthread_mutex_unlock(&vlan_lock);
		return 0;
	}
	snprintf(name, sizeof(name), "vlan%d", vid);
	if (nosaic_tap_svi_add(vid, name, mac) != 0) {
		pthread_mutex_unlock(&vlan_lock);
		if (err != NULL)
			snprintf(err, n, "%s: could not make its tap", name);
		return -1;
	}
	/*
	 * The CPU joins the VLAN, tagged: broadcasts and frames for the SVI's
	 * MAC have to be able to reach it, and a frame it sends into the VLAN
	 * has to be one the VLAN accepts.
	 */
	BCM_PBMP_CLEAR(none);
	rv = bcm_vlan_port_add(vlan_unit, (bcm_vlan_t)vid, cpu_pbm, none);
	if (rv != BCM_E_NONE)
		fprintf(stderr, "vlan: cpu into vlan %d: %s\n", vid, bcm_errmsg(rv));
	stg_forward(vid, cpu_pbm);
	if (nosaic_l3_add_intf(vlan_unit, name, -1, vid, mac, SVI_L3_MTU) != 0)
		fprintf(stderr, "vlan: %s has no router interface; it will answer "
			"but the chip will not route through it\n", name);
	svi_vid[vid] = 1;
	pthread_mutex_unlock(&vlan_lock);
	printf("vlan: %s routes vlan %d, mac %02x:%02x:%02x:%02x:%02x:%02x\n",
	       name, vid, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	fflush(stdout);
	return 0;
}

int nosaic_svi_del(int vid, char *err, size_t n)
{
	char name[16];

	(void)err;
	(void)n;
	if (vlan_unit < 0)
		return -2;
	pthread_mutex_lock(&vlan_lock);
	if (vid < 1 || vid >= MAX_VID || !svi_vid[vid]) {
		pthread_mutex_unlock(&vlan_lock);
		return 0;
	}
	snprintf(name, sizeof(name), "vlan%d", vid);
	svi_vid[vid] = 0;
	nosaic_l3_del_intf(name);
	nosaic_tap_svi_del(vid);
	bcm_vlan_port_remove(vlan_unit, (bcm_vlan_t)vid, cpu_pbm);
	pthread_mutex_unlock(&vlan_lock);
	printf("vlan: %s removed\n", name);
	fflush(stdout);
	return 0;
}

void nosaic_vlan_query(FILE *out)
{
	int vid, first = 1;

	pthread_mutex_lock(&vlan_lock);
	fprintf(out, "{\"ok\":true,\"result\":[");
	for (vid = 1; vid < MAX_VID - 1; vid++) {
		int i, firstm = 1;

		if (!user_vid[vid])
			continue;
		fprintf(out, "%s{\"VID\":%d,\"SVI\":%s,\"Members\":[",
			first ? "" : ",", vid, svi_vid[vid] ? "true" : "false");
		first = 0;
		for (i = 0; i < nosaic_tap_count(); i++) {
			const char *name = NULL;
			int port = -1;

			if (nosaic_tap_info(i, &name, &port, NULL, NULL, NULL) != 0 ||
			    name == NULL || !BCM_PBMP_MEMBER(members[vid], port) ||
			    nosaic_lag_of_port(port) != 0)
				continue;       /* a LAG member is listed as its LAG */
			fprintf(out, "%s{\"Port\":\"%s\",\"Tagged\":%s}",
				firstm ? "" : ",", name,
				BCM_PBMP_MEMBER(untagged[vid], port) ? "false" : "true");
			firstm = 0;
		}
		for (i = 1; i <= NOSAIC_MAX_LAGS; i++) {
			if (!lagvl[i][vid])
				continue;
			fprintf(out, "%s{\"Port\":\"%s\",\"Tagged\":%s}",
				firstm ? "" : ",", nosaic_lag_name(i),
				lagvl[i][vid] == 2 ? "true" : "false");
			firstm = 0;
		}
		fprintf(out, "]}");
	}
	fprintf(out, "]}\n");
	pthread_mutex_unlock(&vlan_lock);
}

int nosaic_l2_port(int unit, const bcm_l2_addr_t *l2)
{
	bcm_gport_t gp;
	bcm_port_t local;

	if (l2->flags & BCM_L2_TRUNK_MEMBER) {
		/* Behind a LAG: the trunk itself, as a gport. An egress object
		 * takes it as its port and the chip hashes across the members;
		 * a transmit from the CPU picks one (nosaic_lag_pick). */
		BCM_GPORT_TRUNK_SET(gp, l2->tgid);
		return gp;
	}
	BCM_GPORT_MODPORT_SET(gp, l2->modid, l2->port);
	if (bcm_port_local_get(unit, gp, &local) != BCM_E_NONE)
		return -1;
	return local;
}

/* ---- lock-free readers for the packet paths; see the header comment ---- */

int nosaic_vlan_is_user(int vid)
{
	return vid > 0 && vid < MAX_VID && user_vid[vid];
}

int nosaic_vlan_port_switched(int port)
{
	int tap = tap_by_port(port);

	return tap >= 0 && pst[tap].switched > 0;
}

int nosaic_vlan_members(int vid, bcm_pbmp_t *m, bcm_pbmp_t *u)
{
	if (!nosaic_vlan_is_user(vid))
		return -1;
	BCM_PBMP_ASSIGN(*m, members[vid]);
	BCM_PBMP_ASSIGN(*u, untagged[vid]);
	return 0;
}
