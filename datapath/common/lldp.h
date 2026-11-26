/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_LLDP_H
#define NOSAIC_LLDP_H

#include <stddef.h>
#include <stdio.h>

/*
 * IEEE 802.1AB Link Layer Discovery Protocol (LLDP).
 *
 * Punts 01:80:c2:00:00:0e (EtherType 0x88cc) to the CPU for front-panel ports
 * and optionally binds an AF_PACKET socket on eth0 when enabled per-interface.
 */
int  nosaic_lldp_start(int unit);
int  nosaic_lldp_set(const char *iface, int enabled, char *err, size_t errlen);
void nosaic_lldp_query(FILE *out);
void nosaic_lldp_config_query(FILE *out);

#endif
