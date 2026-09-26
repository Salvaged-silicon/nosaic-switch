/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_MLAG_H
#define NOSAIC_MLAG_H

#include <stdio.h>
#include <stddef.h>

/*
 * MLAG: two switches joined by a peer-link present each LAG with the same
 * MLAG id as one LACP partner, so a device dual-homed to both bundles its
 * links to the pair as one LAG. switchapi 1.5. See mlag.c.
 */
#define NOSAIC_MLAG_MAX_ID 1000

int  nosaic_mlag_start(int unit);

/* The contract calls. 0, -1 with a reason in err, or -2 for unsupported. */
int  nosaic_mlag_set(int enabled, const char *peer_link, const char *peer_addr,
		     int priority, int hello_ms, int dead_ms, int settle_ms, int hb_port,
		     char *err, size_t errlen);
int  nosaic_mlag_set_lag(const char *name, int id, char *err, size_t errlen);
void nosaic_mlag_query(FILE *out);
int  nosaic_mlag_supported(void);

/*
 * For lag.c. A LAG's MLAG id, 0 for none; the LACP system, key and port
 * offset an MLAG interface presents, or 0 if the LAG is not one; and a LAG
 * going away.
 */
int  nosaic_mlag_id(int lag);
int  nosaic_mlag_lacp(int lag, unsigned char sys[6], unsigned *key, unsigned *port_offset);
void nosaic_mlag_lag_gone(int lag);

/* For rstp.c: an interface -- tap index, or NOSAIC_MAX_TAPS + LAG -- that
 * spanning tree must leave alone, forwarding: an MLAG interface or the
 * peer-link. Lock-free. */
int  nosaic_mlag_stp_excluded(int key);

#endif
