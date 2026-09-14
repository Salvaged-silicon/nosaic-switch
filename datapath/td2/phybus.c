/*
 * Giving the SDK a bus it can reach the copper PHYs on.
 *
 * ⚠ THE 48 BCM84848s ARE NOT ON THE SWITCH CHIP'S MDIO BUS.
 *
 * They hang off the board controller's MDIO accelerators, which the SDK knows
 * nothing about. So bcm_port_probe binds every copper port to the chip's
 * INTERNAL SerDes, and every setting afterwards is accepted by the wrong part:
 * autonegotiation "enables", a speed is reported, the MAC reconfigures to
 * match, nothing returns an error, and the PHY that actually terminates the
 * wire has never been spoken to. Two ports of this board patched to each other
 * stayed dark through exactly that.
 *
 * The SDK has a seam for this. A port with phy_bus_i2c_<port>=1 has its
 * external PHY access routed to phy_i2c_miireg_read/write, which call whatever
 * is installed with phy_i2c_bus_func_hook_set. The property is named for I2C
 * and the hook is not: it is "application-provided PHY bus", and nothing about
 * it is I2C specific.
 *
 * What that buys is large. Broadcom's own BCM84848 driver is compiled into
 * this build, firmware blob and all, and it does the bring-up -- the firmware
 * download, the training, the link. The alternative is reimplementing a
 * 10GBASE-T PHY driver, which is not a trade anybody should take.
 *
 * This file is the Apache-2.0 half: mapping the controller, decoding which
 * accelerator and bus a port's PHY is on, and installing the hook. The
 * transfer protocol itself is ported from Arista's GPL driver and lives in
 * datapath/scdmdio, under its own licence.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sal/types.h>

/*
 * Declared here rather than included.
 *
 * soc/phy/phyctrl.h carries these, but including it drags in phymod headers
 * that this SDK build does not stage, and none of that is needed to install a
 * bus. The two callback types and the hook are copied from that header --
 * include/soc/phy/phyctrl.h:125-126 and :177 in OpenBCM 6.5.24 -- and a
 * mismatch here would be a compile or link error rather than something
 * silent, which is what makes declaring them acceptable.
 *
 * Guarded by INCLUDE_I2C in the SDK, which this build defines.
 */
typedef int (*soc_phy_bus_rd_t)(int, uint32, uint32, uint16 *);
typedef int (*soc_phy_bus_wr_t)(int, uint32, uint32, uint16);
extern int phy_i2c_bus_func_hook_set(int unit, soc_phy_bus_rd_t rd,
				     soc_phy_bus_wr_t wr);

#include "../scdmdio/scdmdio.h"
#include "phybus.h"

/* Three accelerators, 0x100 apart, two buses each and eight copper ports per
 * bus. Offsets within the controller's first BAR. */
#define MDIO_ACCEL_BASE   0xb000
#define MDIO_ACCEL_STRIDE 0x100
#define MDIO_ACCELS       3

/* The last byte this file touches: accelerator 2's response register. The map
 * has to reach it, and must not be larger than the BAR.
 *
 * ⚠ DO NOT MAP A FIXED SIZE. This controller's first BAR is 512 KB, and a
 * request for a round megabyte fails outright -- the copper PHYs then look
 * unreachable on a board where they are fine.
 */
#define SCD_MDIO_END (MDIO_ACCEL_BASE + MDIO_ACCELS * MDIO_ACCEL_STRIDE)

static volatile uint32_t *phybus_bar;

/* How many times the SDK has actually come through this bus, and how many of
 * those failed. Reported because "the hook is installed" and "the hook is
 * used" are different claims, and only the second one means the PHYs are
 * reachable. A zero here on a board with copper ports says the SDK never
 * routed a single access our way, whatever the property file says. */
static unsigned long phybus_reads, phybus_writes, phybus_errors;
static struct nosaic_mdio phybus_accel[MDIO_ACCELS];

/*
 * ⚠ THE PHY ID IS NOT AN MDIO ADDRESS. IT IS A PACKED COORDINATE.
 *
 * port_phy_addr_<n> on this board encodes which accelerator, which of its two
 * buses, and the address on that bus. Reading it as a plain address talks to a
 * real PHY belonging to a different port, which reports that port's link state
 * under this port's name and errors nowhere.
 *
 * Checked against the board's own configuration: port 1 -> 0x001,
 * port 9 -> 0x021, port 48 -> 0x126.
 */
static void phybus_decode(uint32_t phy_id, int *accel, int *bus, int *prtad)
{
	*accel = (int)(((phy_id >> 8) << 1) | ((phy_id >> 6) & 1));
	*bus = (int)((phy_id >> 5) & 1);
	*prtad = (int)(phy_id & 0x1f);
}

/* Clause 45 packs the register as (devad & 0x3f) << 16 | regad. */
static void phybus_split(uint32_t reg, int *devad, int *regad)
{
	*devad = (int)((reg >> 16) & 0x3f);
	*regad = (int)(reg & 0xffff);
}

static int phybus_rd(int unit, uint32 phy_id, uint32 reg, uint16 *data)
{
	int accel, bus, prtad, devad, regad;

	(void)unit;
	phybus_decode(phy_id, &accel, &bus, &prtad);
	phybus_split(reg, &devad, &regad);
	if (phybus_bar == NULL || accel < 0 || accel >= MDIO_ACCELS)
		return -1;
	phybus_reads++;
	if (nosaic_mdio_read(&phybus_accel[accel], bus, prtad, devad,
			     regad, data) != 0) {
		phybus_errors++;
		return -1;
	}
	return 0;
}

static int phybus_wr(int unit, uint32 phy_id, uint32 reg, uint16 data)
{
	int accel, bus, prtad, devad, regad;

	(void)unit;
	phybus_decode(phy_id, &accel, &bus, &prtad);
	phybus_split(reg, &devad, &regad);
	if (phybus_bar == NULL || accel < 0 || accel >= MDIO_ACCELS)
		return -1;
	phybus_writes++;
	if (nosaic_mdio_write(&phybus_accel[accel], bus, prtad, devad,
			      regad, data) != 0) {
		phybus_errors++;
		return -1;
	}
	return 0;
}

void nosaic_phybus_report(void)
{
	printf("phybus: %lu read(s), %lu write(s), %lu failure(s) through the "
	       "copper PHY bus\n", phybus_reads, phybus_writes, phybus_errors);
	if (phybus_reads == 0 && phybus_writes == 0)
		printf("phybus: the SDK never used this bus -- the external PHYs "
		       "were not reached, whatever else reported success\n");
	fflush(stdout);
}

int nosaic_phybus_install(int unit, const char *scd_bdf)
{
	char path[256];
	int fd, i;
	void *map;

	if (scd_bdf == NULL || *scd_bdf == '\0')
		return 0;   /* No controller stated: nothing to install. */

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource0", scd_bdf);
	fd = open(path, O_RDWR | O_SYNC);
	if (fd < 0) {
		printf("phybus: cannot open %s; the copper PHYs are unreachable "
		       "and no copper port will link\n", path);
		return -1;
	}
	{
		struct stat st;

		if (fstat(fd, &st) != 0 || (size_t)st.st_size < SCD_MDIO_END) {
			printf("phybus: %s is %lld bytes, too small for the MDIO "
			       "accelerators at %#x\n", path,
			       (long long)(fstat(fd, &st) == 0 ? st.st_size : 0),
			       MDIO_ACCEL_BASE);
			close(fd);
			return -1;
		}
		map = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE,
			   MAP_SHARED, fd, 0);
	}
	close(fd);
	if (map == MAP_FAILED) {
		printf("phybus: cannot map %s (%s); the copper PHYs are unreachable\n",
		       path, strerror(errno));
		return -1;
	}
	phybus_bar = (volatile uint32_t *)map;

	for (i = 0; i < MDIO_ACCELS; i++) {
		phybus_accel[i].bar = phybus_bar;
		phybus_accel[i].base = MDIO_ACCEL_BASE + (unsigned long)i * MDIO_ACCEL_STRIDE;
		/* 10 MHz, which is what the vendor's own software runs these at. */
		phybus_accel[i].speed = 10;
		phybus_accel[i].req = 0;
	}

	if (phy_i2c_bus_func_hook_set(unit, phybus_rd, phybus_wr) < 0) {
		printf("phybus: the SDK refused the PHY bus hook\n");
		return -1;
	}
	printf("phybus: copper PHY bus installed over the board controller at %s\n",
	       scd_bdf);
	fflush(stdout);
	return 0;
}
