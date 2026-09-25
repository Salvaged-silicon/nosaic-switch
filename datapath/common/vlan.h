/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_VLAN_H
#define NOSAIC_VLAN_H

#include <stdio.h>
#include <stddef.h>

#include <bcm/types.h>
#include <bcm/l2.h>

/*
 * User VLANs: access and trunk membership on front-panel ports, and routed
 * VLAN interfaces (SVIs). switchapi 1.2, implemented in the chip.
 *
 * Every call that can fail returns 0 on success, -1 with a reason in err, or
 * -2 for "this datapath cannot", which the query server reports as
 * unsupported rather than as an error.
 */
int nosaic_vlan_start(int unit);

int nosaic_vlan_add(int vid, char *err, size_t errlen);
int nosaic_vlan_del(int vid, char *err, size_t errlen);
int nosaic_vlan_port_set(const char *port, int vid, int tagged,
			 char *err, size_t errlen);
int nosaic_vlan_port_del(const char *port, int vid, char *err, size_t errlen);
int nosaic_svi_add(int vid, char *err, size_t errlen);
int nosaic_svi_del(int vid, char *err, size_t errlen);

/* The whole table, as the `vlans` query answers it. */
void nosaic_vlan_query(FILE *out);

/*
 * For the tap bridge, which has to know where a frame belongs and where one
 * may go. Cheap and lock-safe from the receive and transmit paths.
 */

/* A VLAN a user created, as opposed to one the datapath uses for itself. */
int nosaic_vlan_is_user(int vid);

/* The port is a member of at least one user VLAN, so it is switched, not
 * routed, and its routed tap must not transmit. */
int nosaic_vlan_port_switched(int port);

/*
 * The local logical port an L2 table entry points at; for an entry learned on
 * a LAG, the trunk as a gport (BCM_GPORT_IS_TRUNK); -1 if it is not ours.
 *
 * ⚠ l2->port ALONE IS NOT THE PORT. A chip with more ports than one module
 * id covers splits them across two, and the L2 table reports (module, port):
 * on an AS5610 the neighbour behind swp51 -- logical port 51 -- comes back
 * as port 19 on the second module, 51 - 32. Built into an egress object as
 * "19", a routed packet left by a dark 10G port instead. The same split is
 * why src_port on the 7050SX2's 40G ports read 17, 19 and 20 for 49, 51 and
 * 52 (tapbridge.c).
 */
int nosaic_l2_port(int unit, const bcm_l2_addr_t *l2);

/* LAG hooks, for lag.c: a member joining or leaving a switched LAG takes on
 * or gives up the LAG's VLANs. Both return whether the LAG is switched. */
int  nosaic_vlan_lag_join(int key, int port);
int  nosaic_vlan_lag_leave(int key, int port);
void nosaic_vlan_lag_forget(int key);
int  nosaic_vlan_lag_switched(int key);       /* lock-free */

/* Members of a user VLAN and the untagged subset; 0 if it exists. */
int nosaic_vlan_members(int vid, bcm_pbmp_t *members, bcm_pbmp_t *untagged);

#endif
