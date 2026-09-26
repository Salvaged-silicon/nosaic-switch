/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_GATEWAY_H
#define NOSAIC_GATEWAY_H

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

/*
 * Virtual gateways: a shared address on an SVI, answered with a virtual MAC
 * and routed for in the chip by every switch that has it -- both of an MLAG
 * pair, active-active. switchapi 1.6. See gateway.c.
 */
int  nosaic_gw_start(int unit);

/* The contract calls. 0, -1 with a reason in err, or -2 for unsupported. */
int  nosaic_gw_set_mac(const char *mac, char *err, size_t errlen);
int  nosaic_gw_add(const char *svi, const char *prefix, char *err, size_t errlen);
int  nosaic_gw_del(const char *svi, const char *prefix, char *err, size_t errlen);
void nosaic_gw_query(FILE *out);
int  nosaic_gw_supported(void);

/* For vlan.c: an SVI went away, and its gateways with it. */
void nosaic_gw_svi_gone(int vid);

/*
 * For tapbridge's receive path, lock-free: whether VLAN vid has any gateway,
 * whether ip (network order) is one of its addresses, and the virtual MAC.
 */
int  nosaic_gw_on(int vid);
int  nosaic_gw_is(int vid, uint32_t ip_be);
void nosaic_gw_mac(unsigned char mac[6]);

#endif
