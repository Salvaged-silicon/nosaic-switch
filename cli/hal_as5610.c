/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The Edgecore AS5610-52X's board controller.
 *
 * A CPLD on the P2020's local bus at chip select 1, which the device tree maps
 * to physical 0xea000000 for 256 bytes:
 *
 *   localbus@ff705000 { ranges = <... 0x1 0x0 0x0 0xea000000 0x00000100>; }
 *   cpld@1,0 { compatible = "accton,as5610-52x-cpld"; reg = <0x1 0x0 0x100>; }
 *
 * Byte-wide registers, reached through /dev/mem. No kernel driver: the same
 * argument as the datapath's BDE, and the same mechanism, so this board's
 * hardware access is one style rather than two.
 */

#include "hal.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CPLD_BASE 0xea000000UL

enum {
	REG_VERSION    = 0x00,
	REG_PSU2       = 0x01,   /* yes, 2 before 1 -- see psus() */
	REG_PSU1       = 0x02,
	REG_FAN_STATUS = 0x03,
	REG_FAN_PWM    = 0x0d,
	REG_LED_SYS    = 0x13,
	REG_LED_LOC    = 0x15,
};

/* Duty is five bits. The board powers up at 31 and stays there until something
 * says otherwise, which is safe and about three times the cooling it needs. */
#define PWM_MAX 31

/* Not zero, and not derived from a reading: EdgeNOS idles this same board at
 * 10/31, so the quiet case here is a number that has already run on it. */
#define PWM_FLOOR 10

/*
 * Mapped, not read.
 *
 * /dev/mem serves device memory through mmap and refuses it through read: a
 * pread of this address returns EFAULT while mmap of the same address works,
 * because the kernel's read path goes via xlate_dev_mem_ptr and this is not
 * system RAM. The first version of this used pread and reported that no board
 * controller answered, which was true of the call and false of the hardware.
 *
 * The datapath's BDE maps the switch chip's BAR for the same reason, so this is
 * the same mechanism on the same box rather than a second way of doing it.
 */
static volatile unsigned char *cpld;

static int cpld_open(void)
{
	int fd;
	void *p;

	if (cpld != NULL)
		return 0;

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0)
		return -1;
	/* The base is page-aligned already, so the mapping starts at the
	 * register block and no offset arithmetic is needed. */
	p = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		 (off_t)CPLD_BASE);
	close(fd);
	if (p == MAP_FAILED)
		return -1;
	cpld = p;
	return 0;
}

static int reg_read(unsigned off)
{
	if (cpld_open() != 0)
		return -1;
	return cpld[off];
}

static int reg_write(unsigned off, unsigned char v)
{
	if (cpld_open() != 0)
		return -1;
	cpld[off] = v;
	return 0;
}

/*
 * Probe by asking the controller its version.
 *
 * 0x00 and 0xff are what absent or unprogrammed hardware reads as, and both
 * would otherwise decode into a confident description of a board that is not
 * there.
 */
static int as5610_probe(void)
{
	int v = reg_read(REG_VERSION);

	return (v > 0x00 && v < 0xff) ? 0 : -1;
}

static int as5610_identify(char *buf, size_t len)
{
	int v = reg_read(REG_VERSION);

	if (v < 0)
		return -1;
	snprintf(buf, len, "AS5610-52X, cpld %d.%d", (v >> 4) & 0xf, v & 0xf);
	return 0;
}

/*
 * Temperatures are not the CPLD's. The max6697 on I2C binds from the device
 * tree and publishes seven sensors through hwmon; the controller has none.
 */
static int read_int_file(const char *path, int *out)
{
	FILE *f = fopen(path, "r");
	int ok;

	if (f == NULL)
		return -1;
	ok = fscanf(f, "%d", out) == 1;
	fclose(f);
	return ok ? 0 : -1;
}

static int as5610_temps(struct nosaic_temp *out, int max)
{
	int n = 0, i;

	for (i = 0; i < 8 && n < max; i++) {
		char dir[64];
		int j;

		snprintf(dir, sizeof(dir), "/sys/class/hwmon/hwmon%d", i);
		for (j = 1; j <= 16 && n < max; j++) {
			char p[128];
			int v;

			snprintf(p, sizeof(p), "%s/temp%d_input", dir, j);
			if (read_int_file(p, &v) != 0)
				continue;
			memset(&out[n], 0, sizeof(out[n]));
			out[n].milli_c = v;

			snprintf(p, sizeof(p), "%s/temp%d_label", dir, j);
			{
				FILE *lf = fopen(p, "r");

				if (lf != NULL && fgets(out[n].name,
						       sizeof(out[n].name), lf)) {
					out[n].name[strcspn(out[n].name, "\n")] = '\0';
					fclose(lf);
				} else {
					if (lf != NULL)
						fclose(lf);
					snprintf(out[n].name, sizeof(out[n].name),
						 "temp%d", j);
				}
			}

			snprintf(p, sizeof(p), "%s/temp%d_crit", dir, j);
			if (read_int_file(p, &v) == 0)
				out[n].crit_milli_c = v;
			n++;
		}
	}
	return n;
}

/*
 * Power supplies, decoded the way the hardware actually works rather than the
 * way the register map reads.
 *
 * The first supply's status is in 0x02 and the second's in 0x01 -- two
 * registers, not two fields of one -- and presence is ACTIVE LOW, so bit 0
 * clear means fitted. Read the obvious way, a running switch reports no power
 * supplies at all, which is what EdgeNOS's own kernel driver does; its Python
 * layer overrides that with this map and records that it came from Cumulus.
 *
 * "fitted but not powered" is the normal state of a second supply with nothing
 * plugged into it, so it is reported rather than called a fault.
 */
static int as5610_psus(struct nosaic_psu *out, int max)
{
	static const unsigned regs[2] = { REG_PSU1, REG_PSU2 };
	int i, n = 0;

	for (i = 0; i < 2 && n < max; i++) {
		int v = reg_read(regs[i]);

		if (v < 0)
			continue;
		out[n].index = i + 1;
		out[n].fitted = (v & 0x01) == 0;
		out[n].powered = (v & 0x02) != 0;
		snprintf(out[n].raw, sizeof(out[n].raw), "reg 0x%02x", v);
		n++;
	}
	return n;
}

static int as5610_fan_get(void)
{
	int v = reg_read(REG_FAN_PWM);

	if (v < 0)
		return -1;
	return (v & PWM_MAX) * 100 / PWM_MAX;
}

static int as5610_fan_set(int percent)
{
	int raw, got;

	if (percent > 100)
		percent = 100;
	raw = percent * PWM_MAX / 100;
	if (raw < PWM_FLOOR)
		raw = PWM_FLOOR;
	if (raw > PWM_MAX)
		raw = PWM_MAX;

	if (reg_write(REG_FAN_PWM, (unsigned char)raw) != 0)
		return -1;

	/* Read it back. This register is write-and-hope otherwise, and a duty
	 * that did not land is the failure that ends with a hot switch. */
	got = reg_read(REG_FAN_PWM);
	if (got < 0 || (got & PWM_MAX) != raw)
		return -1;
	return 0;
}

static int as5610_fan_floor(void)
{
	return PWM_FLOOR * 100 / PWM_MAX;
}

static int as5610_leds(char *buf, size_t len)
{
	int s = reg_read(REG_LED_SYS), l = reg_read(REG_LED_LOC);

	if (s < 0 || l < 0)
		return -1;
	/* Raw. Both registers are known and what their bits mean is not, so
	 * this reports them and does not offer to write one. */
	snprintf(buf, len, "sys %#04x  locator %#04x", s, l);
	return 0;
}

const struct nosaic_hal nosaic_hal_as5610 = {
	.name      = "as5610-cpld",
	.probe     = as5610_probe,
	.identify  = as5610_identify,
	.temps     = as5610_temps,
	.psus      = as5610_psus,
	.fan_get   = as5610_fan_get,
	.fan_set   = as5610_fan_set,
	.fan_floor = as5610_fan_floor,
	.leds      = as5610_leds,
};

static const struct nosaic_hal *all[] = { &nosaic_hal_as5610, NULL };

const struct nosaic_hal *nosaic_hal_find(void)
{
	int i;

	for (i = 0; all[i] != NULL; i++) {
		if (all[i]->probe == NULL || all[i]->probe() == 0)
			return all[i];
	}
	return NULL;
}
