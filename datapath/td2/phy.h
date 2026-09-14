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

#endif
