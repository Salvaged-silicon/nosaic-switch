/* SPDX-License-Identifier: Apache-2.0 */
/*
 * FM6000 register access over a sysfs mapping of BAR0. See pci.h for why this
 * is guarded rather than thin.
 *
 * fm_attach_pcie (the short-lived esched-init path) maps BAR0 via /dev/mem
 * rather than sysfs resource0. The sysfs resource handler invokes
 * pci_enable_device(), which can deadlock with the kernel's PCIe AER error
 * handler; using /dev/mem with the physical address from the sysfs resource
 * file bypasses that path entirely. Confirmed on the 7150S-52: the resource0
 * sysfs file hangs on open while the /dev/mem mapping of the same address
 * works without blocking.
 *
 * The Broadcom boards in this tree use /dev/mem because the SCD's BAR1 is
 * not a standard PCIe BAR (it is not in sysfs as resource1 for the right
 * device). The FM6000 BAR0 IS in sysfs, but we still use /dev/mem for the
 * reason above.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/io.h>
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

/* Find the first device with this vendor:device on the bus. */
static int find_slot_id(char *slot, size_t len, unsigned long want_ven,
			unsigned long want_dev)
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
		if (ven == want_ven && dev == want_dev) {
			snprintf(slot, len, "%s", e->d_name);
			found = FM_OK;
			break;
		}
	}
	closedir(d);
	return found;
}

/* Find the FM6000 native PCIe endpoint (if it has one; absent on the 7150S). */
static int find_slot(char *slot, size_t len)
{
	return find_slot_id(slot, len, FM6000_PCI_VENDOR, FM6000_PCI_DEVICE);
}

/* Find the SCD (PCIe-to-LocalBus bridge, 0x3475:0x0001).  BAR1 is the FM6000
 * LocalBus (chip management registers).  BAR0 holds SCD reset/control registers,
 * NOT the FM6000 DMA engine (those are in the FM6000 native endpoint, 0x8086:0x155b). */
static int find_scd_slot(char *slot, size_t len)
{
	return find_slot_id(slot, len, 0x3475, 0x0001);
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

static int map_bar(struct fm6000 *d, const char *resource)
{
	char path[256];
	struct stat st;

	snprintf(path, sizeof(path), "%s/%s/%s", PCI_DEVICES, d->slot, resource);
	if ((d->bar_fd = open(path, O_RDWR | O_SYNC)) < 0)
		return FM_ERR;
	if (fstat(d->bar_fd, &st) != 0 || st.st_size == 0) {
		close(d->bar_fd);
		d->bar_fd = -1;
		return FM_ERR;
	}
	d->bar_bytes = (size_t)st.st_size;
	d->regs = mmap(NULL, d->bar_bytes, PROT_READ | PROT_WRITE, MAP_SHARED,
		       d->bar_fd, 0);
	if (d->regs == MAP_FAILED) {
		d->regs = NULL;
		close(d->bar_fd);
		d->bar_fd = -1;
		return FM_ERR;
	}
	return FM_OK;
}

int fm_open(struct fm6000 *d, const char *slot)
{
	memset(d, 0, sizeof(*d));
	d->bar_fd = -1;
	d->pcie_bar_fd = -1;
	d->check_writes = 1;
	d->xport = FM_XPORT_PCIE;

	if (slot != NULL) {
		if (strlen(slot) >= sizeof(d->slot))
			return FM_ERR;
		snprintf(d->slot, sizeof(d->slot), "%s", slot);
	} else if (find_slot(d->slot, sizeof(d->slot)) != FM_OK) {
		return FM_ERR;
	}

	return map_bar(d, "resource0");
}

/*
 * The SCD, as a PCI-to-LocalBus bridge. Its BAR1 is the FM6000's register
 * space; see the note at the top of pci.h for why that is the bring-up path.
 */
#define SCD_VENDOR 0x3475
#define SCD_DEVICE 0x0001

int fm_open_lbus(struct fm6000 *d, const char *scd_slot)
{
	char slot[FM_SLOT_LEN];
	char path[256];
	uint16_t cmd = 0x0006;  /* Memory Space + Bus Master */
	int fd;

	if (scd_slot != NULL) {
		if (strlen(scd_slot) >= sizeof(slot))
			return FM_ERR;
		snprintf(slot, sizeof(slot), "%s", scd_slot);
	} else if (find_slot_id(slot, sizeof(slot),
				SCD_VENDOR, SCD_DEVICE) != FM_OK) {
		return FM_ERR;
	}

	/* Enable Memory Space in the command register via the config file.
	 * Writing to the sysfs "enable" file calls pci_enable_device() which
	 * acquires the AER port lock -- see the comment above
	 * fm_reopen_lbus_devmem for why that deadlocks. Directly writing the
	 * command register at offset 4 of the config file bypasses that path. */
	snprintf(path, sizeof(path), "%s/%s/config", PCI_DEVICES, slot);
	if ((fd = open(path, O_RDWR)) >= 0) {
		(void)pwrite(fd, &cmd, 2, 4);
		close(fd);
	}

	/* Map BAR1 via /dev/mem. Opening resource1 also triggers
	 * pci_enable_device() via the kernel's resource mmap handler; /dev/mem
	 * bypasses that path too. */
	return fm_reopen_lbus_devmem(d, slot);
}

/*
 * Open or reopen the LBUS mapping via /dev/mem.
 *
 * Both the sysfs "enable" file and the sysfs "resource1" mmap path call
 * pci_enable_device() inside the kernel. That function acquires the AER port
 * driver lock, which the AER handler also holds when it fires.  Concurrency
 * between a pci_enable_device() call and the AER handler (which fires on any
 * PCIe error, including those caused by ESCHED accessing the FM6000 before
 * bring-up is complete) produces a deadlock: nosd hangs, management IP never
 * appears, only physical link survives.
 *
 * Confirmed on the 7150S-52:
 *   - resource0 sysfs mmap hangs on open; /dev/mem mapping of the same
 *     physical address works (esched-init path, 2026-10-13)
 *   - resource1 sysfs mmap and the "enable" file hang during LBUS reopen
 *     after PCIe SerDes init; devmem works (boot.c step 9, 2026-10-14)
 *   - "enable" and "resource1" also deadlock during the INITIAL fm_open_lbus()
 *     call from main.c on a cold boot (after PDU cycle), because ESCHED init
 *     fires AER events before nosd even starts (2026-10-15)
 *
 * The fix for all three cases: read BAR1's physical address from the resource
 * text file (safe: read-only, no lock), enable Memory Space in the command
 * register by writing directly to the config file at offset 4 (safe: PCI
 * config space writes do not go through pci_enable_device and do not acquire
 * the AER lock), then map the physical address via /dev/mem (safe: no
 * pci_enable_device).
 *
 * fm_open_lbus() uses this function for its initial open; boot.c's step 9
 * calls it directly for the reopen after PCIe SerDes init.
 */
int fm_reopen_lbus_devmem(struct fm6000 *d, const char *scd_slot)
{
	char path[256];
	FILE *rf;
	unsigned long bar_start = 0, bar_end = 0, bar_flags = 0;
	unsigned long dummy_s, dummy_e, dummy_f;
	void *m;
	int fd;

	if (scd_slot == NULL || strlen(scd_slot) >= sizeof(d->slot))
		return FM_ERR;

	/* BAR1 is the second line in the resource text file. */
	snprintf(path, sizeof(path), "%s/%s/resource", PCI_DEVICES, scd_slot);
	if ((rf = fopen(path, "r")) == NULL)
		return FM_ERR;
	/* Skip line 1 (BAR0). resource file uses hex: "0xSTART 0xEND 0xFLAGS\n" */
	if (fscanf(rf, "0x%lx 0x%lx 0x%lx\n", &dummy_s, &dummy_e, &dummy_f) != 3) {
		fclose(rf);
		return FM_ERR;
	}
	/* Read line 2 (BAR1 = LBUS window). */
	if (fscanf(rf, "0x%lx 0x%lx 0x%lx", &bar_start, &bar_end, &bar_flags) != 3
	    || bar_start == 0 || bar_end <= bar_start) {
		fclose(rf);
		return FM_ERR;
	}
	fclose(rf);

	memset(d, 0, sizeof(*d));
	d->bar_fd      = -1;
	d->pcie_bar_fd = -1;
	d->check_writes = 1;
	d->xport       = FM_XPORT_LBUS;
	snprintf(d->slot, sizeof(d->slot), "%s", scd_slot);

	if ((fd = open("/dev/mem", O_RDWR | O_SYNC)) < 0)
		return FM_ERR;

	m = mmap(NULL, (size_t)(bar_end - bar_start + 1),
		 PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)bar_start);
	if (m == MAP_FAILED) {
		close(fd);
		return FM_ERR;
	}

	d->regs      = (volatile uint32_t *)m;
	d->bar_bytes = (size_t)(bar_end - bar_start + 1);
	d->bar_fd    = fd;
	return FM_OK;
}

int fm_open_devmem(struct fm6000 *d, uintptr_t phys, size_t bytes)
{
	void *map;

	memset(d, 0, sizeof(*d));
	d->bar_fd = -1;
	d->pcie_bar_fd = -1;
	d->check_writes = 1;
	d->xport = FM_XPORT_DEVMEM;
	snprintf(d->slot, sizeof(d->slot), "devmem");

	d->bar_fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (d->bar_fd < 0) {
		perror("/dev/mem");
		d->bar_fd = -1;
		return FM_ERR;
	}

	map = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED,
		   d->bar_fd, (off_t)phys);
	if (map == MAP_FAILED) {
		perror("/dev/mem: mmap");
		close(d->bar_fd);
		d->bar_fd = -1;
		return FM_ERR;
	}

	d->regs = (volatile uint32_t *)map;
	d->bar_bytes = bytes;
	return FM_OK;
}

/*
 * Pulse the FM6000's hardware reset via the SCD's BAR0 reset registers.
 *
 * On a 7150S-52, the SCD holds the chip in reset at power-on. A cold chip --
 * or one that has self-reset after a fatal ECC error -- needs a PULSE, not
 * just a release: assert the reset bits, wait briefly, then deassert them.
 * With them merely left deasserted (as they are from power-on) the chip's
 * PIN_STRAP reads 0 and every register returns 0.
 *
 * This opens SCD BAR0 separately for the writes and closes it immediately
 * after; the caller's fm6000 struct is not involved beyond supplying the
 * SCD slot name and transport type. After a successful call, wait ~100 ms
 * for the chip to come up, then call fm_clear_offbus() before trying again.
 *
 * SCD BAR0 register offsets (byte):
 *   +0x4000  resetSet   -- write bits to assert those resets
 *   +0x4010  resetClear -- write bits to deassert those resets
 * Bits 1, 2, 8 (mask 0x106) are the FM6000 reset lines.
 */
int fm_scd_reset_pulse(struct fm6000 *d)
{
	char path[256];
	struct stat st;
	volatile uint32_t *bar0;
	int fd, rv = FM_ERR;

	if (d->xport != FM_XPORT_LBUS)
		return FM_ERR;

	snprintf(path, sizeof(path), "%s/%s/resource0", PCI_DEVICES, d->slot);
	if ((fd = open(path, O_RDWR | O_SYNC)) < 0)
		return FM_ERR;
	if (fstat(fd, &st) != 0 || (size_t)st.st_size < 0x4014)
		goto out;
	bar0 = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED,
		    fd, 0);
	if (bar0 == MAP_FAILED)
		goto out;

	/* resetSet = BAR0 byte 0x4000 = word index 0x1000 */
	nosaic_mmio_wr32(bar0 + 0x1000, 0x106u);
	{ struct timespec ts = { 0, 10000000L }; nanosleep(&ts, NULL); }
	/* resetClear = BAR0 byte 0x4010 = word index 0x1004 */
	nosaic_mmio_wr32(bar0 + 0x1004, 0x106u);

	munmap((void *)bar0, (size_t)st.st_size);
	rv = FM_OK;
out:
	close(fd);
	return rv;
}

/*
 * Same reset pulse, but via /dev/mem at a known SCD BAR0 physical address.
 * Use when the SCD is not enumerated in sysfs (PCIe2 has no kernel driver)
 * but Aboot has left the outbound ATMU window intact at bar0_phys.
 *
 * On a 7150S-52, SCD BAR0 is at physical 0xe1000000 (256 KiB, word-addressed).
 */
int fm_scd_reset_pulse_devmem(uintptr_t bar0_phys)
{
	volatile uint32_t *bar0;
	int fd, rv = FM_ERR;

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0)
		return FM_ERR;
	bar0 = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED,
		    fd, (off_t)bar0_phys);
	if (bar0 == MAP_FAILED)
		goto out;

	/* resetSet = BAR0 byte 0x4000 = word index 0x1000 */
	nosaic_mmio_wr32(bar0 + 0x1000, 0x106u);
	{ struct timespec ts = { 0, 10000000L }; nanosleep(&ts, NULL); }
	/* resetClear = BAR0 byte 0x4010 = word index 0x1004 */
	nosaic_mmio_wr32(bar0 + 0x1004, 0x106u);

	munmap((void *)bar0, 0x10000);
	rv = FM_OK;
out:
	close(fd);
	return rv;
}

void fm_close(struct fm6000 *d)
{
	if (d->regs != NULL)
		munmap((void *)d->regs, d->bar_bytes);
	if (d->bar_fd >= 0)
		close(d->bar_fd);
	d->regs = NULL;
	d->bar_fd = -1;
	if (d->pcie_regs != NULL)
		munmap((void *)d->pcie_regs, d->pcie_bar_bytes);
	if (d->pcie_bar_fd >= 0)
		close(d->pcie_bar_fd);
	d->pcie_regs = NULL;
	d->pcie_bar_fd = -1;
}

int fm_attach_pcie(struct fm6000 *d)
{
	char slot[FM_SLOT_LEN];
	char path[256];
	volatile uint32_t *m;
	int fd;
	FILE *rf;
	unsigned long bar_start = 0, bar_end = 0, bar_flags = 0;
	uint16_t cmd = 0x0406; /* Memory Space + Bus Master + Interrupt Disable */

	if (d->pcie_regs != NULL)
		return FM_OK;

	/*
	 * DMA registers live in the FM6000's own PCIe endpoint (vendor 0x8086
	 * device 0x155b on the 7150S-52), at BAR0+0x5000.  This endpoint is NOT
	 * the SCD; the SCD (0x3475:0x0001) bridges the FM6000 LocalBus for chip
	 * management registers (d->regs).
	 *
	 * Memory Space is disabled (command=0x0000) after the PCIe rescan in
	 * boot step 9.  Enable it here so that subsequent writes to d->pcie_regs
	 * reach the FM6000 DMA engine rather than being silently dropped by the
	 * PCIe fabric.  Writing the config register does not call
	 * pci_enable_device() and is safe during PCIe link training.
	 */
	if (find_slot(slot, sizeof(slot)) != FM_OK) {
		/*
		 * install203: Device not in sysfs (removed after the initial
		 * boot rescan to quiesce pciehp).  Use the BAR0 physical address
		 * cached by the first successful sysfs-path attach.
		 *
		 * After a link bounce the FM6000 clears its BAR registers, so we
		 * must reprogram them.  We use CF8/CFC portio posted writes (bus=2
		 * posted config writes complete immediately — no CTO stall risk —
		 * confirmed install172).  DLLLA=1 is a precondition guaranteed by
		 * fm_wait_link_active() or fm_pcie_enable_portio() called before
		 * this function, so the SMM timer sees a stable chip on bus=2 and
		 * exits normally.  [install203, 2026-10-29]
		 */
		if (d->pcie_bar_phys == 0 || d->pcie_bar_bytes == 0)
			return FM_ERR;
		bar_start = d->pcie_bar_phys;
		if (iopl(3) == 0) {
			uint32_t bar_lo = (uint32_t)(bar_start & 0xFFFFFFF0UL) | 0x4u;
			uint32_t bar_hi = (uint32_t)(bar_start >> 32);
			/* FM6000 at bus=2, dev=0, fn=0 */
			outl(0x80020010u, 0xCF8); outl(bar_lo, 0xCFC); /* BAR0 lo */
			outl(0x80020014u, 0xCF8); outl(bar_hi, 0xCFC); /* BAR0 hi */
			outl(0x80020004u, 0xCF8); outw(cmd,    0xCFC); /* Command */
			iopl(0);
			fprintf(stderr,
				"nosd-fm6000: BAR0 portio-restored: "
				"lo=0x%08x hi=0x%08x [install203]\n",
				bar_lo, bar_hi);
		}
		fd = open("/dev/mem", O_RDWR | O_SYNC);
		if (fd < 0)
			return FM_ERR;
		m = mmap(NULL, d->pcie_bar_bytes, PROT_READ | PROT_WRITE,
			 MAP_SHARED, fd, (off_t)bar_start);
		if (m == MAP_FAILED) { close(fd); return FM_ERR; }
		d->pcie_regs   = m;
		d->pcie_bar_fd = fd;
		return FM_OK;
	}

	snprintf(path, sizeof(path), "%s/%s/config", PCI_DEVICES, slot);
	if ((fd = open(path, O_RDWR)) >= 0) {
		(void)pwrite(fd, &cmd, 2, 4);
		close(fd);
	}

	/*
	 * Read the physical BAR0 address from the sysfs resource file (first
	 * line: start end flags). Then map via /dev/mem rather than resource0.
	 *
	 * The sysfs resource0 open path calls pci_enable_device(), which can
	 * deadlock with the kernel's PCIe AER handler on the 7150S-52. Using
	 * /dev/mem with the physical BAR address avoids that path without
	 * sacrificing safety: the FM6000 BAR is in a reserved MMIO region that
	 * no other device uses, so bounds protection from sysfs is not needed.
	 */
	snprintf(path, sizeof(path), "%s/%s/resource", PCI_DEVICES, slot);
	if ((rf = fopen(path, "r")) == NULL)
		return FM_ERR;
	if (fscanf(rf, "0x%lx 0x%lx 0x%lx", &bar_start, &bar_end, &bar_flags) != 3
	    || bar_start == 0 || bar_end <= bar_start) {
		fclose(rf);
		return FM_ERR;
	}
	fclose(rf);
	d->pcie_bar_phys = bar_start;  /* cache for no-sysfs reattach [install203] */

	/*
	 * The PCIe link bounce (NEXT_CHAIN SerDes reset during boot step 9)
	 * resets the FM6000 endpoint's config space, clearing BAR0 to its
	 * default (all-zeros base, type bits only).  The kernel remembers the
	 * BIOS-assigned address but does not reprogram the BAR after the bounce.
	 * Writes to the kernel-assigned BAR address (0xe2000000) are then
	 * Unsupported Requests because the chip is not decoding that range.
	 *
	 * Reprogram BAR0 (64-bit) with the kernel-assigned address before
	 * mapping it, so the FM6000 endpoint responds to the correct range.
	 */
	{
		char cfgpath[256];
		int cfgfd;
		uint32_t bar_lo = (uint32_t)(bar_start & 0xFFFFFFF0UL) | 0x4u; /* 64-bit type */
		uint32_t bar_hi = (uint32_t)(bar_start >> 32);

		snprintf(cfgpath, sizeof(cfgpath), "%s/%s/config", PCI_DEVICES, slot);
		cfgfd = open(cfgpath, O_RDWR);
		if (cfgfd >= 0) {
			(void)pwrite(cfgfd, &bar_lo, 4, 0x10); /* BAR0 lower */
			(void)pwrite(cfgfd, &bar_hi, 4, 0x14); /* BAR0 upper */
			close(cfgfd);
			fprintf(stderr, "nosd-fm6000: BAR0 restored: lo=0x%08x hi=0x%08x\n",
				bar_lo, bar_hi);
		}
	}

	if ((fd = open("/dev/mem", O_RDWR | O_SYNC)) < 0)
		return FM_ERR;

	m = mmap(NULL, (size_t)(bar_end - bar_start + 1),
		 PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)bar_start);
	if (m == MAP_FAILED) {
		close(fd);
		return FM_ERR;
	}
	d->pcie_regs      = m;
	d->pcie_bar_bytes = (size_t)(bar_end - bar_start + 1);
	d->pcie_bar_fd    = fd;
	return FM_OK;
}

void fm_detach_pcie(struct fm6000 *d)
{
	if (d->pcie_regs != NULL)
		munmap((void *)d->pcie_regs, d->pcie_bar_bytes);
	if (d->pcie_bar_fd >= 0)
		close(d->pcie_bar_fd);
	d->pcie_regs = NULL;
	d->pcie_bar_fd = -1;
}

/*
 * install204: Reprogram FM6000 BAR0 via CF8 portio using the cached physical
 * address in d->pcie_bar_phys.  After device removal from sysfs the FM6000
 * self-resets clear the BAR register; this restores it so that the existing
 * /dev/mem mapping (d->pcie_regs) lands on the correct MMIO window without
 * a detach/reattach cycle.  Call ONLY after DLLLA=1 is confirmed.
 *
 * Returns 0 on success, -1 if the cache is empty or iopl(3) fails.
 * [install204, 2026-10-29]
 */
int fm_reprogram_bar_portio(struct fm6000 *d)
{
	uint32_t bar_lo, bar_hi;

	if (d->pcie_bar_phys == 0)
		return -1;
	if (iopl(3) != 0)
		return -1;
	bar_lo = (uint32_t)(d->pcie_bar_phys & 0xFFFFFFF0UL) | 0x4u; /* 64-bit */
	bar_hi = (uint32_t)(d->pcie_bar_phys >> 32);
	/* FM6000 PCIe endpoint: bus=2, dev=0, fn=0 */
	outl(0x80020010u, 0xCF8); outl(bar_lo, 0xCFC); /* BAR0 lower */
	outl(0x80020014u, 0xCF8); outl(bar_hi, 0xCFC); /* BAR0 upper */
	iopl(0);
	return 0;
}

/*
 * Reprogram BAR0 and Command (MemSp + BusMaster + IntDis) in the FM6000's
 * PCIe config space via CF8/CFC portio.  Used after ESCHED FALTALs clear
 * config space; SERR# must be disabled when called (posted config writes
 * complete immediately per install172, no SMM trigger).  [install219]
 */
int fm_reprogram_bar_and_cmd_portio(struct fm6000 *d)
{
	uint32_t bar_lo, bar_hi;
	uint16_t cmd = 0x0406; /* MemSp + BusMaster + IntDis */

	if (d->pcie_bar_phys == 0)
		return -1;
	if (iopl(3) != 0)
		return -1;
	bar_lo = (uint32_t)(d->pcie_bar_phys & 0xFFFFFFF0UL) | 0x4u;
	bar_hi = (uint32_t)(d->pcie_bar_phys >> 32);
	outl(0x80020010u, 0xCF8); outl(bar_lo, 0xCFC); /* BAR0 lo */
	outl(0x80020014u, 0xCF8); outl(bar_hi, 0xCFC); /* BAR0 hi */
	outl(0x80020004u, 0xCF8); outw(cmd,    0xCFC); /* Command */
	iopl(0);
	return 0;
}

/*
 * Poll the FM6000's PCIe config-space vendor ID until the chip is alive
 * or max_ms elapses.  Config-space reads are handled by the root complex and
 * return 0xffff quickly (a few ms) when the chip is off-bus; they do NOT go
 * through the SCD LocalBus bridge, so they are safe to call even when LBUS
 * reads would stall indefinitely.  Returns FM_OK when the vendor ID is valid,
 * FM_ERR if the chip is still off-bus at timeout.
 */
int fm_wait_pcie_alive(int max_ms)
{
	char slot[FM_SLOT_LEN];
	char path[256];
	int i, n_polls, n_stable = 0;

	/*
	 * Require STABLE consecutive successes before declaring the chip alive.
	 * ESCHED PCIe writes are posted (no mfence): the FM6000 self-resets
	 * asynchronously AFTER fm_esched_init() returns as it processes each
	 * write.  A single successful poll may catch the chip between resets.
	 * Requiring 5 polls with 50ms spacing (250ms stable window) ensures
	 * all post-ESCHED self-resets have finished before ring_init starts.
	 *
	 * If the FM6000 is not yet in sysfs (device was removed, pciehp has
	 * not yet enumerated it), retry find_slot in the same poll loop rather
	 * than returning FM_ERR immediately.  This allows the caller to call
	 * fm_wait_pcie_alive right after re-enabling the upstream link without
	 * an explicit sleep to wait for the sysfs entry to appear.
	 */
#define FM_ALIVE_STABLE  5    /* required consecutive successes */
#define FM_ALIVE_POLL_US 50000 /* 50 ms between polls */

	n_polls = max_ms / (FM_ALIVE_POLL_US / 1000) + 1;
	slot[0] = '\0';
	path[0] = '\0';

	for (i = 0; i < n_polls; i++) {
		unsigned char cfg[2] = { 0xff, 0xff };
		int fd;

		/* Re-scan sysfs for the endpoint on each iteration until found.
		 * pciehp may be silenced (fm6000_kdrv suppresses slot events), so
		 * we also retry the PCIe rescan every 2 s to add the device. */
		if (slot[0] == '\0') {
			if (find_slot(slot, sizeof(slot)) != FM_OK) {
				/* Retry rescan every ~2 s (40 × 50 ms polls). */
				if (i % 40 == 0) {
					int rfd = open(
					    "/sys/class/pci_bus/0000:02/rescan",
					    O_WRONLY);
					if (rfd < 0)
						rfd = open(
						    "/sys/devices/pci0000:00"
						    "/0000:00:04.0/pci0000:02"
						    "/rescan", O_WRONLY);
					if (rfd >= 0) {
						(void)write(rfd, "1\n", 2);
						close(rfd);
					}
				}
				usleep(FM_ALIVE_POLL_US);
				continue;
			}
			snprintf(path, sizeof(path), "%s/%s/config",
				 PCI_DEVICES, slot);
		}

		fd = open(path, O_RDONLY);
		if (fd >= 0) {
			(void)read(fd, cfg, sizeof(cfg));
			close(fd);
		} else {
			/* Sysfs entry disappeared (removal race). Reset. */
			slot[0] = '\0';
		}

		if (cfg[0] != 0xff || cfg[1] != 0xff) {
			if (++n_stable >= FM_ALIVE_STABLE)
				return FM_OK;
		} else {
			n_stable = 0;
		}
		usleep(FM_ALIVE_POLL_US);
	}
	return FM_ERR;
}

int fm_pcie_enable_command(struct fm6000 *d)
{
	char slot[FM_SLOT_LEN];
	char path[256];
	int fd;
	uint16_t cmd = 0x0406;  /* Memory Space + Bus Master + Interrupt Disable */

	/* Write to the FM6000 DMA endpoint command register.  Memory Space must
	 * be set for DMA register writes to reach the chip (silently dropped
	 * otherwise).  Bus Master is required for the DMA engine to write to
	 * host memory.  Interrupt Disable prevents spurious INTx when no handler
	 * is registered. */
	if (find_slot(slot, sizeof(slot)) != FM_OK)
		return FM_ERR;

	snprintf(path, sizeof(path), "%s/%s/config", PCI_DEVICES, slot);
	fd = open(path, O_RDWR);
	if (fd < 0)
		return FM_ERR;
	if (pwrite(fd, &cmd, 2, 4) != 2) {
		close(fd);
		return FM_ERR;
	}
	close(fd);
	return FM_OK;
}

/*
 * Re-enable Memory Space + Bus Master on the FM6000's PCIe endpoint using
 * the legacy x86 PCI config space I/O port mechanism (CF8h/CFCh).
 *
 * Root cause of previous hangs: writing CF8/CFC for a subordinate bus (bus=2)
 * generates a Type 1 Config Write TLP.  If the endpoint is mid-FATAL its link
 * is cycling, the TLP cannot complete, and the root port fires a Completion
 * Timeout → SERR# → NMI.  The NMI handler calls printk, which tries to
 * acquire the spinlock already held (IRQs disabled) by pci_conf1_write →
 * deadlock; or, in user-space iopl path, the NMI panics the kernel.
 *
 * Fix: clear SERR Enable on all PCIe root ports (bus=0) before the write.
 * Bus=0 config accesses are handled locally by the root complex (no TLP
 * issued, no stall, no CTO risk).  With SERR disabled, a CTO on bus=2 is
 * bounded (~50 ms) and non-fatal; we read the register back and retry until
 * the chip is stable enough to accept the write.
 *
 * This function is x86-only.  On a non-x86 build iopl() fails and the
 * function returns FM_ERR without touching hardware.
 */
static void portio_kmsg(const char *s)
{
	int fd = open("/dev/kmsg", O_WRONLY | O_NOCTTY);
	if (fd >= 0) {
		write(fd, s, strlen(s));
		write(fd, "\n", 1);
		close(fd);
	}
	fprintf(stderr, "nosd-fm6000: KMSG: %s\n", s);
}

/*
 * The upstream root port for the FM6000 is the AMD RS780 bridge at
 * 0000:00:04.0.  Its config space is always accessible via sysfs (never
 * stalls) because the root complex handles it locally without issuing a
 * Type 1 TLP to bus 2.
 */
#define UPSTREAM_PORT		"/sys/bus/pci/devices/0000:00:04.0/config"
#define PCI_CAP_ID_PCIE		0x10

/*
 * Walk the PCI capability list on bus=0 for a given dev/func.
 * Returns the config-space offset of the PCI Express (0x10) capability,
 * or 0 if not found.  All accesses are bus=0 and complete locally.
 */
static uint8_t bus0_find_pcie_cap(uint8_t dev, uint8_t func)
{
	uint32_t cf8;
	uint8_t  ptr;
	int      i;

	/* Capability Pointer at config offset 0x34 */
	cf8 = 0x80000000u | ((uint32_t)dev << 11) | ((uint32_t)func << 8) | 0x34u;
	outl(cf8, 0xCF8);
	ptr = (uint8_t)(inl(0xCFC) & 0xFCu);

	for (i = 0; ptr && i < 16; i++) {
		uint32_t cap;
		cf8 = 0x80000000u | ((uint32_t)dev << 11) | ((uint32_t)func << 8)
		    | (ptr & 0xFCu);
		outl(cf8, 0xCF8);
		cap = inl(0xCFC);
		if ((cap & 0xFFu) == 0x10u)   /* PCIe capability ID */
			return ptr;
		ptr = (uint8_t)((cap >> 8) & 0xFCu);
	}
	return 0;
}

/*
 * Scan the known AMD RS780 root-port devices on bus=0 to find which one
 * bridges to target_bus (by reading Secondary Bus Number at offset 0x19).
 * Returns the device number (4 / 8 / 9 / 10) or 0xFF if not found.
 */
static uint8_t find_root_port_for_bus(uint8_t target_bus)
{
	static const uint8_t rp_devs[] = { 4, 8, 9, 10 };
	int i;

	for (i = 0; i < 4; i++) {
		uint32_t cf8 = 0x80000000u | ((uint32_t)rp_devs[i] << 11) | 0x18u;
		outl(cf8, 0xCF8);
		uint8_t sec = (uint8_t)((inl(0xCFC) >> 8) & 0xFFu);
		if (sec == target_bus)
			return rp_devs[i];
	}
	return 0xFF;
}

/*
 * Read the 16-bit Link Status field from the PCIe capability of a root port
 * on bus=0.  pcie_cap is the byte offset of the PCIe capability header.
 * All accesses are bus=0 and complete locally.
 */
static uint16_t bus0_read_link_status(uint8_t rp_dev, uint8_t pcie_cap)
{
	uint32_t cf8, dword;
	uint8_t  off = pcie_cap + 0x12u;   /* Link Status at cap+0x12 */

	cf8 = 0x80000000u | ((uint32_t)rp_dev << 11) | (off & 0xFCu);
	outl(cf8, 0xCF8);
	dword = inl(0xCFC);
	/* Link Status is the word at byte off; determine which half of dword */
	return (off & 2u) ? (uint16_t)(dword >> 16) : (uint16_t)(dword & 0xFFFFu);
}

/*
 * Clear SERR# Enable (PCI_COMMAND bit 8) on every root port via CF8/CFC to
 * bus=0.  Acquires I/O permissions (iopl(3)) internally.
 *
 * Called just before fm_ssched_tx_reload to suppress the SMM deadlock path:
 *   IC=1 → FM6000 FATAL → ERR_FATAL PCIe message → root port asserts SERR#
 *   → SMI → BIOS SMM handler → CF8/CFC read to bus=2 (FM6000, link bouncing)
 *   → AMD RS780 CTO: error completion only generated when SERR# can fire, but
 *     SMM has NMI/SMI masked → stall in SMM forever → OS frozen.
 *
 * With SERR# disabled: ERR_FATAL is received by the root port but no SERR# is
 * asserted, so no SMI fires and the OS never enters SMM.  fm_pcie_enable_portio
 * (called after the post-tx_reload stability waits) re-enables SERR# once the
 * chip FATAL has settled and the PCIe link is stable again.
 * [RE 2026-10-27, install123]
 */
/*
 * Re-enable SERR# Enable (PCI_COMMAND bit 8) on every root port.
 *
 * Counterpart to fm_pcie_serr_disable.  Called after the IC=1 FATAL has
 * settled (install114 fc stability wait confirms no new FALTALs for 250 ms)
 * and before ESCHED set_cfg2 -- so that the CFG_2 FATAL, if it fires, can
 * generate a CTO error completion via SERR# and allow BIOS SMM to exit.
 * [RE 2026-10-27, install124]
 */
void fm_pcie_serr_reenable(void)
{
	static const uint8_t rp_devs[] = { 4, 8, 9, 10 };
	char msg[128];
	uint16_t cmd0 = 0xFFFFu;

	if (iopl(3) != 0) {
		portio_kmsg("nosd-fm6000: pre-esched: iopl failed; SERR# not re-enabled");
		return;
	}

	for (int i = 0; i < 4; i++) {
		uint32_t cf8 = 0x80000000u | ((uint32_t)rp_devs[i] << 11) | 4u;
		outl(cf8, 0xCF8);
		uint16_t rp_cmd = inw(0xCFC);
		if (i == 0) cmd0 = rp_cmd;
		if (rp_cmd != 0xFFFFu) {
			outl(cf8, 0xCF8);
			outw(rp_cmd | (uint16_t)0x0100u, 0xCFC);  /* SET SERR Enable */
		}
	}
	snprintf(msg, sizeof msg,
		"nosd-fm6000: pre-esched: SERR# re-enabled; rp4_cmd was 0x%04x",
		(unsigned)cmd0);
	portio_kmsg(msg);
}

void fm_pcie_serr_disable(void)
{
	static const uint8_t rp_devs[] = { 4, 8, 9, 10 };
	char msg[128];
	uint16_t cmd0 = 0xFFFFu;

	if (iopl(3) != 0) {
		portio_kmsg("nosd-fm6000: pre-tx_reload: iopl failed; SERR# not disabled");
		return;
	}

	for (int i = 0; i < 4; i++) {
		uint32_t cf8 = 0x80000000u | ((uint32_t)rp_devs[i] << 11) | 4u;
		outl(cf8, 0xCF8);
		uint16_t rp_cmd = inw(0xCFC);
		if (i == 0) cmd0 = rp_cmd;  /* save rp_dev=4 (FM6000 root port) */
		if (rp_cmd != 0xFFFFu) {
			outl(cf8, 0xCF8);
			outw(rp_cmd & ~(uint16_t)0x0100u, 0xCFC);  /* CLEAR SERR Enable */
		}
		/* Clear Root Control bits 0-2 (SECEE/SENFEE/SEFEE) to block the
		 * BIOS SMI path: without these bits the root port cannot generate
		 * the system-error interrupt that the SMM handler polls for.
		 * [install146] */
		{
			uint8_t  pcie_cap = bus0_find_pcie_cap(rp_devs[i], 0);
			uint32_t rc_off   = ((uint32_t)pcie_cap + 0x1Cu) & 0xFCu;
			/* rc_off < 0x40 guards both "cap not found" (pcie_cap=0 →
			 * rc_off=0x1C) and the unlikely overflow case. */
			if (pcie_cap && rc_off >= 0x40u) {
				uint32_t rc_cf8 = 0x80000000u
				    | ((uint32_t)rp_devs[i] << 11) | rc_off;
				outl(rc_cf8, 0xCF8);
				uint16_t root_ctrl = (uint16_t)(inl(0xCFC) & 0xFFFFu);
				if (root_ctrl & 0x0007u) {
					outl(rc_cf8, 0xCF8);
					outw(root_ctrl & ~(uint16_t)0x0007u, 0xCFC);
				}
			}
		}
	}
	snprintf(msg, sizeof msg,
		"nosd-fm6000: pre-tx_reload: SERR# disabled; rp4_cmd was 0x%04x",
		(unsigned)cmd0);
	portio_kmsg(msg);
}

/*
 * fm_pcie_clear_bridge_status_sysfs — clear PCI/Secondary/Device Status via
 * sysfs config file (kernel MMCONFIG path, no CF8/CFC I/O port access).
 *
 * Replaces fm_pcie_clear_bridge_status() in the ring-reinit boot_cold path
 * (install185).  The CF8-based version triggers the AMD SB700 I/O-port SMI
 * trap on every call.  In the ring-reinit context SERR# is still disabled and
 * the RS780 may hold a CTO latch from the SSCHED link bounce; the BIOS SMM
 * handler reads bus=2 via CF8/CFC and stalls permanently.
 *
 * Using sysfs (kernel MMCONFIG/ECAM path) generates no CF8 I/O port write and
 * therefore does not trigger the I/O-port SMI trap.  The only remaining BIOS
 * SMI risk is the periodic timer (~3-4 s); this function completes in <1 ms,
 * making the probability of the timer coinciding with a CTO window negligible.
 * [install185, 2026-10-28]
 */
int fm_pcie_clear_bridge_status_sysfs(void)
{
	static const char *const rp_paths[] = {
		PCI_DEVICES "/0000:00:04.0",
		PCI_DEVICES "/0000:00:08.0",
		PCI_DEVICES "/0000:00:09.0",
		PCI_DEVICES "/0000:00:0a.0",
	};
	char path[256];
	uint16_t v, clr;
	uint16_t stat0 = 0, sec0 = 0, ds0 = 0;
	int fd;

	for (int i = 0; i < 4; i++) {
		snprintf(path, sizeof(path), "%s/config", rp_paths[i]);
		fd = open(path, O_RDWR);
		if (fd < 0) continue;
		/* PCI Status (RW1C) at offset 0x06 */
		if (pread(fd, &v, 2, 6) == 2 && v != 0xFFFFu) {
			if (i == 0) stat0 = v;
			if (v & 0xF910u) {
				clr = 0xF910u;
				(void)pwrite(fd, &clr, 2, 6);
			}
		}
		/* Secondary Status (RW1C) at offset 0x1E */
		if (pread(fd, &v, 2, 0x1E) == 2 && v != 0xFFFFu) {
			if (i == 0) sec0 = v;
			if (v & 0xF910u) {
				clr = 0xF910u;
				(void)pwrite(fd, &clr, 2, 0x1E);
			}
		}
		close(fd);
	}
	/* PCIe Device Status (RW1C) at pcie_cap+0x0A for dev=4 (bridges bus=2).
	 * pcie_cap=0x58 confirmed from hardware; Device Status at 0x58+0x0A=0x62. */
	snprintf(path, sizeof(path), "%s/config", rp_paths[0]);
	fd = open(path, O_RDWR);
	if (fd >= 0) {
		if (pread(fd, &ds0, 2, 0x62) == 2 && ds0 != 0xFFFFu && (ds0 & 0x000Fu)) {
			clr = 0x000Fu;
			(void)pwrite(fd, &clr, 2, 0x62);
		}
		close(fd);
	}

	char msg[192];
	snprintf(msg, sizeof(msg),
		 "nosd-fm6000: bridge-status cleared via sysfs"
		 " pci_stat=0x%04x sec_stat=0x%04x dev_stat=0x%04x"
		 " [install185]",
		 (unsigned)stat0, (unsigned)sec0, (unsigned)ds0);
	portio_kmsg(msg);
	return 0;
}

/*
 * fm_pcie_serr_reenable_sysfs — set SERR# Enable (PCI_COMMAND bit 8) on all
 * root ports via sysfs config file (kernel MMCONFIG path, no CF8/CFC).
 *
 * Counterpart to fm_pcie_serr_reenable().  Using sysfs avoids the AMD SB700
 * I/O-port SMI trap that fires on every CF8 write.  [install185, 2026-10-28]
 */
void fm_pcie_serr_reenable_sysfs(void)
{
	static const char *const rp_paths[] = {
		PCI_DEVICES "/0000:00:04.0",
		PCI_DEVICES "/0000:00:08.0",
		PCI_DEVICES "/0000:00:09.0",
		PCI_DEVICES "/0000:00:0a.0",
	};
	char path[256];
	uint16_t cmd0 = 0xFFFFu, cmd;
	int fd;

	for (int i = 0; i < 4; i++) {
		snprintf(path, sizeof(path), "%s/config", rp_paths[i]);
		fd = open(path, O_RDWR);
		if (fd < 0) continue;
		if (pread(fd, &cmd, 2, 4) == 2 && cmd != 0xFFFFu) {
			if (i == 0) cmd0 = cmd;
			cmd |= 0x0100u;  /* set SERR# Enable */
			(void)pwrite(fd, &cmd, 2, 4);
		}
		close(fd);
	}

	char msg[128];
	snprintf(msg, sizeof(msg),
		 "nosd-fm6000: pre-esched: SERR# re-enabled via sysfs; rp4_cmd was 0x%04x [install185]",
		 (unsigned)cmd0);
	portio_kmsg(msg);
}

/*
 * Clear PCI Status (0x06), Secondary Status (0x1E), and PCIe Device Status
 * (pcie_cap+0x0A) for all RS780 root ports via CF8/CFC (bus=0 only).
 *
 * These RW1C error accumulator bits are what the AMD SB700 periodic BIOS SMI
 * timer (~3-4s) polls.  When ESCHED link bounces set them, the timer fires at
 * T+3-4s and issues CF8/CFC to bus=2 (FM6000 endpoint).  With SERR# disabled,
 * a CTO on bus=2 has no error completion on AMD RS780 → CPU stalls forever.
 *
 * Clearing here (bus=0, always local, never stalls) resets the ~3-4s countdown
 * so the next BIOS SMI fires AFTER portio + DMA init complete, at which point
 * the FM6000 is running and responds to Config Reads normally.
 *
 * Callable from main.c before fm_pcie_enable_portio() because iopl(3) is
 * acquired by fm_pcie_serr_disable() and persists for the process lifetime.
 * [install169, 2026-10-27]
 */
int fm_pcie_clear_bridge_status(void)
{
	static const uint8_t rp_devs[] = { 4, 8, 9, 10 };
	char     msg[160];
	uint16_t stat0 = 0, sec0 = 0, ds0 = 0;
	uint8_t  rp_dev, pcie_cap;

	if (iopl(3) != 0) {
		portio_kmsg("nosd-fm6000: bridge-status: iopl failed");
		return -1;
	}

	for (int i = 0; i < 4; i++) {
		uint32_t cf8;
		uint16_t v;
		/* PCI Status (0x06): high 16 bits of DWORD at 0x04.
		 * install181: use 0xF910 to also clear bit 4 (MDPE, Master Data
		 * Parity Error) which was never cleared by 0xF900 and causes the
		 * BIOS SMI timer to always see an error bit (confirmed install180). */
		cf8 = 0x80000000u | ((uint32_t)rp_devs[i] << 11) | 0x04u;
		outl(cf8, 0xCF8); v = inw(0xCFE);
		if (i == 0) stat0 = v;
		if (v != 0xFFFFu && (v & 0xF910u)) {
			outl(cf8, 0xCF8); outw(0xF910u, 0xCFE);
		}
		/* Secondary Status (0x1E): high 16 bits of DWORD at 0x1C */
		cf8 = 0x80000000u | ((uint32_t)rp_devs[i] << 11) | 0x1Cu;
		outl(cf8, 0xCF8); v = inw(0xCFE);
		if (i == 0) sec0 = v;
		if (v != 0xFFFFu && (v & 0xF910u)) {
			outl(cf8, 0xCF8); outw(0xF910u, 0xCFE);
		}
	}
	/* PCIe Device Status (pcie_cap+0x0A) for the root port bridging bus=2 */
	rp_dev   = find_root_port_for_bus(2);
	pcie_cap = (rp_dev != 0xFFu) ? bus0_find_pcie_cap(rp_dev, 0) : 0;
	if (rp_dev != 0xFFu && pcie_cap != 0u) {
		uint8_t  ds_dw = (pcie_cap + 0x08u) & 0xFCu;
		uint32_t cf8   = 0x80000000u | ((uint32_t)rp_dev << 11) | ds_dw;
		outl(cf8, 0xCF8); ds0 = inw(0xCFE);
		if (ds0 != 0xFFFFu && (ds0 & 0x000Fu)) {
			outl(cf8, 0xCF8); outw(0x000Fu, 0xCFE);
		}
	}
	snprintf(msg, sizeof(msg),
		 "nosd-fm6000: bridge-status cleared"
		 " pci_stat=0x%04x sec_stat=0x%04x dev_stat=0x%04x"
		 " [install169]",
		 (unsigned)stat0, (unsigned)sec0, (unsigned)ds0);
	portio_kmsg(msg);
	return 0;
}

/*
 * Disable/enable Memory Space (PCI_COMMAND bit 1) on the FM6000 endpoint
 * (bus=2, dev=0, func=0) via CF8/CFC.
 *
 * boot.c established that enabling PCIe Memory Space before IC=1 stalls the
 * TX ring engine: TX_REPLACE_TOKEN sticks at ring slot 0, TX_FOUND never
 * fires, yet no FATAL is generated.  The fix used during initial boot is
 * pcie_regs==NULL (Memory Space never enabled before ring_init).  In the
 * post-install113 path Memory Space is already enabled; these functions
 * bracket the ring re-initialisation to replicate the boot.c condition.
 *
 * Prerequisites:
 *   • PCIe link must be up and stable (confirmed by Phase 3 stability wait).
 *   • SERR# must be disabled (fm_pcie_serr_disable() called first): a CF8/CFC
 *     write to bus=2 that happens to land during a link bounce causes a CTO;
 *     on AMD RS780 a CTO only generates an error completion when SERR# is
 *     asserted -- without it the outl/outw stalls indefinitely.
 *   • iopl(3) must be held (fm_pcie_serr_disable calls it).
 * [RE 2026-10-27, install125]
 */

/*
 * fm_pcie_wait_dllla_stable — poll root-port LNKSTA DLLLA via bus=0 CF8/CFC
 * until the bit stays set for @required_stable consecutive reads.
 *
 * install173: restructured from install170.
 *
 * Root cause of install172 freeze: the 50ms nanosleep came BEFORE the first
 * LNKSTA read.  The BIOS periodic SMM timer fired during that first sleep
 * while the FM6000 was still off-bus (link retraining after boot_cold reset
 * pulse).  With SERR# disabled the CTO deadlocked inside SMM.
 *
 * Fix:
 *   1. Read LNKSTA FIRST (before any sleep) — if DLLLA=1 already, we skip
 *      the dangerous first sleep entirely.
 *   2. On the first DLLLA=1 read, immediately enable SERR# (bus=0 writes,
 *      always local, never stall).  With SERR# enabled, any subsequent BIOS
 *      timer CTO on bus=2 gets a hardware error completion and the BIOS SMM
 *      exits cleanly — no deadlock even if FM6000 briefly FATALs later.
 *   3. Sleep AFTER the read and SERR# enable, not before.
 *
 * SERR# is enabled only when @enable_serr_on_dllla is non-zero; portio
 * manages SERR# through its own logic (install172: never enables SERR# in
 * portio), so portio callers pass 0.
 *
 * Bus=0 reads are always local; SERR# state does not affect them.
 * iopl(3) must already be held (fm_pcie_serr_disable calls it before the
 * ring-reinit sequence).
 *
 * pcie_cap=0x58, dev=4 hard-coded (confirmed install167, this HW).
 * LNKSTA at (0x58+0x12)&0xFC = 0x68; DLLLA = bit 12 of LNKSTA.
 * Gives up after 60 iterations (3 seconds); caller proceeds to portio.
 * [install170, install173]
 */
void fm_pcie_wait_dllla_stable(int required_stable, int enable_serr_on_dllla)
{
	static const uint8_t ln_dev = 4;
	static const uint8_t ln_off = (0x58u + 0x12u) & 0xFCu; /* 0x68 */
	const uint32_t       ln_cf8 = 0x80000000u |
	                              ((uint32_t)ln_dev << 11) | ln_off;
	static const uint8_t rp_devs[4] = { 4, 8, 9, 10 };
	int stable = 0, serr_done = 0, iter;

	/* install181: entry marker — appears in nosd log before any CF8 read */
	portio_kmsg("nosd-fm6000: dllla-stable: enter [install181]");

	for (iter = 0; iter < 60 && stable < required_stable; iter++) {
		struct timespec ts = { 0, 50000000L }; /* 50 ms */
		uint32_t dword;
		uint16_t lnksta;
		char     lmsg[96];

		/* Read FIRST, sleep after — avoids sleeping with FM6000 off-bus
		 * immediately after boot_cold reset. [install173] */
		outl(ln_cf8, 0xCF8);
		dword  = inl(0xCFC);
		lnksta = (uint16_t)(dword >> 16);

		if (lnksta & (1u << 12)) {  /* DLLLA */
			stable++;
			/* install173: enable SERR# on first DLLLA=1 (FM6000 on-bus)
			 * so any BIOS timer CTO during subsequent sleeps gets error
			 * completion instead of deadlocking.  Ring-reinit boot_cold
			 * uses a clean PCIe fundamental reset (no CTO latch in RS780),
			 * so enabling SERR# here does not trigger immediate BIOS SMM
			 * (unlike after the install113 link-bounce CTO). */
			if (enable_serr_on_dllla && !serr_done) {
				for (int i = 0; i < 4; i++) {
					uint32_t cf8 = 0x80000000u |
					               ((uint32_t)rp_devs[i] << 11) | 4u;
					outl(cf8, 0xCF8);
					uint16_t rp_cmd = inw(0xCFC);
					if (rp_cmd != 0xFFFFu) {
						outl(cf8, 0xCF8);
						outw(rp_cmd | (uint16_t)0x0100u, 0xCFC);
					}
				}
				portio_kmsg("nosd-fm6000: post-ring-reinit:"
				            " SERR# enabled on DLLLA=1 [install173]");
				serr_done = 1;
			}
		} else {
			stable = 0;
		}

		snprintf(lmsg, sizeof lmsg,
			 "nosd-fm6000: post-ring-reinit:"
			 " lnksta=0x%04x stable=%d [install170]",
			 (unsigned)lnksta, stable);
		portio_kmsg(lmsg);

		if (stable < required_stable)
			nanosleep(&ts, NULL);
	}
}

void fm_pcie_dev_memspc_disable(void)
{
	/* bus=2, dev=0, func=0, PCI_COMMAND at offset 4 */
	uint32_t cfgaddr = 0x80000000u | (2u << 16) | 4u;
	outl(cfgaddr, 0xCF8);
	outw(0x0404u, 0xCFC);   /* Bus Master | Interrupt Disable; clear Memory Space */
	portio_kmsg("nosd-fm6000: ring-reinit: Memory Space disabled on FM6000 (0x0404)");
}

void fm_pcie_dev_memspc_enable(void)
{
	/* Restore Memory Space | Bus Master | Interrupt Disable = 0x0406.
	 * Called after ring_init's resets (which clear PCI_COMMAND) and the
	 * post-ring stability wait (link confirmed stable) but BEFORE
	 * fm_esched_set_cfg2 writes to FM6000 BAR0. */
	uint32_t cfgaddr = 0x80000000u | (2u << 16) | 4u;
	outl(cfgaddr, 0xCF8);
	outw(0x0406u, 0xCFC);   /* Memory Space | Bus Master | Interrupt Disable */
	portio_kmsg("nosd-fm6000: ring-reinit: Memory Space re-enabled on FM6000 (0x0406)");
}

/*
 * Clear AER Root Error Command (disables AER MSI) and Root Error Status in
 * the upstream root port (0000:00:04.0) via sysfs config.
 *
 * Two mechanisms can freeze the CPU during ESCHED/pipeline_init link bounces:
 *
 *   1. AMD SB700 BIOS SMI timer (~3-4 s poll): fires a BIOS SMI when Root
 *      Error Status is non-zero; SMM handler does CF8/CFC to bus=2 while the
 *      link is training → permanent stall.  Fixed by clearing Root Error
 *      Status (AER_cap+0x30, RW1C) and by fm_pcie_serr_disable() keeping
 *      PCIe Root Control SECEE/SENFEE/SEFEE=0.
 *      [install146, 2026-10-27]
 *
 *   2. Kernel AER MSI interrupt: when ESCHED FATALs bounce the PCIe link,
 *      the root port logs a link-error in its own AER Correctable/Uncorrectable
 *      Error Status and sets Root Error Status.  If AER Root Error Command
 *      (AER_cap+0x2C) has ECE/ENFE/EFE bits set (armed by aer_probe at load
 *      time), the root port fires an MSI.  The kernel's aer_isr() reads Root
 *      Error Source ID (AER_cap+0x34) and then calls pci_read_config_dword on
 *      bus=2 (the FM6000 endpoint) to fetch the device's AER status.  On
 *      AMD RS780, a config-space read to an endpoint whose link is in
 *      retraining blocks the kernel permanently (no CTO for kernel CF8/CFC
 *      with SERR# disabled).  Fixed by writing 0 to AER Root Error Command to
 *      disable the MSI before ESCHED/pipeline_init writes.
 *      [install151, 2026-10-27]
 *
 * Call this AFTER fm_pcie_serr_disable() and BEFORE pipeline_init().
 */
void fm_pcie_clear_root_aer_errors(void)
{
	int      fd;
	uint16_t next = 0x100;

	fd = open(UPSTREAM_PORT, O_RDWR);
	if (fd < 0) {
		portio_kmsg("nosd-fm6000: aer-clear: open failed");
		return;
	}
	/* Walk PCI extended capability list (PCIe r5.0 §7.6.3).
	 * Each 4-byte header: bits 15:0 = cap ID, 19:16 = version,
	 * 31:20 = next offset (0 = end-of-list). */
	for (int guard = 0; guard < 48 && next >= 0x100u; guard++) {
		uint32_t hdr;
		ssize_t  nr = pread(fd, &hdr, 4, (off_t)next);
		if (guard == 0) {  /* install167: log first entry for diagnosis */
			char dmsg[128];
			snprintf(dmsg, sizeof(dmsg),
				 "nosd-fm6000: aer-clear: [0x%03x] nr=%zd hdr=0x%08x [install167]",
				 next, nr, (nr == 4 ? hdr : 0xDEADBEEFu));
			portio_kmsg(dmsg);
		}
		if (nr != 4)
			break;
		if ((hdr & 0xFFFFu) == 0x0001u) {  /* AER extended cap ID */
			uint32_t ones = 0xFFFFFFFFu;
			/* Root Error Status at AER_cap+0x30 (RW1C): clear stale bits.
			 * Root Error Command (+0x2C) is intentionally left unchanged:
			 * writing it can deadlock if the kernel AER handler is in
			 * flight, and SERR# disabled (fm_pcie_serr_disable) already
			 * blocks new SMI generation.  [install153, 2026-10-27] */
			if (pwrite(fd, &ones, 4, (off_t)(next + 0x30u)) == 4)
				portio_kmsg("nosd-fm6000: aer-clear: Root Error Status cleared");
			else
				portio_kmsg("nosd-fm6000: aer-clear: Root Error Status pwrite failed");
			/* install166: NOTE - Root Error Command (+0x2C) is cleared
			 * separately by fm_pcie_disable_root_aer_cmd() which must be
			 * called once before ESCHED writes; do not touch it here
			 * (multiple callers) to avoid repeated save/restore. */
			break;
		}
		next = (uint16_t)((hdr >> 20) & 0xFFFu);
	}
	close(fd);
}

/*
 * install166: save and clear AER Root Error Command on the upstream root port
 * to prevent the kernel aer_isr() from firing MSIs during ESCHED/pipeline_init
 * link bounces.
 *
 * Root cause: aer_probe() arms Root Error Command bits (CERE/NFERE/FERE) at
 * driver load time.  When ESCHED CFG_1/CFG_2 writes cause a PCIe FATAL that
 * bounces the link, the root port fires an MSI.  The kernel's aer_isr() reads
 * Root Error Source ID then calls pci_read_config_dword on bus=2 (FM6000).
 * On AMD RS780 with SERR# disabled, a config-space read to an endpoint whose
 * link is in retraining blocks the CPU permanently (no CTO).
 *
 * Writing 0 to Root Error Command prevents the MSI from being generated.
 * Call BEFORE fm_esched_set_cfg2(), after the HPIE/DLLSCE/PDCE quiescence
 * (when no AER MSI is in flight) and BEFORE any link-bouncing writes.
 * Restore with fm_pcie_restore_root_aer_cmd() after DMA init succeeds.
 * [install166, 2026-10-27]
 */
static uint32_t g_saved_aer_root_cmd = 0xFFFFFFFFu; /* 0xFFFFFFFF = not yet saved */

void fm_pcie_disable_root_aer_cmd(void)
{
	int      fd;
	uint16_t next = 0x100;
	char     msg[128];
	int      guard;

	fd = open(UPSTREAM_PORT, O_RDWR);
	if (fd < 0) {
		portio_kmsg("nosd-fm6000: aer-cmd-disable: open failed");
		return;
	}
	/* install167: trace cap walk so silent failures are visible */
	portio_kmsg("nosd-fm6000: aer-cmd-disable: fd ok [install167]");
	for (guard = 0; guard < 48 && next >= 0x100u; guard++) {
		uint32_t hdr;
		ssize_t  nr = pread(fd, &hdr, 4, (off_t)next);
		snprintf(msg, sizeof(msg),
			 "nosd-fm6000: aer-cmd-disable: [0x%03x] nr=%zd hdr=0x%08x [install167]",
			 next, nr, (nr == 4 ? hdr : 0xDEADBEEFu));
		portio_kmsg(msg);
		if (nr != 4)
			break;
		if ((hdr & 0xFFFFu) == 0x0001u) {  /* AER extended cap ID */
			uint32_t zero = 0;
			if (pread(fd, &g_saved_aer_root_cmd, 4,
				  (off_t)(next + 0x2Cu)) == 4) {
				snprintf(msg, sizeof(msg),
					 "nosd-fm6000: aer-cmd-disable: "
					 "Root Error Command was 0x%08x -> 0 [install166]",
					 g_saved_aer_root_cmd);
				portio_kmsg(msg);
				if (pwrite(fd, &zero, 4, (off_t)(next + 0x2Cu)) != 4)
					portio_kmsg("nosd-fm6000: aer-cmd-disable: pwrite failed");
			} else {
				portio_kmsg("nosd-fm6000: aer-cmd-disable: pread +0x2c failed");
			}
			break;
		}
		next = (uint16_t)((hdr >> 20) & 0xFFFu);
	}
	if (guard >= 48 || next < 0x100u) {
		snprintf(msg, sizeof(msg),
			 "nosd-fm6000: aer-cmd-disable: cap not found guard=%d next=0x%03x [install167]",
			 guard, next);
		portio_kmsg(msg);
	}
	close(fd);
}

void fm_pcie_restore_root_aer_cmd(void)
{
	int      fd;
	uint16_t next = 0x100;

	if (g_saved_aer_root_cmd == 0xFFFFFFFFu) {
		portio_kmsg("nosd-fm6000: aer-cmd-restore: no saved value; skipping");
		return;
	}
	fd = open(UPSTREAM_PORT, O_RDWR);
	if (fd < 0) {
		portio_kmsg("nosd-fm6000: aer-cmd-restore: open failed");
		return;
	}
	for (int guard = 0; guard < 48 && next >= 0x100u; guard++) {
		uint32_t hdr;
		if (pread(fd, &hdr, 4, (off_t)next) != 4)
			break;
		if ((hdr & 0xFFFFu) == 0x0001u) {  /* AER extended cap ID */
			if (pwrite(fd, &g_saved_aer_root_cmd, 4,
				   (off_t)(next + 0x2Cu)) == 4)
				portio_kmsg("nosd-fm6000: aer-cmd-restore: "
					    "Root Error Command restored [install166]");
			else
				portio_kmsg("nosd-fm6000: aer-cmd-restore: pwrite failed");
			break;
		}
		next = (uint16_t)((hdr >> 20) & 0xFFFu);
	}
	close(fd);
}

/*
 * Clear DevCtl error-reporting bits 0-3 on the FM6000 PCIe endpoint
 * (CER/NFER/FER/UR enable) so it does not send ERR_FATAL messages during
 * pipeline_init and L2F init, which perform SRAM writes that generate
 * FATALs and bounce the PCIe link.
 *
 * With error reporting disabled the AER Root Error Status register stays
 * clear, the BIOS SMI timer never fires, and the portio / DMA init
 * sequence completes before any BIOS activity.
 *
 * Requires SERR# to already be disabled (fm_pcie_serr_disable called) and
 * iopl(3) to be held, and the PCIe link to be up (fm_wait_link_active
 * confirmed).  The CF8/CFC reads to bus=2 stall the CPU if the link is
 * down, so call this only after the link is confirmed active.
 * [install146, 2026-10-27]
 */
int fm_pcie_ep_disable_error_reporting(uint8_t bus, uint8_t dev, uint8_t func)
{
	uint8_t ptr;
	char    msg[128];

	if (iopl(3) != 0) {
		portio_kmsg("nosd-fm6000: ep-err-disable: iopl failed");
		return FM_ERR;
	}

	/* Capability Pointer at offset 0x34 (bus=2 access -- link must be up) */
	uint32_t cf8 = 0x80000000u | ((uint32_t)bus  << 16)
	             | ((uint32_t)dev << 11) | ((uint32_t)func << 8) | 0x34u;
	outl(cf8, 0xCF8);
	ptr = (uint8_t)(inl(0xCFC) & 0xFCu);

	for (int i = 0; ptr && i < 16; i++) {
		uint32_t cap;
		cf8 = 0x80000000u | ((uint32_t)bus  << 16)
		    | ((uint32_t)dev << 11) | ((uint32_t)func << 8)
		    | (uint32_t)(ptr & 0xFCu);
		outl(cf8, 0xCF8);
		cap = inl(0xCFC);
		if ((cap & 0xFFu) == 0x10u) {  /* PCIe capability */
			/* DevCtl at pcie_cap + 8 (DWORD-aligned; lower 16 bits) */
			uint32_t devctl_cf8 = 0x80000000u
			    | ((uint32_t)bus  << 16)
			    | ((uint32_t)dev << 11) | ((uint32_t)func << 8)
			    | (uint32_t)((ptr + 8u) & 0xFCu);
			outl(devctl_cf8, 0xCF8);
			uint16_t devctl     = (uint16_t)(inl(0xCFC) & 0xFFFFu);
			uint16_t devctl_new = devctl & ~(uint16_t)0x000Fu;
			outl(devctl_cf8, 0xCF8);
			outw(devctl_new, 0xCFC);  /* write DevCtl; DevSta (upper 16) untouched */
			snprintf(msg, sizeof(msg),
			    "nosd-fm6000: ep-err-disable: DevCtl 0x%04x->0x%04x"
			    " bus=%u dev=%u func=%u",
			    (unsigned)devctl, (unsigned)devctl_new,
			    (unsigned)bus, (unsigned)dev, (unsigned)func);
			portio_kmsg(msg);
			return FM_OK;
		}
		ptr = (uint8_t)((cap >> 8) & 0xFCu);
	}
	portio_kmsg("nosd-fm6000: ep-err-disable: PCIe cap not found");
	return FM_ERR;
}

int fm_pcie_enable_portio(uint8_t bus, uint8_t dev, uint8_t func)
{
	static const uint8_t rp_devs[] = { 4, 8, 9, 10 };
	uint32_t cfgaddr;
	int      attempt;
	int      link_wait = 0;
	uint8_t  rp_dev, pcie_cap;
	char     msg[128];

	portio_kmsg("nosd-fm6000: portio: enter");

	/*
	 * install177: do NOT call fm_pcie_clear_root_aer_errors() here.
	 *
	 * Two reasons:
	 *
	 * 1. This platform's RS780 root port has no AER extended capability
	 *    (the cap chain is VSEC@0x100 → VC@0x110 → END; ID=0x0001 is never
	 *    found), so the function would silently clear nothing.
	 *
	 * 2. fm_pcie_clear_root_aer_errors() opens UPSTREAM_PORT via sysfs and
	 *    calls pread(), which enters the kernel's PCI config access path and
	 *    acquires raw_spin_lock_irq(&pci_lock).  That disables hardware IRQs
	 *    but NOT SMIs.  If the AMD SB700 periodic BIOS SMI timer fires while
	 *    pci_lock is held, the BIOS SMI handler issues CF8/CFC to bus=2.
	 *    After the install113 link-disable/re-enable recovery the RS780 still
	 *    holds its internal CTO latch; the bus=2 CF8/CFC read stalls in SMM
	 *    permanently, pci_lock is never released, and the kernel hangs.
	 *
	 *    Empirically confirmed in install175 (second aer_clear, pread stalls
	 *    at offset 0x110) and install176 (first aer_clear, pread stalls at
	 *    offset 0x100): sysfs pread on UPSTREAM_PORT is not safe in portio.
	 *
	 * The install168 CF8/CFC bridge-status clear below (userspace outl/inl
	 * via iopl, no kernel lock) clears the error accumulator bits the BIOS
	 * timer actually polls and is sufficient.  [install177, 2026-10-27]
	 */

	if (iopl(3) != 0)
		return FM_ERR;

	/*
	 * Step 1: Find root port and wait for DLLLA=1 BEFORE re-enabling SERR#.
	 *
	 * install145: moved SERR# re-enable to after LNKSTA poll.  Previous
	 * order (SERR# first, then poll) caused a permanent freeze: a late
	 * pipeline_init FATAL at T≈2-4s after ring-reinit bounced the PCIe
	 * link; the periodic BIOS SMI timer fired at the same time, did
	 * CF8/CFC to bus=2, found the link bouncing, and with SERR# enabled
	 * entered an infinite error-recovery loop in SMM that never exits
	 * (each retry stalls 50ms on CTO, NMI deferred inside SMM, CPU never
	 * returns to OS).  By calling fm_pcie_enable_portio immediately (no
	 * 4s post-ESCHED tick loop) and confirming the link is stable here
	 * FIRST, the bus=2 write and DMA init both complete before T≈2-4s
	 * so the FM6000 is fully running when the BIOS SMI fires, responds
	 * to config reads with valid data, and the BIOS exits normally.
	 * [install145, 2026-10-27]
	 *
	 * All LNKSTA reads are bus=0 and always complete locally.
	 */
	rp_dev = find_root_port_for_bus(bus);
	pcie_cap = (rp_dev != 0xFFu) ? bus0_find_pcie_cap(rp_dev, 0) : 0;

	if (rp_dev == 0xFFu || pcie_cap == 0) {
		portio_kmsg("nosd-fm6000: portio: root port not found; proceeding blind");
	} else {
		snprintf(msg, sizeof(msg),
			 "nosd-fm6000: portio: root port dev=%u pcie_cap=0x%02x",
			 (unsigned)rp_dev, (unsigned)pcie_cap);
		portio_kmsg(msg);

		for (attempt = 0; attempt < 100; attempt++) {
			uint16_t lnksta = bus0_read_link_status(rp_dev, pcie_cap);
			if (lnksta & (1u << 12)) {  /* DLLLA (Data Link Layer Link Active) */
				link_wait = attempt;
				break;
			}
			snprintf(msg, sizeof(msg),
				 "nosd-fm6000: portio: link down lnksta=0x%04x; waiting",
				 (unsigned)lnksta);
			portio_kmsg(msg);
			struct timespec ts = { 0, 100000000L };
			nanosleep(&ts, NULL);
			if (attempt == 99) {
				portio_kmsg("nosd-fm6000: portio: link never active; giving up");
				iopl(0);
				return FM_ERR;
			}
		}
	}

	/*
	 * install176: removed install163 (second fm_pcie_clear_root_aer_errors
	 * call between LNKSTA poll and install168 bridge-status clear).
	 *
	 * Root cause of install175 freeze: the sysfs pread at cap offset 0x110
	 * inside the second aer_clear stalls permanently.  The entry aer_clear
	 * (install165) already cleared Root Error Status; no new FATAL can occur
	 * between that clear and here (LNKSTA poll is bus=0, non-generating).
	 * install168 (CF8/CFC bridge-status clear below) covers the PCI Status,
	 * Secondary Status, and Device Status bits the BIOS timer actually polls.
	 * The second sysfs pread walk is redundant and is the freeze point;
	 * removing it closes the gap.  [install176, 2026-10-27]
	 */

	/*
	 * install172: SERR# is NOT re-enabled here.
	 *
	 * install171 confirmed: writing SERR# Enable=1 to the RS780 root port
	 * Command register while the pre-DMA install113 CTO latch is still
	 * held in the RS780 causes an IMMEDIATE BIOS SMM (not the periodic
	 * ~3-4s timer).  The BIOS SERR# handler fires, reads bus=2 (FM6000
	 * config), and if the link is momentarily unstable the SB700 CTO
	 * deadlocks inside SMM — the CPU never exits.  Confirmed: all four
	 * install170/171 freezes happen within microseconds of the SERR#
	 * Enable write.
	 *
	 * portio's only bus=2 CF8/CFC access is the outw write below (Memory
	 * Space + Bus Master).  PCIe config WRITES are posted — the outw
	 * completes immediately from the CPU's perspective; a CTO on the write
	 * TLP does NOT stall the CPU.  Therefore SERR# protection is not
	 * needed during portio; the caller re-enables it after DMA attach and
	 * link stability are confirmed.  [install172, 2026-10-27]
	 */

	/*
	 * install168: clear PCI Status (0x06), Secondary Status (0x1E), and
	 * PCIe Device Status (pcie_cap+0x0A) for every root port.
	 *
	 * Defence-in-depth: clears any residual error bits before the bus=2
	 * write below.  All bus=0 accesses; never stalls.
	 * [install168, 2026-10-27]
	 */
	{
		char smsg[128];
		uint16_t stat0 = 0, sec0 = 0, ds0 = 0;
		for (int i = 0; i < 4; i++) {
			uint32_t cf8;
			uint32_t dw;
			uint16_t v;
			/* PCI Status (0x06): high 16 bits of DWORD at 0x04.
			 * Use inl(0xCFC)>>16 instead of inw(0xCFE): consistent with
			 * all other CF8/CFC reads in this file. [install171] */
			cf8 = 0x80000000u | ((uint32_t)rp_devs[i] << 11) | 0x04u;
			outl(cf8, 0xCF8); dw = inl(0xCFC); v = (uint16_t)(dw >> 16);
			if (i == 0) stat0 = v;
			if (v != 0xFFFFu && (v & 0xF900u)) {
				outl(cf8, 0xCF8); outw(0xF900u, 0xCFE);
			}
			/* Secondary Status (0x1E): high 16 bits of DWORD at 0x1C */
			cf8 = 0x80000000u | ((uint32_t)rp_devs[i] << 11) | 0x1Cu;
			outl(cf8, 0xCF8); dw = inl(0xCFC); v = (uint16_t)(dw >> 16);
			if (i == 0) sec0 = v;
			if (v != 0xFFFFu && (v & 0xF900u)) {
				outl(cf8, 0xCF8); outw(0xF900u, 0xCFE);
			}
		}
		/* PCIe Device Status (pcie_cap+0x0A): high 16 bits of DWORD at
		 * pcie_cap+0x08 (DevCtl=low, DevStatus=high). */
		if (rp_dev != 0xFFu && pcie_cap != 0u) {
			uint8_t  ds_dw = (pcie_cap + 0x08u) & 0xFCu;
			uint32_t cf8 = 0x80000000u | ((uint32_t)rp_dev << 11) | ds_dw;
			uint32_t dw;
			outl(cf8, 0xCF8); dw = inl(0xCFC); ds0 = (uint16_t)(dw >> 16);
			if (ds0 != 0xFFFFu && (ds0 & 0x000Fu)) {
				outl(cf8, 0xCF8); outw(0x000Fu, 0xCFE);
			}
		}
		snprintf(smsg, sizeof(smsg),
			 "nosd-fm6000: portio: bridge status cleared"
			 " pci_stat=0x%04x sec_stat=0x%04x dev_stat=0x%04x"
			 " [install168/install171]",
			 (unsigned)stat0, (unsigned)sec0, (unsigned)ds0);
		portio_kmsg(smsg);
	}

	cfgaddr = 0x80000000u
		| ((uint32_t)bus  << 16)
		| ((uint32_t)dev  << 11)
		| ((uint32_t)func <<  8)
		| 4u;

	/*
	 * Step 3: write the command register, then verify via LNKSTA (bus=0)
	 * instead of a Config Read to bus=2.
	 *
	 * On AMD RS780, any CF8/CFC access (read OR write) to bus=2 stalls
	 * the CPU if the PCIe link is down or retraining at that instant.
	 * Reads stall unconditionally; writes stall while the link trains and
	 * then proceed, but a readback issued immediately after a write opens
	 * a race: the write TLP in flight can cause a FATAL, the link bounces,
	 * and the CPU stalls on the readback inw indefinitely.
	 *
	 * Mitigation: write once with zero intervening log calls (link was
	 * just confirmed active), then check LNKSTA on the root port (bus=0,
	 * always local, never stalls).  If the link is still up the write
	 * landed; if it dropped, wait 200 ms and retry.
	 */
	for (attempt = 0; attempt < 5; attempt++) {
		outl(cfgaddr, 0xCF8);
		outw(0x0406u, 0xCFC);

		if (rp_dev != 0xFFu && pcie_cap != 0u) {
			uint16_t post_lnk = bus0_read_link_status(rp_dev, pcie_cap);
			if (post_lnk & (1u << 13)) {
				snprintf(msg, sizeof(msg),
					 "nosd-fm6000: portio: CF8=0x%08x link_wait=%d"
					 " cmd written lnk=0x%04x (attempt %d)",
					 cfgaddr, link_wait,
					 (unsigned)post_lnk, attempt + 1);
				portio_kmsg(msg);
				iopl(0);
				return FM_OK;
			}
			snprintf(msg, sizeof(msg),
				 "nosd-fm6000: portio: CF8=0x%08x link dropped after"
				 " write attempt %d; retrying",
				 cfgaddr, attempt + 1);
			portio_kmsg(msg);
		} else {
			portio_kmsg("nosd-fm6000: portio: cmd written (no rp; blind)");
			iopl(0);
			return FM_OK;
		}

		struct timespec ts2 = { 0, 200000000L };   /* 200 ms */
		nanosleep(&ts2, NULL);

		/* Re-check link active before next write attempt. */
		{
			uint16_t lnk2 = bus0_read_link_status(rp_dev, pcie_cap);
			if (!(lnk2 & (1u << 13))) {
				snprintf(msg, sizeof(msg),
					 "nosd-fm6000: portio: link still down"
					 " after 200 ms (attempt %d); waiting",
					 attempt + 1);
				portio_kmsg(msg);
				continue;
			}
		}
	}

	portio_kmsg("nosd-fm6000: portio: link stayed down; cmd write failed");
	iopl(0);
	return FM_ERR;
}

void fm_clear_offbus(struct fm6000 *d)
{
	d->offbus = 0;
}

int fm_alive(struct fm6000 *d)
{
	uint32_t v;

	if (d->regs == NULL)
		return FM_ERR;

	if (d->xport == FM_XPORT_PCIE) {
		int off = fm_check_offbus(d);
		return off < 0 ? off : !off;
	}

	/* Local bus: read the strap directly, going around guard() -- which
	 * would refuse once the latch is set, and the whole point of this call
	 * is to find out whether that latch is still true. */
	v = nosaic_mmio_rd32((const void *)((const char *)d->regs +
					    FM6000_PIN_STRAP * 4));
	if (v != FM6000_PIN_STRAP_VALUE) {
		d->offbus = 1;
		return 0;
	}
	return 1;
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
	return word >= FM6000_BANK_STATS_BASE &&
	       word < FM6000_BANK_STATS_BASE + FM6000_BANK_STATS_SPAN;
}

const char *fm_block_name(uint32_t word)
{
	if (word >= FM6000_BLK_ESCHED &&
	    word < FM6000_BLK_ESCHED + FM6000_BLK_ESCHED_SPAN)
		return "ESCHED";
	if (word >= FM6000_BLK_SSCHED && word < FM6000_BLK_SSCHED + 0x1000)
		return "SSCHED";
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
	if (fm_is_bank(word))
		return "STATS (bank memory)";
	if (word >= FM6000_BLK_MCAST_MID && word < FM6000_BLK_MCAST_MID + 0x1000)
		return "0x240000 block";
	if (word >= FM6000_BLK_MCAST_POST && word < FM6000_BLK_MCAST_POST + 0x1000)
		return "0x260000 block";
	return "?";
}

int fm_open_auto(struct fm6000 *d, const char *slot, const char *scd_slot)
{
	if (fm_open(d, slot) == FM_OK)
		return FM_OK;
	return fm_open_lbus(d, scd_slot);
}

void fm_boot_mark_done(struct fm6000 *d)
{
	d->boot_done = 1;
}

int fm_mem_fill_paced(struct fm6000 *d, uint32_t base, uint32_t words,
		      uint32_t val, unsigned every)
{
	int saved, rv = FM_OK;
	uint32_t i;

	if (d->regs == NULL)
		return FM_ERR;
	if (d->offbus)
		return FM_EOFFBUS;
	if ((uint64_t)(base + words) * 4 > d->bar_bytes)
		return FM_ERR;
	if (every == 0)
		return fm_mem_fill(d, base, words, val);

	saved = d->check_writes;
	d->check_writes = 0;
	for (i = 0; i < words; i++) {
		nosaic_mmio_wr32((void *)((char *)d->regs + (base + i) * 4), val);
		d->writes++;
		if ((i % every) == every - 1) {
			struct timespec ts = { 0, 1000 };

			nosaic_mmio_barrier();
			/* A real gap, not just an ordering fence. The barrier
			 * alone was measured insufficient; see pci.h. */
			nanosleep(&ts, NULL);
		}
	}
	nosaic_mmio_barrier();
	d->check_writes = saved;
	if (!fm_alive(d))
		rv = FM_EOFFBUS;
	return rv;
}

int fm_mem_fill(struct fm6000 *d, uint32_t base, uint32_t words, uint32_t val)
{
	int saved, rv = FM_OK;
	uint32_t i;

	if (d->regs == NULL)
		return FM_ERR;
	if (d->offbus)
		return FM_EOFFBUS;
	if ((uint64_t)(base + words) * 4 > d->bar_bytes)
		return FM_ERR;

	saved = d->check_writes;
	d->check_writes = 0;
	for (i = 0; i < words; i++) {
		nosaic_mmio_wr32((void *)((char *)d->regs + (base + i) * 4), val);
		d->writes++;
	}
	nosaic_mmio_barrier();
	d->check_writes = saved;

	/* The one check the burst owes. On the local bus a dead chip answers
	 * zeros rather than ones, so ask the liveness beacon rather than config
	 * space -- fm_alive() picks the right question for the transport. */
	if (fm_alive(d) == 0)
		rv = FM_EOFFBUS;
	return rv;
}

const char *fm_hazard(const struct fm6000 *d, uint32_t word, int writing)
{
	/* Measured fatal to write, and reading has never been tried, so both
	 * directions are refused -- but for different reasons, and the message
	 * says which. */
	if (word == FM6000_FATAL_WRITE_1 || word == FM6000_FATAL_WRITE_2)
		return writing
		       ? "writing this word is measured to take the chip off "
			 "the bus (2026-09-25)"
		       : "reading this word has never been tried and writing it "
			 "is fatal; not the place to find out";

	/* EPL is the one block whose hazard the documented boot sequence
	 * actually clears, so it is checked against its own flag rather than
	 * against the bank one. */
	/* The EPL hazard is a READ hazard: the lane bring-up writes all over
	 * this block before the boot flag is set, and must not be refused. */
	if (!writing &&
	    word >= FM6000_BLK_EPL && word < FM6000_BLK_EPL + FM6000_BLK_EPL_SPAN)
		return d->boot_done ? NULL :
		       "EPL block: READING it before the cold boot has run takes "
		       "the chip off the bus (measured 2026-09-25 at 0x0e3b02). "
		       "Run fm_boot_cold() first";
	if (d->banks_ready)
		return NULL;
	if (fm_is_bank(word))
		return "ECC bank memory, uninitialised: one access takes the "
		       "chip off the PCIe bus";
	/*
	 * The egress scheduler, which is unreachable until the scheduler ring
	 * circulates. Both directions, because both are fatal -- a read at
	 * once, a write once the bridge's queue of writes that can never
	 * retire is full.
	 */
	if (word >= FM6000_BLK_ESCHED &&
	    word < FM6000_BLK_ESCHED + FM6000_BLK_ESCHED_SPAN)
		return d->sched_ready ? NULL :
		       "ESCHED: the scheduler ring is not circulating, so "
		       "nothing in this block can complete an access. Touching "
		       "it takes the chip off the bus. Run the ring init first";
	return NULL;
}

void fm_sched_mark_ready(struct fm6000 *d)
{
	d->sched_ready = 1;
}

int fm_rd_hazardous(struct fm6000 *d, uint32_t word, uint32_t *out)
{
	size_t off = (size_t)word * 4;

	if (d->regs == NULL)
		return FM_ERR;
	if (d->offbus) {
		d->refused++;
		return FM_EOFFBUS;
	}
	if (off + 4 > d->bar_bytes)
		return FM_ERR;

	*out = nosaic_mmio_rd32((const void *)((const char *)d->regs + off));
	d->reads++;
	return FM_OK;
}

int fm_wr_hazardous(struct fm6000 *d, uint32_t word, uint32_t val)
{
	size_t off = (size_t)word * 4;

	if (d->regs == NULL)
		return FM_ERR;
	if (d->offbus) {
		d->refused++;
		return FM_EOFFBUS;
	}
	if (off + 4 > d->bar_bytes)
		return FM_ERR;

	nosaic_mmio_wr32((void *)((char *)d->regs + off), val);
	d->writes++;
	return FM_OK;
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
static int guard(struct fm6000 *d, uint32_t word, size_t byte_off, int writing)
{
	if (d->regs == NULL)
		return FM_ERR;
	if (d->offbus) {
		d->refused++;
		return FM_EOFFBUS;
	}
	if (byte_off + 4 > d->bar_bytes)
		return FM_ERR;
	if (fm_hazard(d, word, writing) != NULL) {
		d->refused++;
		return FM_EUNSAFE;
	}
	return FM_OK;
}

int fm_rd_byte(struct fm6000 *d, uint32_t off, uint32_t *out)
{
	uint32_t v;
	int rv = guard(d, off / 4, off, 0);

	if (rv != FM_OK)
		return rv;

	v = nosaic_mmio_rd32((const void *)((const char *)d->regs + off));
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
	int rv = guard(d, off / 4, off, 1);

	if (rv != FM_OK)
		return rv;

	nosaic_mmio_wr32((void *)((char *)d->regs + off), val);
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

/*
 * The watchdog registers live at words 6, 7 and 8, below everything the
 * hazard table covers, and they are readable whenever the chip answers at all
 * -- they are in the management module, which is the thing that comes back
 * after a self-reset. Read them plainly.
 */
int fm_fatal_read(struct fm6000 *d, struct fm_fatal *out)
{
	int rv;

	if (d == NULL || out == NULL)
		return FM_ERR;

	out->code = out->last = out->count = 0;
	out->readable = 0;

	rv = fm_rd(d, FM6000_FATAL_CODE, &out->code);
	if (rv != FM_OK)
		return rv;
	rv = fm_rd(d, FM6000_LAST_FATAL_CODE, &out->last);
	if (rv != FM_OK)
		return rv;
	rv = fm_rd(d, FM6000_FATAL_COUNT, &out->count);
	if (rv != FM_OK)
		return rv;

	out->readable = 1;
	return FM_OK;
}

/*
 * Walk the PCIe capability list of the upstream port (0000:00:04.0) and
 * return the byte offset of the PCIe Capability structure, or -1 on failure.
 */
static int upstream_pcie_cap_offset(int fd)
{
	uint8_t next;

	if (pread(fd, &next, 1, 0x34) != 1)
		return -1;
	next &= ~3u;  /* capability pointers are DWORD-aligned */

	for (int guard = 0; guard < 48 && next >= 0x40; guard++) {
		uint8_t id;
		if (pread(fd, &id, 1, next) != 1)
			return -1;
		if (id == PCI_CAP_ID_PCIE)
			return (int)(unsigned)next;
		if (pread(fd, &next, 1, next + 1) != 1)
			return -1;
		next &= ~3u;
	}
	return -1;
}

/*
 * Set or clear the Link Disable bit (bit 4) in the upstream port's Link
 * Control register.  Setting it prevents pciehp from seeing any link-up
 * event while the FM6000 SerDes is initialising; clearing it lets LTSSM
 * complete link training from a stable FM6000 endpoint.
 *
 * Returns FM_OK on success, FM_ERR if the config file could not be opened
 * or if no PCIe capability was found.
 */
int fm_upstream_link_disable(int disable)
{
	int fd, cap;
	uint16_t lnkctl;

	fd = open(UPSTREAM_PORT, O_RDWR);
	if (fd < 0)
		return FM_ERR;

	cap = upstream_pcie_cap_offset(fd);
	if (cap < 0) {
		close(fd);
		return FM_ERR;
	}

	/* Link Control is at cap_base + 0x10 (PCIe spec table 7-19). */
	if (pread(fd, &lnkctl, 2, cap + 0x10) != 2) {
		close(fd);
		return FM_ERR;
	}

	if (disable) {
		lnkctl |= (1u << 4);   /* set Link Disable */
	} else {
		/*
		 * Clear Link Disable ONLY.  Do NOT set Retrain Link (bit 5).
		 *
		 * Retrain Link drives the LTSSM to the Recovery state, which is
		 * the correct trigger for retraining a link already in L0.
		 * When Link Disable is being cleared, the LTSSM is in the
		 * Disabled state; setting Retrain Link simultaneously sends
		 * conflicting directives and prevents the LTSSM from completing
		 * the natural Disabled → Detect → Polling → L0 sequence.
		 * Clearing bit 4 alone lets the AMD RS780 restart LTSSM from
		 * Detect automatically per PCIe spec §4.2.6.3.1.  [install89]
		 */
		lnkctl &= ~(uint16_t)(1u << 4); /* clear Link Disable only */
	}

	if (pwrite(fd, &lnkctl, 2, cap + 0x10) != 2) {
		close(fd);
		return FM_ERR;
	}

	/* Readback to confirm hardware accepted the write. */
	{
		uint16_t rb = 0;
		char msg[128];
		(void)pread(fd, &rb, 2, cap + 0x10);
		snprintf(msg, sizeof msg,
			 "nosd-fm6000: pre-DMA: link_disable %s lnkctl=0x%04x (wrote 0x%04x)",
			 disable ? "set" : "clear", rb, lnkctl);
		fprintf(stderr, "%s\n", msg);
	}

	close(fd);
	return FM_OK;
}

uint32_t fm_fatal_count(struct fm6000 *d)
{
	struct fm_fatal f;

	if (fm_fatal_read(d, &f) != FM_OK)
		return 0;
	return f.count;
}

/*
 * Assert or deassert Secondary Bus Reset on the FM6000's upstream port
 * (0000:00:04.0 Bridge Control register, bit 6).  SBR asserts PERST# to
 * the FM6000 slot, which physically resets the FM6000's PCIe endpoint
 * and forces the AMD RS780's LTSSM back to Detect — clearing the sticky
 * DL_Active bit that persists when the link was previously in L0.
 *
 * After deassert the caller must wait for the FM6000 to exit reset
 * (~100 ms), then clear Link Disable to let LTSSM train naturally.
 *
 * Bridge Control is at PCI config offset 0x3E of the bridge device;
 * this is bus 0 (RS780 internal) — always safe to access via config file.
 *
 * assert=1 → set SBR bit; assert=0 → clear SBR bit.
 */
#define UPSTREAM_BRIDGE_CTRL_OFF  0x3e
#define UPSTREAM_BRIDGE_CTRL_SBR  (1u << 6)

int fm_upstream_sbr(int assert)
{
	int fd;
	uint16_t bc;
	ssize_t n;

	fd = open(UPSTREAM_PORT, O_RDWR);
	if (fd < 0)
		return FM_ERR;

	if (pread(fd, &bc, 2, UPSTREAM_BRIDGE_CTRL_OFF) != 2) {
		close(fd);
		return FM_ERR;
	}

	if (assert)
		bc |= UPSTREAM_BRIDGE_CTRL_SBR;
	else
		bc &= ~UPSTREAM_BRIDGE_CTRL_SBR;

	n = pwrite(fd, &bc, 2, UPSTREAM_BRIDGE_CTRL_OFF);
	if (n != 2) {
		close(fd);
		return FM_ERR;
	}

	/* Verify: read back immediately to confirm hardware accepted the bit. */
	{
		uint16_t rb = 0;
		char msg[128];
		(void)pread(fd, &rb, 2, UPSTREAM_BRIDGE_CTRL_OFF);
		snprintf(msg, sizeof msg,
			 "nosd-fm6000: pre-DMA: SBR %s bridge_ctrl=0x%04x (wrote 0x%04x)",
			 assert ? "assert" : "deassert", rb, bc);
		fprintf(stderr, "%s\n", msg);
	}

	close(fd);
	return FM_OK;
}

/*
 * Clear the three pciehp interrupt enables — HPIE, DLLSCE, PDCE — in the
 * upstream port's Slot Control register.
 *
 * After the FM6000 is removed from the PCIe device tree (via sysfs/remove),
 * pciehp transitions to EMPTY state and re-arms all three bits so it can
 * detect a device plugged into the now-vacant slot.  Subsequent chip FALTALs
 * (e.g. from ESCHED CFG_2 or pipeline SRAM writes) bounce the PCIe link.
 * With HPIE live, pciehp's slot_interrupt handler fires on the link-up edge
 * and issues a CF8/CFC Config Space read to bus 2 (FM6000) while the link is
 * still training.  On AMD RS780 that CF8/CFC stalls indefinitely when SERR#
 * is disabled (no error completion) → kernel deadlock.
 *
 * Clearing the three bits directly from user-space — accessing bus 0 (the
 * upstream bridge), which is handled locally by the root complex and never
 * stalls — quiesces pciehp without re-enumerating the FM6000 endpoint.
 * Unlike a PCIe rescan, this path issues no CF8/CFC to bus 2.
 * [install137, 2026-10-27]
 */
#define PCI_EXP_SLTCTL_OFF	0x18	/* SLTCTL at cap_base + 0x18 */
#define SLTCTL_HPIE		(1u << 0)
#define SLTCTL_PDCE		(1u << 3)
#define SLTCTL_DLLSCE		(1u << 12)

int fm_pcie_sltctl_clear_hpie(void)
{
	int fd, cap;
	uint16_t sltctl;

	fd = open(UPSTREAM_PORT, O_RDWR);
	if (fd < 0)
		return FM_ERR;

	cap = upstream_pcie_cap_offset(fd);
	if (cap < 0) {
		close(fd);
		return FM_ERR;
	}

	if (pread(fd, &sltctl, 2, cap + PCI_EXP_SLTCTL_OFF) != 2) {
		close(fd);
		return FM_ERR;
	}

	sltctl &= ~(uint16_t)(SLTCTL_HPIE | SLTCTL_PDCE | SLTCTL_DLLSCE);

	if (pwrite(fd, &sltctl, 2, cap + PCI_EXP_SLTCTL_OFF) != 2) {
		close(fd);
		return FM_ERR;
	}

	close(fd);
	return FM_OK;
}

/*
 * Unbind or rebind pcieport from/to the FM6000's upstream root port
 * (0000:00:04.0).  Unbinding removes the AER interrupt handler and pciehp
 * service so that neither can issue Config Space reads to bus 2 during
 * fm_pcie_serdes_init's ~5 s link-training window.
 *
 * On AMD RS780, any CF8/CFC Config Space access to a device whose link is
 * down (no Completion) loops forever — there is no hardware Completion
 * Timeout on the legacy I/O port path.  The AER core's find_source_device()
 * call scans bus 2 to locate the error source; that scan stalls the CPU and
 * freezes the kernel.  Removing pcieport before training prevents the scan.
 *
 * The pcieport struct pci_dev is NOT freed by unbind; pdev->bus->self in
 * fm6000_kdrv.probe() remains valid and its Config Space (bus 0, handled
 * locally by the root complex) is always safe to access.
 *
 * bind=0 → unbind; bind=1 → bind.
 */
#define PCIEPORT_UNBIND "/sys/bus/pci/drivers/pcieport/unbind"
#define PCIEPORT_BIND   "/sys/bus/pci/drivers/pcieport/bind"
#define UPSTREAM_BDF    "0000:00:04.0"

/*
 * Poll the upstream port's Link Status register (0000:00:04.0 config space,
 * bus 0 — always safe) until Current Link Speed is non-zero AND
 * DL_Active = 1, indicating the link has reached L0.  Returns FM_OK as soon
 * as the link is active, or FM_ERR after max_ms.  Logs the final Link Status.
 */
int fm_wait_link_active(int max_ms)
{
	char msg[128];
	uint16_t ls = 0;
	int fd, cap, i, n_polls;

	fd = open(UPSTREAM_PORT, O_RDONLY);
	if (fd < 0)
		return FM_ERR;
	cap = upstream_pcie_cap_offset(fd);
	if (cap < 0) {
		close(fd);
		return FM_ERR;
	}

	n_polls = max_ms / 50;
	for (i = 0; i < n_polls; i++) {
		struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000000L };

		if (pread(fd, &ls, 2, cap + 0x12) != 2)
			break;
		/* DL_Active = bit 12; Current Link Speed = bits [3:0] */
		if ((ls & 0x1000) && (ls & 0x000f)) {
			close(fd);
			snprintf(msg, sizeof msg,
				 "nosd-fm6000: pre-DMA: link active lnksta=0x%04x speed=%u width=%u",
				 ls, ls & 0xf, (ls >> 4) & 0x3f);
			fprintf(stderr, "%s\n", msg);
			return FM_OK;
		}
		nanosleep(&ts, NULL);
	}

	close(fd);
	snprintf(msg, sizeof msg,
		 "nosd-fm6000: pre-DMA: link NOT active after %dms lnksta=0x%04x",
		 max_ms, ls);
	fprintf(stderr, "%s\n", msg);
	return FM_ERR;
}

int fm_pcieport_bind(int bind)
{
	const char *path = bind ? PCIEPORT_BIND : PCIEPORT_UNBIND;
	int fd;
	ssize_t n;

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return FM_ERR;
	n = write(fd, UPSTREAM_BDF, sizeof(UPSTREAM_BDF) - 1);
	close(fd);
	return (n == sizeof(UPSTREAM_BDF) - 1) ? FM_OK : FM_ERR;
}

/* ---- AMD SB700 Global SMI disable (install208) --------------------------- */
/*
 * The AMD SB700 fires a periodic SMI every ~3-4 s via a hardware PM timer.
 * The SMM handler reads the FM6000 root port (bus 2) via CF8/CFC to check
 * for errors.  If the FM6000 link is bouncing during DMA init, that CF8 read
 * stalls the CPU in SMM permanently.
 *
 * Disabling GBL_SMI_EN (bit 0 of PMIO+0x34) prevents any SMI from firing
 * during the ~30 ms DMA init window.  PMIO base is read from the SB700 LPC
 * bridge (D14F3, PCI config offset 0x60, bits[15:1]).
 *
 * AMD SB700 PMIO layout: +0x30=SMI_STS (RW1C status), +0x34=SMI_EN (enable).
 * Intel ICH has them swapped (+0x30=SMI_EN, +0x34=SMI_STS).
 *
 * This is safe: PMIO+0x34 accesses are plain I/O port writes (not CF8), so
 * they cannot themselves trigger the CF8-trap SMI.  We restore the saved
 * value immediately after fm_dma_init returns.
 */

static uint16_t g_pmio_base    = 0;
static uint32_t g_saved_smi_en = 0xFFFFFFFFu;  /* sentinel = not disabled */
static int      g_smi_via_mmio = 0;            /* 1 = disabled via FCH MMIO, not PMIO */

static uint16_t sb700_pmio_base(void)
{
	unsigned char buf[4];
	ssize_t n;
	int fd;

	/* Method 1: SB700 LPC bridge via sysfs (0000:00:14.3, config+0x24).
	 * AMD SB700 LPC D14F3 reg 0x24 = PM IO decode: bit[0]=PMIOEn,
	 * bits[15:1]=PMIOBase.  NOT reg 0x60 (that is KBC/serial decode). */
	fd = open("/sys/bus/pci/devices/0000:00:14.3/config", O_RDONLY);
	if (fd >= 0) {
		if (lseek(fd, 0x24, SEEK_SET) == (off_t)0x24 && read(fd, buf, 2) == 2) {
			uint16_t v = (uint16_t)((unsigned)buf[0] | ((unsigned)buf[1] << 8));
			close(fd);
			{
				char _lm[96];
				snprintf(_lm, sizeof _lm,
					"nosd-fm6000: install211: D14F3@0x24 raw=0x%04x"
					" PMIOEn=%u base=0x%04x [method1]",
					(unsigned)v, (unsigned)(v & 1u),
					(unsigned)(v & 0xFFFEu));
				portio_kmsg(_lm);
			}
			if ((v & 1u) && (v & 0xFFFEu)) {
				portio_kmsg("nosd-fm6000: install211: PMIO base from LPC sysfs @0x24");
				return v & 0xFFFEu;
			}
		} else {
			close(fd);
		}
	}

	/* Method 2: ACPI FADT PM1a_EVT_BLK at offset 0x38 = PMIO base on SB700.
	 * The FADT is authoritative; PM1a event block starts at PMIO+0x00. */
	fd = open("/sys/firmware/acpi/tables/FACP", O_RDONLY);
	if (fd >= 0) {
		if (lseek(fd, 0x38, SEEK_SET) == (off_t)0x38 && read(fd, buf, 4) == 4) {
			uint32_t pm1a = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8)
				      | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
			close(fd);
			if (pm1a && pm1a < 0x10000u) {
				portio_kmsg("nosd-fm6000: install208: PMIO base from ACPI FADT");
				return (uint16_t)(pm1a & 0xFFFEu);
			}
		} else {
			close(fd);
		}
	}

	/* Method 3: direct CF8/CFC portio read of LPC bridge (bus=0 — safe).
	 * SB700 LPC at 0000:00:14.3: CF8 = 0x8000A324 (bus=0,dev=20,fn=3,reg=0x24).
	 * iopl(3) must already be in effect (fm_pcie_serr_disable() raised it). */
	{
		uint32_t cf8 = 0x8000A324u;  /* bus=0, dev=20, fn=3, reg=0x24 */
		uint32_t data;
		outl(cf8, 0xCF8);
		data = inl(0xCFC);
		if (data != 0xFFFFFFFFu) {
			uint16_t v = (uint16_t)(data & 0xFFFFu);
			if ((v & 1u) && (v & 0xFFFEu)) {
				portio_kmsg("nosd-fm6000: install211: PMIO base from CF8 portio @0x24");
				return v & 0xFFFEu;
			}
		}
	}

	/*
	 * Method 4: BIOS left PMIOEn=0 (D14F3 reg 0x24 bit 0).  Enable it by
	 * writing PMIOBASE=0x0400 | PMIOEn=1 (= 0x0401) via sysfs config write.
	 * sysfs uses MMCONFIG or CF8 internally — either way, this fires one SMI
	 * before we clear GBL_SMI_EN, but fm_pcie_clear_bridge_status_sysfs() was
	 * already called so no CTO latch is pending and SMM exits normally.
	 */
	fd = open("/sys/bus/pci/devices/0000:00:14.3/config", O_RDWR);
	if (fd >= 0) {
		unsigned char wbuf[2];
		uint16_t enable_val = 0x0401u;  /* PMIOBASE=0x0400, PMIOEn=1 */
		wbuf[0] = (unsigned char)(enable_val & 0xFFu);
		wbuf[1] = (unsigned char)(enable_val >> 8);
		if (lseek(fd, 0x24, SEEK_SET) == (off_t)0x24 && write(fd, wbuf, 2) == 2) {
			/* Verify the write took */
			unsigned char rbuf[2];
			if (lseek(fd, 0x24, SEEK_SET) == (off_t)0x24 && read(fd, rbuf, 2) == 2) {
				uint16_t v = (uint16_t)((unsigned)rbuf[0] | ((unsigned)rbuf[1] << 8));
				close(fd);
				if ((v & 1u) && (v & 0xFFFEu)) {
					portio_kmsg("nosd-fm6000: install211: PMIO enabled via sysfs D14F3@0x24");
					return v & 0xFFFEu;
				}
				portio_kmsg("nosd-fm6000: install211: WARNING: PMIO enable write did not stick");
			} else {
				close(fd);
			}
		} else {
			close(fd);
		}
	}

	/*
	 * Method 4.5: Try direct PMIO I/O port read with known base 0x0400
	 * (SB700 default) even if PMIOEn=0 in D14F3@0x24.  Some SB700 revisions
	 * decode PMIO at the standard base regardless of the enable bit; the BIOS
	 * SMM handler also accesses PMIO+0x34 directly without going through CF8.
	 * This I/O port read does NOT fire the CF8 I/O-port SMI trap; iopl(3) is
	 * raised defensively since it may not have been acquired yet.
	 * [install212, 2026-11-09]
	 */
	if (iopl(3) == 0) {
		static const uint16_t try_bases[] = { 0x0400u, 0x0800u, 0x0C00u, 0 };
		for (int _bi = 0; try_bases[_bi]; _bi++) {
			uint16_t tb = try_bases[_bi];
			uint32_t sv = inl(tb + 0x34u);
			char _lm[128];
			snprintf(_lm, sizeof _lm,
				"nosd-fm6000: install211: direct PMIO"
				" @0x%04x+0x34=0x%08x [method4_5]",
				(unsigned)tb, (unsigned)sv);
			portio_kmsg(_lm);
			if (sv != 0xFFFFFFFFu) {
				/* PMIO decoded at this base — found it. */
				portio_kmsg("nosd-fm6000: install211: PMIO base from direct I/O [method4_5]");
				return tb;
			}
		}
	}

	return 0;
}

int fm_pcie_smi_global_disable(void)
{
	char msg[128];
	uint16_t base;
	uint32_t smi_en;

	g_saved_smi_en = 0xFFFFFFFFu;

	base = sb700_pmio_base();
	if (base) {
		g_pmio_base = base;

		/* iopl(3) already raised by fm_pcie_serr_disable() before us; call it
		 * again defensively in case this function is ever called standalone. */
		if (iopl(3) != 0) {
			portio_kmsg("nosd-fm6000: install208: WARNING: iopl failed, SMI not disabled");
			return 0;
		}

		smi_en = inl(base + 0x34u);         /* AMD SB700 SMI_EN at PMIO+0x34 */
		g_saved_smi_en = smi_en;
		outl(smi_en & ~1u, base + 0x34u);  /* clear GBL_SMI_EN */

		snprintf(msg, sizeof msg,
			"nosd-fm6000: install208: SMI_EN 0x%08x -> 0x%08x (pmio=0x%04x) [install208]",
			(unsigned)smi_en, (unsigned)(smi_en & ~1u), (unsigned)base);
		portio_kmsg(msg);
		return 1;
	}

	/*
	 * Method 5: PMIO decode disabled (or BIOS SMM handler restores PMBASE).
	 * AMD SB700 FCH has a fixed ACPI MMIO aperture at 0xFED80000 that is
	 * always decoded in hardware, independent of PMIO decode enable.  The PM
	 * block within that space starts at +0x200; SMI_ENABLE_0 (mirror of
	 * PMIO+0x34, GBL_SMI_EN = bit 0) is at 0xFED80234.  Accessing it via
	 * /dev/mem does not go through CF8/CFC so it cannot itself fire an SMI.
	 */
	{
		int memfd = open("/dev/mem", O_RDWR | O_SYNC);
		if (memfd >= 0) {
			/*
			 * Map the full 4 KB page at 0xFED80000.  0xFED80000 is
			 * already page-aligned so no rounding needed.  offset
			 * 0x234 within the mapping reaches SMI_ENABLE_0.
			 */
			void *fch = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
					 MAP_SHARED, memfd,
					 (off_t)0xFED80000UL);
			close(memfd);
			if (fch != MAP_FAILED) {
				volatile uint32_t *smi_reg =
					(volatile uint32_t *)
					((unsigned char *)fch + 0x234u);
				smi_en = *smi_reg;
				g_saved_smi_en = smi_en;
				g_smi_via_mmio = 1;
				*smi_reg = smi_en & ~1u;
				__sync_synchronize();
				uint32_t check = *smi_reg;
				munmap(fch, 4096);
				snprintf(msg, sizeof msg,
					"nosd-fm6000: install211: FCH-MMIO SMI_EN "
					"0x%08x->0x%08x check=0x%08x [method5]",
					(unsigned)smi_en,
					(unsigned)(smi_en & ~1u),
					(unsigned)check);
				portio_kmsg(msg);
				if (!(check & 1u))
					return 1;
				portio_kmsg("nosd-fm6000: install211: "
					    "WARNING: FCH-MMIO GBL_SMI_EN did not clear");
				g_smi_via_mmio = 0;
				g_saved_smi_en = 0xFFFFFFFFu;
			}
		}
	}

	portio_kmsg("nosd-fm6000: install208: WARNING: PMIO base=0 and FCH-MMIO failed, SMI not disabled");
	return 0;
}

void fm_pcie_smi_global_restore(void)
{
	char msg[128];

	if (g_saved_smi_en == 0xFFFFFFFFu) {
		portio_kmsg("nosd-fm6000: install208: SMI restore skipped (was not disabled)");
		return;
	}

	if (g_smi_via_mmio) {
		int memfd = open("/dev/mem", O_RDWR | O_SYNC);
		if (memfd >= 0) {
			void *fch = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
					 MAP_SHARED, memfd, (off_t)0xFED80000UL);
			close(memfd);
			if (fch != MAP_FAILED) {
				volatile uint32_t *smi_reg =
					(volatile uint32_t *)
					((unsigned char *)fch + 0x234u);
				*smi_reg = g_saved_smi_en;
				__sync_synchronize();
				munmap(fch, 4096);
			}
		}
		snprintf(msg, sizeof msg,
			"nosd-fm6000: install208: SMI_EN restored to 0x%08x (FCH-MMIO) [install208]",
			(unsigned)g_saved_smi_en);
	} else {
		outl(g_saved_smi_en, g_pmio_base + 0x34u);
		snprintf(msg, sizeof msg,
			"nosd-fm6000: install208: SMI_EN restored to 0x%08x (pmio=0x%04x) [install208]",
			(unsigned)g_saved_smi_en, (unsigned)g_pmio_base);
	}

	portio_kmsg(msg);
	g_saved_smi_en = 0xFFFFFFFFu;
	g_smi_via_mmio = 0;
}
