#ifndef NOSAIC_TD2_PHY_H
#define NOSAIC_TD2_PHY_H

/* The 48 external copper PHYs.
 *
 * Start after the ports are enabled; poll from the datapath tick. Both are
 * no-ops on a board whose properties declare no external PHYs, so this costs
 * nothing on one that has none.
 */
/* Bind the external PHY drivers and enable autonegotiation. Call BEFORE the
 * ports are enabled: binding re-initialises the port it belongs to. */
int  nosaic_phy_bind(int unit);
int  nosaic_phy_start(int unit);
void nosaic_phy_poll(void);
void nosaic_phy_stop(void);

/* Dump every external PHY's status registers as JSON array elements, for the
 * query socket's `phy.dump`. Registered with the socket by nosaic_phy_start,
 * including on a board where this file drives nothing else. */
#include <stdio.h>
void nosaic_phy_dump(FILE *out);

/* Read a run of registers from one PHY's MMD, for the socket's `phy.read`. */
void nosaic_phy_read(FILE *out, int port, int devad, int reg, int count);

#endif
