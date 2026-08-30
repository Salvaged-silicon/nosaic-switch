/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_TD2P_L3SYNC_H
#define NOSAIC_TD2P_L3SYNC_H

#include <bcm/types.h>

/* Give the chip a router interface on a port. Call once per routed port,
 * with the same MAC, VLAN and MTU the tap has. */
int nosaic_l3_add_intf(int unit, const char *ifname, int port, int vlan,
		       const bcm_mac_t mac, int mtu);

/* Mirror the current kernel FIB into the chip. Cheap to call repeatedly. */
void nosaic_l3_poll(void);

/* What is in the chip, from the chip's own accounting. */
void nosaic_l3_stats(void);

#endif
