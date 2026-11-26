/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_VXLAN_H
#define NOSAIC_VXLAN_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <bcm/types.h>

/*
 * Hardware VXLAN / EVPN datapath (L2 VNIs + L3 VNIs).
 *
 * Initializes bcm_vxlan on silicon that supports it (Trident II, Trident II+),
 * tracks the local VTEP IP and VNI <-> VLAN/VRF mappings, binds local access
 * ports and remote VTEP tunnels, syncs FRR EVPN Type-2/Type-3 FDBs via netlink,
 * and exposes VRF table + L3 VNI egress resolution to l3sync.c for EVPN Type-5.
 */
int  nosaic_vxlan_start(int unit);
int  nosaic_vxlan_supported(void);
int  nosaic_vxlan_set_vtep(const char *ip, char *err, size_t errlen);
int  nosaic_vxlan_add_vni(int vni, int vid, const char *vrf, int l3,
			  char *err, size_t errlen);
int  nosaic_vxlan_del_vni(int vni, char *err, size_t errlen);
void nosaic_vxlan_sync_vlan(int vid);
int  nosaic_vxlan_vrf_for_table(uint32_t table_id, bcm_vrf_t *vrf_out);
int  nosaic_vxlan_l3_egress(bcm_vrf_t vrf, uint32_t gw_hbo, bcm_if_t *egr_out);
void nosaic_vxlan_query(FILE *out);

#endif
