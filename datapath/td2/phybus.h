#ifndef NOSAIC_TD2_PHYBUS_H
#define NOSAIC_TD2_PHYBUS_H

/* Install the copper PHY bus before any port is probed.
 *
 * The external PHYs are on the board controller, not the switch chip, so
 * without this the SDK binds every copper port to the internal SerDes and
 * silently configures the wrong part. Harmless on a board that states no
 * controller: it does nothing and says so.
 */
int nosaic_phybus_install(int unit, const char *scd_bdf);

/* Say how much traffic the SDK actually put through the bus. "Installed" and
 * "used" are different claims and only the second one matters. */
void nosaic_phybus_report(void);

#endif
