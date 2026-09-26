/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_LAG_H
#define NOSAIC_LAG_H

#include <stdio.h>
#include <stddef.h>

#include <bcm/types.h>

/*
 * Link aggregation: port-channels in the chip's trunk table, static or
 * negotiated by LACP (IEEE 802.1AX), with a Linux interface po<N>.
 * switchapi 1.3.
 *
 * A LAG is ROUTED until it joins a VLAN, exactly like a port: it has a
 * private service VLAN, NOSAIC_LAG_SVC_BASE + N, a tap po<N>, and a router
 * interface whose next hops point at the trunk rather than at a port.
 */
#define NOSAIC_MAX_LAGS      64
#define NOSAIC_MAX_LAGM      16      /* members; the Trident+ caps a trunk at 16 */
#define NOSAIC_LAG_SVC_BASE  4000    /* service VLANs 4001..4064 are reserved */

int nosaic_lag_start(int unit);

/* The contract calls. 0, -1 with a reason in err, or -2 for unsupported. */
int nosaic_lag_add(const char *name, int lacp, char *err, size_t errlen);
int nosaic_lag_members(const char *name, const char **ports, int n,
		       char *err, size_t errlen);
int nosaic_lag_del(const char *name, char *err, size_t errlen);
void nosaic_lag_query(FILE *out);
void nosaic_lag_capability(int *max_lags, int *max_members);
/* LACP options: rate "fast"/"slow" ("" is fast), passive, port priority (0 for
 * 32768); and the switch's system priority (0 for 32768). */
int  nosaic_lag_options(const char *name, const char *rate, int passive, int port_prio,
			char *err, size_t errlen);
int  nosaic_lag_sys_prio(int prio, char *err, size_t errlen);

/*
 * For vlan.c, which treats a LAG as a set of ports that join and leave VLANs
 * together. key identifies the LAG (1..NOSAIC_MAX_LAGS); ports are its
 * configured members, whether or not they are distributing. Returns 0 if name
 * is a LAG.
 */
int nosaic_lag_iface(const char *name, int *key, bcm_pbmp_t *ports, int *svc);
/* The LAG a port is a member of, 0 if none; and its name. */
int nosaic_lag_of_port(int port);
const char *nosaic_lag_name(int key);

/*
 * For tapbridge and l3sync. Lock-free: they run on the packet paths.
 */
/* A member port's own routed tap must not transmit. */
int nosaic_lag_port_claimed(int port);
/* One distributing member of LAG key to send a frame to, chosen by hash;
 * -1 if none is distributing. */
int nosaic_lag_pick(int key, unsigned hash);
/* Keep one member per LAG in a flood bitmap, so a partner gets one copy. */
void nosaic_lag_flood_mask(bcm_pbmp_t *pbm, unsigned hash);
/* The LAG a trunk id belongs to, 0 if none; and a LAG's trunk id, -1 if none. */
int nosaic_lag_of_tid(int tid);
int nosaic_lag_tid(int key);
/* How many of a LAG's members are distributing. */
int nosaic_lag_active(int key);

#endif
