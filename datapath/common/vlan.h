/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_VLAN_H
#define NOSAIC_VLAN_H

#include <stdio.h>
#include <stddef.h>

#include <bcm/types.h>

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

/* Members of a user VLAN and the untagged subset; 0 if it exists. */
int nosaic_vlan_members(int vid, bcm_pbmp_t *members, bcm_pbmp_t *untagged);

#endif
