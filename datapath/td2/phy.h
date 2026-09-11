#ifndef NOSAIC_TD2_PHY_H
#define NOSAIC_TD2_PHY_H

/* The 48 external copper PHYs.
 *
 * Start after the ports are enabled; poll from the datapath tick. Both are
 * no-ops on a board whose properties declare no external PHYs, so this costs
 * nothing on one that has none.
 */
int  nosaic_phy_start(int unit);
void nosaic_phy_poll(void);
void nosaic_phy_stop(void);

#endif
