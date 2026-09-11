/*
 * The 48 external copper PHYs on the Arista 7050TX-64.
 *
 * Neither board that came before this one has an external PHY: their cages are
 * direct SerDes, and the chip drives them end to end. Here every front-panel
 * port below the QSFP cages is 10GBASE-T behind a BCM84848 that runs its own
 * firmware, reached over the board controller's MDIO bus.
 *
 * The SDK owns the PHY itself -- its phy8481 driver downloads the firmware and
 * runs link training, given load_firmware and phy_bus_i2c_<n> in asic.conf.
 * What the SDK does NOT do is keep the switch chip's side of the port agreeing
 * with what the PHY negotiated on the wire, and that is what this file is for.
 *
 * ⚠ THE MAC INTERFACE MUST FOLLOW THE NEGOTIATED SPEED.
 *
 * A 10GBASE-T port left at its XFI default while the copper side negotiates 1G
 * gives a PHY with a real link on BOTH sides that bridges nothing between them.
 * Both ends report the port up, both transmit, neither receives, and there are
 * zero errors on either side -- so every status the SDK offers says the port is
 * fine. It reads as a dead cable and it is a one-line configuration mismatch.
 *
 * SGMII at or below 2.5G, XFI at 10G.
 *
 * ⚠ AND ONLY WHEN IT ACTUALLY DIFFERS.
 *
 * bcm_port_interface_set takes the MAC through reset to apply a change. On the
 * sibling board, applying a setting a port had already been given correctly at
 * init left both 40G ports linked at the PCS and deaf at the MAC -- the same
 * zero-frames, zero-errors signature, arrived at from the opposite direction.
 * So this reads first and writes only on a genuine mismatch. A port the chip
 * configured correctly does not want configuring again.
 *
 * ⚠ MDIO IS A SHARED BUS AND THE DATAPATH DEPENDS ON IT.
 *
 * bcm_port_speed_get on a port behind an external PHY is an MDIO round trip.
 * Issuing 48 of them on a two-second timer starved the bus badly enough to kill
 * copper RECEIVE on every port, while the direct-SerDes 40G ports carried on
 * working -- proven by rolling back the binary that did it. So: link state
 * comes from bcm_port_link_status_get, which is software state linkscan
 * maintains and costs nothing; the speed read happens only for a port that has
 * link and has not been matched yet, and at most PHY_READS_PER_PASS of those
 * per pass.
 *
 * A port is matched once and then left alone until its link drops, which is the
 * event that can change the negotiated speed.
 */
#include <stdio.h>
#include <string.h>

#include <bcm/port.h>
#include <bcm/error.h>

#include "props.h"
#include "phy.h"

/* Ports are 1-based and this board has 52. 64 covers the chip's range. */
#define PHY_MAX_PORT 64

/* MDIO reads per poll. The bus is shared with the SDK's own linkscan and with
 * the PHY firmware; this is the budget the datapath can spend without taking
 * receive down, and it is deliberately small because a port that waits one more
 * second to be matched costs nothing.
 */
#define PHY_READS_PER_PASS 4

static int phy_unit = -1;
static int phy_any;                        /* any external PHY on this board */
static char phy_copper[PHY_MAX_PORT + 1];  /* port has an external PHY */
static char phy_matched[PHY_MAX_PORT + 1]; /* interface agrees with the wire */
static int  phy_rr = 1;                    /* round robin across candidates */

/* Which ports have an external PHY, from the properties rather than from a
 * port number range. The board declares phy_bus_i2c_<n> for exactly the ports
 * whose PHY hangs off the controller's MDIO bus, so a board with none -- or
 * with a different count -- needs no change here. */
static void phy_scan_properties(void)
{
	int p;

	for (p = 1; p <= PHY_MAX_PORT; p++) {
		char key[32];

		snprintf(key, sizeof(key), "phy_bus_i2c_%d", p);
		if (nosaic_props_get(key) != NULL) {
			phy_copper[p] = 1;
			phy_any = 1;
		}
	}
}

/* What the MAC side should be for a speed the PHY negotiated. */
static bcm_port_if_t phy_want_interface(int speed)
{
	return (speed > 0 && speed <= 2500) ? BCM_PORT_IF_SGMII : BCM_PORT_IF_XFI;
}

static const char *phy_if_name(bcm_port_if_t i)
{
	switch (i) {
	case BCM_PORT_IF_SGMII: return "SGMII";
	case BCM_PORT_IF_XFI:   return "XFI";
	case BCM_PORT_IF_XGMII: return "XGMII";
	default:                return "other";
	}
}

int nosaic_phy_start(int unit)
{
	int p, n = 0;

	phy_unit = unit;
	memset(phy_copper, 0, sizeof(phy_copper));
	memset(phy_matched, 0, sizeof(phy_matched));
	phy_any = 0;

	phy_scan_properties();
	if (!phy_any) {
		/* Not an error. Every other board in the tree is like this. */
		return 0;
	}
	for (p = 1; p <= PHY_MAX_PORT; p++)
		if (phy_copper[p])
			n++;
	printf("phy: %d port(s) behind external PHYs; matching the MAC interface "
	       "to the negotiated speed as they link\n", n);
	fflush(stdout);
	return 0;
}

void nosaic_phy_poll(void)
{
	int reads = 0, n;

	if (phy_unit < 0 || !phy_any)
		return;

	/* One sweep of the free information first: a link that has gone away
	 * un-matches its port, because the speed can differ next time it comes
	 * back. This costs nothing -- link state is software state linkscan
	 * maintains, not a bus transaction. */
	for (n = 1; n <= PHY_MAX_PORT; n++) {
		int link = 0;

		if (!phy_copper[n] || !phy_matched[n])
			continue;
		if (bcm_port_link_status_get(phy_unit, n, &link) != BCM_E_NONE || !link)
			phy_matched[n] = 0;
	}

	/* Then spend the MDIO budget, round robin so no port can starve behind a
	 * lower-numbered one that keeps flapping. */
	for (n = 0; n < PHY_MAX_PORT && reads < PHY_READS_PER_PASS; n++) {
		int port = 1 + ((phy_rr + n) % PHY_MAX_PORT);
		int link = 0, speed = 0;
		bcm_port_if_t have;
		bcm_port_if_t want;

		if (!phy_copper[port] || phy_matched[port])
			continue;
		if (bcm_port_link_status_get(phy_unit, port, &link) != BCM_E_NONE || !link)
			continue;

		phy_rr = port + 1;
		reads++;

		/* ⚠ A LINK WITH NO SPEED IS NOT A LINK.
		 *
		 * Every unconnected port on this board reports "Link Up with Speed
		 * 0M" once the PHY driver is bound. Taking that at face value
		 * configures all 48 as though they were cabled, and the interface
		 * chosen for a speed of zero is wrong for whatever eventually
		 * arrives. */
		if (bcm_port_speed_get(phy_unit, port, &speed) != BCM_E_NONE || speed <= 0)
			continue;

		if (bcm_port_interface_get(phy_unit, port, &have) != BCM_E_NONE)
			continue;

		want = phy_want_interface(speed);
		if (have == want) {
			/* Already right. Matched without writing anything, which is
			 * the common case and the one that must stay cheap. */
			phy_matched[port] = 1;
			continue;
		}

		if (bcm_port_interface_set(phy_unit, port, want) != BCM_E_NONE) {
			printf("phy: port %d negotiated %d Mb but the MAC would not take "
			       "%s; it will link and pass nothing\n",
			       port, speed, phy_if_name(want));
			fflush(stdout);
			continue;
		}
		phy_matched[port] = 1;
		printf("phy: port %d negotiated %d Mb, MAC interface %s -> %s\n",
		       port, speed, phy_if_name(have), phy_if_name(want));
		fflush(stdout);
	}
}

void nosaic_phy_stop(void)
{
	phy_unit = -1;
	phy_any = 0;
}
