/* SPDX-License-Identifier: Apache-2.0 */
/*
 * FM6000 register access over a sysfs mapping of BAR0. See pci.h for why this
 * is guarded rather than thin.
 *
 * BAR0 is mapped through /sys/bus/pci/devices/<slot>/resource0 rather than
 * /dev/mem. The Broadcom boards in this tree use /dev/mem and need
 * `iomem=relaxed` on the kernel command line to be allowed to; sysfs needs
 * neither, bounds the mapping to the device's own BAR, and goes away when the
 * device does. There is no reason to carry the older approach onto a new board.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mmio.h"
#include "pci.h"
#include "regs.h"

#define PCI_DEVICES "/sys/bus/pci/devices"

static int read_sysfs_hex(const char *slot, const char *file, unsigned long *out)
{
	char path[256], buf[64];
	int fd;
	ssize_t n;

	snprintf(path, sizeof(path), "%s/%s/%s", PCI_DEVICES, slot, file);
	if ((fd = open(path, O_RDONLY)) < 0)
		return FM_ERR;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return FM_ERR;
	buf[n] = '\0';
	*out = strtoul(buf, NULL, 0);
	return FM_OK;
}

/* Find the first 8086:155b on the bus. */
static int find_slot(char *slot, size_t len)
{
	DIR *d = opendir(PCI_DEVICES);
	struct dirent *e;
	int found = FM_ERR;

	if (!d)
		return FM_ERR;
	while ((e = readdir(d)) != NULL) {
		unsigned long ven = 0, dev = 0;

		if (e->d_name[0] == '.')
			continue;
		/* A PCI address is "0000:02:00.0" -- twelve characters. Anything
		 * longer is not one, so this rejects rather than truncates: a
		 * truncated slot name would open the wrong device's sysfs. */
		if (strlen(e->d_name) >= len)
			continue;
		if (read_sysfs_hex(e->d_name, "vendor", &ven) != FM_OK)
			continue;
		if (read_sysfs_hex(e->d_name, "device", &dev) != FM_OK)
			continue;
		if (ven == FM6000_PCI_VENDOR && dev == FM6000_PCI_DEVICE) {
			snprintf(slot, len, "%s", e->d_name);
			found = FM_OK;
			break;
		}
	}
	closedir(d);
	return found;
}

/* Make sure the device's memory space is decoded. With no driver bound nothing
 * else will have done it, and a BAR that is assigned but not enabled maps
 * cleanly and reads as ones -- which is indistinguishable from the chip being
 * off the bus, and would send a reader hunting the wrong fault. */
static void enable_device(const char *slot)
{
	char path[256];
	int fd;

	snprintf(path, sizeof(path), "%s/%s/enable", PCI_DEVICES, slot);
	if ((fd = open(path, O_WRONLY)) < 0)
		return;
	if (write(fd, "1\n", 2) < 0)
		/* Already enabled is the common case and is not a failure. */
		(void)0;
	close(fd);
}

int fm_open(struct fm6000 *d, const char *slot)
{
	char path[256];
	struct stat st;

	memset(d, 0, sizeof(*d));
	d->bar_fd = -1;
	d->check_writes = 1;

	if (slot != NULL) {
		if (strlen(slot) >= sizeof(d->slot))
			return FM_ERR;
		snprintf(d->slot, sizeof(d->slot), "%s", slot);
	} else if (find_slot(d->slot, sizeof(d->slot)) != FM_OK) {
		return FM_ERR;
	}

	enable_device(d->slot);

	snprintf(path, sizeof(path), "%s/%s/resource0", PCI_DEVICES, d->slot);
	if ((d->bar_fd = open(path, O_RDWR | O_SYNC)) < 0)
		return FM_ERR;
	if (fstat(d->bar_fd, &st) != 0 || st.st_size == 0) {
		close(d->bar_fd);
		d->bar_fd = -1;
		return FM_ERR;
	}
	d->bar_bytes = (size_t)st.st_size;
	d->bar0 = mmap(NULL, d->bar_bytes, PROT_READ | PROT_WRITE, MAP_SHARED,
		       d->bar_fd, 0);
	if (d->bar0 == MAP_FAILED) {
		d->bar0 = NULL;
		close(d->bar_fd);
		d->bar_fd = -1;
		return FM_ERR;
	}
	return FM_OK;
}

void fm_close(struct fm6000 *d)
{
	if (d->bar0 != NULL)
		munmap((void *)d->bar0, d->bar_bytes);
	if (d->bar_fd >= 0)
		close(d->bar_fd);
	d->bar0 = NULL;
	d->bar_fd = -1;
}

int fm_check_offbus(struct fm6000 *d)
{
	char path[256];
	unsigned char cfg[4];
	int fd;
	ssize_t n;

	snprintf(path, sizeof(path), "%s/%s/config", PCI_DEVICES, d->slot);
	if ((fd = open(path, O_RDONLY)) < 0)
		return FM_ERR;
	n = read(fd, cfg, sizeof(cfg));
	close(fd);
	if (n != (ssize_t)sizeof(cfg))
		return FM_ERR;

	/* A live endpoint answers its vendor ID. A chip that has gone fatal
	 * answers 0xffff here as well as in the BAR, and that is the only way
	 * to tell the two apart from software. */
	if (cfg[0] == 0xff && cfg[1] == 0xff) {
		d->offbus = 1;
		return 1;
	}
	return 0;
}

int fm_is_bank(uint32_t word)
{
	static const uint32_t base[] = {
		FM6000_BANK_STATS_BASE,
		FM6000_BANK_MCAST_MID_BASE,
		FM6000_BANK_MCAST_POST_BASE,
	};
	size_t i;

	for (i = 0; i < sizeof(base) / sizeof(base[0]); i++)
		if (word >= base[i] && word < base[i] + FM6000_BANK_SPAN)
			return 1;
	return 0;
}

const char *fm_block_name(uint32_t word)
{
	if (word >= FM6000_BLK_MGMT && word < FM6000_BLK_MGMT + 0x1000)
		return "MGMT";
	if (word >= FM6000_BLK_CRM && word < FM6000_BLK_CRM + 0x1000)
		return "CRM";
	if (word >= FM6000_BLK_EPL && word < FM6000_BLK_EPL + 0x2000)
		return "EPL";
	if (word >= FM6000_BLK_CM && word < FM6000_BLK_CM + 0x40000)
		return "CM";
	if (word >= FM6000_BLK_MOD && word <= FM6000_BLK_MOD_END)
		return "MOD";
	if (word >= FM6000_BLK_L2F && word < FM6000_BLK_L2F + 0x20000)
		return "L2F";
	if (word >= FM6000_BANK_STATS_BASE &&
	    word < FM6000_BANK_STATS_BASE + FM6000_BANK_SPAN)
		return "STATS (bank)";
	if (word >= FM6000_BANK_MCAST_MID_BASE &&
	    word < FM6000_BANK_MCAST_MID_BASE + FM6000_BANK_SPAN)
		return "MCAST_MID (bank)";
	if (word >= FM6000_BANK_MCAST_POST_BASE &&
	    word < FM6000_BANK_MCAST_POST_BASE + FM6000_BANK_SPAN)
		return "MCAST_POST (bank)";
	return "?";
}

const char *fm_hazard(const struct fm6000 *d, uint32_t word)
{
	if (d->banks_ready)
		return NULL;
	if (fm_is_bank(word))
		return "ECC bank memory, uninitialised: one access takes the "
		       "chip off the PCIe bus";
	if (word == FM6000_ESCHED_READ_HAZARD)
		return "ESCHED 0x2000: READING this off-buses a cold chip";
	return NULL;
}

void fm_bank_mark_initialised(struct fm6000 *d)
{
	d->banks_ready = 1;
}

void fm_set_write_check(struct fm6000 *d, int on)
{
	d->check_writes = on ? 1 : 0;
}

/* Shared preamble for both accessors: everything that can refuse before the
 * access happens, in the order that costs least. */
static int guard(struct fm6000 *d, uint32_t word, size_t byte_off)
{
	if (d->bar0 == NULL)
		return FM_ERR;
	if (d->offbus) {
		d->refused++;
		return FM_EOFFBUS;
	}
	if (byte_off + 4 > d->bar_bytes)
		return FM_ERR;
	if (fm_hazard(d, word) != NULL) {
		d->refused++;
		return FM_EUNSAFE;
	}
	return FM_OK;
}

int fm_rd_byte(struct fm6000 *d, uint32_t off, uint32_t *out)
{
	uint32_t v;
	int rv = guard(d, off / 4, off);

	if (rv != FM_OK)
		return rv;

	v = nosaic_mmio_rd32((const void *)((const char *)d->bar0 + off));
	d->reads++;

	/*
	 * All ones is the signature of a chip that has left the bus -- and it
	 * is also a legitimate value for plenty of registers, so it is a
	 * suspicion rather than a verdict. Confirm against config space, which
	 * is the only place the two cases differ.
	 *
	 * The confirmation costs a sysfs read, which is why it happens on the
	 * suspicious value and not on every access.
	 */
	if (v == 0xffffffff && fm_check_offbus(d) == 1)
		return FM_EOFFBUS;

	*out = v;
	return FM_OK;
}

int fm_wr_byte(struct fm6000 *d, uint32_t off, uint32_t val)
{
	int rv = guard(d, off / 4, off);

	if (rv != FM_OK)
		return rv;

	nosaic_mmio_wr32((void *)((char *)d->bar0 + off), val);
	nosaic_mmio_barrier();
	d->writes++;

	/*
	 * A write cannot report failure, and on this chip a write is exactly
	 * what takes it off the bus -- so this is where the damage is caught,
	 * at the write that did it rather than at some unrelated read later.
	 *
	 * A bulk writer that has turned this off owes a fm_check_offbus() when
	 * its burst ends; see fm_set_write_check().
	 */
	if (d->check_writes && fm_check_offbus(d) == 1)
		return FM_EOFFBUS;
	return FM_OK;
}

int fm_rd(struct fm6000 *d, uint32_t word, uint32_t *out)
{
	if (word > FM6000_WORD_MAX)
		return FM_ERR;
	return fm_rd_byte(d, word * 4, out);
}

int fm_wr(struct fm6000 *d, uint32_t word, uint32_t val)
{
	if (word > FM6000_WORD_MAX)
		return FM_ERR;
	return fm_wr_byte(d, word * 4, val);
}
