/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_VXLAN_H
#define NOSAIC_VXLAN_H

#include <stddef.h>
#include <stdio.h>

/*
 * Hardware VXLAN / EVPN datapath (L2 VNIs + L3 VNIs).
 *
 * Initializes bcm_vxlan on silicon that supports it (Trident II, Trident II+),
 * tracks the local VTEP IP and VNI <-> VLAN/VRF mappings, and creates hardware
 * VXLAN VPNs and BUM multicast groups. Returns unsupported on older silicon
 * (e.g. Trident+).
 */
int  nosaic_vxlan_start(int unit);
int  nosaic_vxlan_supported(void);
int  nosaic_vxlan_set_vtep(const char *ip, char *err, size_t errlen);
int  nosaic_vxlan_add_vni(int vni, int vid, const char *vrf, int l3,
			  char *err, size_t errlen);
int  nosaic_vxlan_del_vni(int vni, char *err, size_t errlen);
void nosaic_vxlan_query(FILE *out);

#endif
