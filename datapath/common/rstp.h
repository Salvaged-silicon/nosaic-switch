/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_RSTP_H
#define NOSAIC_RSTP_H

#include <stdio.h>
#include <stddef.h>

/*
 * Rapid spanning tree (IEEE 802.1D-2004 clause 17, originally 802.1w), one
 * instance for every user VLAN. switchapi 1.4.
 *
 * It runs on switched interfaces only -- ports and LAGs that are members of a
 * VLAN -- keyed exactly as vlan.c keys them: a port by its tap index, a LAG
 * by NOSAIC_MAX_TAPS + its number. Every user VLAN sits in one spanning-tree
 * group of the chip's, and an interface's state is written there for each of
 * its ports.
 */
int  nosaic_rstp_start(int unit);

/* The contract calls. 0, -1 with a reason in err, or -2 for unsupported. */
int  nosaic_rstp_set(int enabled, int priority, int hello, int fwd_delay, int max_age,
		     char *err, size_t errlen);
int  nosaic_rstp_port(const char *name, int edge, int cost, int priority,
		      char *err, size_t errlen);
void nosaic_rstp_query(FILE *out);
int  nosaic_rstp_supported(void);

/*
 * For vlan.c. An interface became switched (1) or routed again (0); a user
 * VLAN was created and has to join the spanning tree's group.
 */
void nosaic_rstp_iface(int key, int switched);
void nosaic_rstp_vlan(int vid);

/*
 * The state a port must have in the user VLANs' group: BCM_STG_STP_FORWARD
 * when spanning tree is off, otherwise its interface's state. Lock-free.
 */
int  nosaic_rstp_port_state(int port);
/* Whether the CPU may send a switched frame out of this port. Lock-free. */
int  nosaic_rstp_forwarding(int port);

#endif
