/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Front-panel port LEDs.
 *
 * A switch whose ports work and whose panel is dark reads, to anyone standing
 * in front of the rack, as a switch that is not working. This board's LEDs are
 * on the Arista SCD rather than driven by the chip's own LED processors -- the
 * Trident2 LED microcontrollers are present but disabled, on this box and
 * under EOS alike -- so nothing lights them unless we do.
 *
 * The register map, and the fact that it is bit 28 rather than any of the
 * obvious alternatives, came out of the reverse-engineering work on this board
 * and was established by writing values and having somebody look at the panel:
 *
 *   0xA000 + 0x10*(i*4 + j)    QSFP cage i, lane j         (Ethernet49..52)
 *
 *   bit 28  green      bit 27  amber, and amber wins when both are set
 *
 * A QSFP cage is a single 40G logical port at 49, 53, 57 or 61, and all four of
 * its lane lamps are driven together because the cage has one physical light.
 *
 * ⚠ THIS FILE USED TO DRIVE PORTS 1-48 FROM THE SCD, AND THAT WAS THE SIBLING'S
 * MAP, NOT THIS BOARD'S.
 *
 * It wrote 0x6100 + 0x10*n for "SFP+ port n+1" and put the QSFP lamps at
 * 0x6400, over six cages -- which is the 7050SX2-72Q exactly: 48 SFP+ cages,
 * six QSFP, 72 ports. This board has 48 ports of 10GBASE-T and four QSFP, and
 * its board description file creates LED blocks only for status, fan, both
 * PSUs and sixteen QSFP lanes at 0xA000. There is no SCD lamp for a copper port
 * here at all, so every write to 0x6100+ addressed nothing and returned
 * happily, and the copper half of the panel stayed dark.
 *
 * The copper LEDs are on the PHYs and are driven from phy.c, which is where the
 * link-with-a-real-speed test already lives. This file is QSFP only.
 *
 * ⚠ And the QSFP base was wrong too -- 0x6400 rather than 0xA000 -- so the
 * blocks it wrote were not lamps on this board either. Writing unknown SCD
 * offsets is not free here: blind register access has wedged this box before.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/mman.h>

#include <bcm/error.h>
#include <bcm/port.h>
#include <bcm/stg.h>

#include "led.h"

#define SCD_VENDOR   0x3475
#define SCD_MAP_LEN  0x80000u

#define QSFP_BASE    0xA000u
#define LED_STRIDE   0x10u
#define LED_GREEN    0x10000000u
#define LED_AMBER    0x08000000u

#define QSFP_CAGES   4
#define QSFP_LANES   4
#define FIRST_QSFP   49        /* logical port of Ethernet49/1 */
#define QSFP_STRIDE  4         /* logical ports per cage in this port map */
#define MAX_LED_PORT (FIRST_QSFP + QSFP_CAGES * QSFP_STRIDE)

static volatile uint32_t *scd;
static int   led_unit = -1;
static int   led_on;
static uint32_t led_last[MAX_LED_PORT + 1];

/*
 * The SCD is 3475:0001 and its PCI address is not fixed -- 05:00.0 under EOS,
 * 04:00.0 under this kernel -- so it is found by vendor id rather than named.
 */
static int led_map_scd(void)
{
	DIR *d = opendir("/sys/bus/pci/devices");
	struct dirent *e;
	char path[256];
	int fd;

	if (d == NULL)
		return -1;
	while ((e = readdir(d)) != NULL) {
		unsigned vendor = 0;
		FILE *f;

		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor", e->d_name);
		if ((f = fopen(path, "r")) == NULL)
			continue;
		if (fscanf(f, "%x", &vendor) != 1)
			vendor = 0;
		fclose(f);
		if (vendor != SCD_VENDOR)
			continue;

		snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource0", e->d_name);
		closedir(d);
		if ((fd = open(path, O_RDWR | O_SYNC)) < 0)
			return -1;
		scd = mmap(NULL, SCD_MAP_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		close(fd);
		if (scd == MAP_FAILED) {
			scd = NULL;
			return -1;
		}
		return 0;
	}
	closedir(d);
	return -1;
}

/*
 * The lamps for one logical port. A QSFP cage has four, and they are written
 * together: the cage is one light on the panel however many lanes it carries.
 */
static int led_regs(int port, uint32_t *out)
{
	int cage, i;

	/* Copper ports have no SCD lamp on this board; phy.c drives theirs. */
	if (port < FIRST_QSFP || port >= MAX_LED_PORT)
		return 0;
	if ((port - FIRST_QSFP) % QSFP_STRIDE != 0)
		return 0;                       /* a breakout lane, not a cage */
	cage = (port - FIRST_QSFP) / QSFP_STRIDE;
	for (i = 0; i < QSFP_LANES; i++)
		out[i] = QSFP_BASE + LED_STRIDE * (uint32_t)(cage * QSFP_LANES + i);
	return QSFP_LANES;
}

int nosaic_led_start(int unit)
{
	int port, i;

	led_unit = unit;
	if (led_map_scd() != 0) {
		fprintf(stderr, "led: no SCD found; the front panel stays dark\n");
		return -1;
	}

	/* Start from a known state.
	 *
	 * Whatever is on these registers -- EOS's last write before this machine
	 * changed hands, or an animation somebody ran -- is not information about
	 * the ports now. Clear them and let the first poll light what is up. */
	for (port = 1; port <= MAX_LED_PORT; port++) {
		uint32_t off[QSFP_LANES];
		int n = led_regs(port, off);

		for (i = 0; i < n; i++)
			scd[off[i] / 4] = 0;
		led_last[port] = 0;
	}
	led_on = 1;
	printf("led: driving the front panel from link state\n");
	fflush(stdout);
	return 0;
}

/*
 * Compare against the HARDWARE, not against what we last wrote.
 *
 * Caching our own writes and touching a register only when link state changes
 * is cheaper and wrong: it cannot repair a register something else scribbled
 * on, which is the whole class of fault this exists to fix. An LED left lit by
 * an animation, a test, or the vendor OS before we took the machine would stay
 * lit for ever, because as far as the cache is concerned nothing changed.
 *
 * Reading back makes this loop authoritative over the panel: if the hardware
 * disagrees with the link, it is corrected within one interval no matter who
 * wrote it. Seventy-two reads of a memory-mapped BAR costs nothing worth
 * measuring.
 */
void nosaic_led_poll(void)
{
	int port, changed = 0;

	if (!led_on || scd == NULL)
		return;

	for (port = 1; port <= MAX_LED_PORT; port++) {
		uint32_t off[QSFP_LANES], want = 0;
		int n = led_regs(port, off), link = 0, i;

		if (n == 0)
			continue;

		/* A port the SDK will not answer for is treated as DOWN rather than
		 * skipped: a lamp must never stay lit for something that is not
		 * there. */
		if (bcm_port_link_status_get(led_unit, port, &link) != BCM_E_NONE)
			link = 0;

		if (link) {
			int stp = BCM_STG_STP_FORWARD;

			/* Green needs link AND forwarding. A failed STP query falls back
			 * to green, which is the opposite of the rule a thermal loop
			 * would use -- there an unreadable sensor is a safety matter. A
			 * failed query here would repaint the whole panel amber and turn
			 * one unknown into a panel that cries wolf. */
			if (bcm_port_stp_get(led_unit, port, &stp) != BCM_E_NONE)
				stp = BCM_STG_STP_FORWARD;
			want = (stp == BCM_STG_STP_FORWARD) ? LED_GREEN : LED_AMBER;
		}

		for (i = 0; i < n; i++) {
			if (scd[off[i] / 4] != want)
				scd[off[i] / 4] = want;
		}
		if (led_last[port] != want)
			changed++;
		led_last[port] = want;
	}
	if (changed) {
		printf("led: %d port LED(s) changed\n", changed);
		fflush(stdout);
	}
}

/* Dark rather than frozen on a stale picture. */
void nosaic_led_stop(void)
{
	int port, i;

	if (!led_on || scd == NULL)
		return;
	for (port = 1; port <= MAX_LED_PORT; port++) {
		uint32_t off[QSFP_LANES];
		int n = led_regs(port, off);

		for (i = 0; i < n; i++)
			scd[off[i] / 4] = 0;
	}
	led_on = 0;
}
