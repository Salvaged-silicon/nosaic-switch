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
 * ⚠ AND IT IS WRITTEN EVEN WHEN IT ALREADY READS BACK CORRECT.
 *
 * This file used to read the interface first and write only on a mismatch,
 * carried over from the sibling board where re-applying a setting a 40G port
 * already had left both its ports linked at the PCS and deaf at the MAC.
 *
 * That lesson does not transfer, and skipping the write is what kept the 48
 * copper ports silent. A port that negotiates 10G wants XFI, XFI is also the
 * chip's default, and the SDK's own linkscan moves the MAC there on link-up --
 * so the value matched, nothing was written, and the port passed not one frame
 * in either direction with every status saying it was fine. There was not a
 * single line of this file's output in the log to say so.
 *
 * Reading the field back is not evidence the port was configured. It is the
 * field's default, and on this board it is not even stable: a late-cabled port
 * accepted an interface change and silently reverted to XFI afterwards.
 *
 * The predecessor never had the bug because it never made the check -- it sets
 * the interface unconditionally for any external-PHY port that reports a real
 * speed, and its copper ports carried traffic. So does this now.
 *
 * The sibling's warning is still real, and what keeps it satisfied is that this
 * happens ONCE PER LINK EVENT, not on a timer: phy_matched[] is set on success
 * and cleared only when the link drops. Re-applying repeatedly is the harm;
 * applying once to a port that just came up is the bring-up.
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
#include "tapbridge.h"
#include "phy.h"

/* Ports are 1-based and this board has 52. 64 covers the chip's range. */
#define PHY_MAX_PORT 64

/* MDIO reads per poll. The bus is shared with the SDK's own linkscan and with
 * the PHY firmware; this is the budget the datapath can spend without taking
 * receive down, and it is deliberately small because a port that waits one more
 * second to be matched costs nothing.
 */
#define PHY_READS_PER_PASS 4

/*
 * The front-panel LED for a copper port, which is on the PHY and nowhere else.
 *
 * ⚠ THE SCD DOES NOT DRIVE THESE, WHATEVER THE SIBLING BOARD DOES.
 *
 * led.c lights ports 1-48 through SCD blocks at 0x6100 + 0x10*n, and its own
 * comment calls them "SFP+" -- because on the sibling they are SFP+ cages and
 * that is where those blocks come from. This board's ports 1-48 are 10GBASE-T
 * behind BCM84848s, and the board description file creates LED blocks only for
 * status, fan, both PSUs and the sixteen QSFP lanes. Writing 0x6100+ here
 * addresses nothing and reports success, which is why the panel was dark.
 *
 * The real control is PHY register 1.0xa83b: five 3-bit mode fields, where 0 is
 * dark, 2 is lit, 3 is the parked state the driver's halt path writes, and 4
 * means "the PHY's own firmware drives this". The vendor OS holds all five at 4
 * and lets firmware flip field 0 to 2 on link -- 0x4924 down, 0x4922 up.
 *
 * OUR firmware never writes the register at all, so the field that is supposed
 * to follow link simply never moves. That is the difference, and it is why
 * matching the vendor's three configuration registers is necessary and still
 * lights nothing on its own: something has to write 0xa83b, and here it is us.
 *
 * ★ AND THE CONDITION IS LINK **AND** SPEED, NOT LINK.
 *
 * Every copper port on this board reports link with the driver bound, cable or
 * not -- "Link Up with Speed 0M!". Gating on link alone lights all 48 with
 * nothing plugged in, which looks like a working panel and is worse than a dark
 * one. phy_matched[] is already exactly "has link and a real negotiated speed",
 * so the LED follows it rather than a second, weaker test.
 */
#define PHY_LED_CTRL     0xa83b   /* five 3-bit mode fields */
#define PHY_LED_LIT      0x4922   /* field 0 = 2 (lit),  rest firmware-driven */
#define PHY_LED_DARK     0x4924   /* field 0 = 4 (firmware), and it never moves */
#define PHY_LED_MODE_VAL 0x0020   /* what the vendor holds the three below at */

static const int phy_led_mode_regs[] = { 0xa82c, 0xa82f, 0xa835 };

static int phy_unit = -1;
static int phy_any;                        /* any external PHY on this board */
static char phy_led_lit[PHY_MAX_PORT + 1]; /* what we last wrote to 0xa83b */
static char phy_copper[PHY_MAX_PORT + 1];  /* port has an external PHY */
static char phy_matched[PHY_MAX_PORT + 1]; /* interface agrees with the wire */
static int  phy_rr = 1;                    /* round robin across candidates */

/*
 * Does this port really have link? The answer tapbridge's silent-port
 * diagnostic needs, and the ★ condition from the LED note above: link AND a
 * real negotiated speed, not link.
 *
 * A port with no external PHY is not ours to judge -- the uplinks are direct
 * SerDes and their link status means what it says -- so it answers yes and
 * lets the SDK stand. Only copper is filtered, because only copper lies.
 *
 * Free to call: phy_matched[] is already maintained per link event, so this
 * reads software state and issues no MDIO. A per-interval speed sweep across
 * 48 external PHYs is the call pattern that once killed copper receive here,
 * which is exactly why this is answered from cached state and not asked.
 */
static int phy_link_is_real(int port)
{
	if (port < 1 || port > PHY_MAX_PORT)
		return 1;
	if (!phy_copper[port])
		return 1;
	return phy_matched[port] ? 1 : 0;
}

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
		if (nosaic_props_get_unit(key, phy_unit) != NULL) {
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

/*
 * ⚠ THESE PHYs ARE ONLY REACHABLE THROUGH THE DRIVER'S OWN ACCESSORS.
 *
 * bcm_port_phy_set with BCM_PORT_PHY_CLAUSE45 goes to soc_miimc45_write
 * (src/bcm/esw/port.c) -- the switch chip's internal MIIM controller, on pins
 * that have nothing attached here, because Arista hangs the copper PHYs off the
 * SCD's MDIO accelerators. It returns BCM_E_NONE having reached nothing, which
 * is how the first version of this code configured 48 LEDs, reported success
 * for every one of them, and left the panel dark.
 *
 * pc->read / pc->write are the pointers the bound driver itself uses, so they
 * land on the bus phybus.c serves. Address encoding is the SDK's own: devad in
 * bits 21:16, regad in 15:0.
 */
#define PHY_C45_ADDR(_devad, _reg) \
	((((uint32)(_devad) & 0x3f) << 16) | ((uint32)(_reg) & 0xffff))

/*
 * Declared here rather than included. soc/phy/phyctrl.h drags in phymod, whose
 * headers the openbcm package does not stage, and the struct behind those
 * accessors must not be hand-declared -- a struct this file and the SDK
 * disagree about compiles, links and corrupts silently. A function prototype
 * carries no layout, so declaring these two is safe in a way that declaring
 * phy_ctrl_t would not be. Same arrangement, and same reasoning, as the bus
 * hook in phybus.c.
 *
 * With SOC_PHY_INTERNAL clear these take the EXTERNAL PHY's driver, and for a
 * BCM84848 in copper mode phy_8481_reg_write ends at WRITE_PHY_REG -- which is
 * pc->write, the accessor that reaches the SCD's MDIO accelerators.
 */
extern int soc_phyctrl_reg_write(int unit, int port, uint32 flags,
				 uint32 addr, uint32 data);
extern int soc_phyctrl_reg_read(int unit, int port, uint32 flags,
				uint32 addr, uint32 *data);

static int phy_reg_write(int port, int devad, int reg, uint16 val)
{
	return soc_phyctrl_reg_write(phy_unit, port, 0,
				     PHY_C45_ADDR(devad, reg), val) < 0 ? -1 : 0;
}

static int phy_reg_read(int port, int devad, int reg, uint16 *val)
{
	uint32 v = 0;

	if (soc_phyctrl_reg_read(phy_unit, port, 0,
				 PHY_C45_ADDR(devad, reg), &v) < 0)
		return -1;
	*val = (uint16)v;
	return 0;
}

/* Drive one copper port's LED. One MDIO write, only on a change of state. */
static void phy_led_set(int port, int lit)
{
	if (phy_reg_write(port, 1, PHY_LED_CTRL,
			  lit ? PHY_LED_LIT : PHY_LED_DARK) != 0) {
		/* Reported once per port rather than per attempt: a PHY that
		 * refuses this is a dark port, not a broken switch. */
		printf("phy: port %d would not take its LED control word; its "
		       "front-panel light will not follow link\n", port);
		fflush(stdout);
		return;
	}
	phy_led_lit[port] = (char)(lit ? 1 : 0);
}

/*
 * Bind the external PHY drivers and enable autonegotiation.
 *
 * Separate from nosaic_phy_start and called BEFORE the ports are enabled,
 * because binding a driver re-initialises its port: see the note at the call
 * site in main.c.
 */
int nosaic_phy_bind(int unit)
{
	int p, n = 0;

	phy_unit = unit;
	memset(phy_copper, 0, sizeof(phy_copper));
	memset(phy_matched, 0, sizeof(phy_matched));
	phy_any = 0;

	phy_scan_properties();
	if (!phy_any)
		return 0;   /* Not an error. Every other board is like this. */

	/*
	 * ⚠ BIND THE EXTERNAL PHY DRIVER FIRST, OR EVERY SETTING BELOW GOES TO
	 * THE WRONG PART.
	 *
	 * The chip comes up with every port bound to its INTERNAL SerDes, even
	 * on a board whose front panel is 10GBASE-T behind external PHYs.
	 * bcm_port_probe is what walks the configured addresses --
	 * port_phy_addr_<n> and port_phy_clause_<n> from the port map -- and
	 * binds the real driver to each one.
	 *
	 * Without it the SDK answers every question from the internal SerDes
	 * and accepts every setting there too, which is the worst possible
	 * failure: enabling autonegotiation "works", the port reports a speed,
	 * the MAC reconfigures itself to match, nothing returns an error, and
	 * the BCM84848 that actually terminates the wire was never spoken to at
	 * all. Two of this board's own ports, patched to each other, stayed
	 * dark through exactly that.
	 *
	 * Probed as one bitmap rather than per port: the SDK reports back which
	 * ones succeeded, and a port that did not bind is a port whose copper
	 * side is unreachable no matter what else is configured.
	 */
	{
		bcm_pbmp_t want, okay;
		int probed = 0;

		BCM_PBMP_CLEAR(want);
		BCM_PBMP_CLEAR(okay);
		for (p = 1; p <= PHY_MAX_PORT; p++)
			if (phy_copper[p])
				BCM_PBMP_PORT_ADD(want, p);

		if (bcm_port_probe(phy_unit, want, &okay) != BCM_E_NONE) {
			printf("phy: bcm_port_probe failed; the external PHYs are not "
			       "bound and no copper port can link\n");
		} else {
			for (p = 1; p <= PHY_MAX_PORT; p++)
				if (phy_copper[p] && BCM_PBMP_MEMBER(okay, p))
					probed++;
			printf("phy: %d external PHY(s) bound by probe\n", probed);
		}
		fflush(stdout);
	}

	/*
	 * ⚠ AUTONEGOTIATION IS NOT OPTIONAL ON 10GBASE-T. IT IS THE STANDARD.
	 *
	 * Copper here does not "prefer" to negotiate the way a 1000BASE-T port
	 * does -- 10GBASE-T has no way to bring a link up without it. A port
	 * left with autoneg off is a port that will never link, to anything,
	 * for ever, and it presents as a dead cable: no carrier, no speed, no
	 * error, and every status the SDK offers saying the port is enabled and
	 * fine.
	 *
	 * Proved on this board with a patch cable joining two of its own
	 * front-panel ports, which is as controlled as a link test gets: two
	 * BCM84848s, both ends ours, both configured, and no link at all until
	 * this call was added.
	 *
	 * The MAC interface is deliberately NOT set here. It has to follow the
	 * NEGOTIATED speed, which is unknown until something is plugged in --
	 * forcing SGMII on an idle port would cap a 10GBASE-T neighbour at 1G.
	 * nosaic_phy_poll does it once a port actually links.
	 *
	 * One MDIO write per port, once, at startup. That is not the budget the
	 * warning above is about: what starved the bus was POLLING every port
	 * on a timer, not configuring each of them a single time.
	 */
	for (p = 1; p <= PHY_MAX_PORT; p++) {
		if (!phy_copper[p])
			continue;
		n++;
		if (bcm_port_autoneg_set(unit, p, 1) != BCM_E_NONE) {
			printf("phy: port %d would not take autoneg; 10GBASE-T cannot "
			       "link without it and this port will stay down\n", p);
			fflush(stdout);
		}
	}
	printf("phy: %d port(s) behind external PHYs; autonegotiation enabled\n", n);

	/*
	 * The LED configuration the vendor OS holds, and a dark panel to start.
	 *
	 * Three registers at a fixed value, written once: our PHY firmware
	 * leaves them at 0x0008/0x0010/0x0040 and the vendor holds all three at
	 * 0x0020 whether the port is linked or not, so they are configuration
	 * rather than state. Then 0xa83b explicitly dark, because the firmware
	 * default of 0x0400 is neither of the two states this code drives and a
	 * port nothing is plugged into should not be lit.
	 *
	 * 4 writes per port, once. That is not the budget the warning at the top
	 * of this file is about -- what starved the bus was POLLING every port on
	 * a timer.
	 */
	for (p = 1; p <= PHY_MAX_PORT; p++) {
		unsigned i;

		if (!phy_copper[p])
			continue;
		for (i = 0; i < sizeof(phy_led_mode_regs) / sizeof(phy_led_mode_regs[0]); i++)
			(void)phy_reg_write(p, 1, phy_led_mode_regs[i], PHY_LED_MODE_VAL);
		phy_led_set(p, 0);

		/* ⚠ READ ONE BACK. A write that reaches nothing reports success,
		 * and that is exactly how this was wrong the first time. */
		if (p == 1) {
			uint16 v = 0;

			if (phy_reg_read(p, 1, PHY_LED_CTRL, &v) != 0)
				printf("phy: port 1 LED control is not readable; the "
				       "panel will not follow link\n");
			else if (v == 0xffff)
				printf("phy: port 1 LED control reads 0xffff -- an idle "
				       "bus, not data; the write did not land\n");
			else
				printf("phy: port 1 LED control reads %#06x after the "
				       "dark write (expect %#06x)\n", v, PHY_LED_DARK);
		}
	}
	printf("phy: %d port LED(s) configured and dark; they follow link from here\n", n);
	fflush(stdout);
	return 0;
}

int nosaic_phy_start(int unit)
{
	int p, n = 0;

	phy_unit = unit;

	/* So the silent-port diagnostic does not fire for every unconnected
	 * copper port. Without it 42 of this board's 52 ports match "link and
	 * no traffic" every interval and the one real fault is invisible. */
	nosaic_tap_link_filter(phy_link_is_real);
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

	/* The panel follows phy_matched[], which is already "link with a real
	 * speed" -- see the LED note at the top. One write per transition, so a
	 * steady switch writes nothing at all. */
	for (n = 1; n <= PHY_MAX_PORT; n++) {
		if (!phy_copper[n])
			continue;
		if (phy_matched[n] != phy_led_lit[n])
			phy_led_set(n, phy_matched[n]);
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

		/*
		 * Written whether or not it already reads back right. See the
		 * header: the read-back is the field's default, not evidence the
		 * port was ever configured, and skipping the write here is what
		 * kept every copper port silent.
		 *
		 * Once per link event, because phy_matched[] is set below and
		 * cleared only when the link drops.
		 */
		if (bcm_port_interface_set(phy_unit, port, want) != BCM_E_NONE) {
			printf("phy: port %d negotiated %d Mb but the MAC would not take "
			       "%s; it will link and pass nothing\n",
			       port, speed, phy_if_name(want));
			fflush(stdout);
			continue;
		}
		phy_matched[port] = 1;
		if (have == want)
			printf("phy: port %d negotiated %d Mb, MAC interface %s "
			       "re-applied\n", port, speed, phy_if_name(want));
		else
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
