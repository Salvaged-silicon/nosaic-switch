/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "bde.h"
#include "mmio.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/mman.h>
#include <unistd.h>

#define BCM_VENDOR 0x14e4

static long sysfs_hex(const char *dir, const char *name)
{
	char path[512], buf[64];
	int fd, n;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	if ((fd = open(path, O_RDONLY)) < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	return strtol(buf, NULL, 16);
}

/* BAR0's length, from the resource file rather than by writing the BAR and
 * reading back the size mask -- which is the classic method and disturbs a
 * device that may already be running. */
static size_t bar0_len(const char *bdf)
{
	char path[512], line[256];
	unsigned long long start = 0, end = 0, flags = 0;
	FILE *f;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource", bdf);
	if (!(f = fopen(path, "r")))
		return 0;
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return 0;
	}
	fclose(f);
	if (sscanf(line, "%llx %llx %llx", &start, &end, &flags) != 3 || end <= start)
		return 0;
	return (size_t)(end - start + 1);
}

static int find_device(char *out, size_t outlen, uint16_t *devid)
{
	DIR *d = opendir("/sys/bus/pci/devices");
	struct dirent *e;
	int found = 0;

	if (!d)
		return -1;
	while ((e = readdir(d))) {
		char cand[512];
		long id;

		if (e->d_name[0] == '.')
			continue;
		/* A bus address is "0001:01:00.0" and nothing longer is one, so a
		 * name that does not fit is not a device we are looking for --
		 * checked rather than truncated, because a truncated bus address
		 * names a different device or none. */
		if (strlen(e->d_name) >= outlen)
			continue;
		snprintf(cand, sizeof(cand), "/sys/bus/pci/devices/%s", e->d_name);
		if (sysfs_hex(cand, "vendor") != BCM_VENDOR)
			continue;
		id = sysfs_hex(cand, "device");
		if (id < 0)
			continue;
		memcpy(out, e->d_name, strlen(e->d_name) + 1);
		*devid = (uint16_t)id;
		found = 1;
		break;
	}
	closedir(d);
	return found ? 0 : -1;
}

uint32_t nosaic_tdp_bde_rd(struct nosaic_tdp_bde *b, uint32_t off)
{
	if ((size_t)off + 4 > b->bar_len) {
		fprintf(stderr, "nosd-tdp: read past BAR0: %#x\n", off);
		return 0xffffffffu;
	}
	return nosaic_mmio_rd32((volatile char *)b->bar + off);
}

void nosaic_tdp_bde_wr(struct nosaic_tdp_bde *b, uint32_t off, uint32_t v)
{
	if ((size_t)off + 4 > b->bar_len) {
		fprintf(stderr, "nosd-tdp: write past BAR0: %#x\n", off);
		return;
	}
	nosaic_mmio_wr32((volatile char *)b->bar + off, v);
}

/* Read a big-endian value of `cells` 32-bit words. Device tree property data
 * is big-endian regardless of the host, though on this board they agree. */
static uint64_t be_cells(const unsigned char *p, int cells)
{
	uint64_t v = 0;
	for (int i = 0; i < cells * 4; i++)
		v = (v << 8) | p[i];
	return v;
}

int nosaic_tdp_bde_map_dma(struct nosaic_tdp_bde *b)
{
	const char *base = "/proc/device-tree/reserved-memory";
	DIR *d = opendir(base);
	struct dirent *e;
	unsigned char reg[32];
	int found = 0;

	if (!d) {
		fprintf(stderr, "nosd-tdp: no %s: this kernel has no reserved memory\n", base);
		return -1;
	}
	while ((e = readdir(d)) && !found) {
		char path[512];
		int fd, n;

		if (strncmp(e->d_name, "nosaic-dma", 10) != 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s/reg", base, e->d_name);
		if ((fd = open(path, O_RDONLY)) < 0)
			continue;
		n = read(fd, reg, sizeof(reg));
		close(fd);
		/* #address-cells = 2 and #size-cells = 2 at the root, so the
		 * property is four 32-bit words. */
		if (n < 16)
			continue;
		b->dma_phys = be_cells(reg, 2);
		b->dma_len  = (size_t)be_cells(reg + 8, 2);
		found = 1;
	}
	closedir(d);

	if (!found) {
		fprintf(stderr, "nosd-tdp: no nosaic-dma node under %s\n", base);
		return -1;
	}

	b->mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (b->mem_fd < 0) {
		fprintf(stderr, "nosd-tdp: /dev/mem: %s\n", strerror(errno));
		return -1;
	}
	b->dma = mmap(NULL, b->dma_len, PROT_READ | PROT_WRITE, MAP_SHARED,
		      b->mem_fd, (off_t)b->dma_phys);
	if (b->dma == MAP_FAILED) {
		/* The likely cause is the region still being System RAM, which
		 * means the node lacks no-map: CONFIG_STRICT_DEVMEM then refuses
		 * it, and it refuses it with EPERM rather than anything that
		 * names the reason. */
		fprintf(stderr, "nosd-tdp: mapping the DMA pool at %#llx: %s\n",
			(unsigned long long)b->dma_phys, strerror(errno));
		b->dma = NULL;
		return -1;
	}
	return 0;
}

static uint32_t cfg_swap(uint32_t v);

/*
 * Bus mastering on every bridge between the chip and memory, not just the chip.
 *
 * A bridge's Bus Master Enable does not describe the bridge; it controls
 * whether the bridge forwards transactions from its secondary bus upstream. On
 * this board the switch sits behind the P2020's PCIe root port, and Linux binds
 * no driver to either -- so nothing calls pci_set_master() on either one, and
 * the root port came out of firmware with the bit clear.
 *
 * The chip's own bit was set and every descriptor was correct. Every DMA the
 * chip issued was then discarded one hop later, at a device nobody was looking
 * at, and each engine reported it in its own vocabulary:
 *
 *   soc_counter_thread: DMA did not finish buf32=0xb3a71600     (counters)
 *   soc_misc_init(0) returned -9 (Operation timed out)          (SLAM)
 *   Starting DV: c=0 dv=0x18761438  ... then nothing, forever   (packet TX)
 *
 * Three unrelated-looking failures, one cause, none of them naming it -- which
 * is why this cost days and why the check below is by hierarchy rather than by
 * device. Walking the sysfs path is what makes it general: it fixes the bridge
 * we know about and any other that turns up between here and memory.
 */
static void enable_upstream_mastering(const char *bdf)
{
	char link[PATH_MAX], path[PATH_MAX];
	char *seg, *save;
	ssize_t n;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s", bdf);
	n = readlink(path, link, sizeof(link) - 1);
	if (n < 0)
		return;
	link[n] = '\0';

	/* The symlink target spells out the hierarchy, parent first. Everything
	 * that looks like a BDF except the device itself is a bridge we are
	 * behind. */
	for (seg = strtok_r(link, "/", &save); seg; seg = strtok_r(NULL, "/", &save)) {
		unsigned dom, bus, dev, fn;
		uint32_t cmd, raw;
		int fd;

		if (sscanf(seg, "%x:%x:%x.%x", &dom, &bus, &dev, &fn) != 4)
			continue;
		if (!strcmp(seg, bdf))
			continue;

		snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/config", seg);
		fd = open(path, O_RDWR);
		if (fd < 0)
			continue;

		raw = 0;
		if (pread(fd, &raw, 4, 0x04) == 4) {
			cmd = cfg_swap(raw);
			if (!(cmd & (1u << 2))) {
				raw = cfg_swap(cmd | (1u << 2));
				if (pwrite(fd, &raw, 4, 0x04) == 4)
					printf("bus master   enabled on bridge %s\n", seg);
				else
					fprintf(stderr, "nosd-tdp: could not enable bus "
						"mastering on bridge %s: %s\n",
						seg, strerror(errno));
			}
		}
		close(fd);
	}
}

int nosaic_tdp_bde_open(struct nosaic_tdp_bde *b, const char *bdf)
{
	char path[512];

	memset(b, 0, sizeof(*b));
	b->bar_fd = b->cfg_fd = b->mem_fd = -1;

	if (bdf) {
		snprintf(b->bdf, sizeof(b->bdf), "%s", bdf);
		snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s", bdf);
		long id = sysfs_hex(path, "device");
		if (id < 0) {
			fprintf(stderr, "nosd-tdp: %s: no such PCI device\n", bdf);
			return -1;
		}
		b->device_id = (uint16_t)id;
	} else if (find_device(b->bdf, sizeof(b->bdf), &b->device_id) != 0) {
		fprintf(stderr, "nosd-tdp: no Broadcom device (vendor %#x) on the PCI bus\n",
			BCM_VENDOR);
		return -1;
	}

	b->bar_len = bar0_len(b->bdf);
	if (b->bar_len == 0) {
		fprintf(stderr, "nosd-tdp: %s has a zero-length BAR0\n", b->bdf);
		return -1;
	}

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource0", b->bdf);
	b->bar_fd = open(path, O_RDWR | O_SYNC);
	if (b->bar_fd < 0) {
		fprintf(stderr, "nosd-tdp: %s: %s\n", path, strerror(errno));
		return -1;
	}
	b->bar = mmap(NULL, b->bar_len, PROT_READ | PROT_WRITE, MAP_SHARED, b->bar_fd, 0);
	if (b->bar == MAP_FAILED) {
		fprintf(stderr, "nosd-tdp: mapping BAR0: %s\n", strerror(errno));
		b->bar = NULL;
		return -1;
	}

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/config", b->bdf);
	b->cfg_fd = open(path, O_RDWR);
	if (b->cfg_fd < 0) {
		fprintf(stderr, "nosd-tdp: %s: %s\n", path, strerror(errno));
		return -1;
	}

	/*
	 * Bus mastering, so the chip can reach host memory.
	 *
	 * The SDK programs most tables through SBUS DMA: it builds a descriptor
	 * in the DMA pool and has the chip fetch it. A device that cannot master
	 * the bus never fetches anything, and the symptom is not "DMA is off" --
	 * it is a table operation that times out, which reads as broken silicon
	 * or a bad port map. This board produced it as
	 *
	 *   soc_misc_init(0) returned -9 (Operation timed out)
	 *
	 * with everything else correct: chip identified, DMA pool mapped and
	 * readable, soc_attach completed.
	 *
	 * Nothing enables this for us. No kernel driver is bound to the device,
	 * and the firmware left it clear.
	 */
	{
		uint32_t cmd = nosaic_tdp_bde_cfg_read(b, 0x04);

		enable_upstream_mastering(b->bdf);

		if (!(cmd & (1u << 2))) {
			nosaic_tdp_bde_cfg_write(b, 0x04, cmd | (1u << 2));
			if (!(nosaic_tdp_bde_cfg_read(b, 0x04) & (1u << 2))) {
				fprintf(stderr, "nosd-tdp: bus mastering did not enable; "
					"COMMAND reads %#06x\n",
					(unsigned)nosaic_tdp_bde_cfg_read(b, 0x04));
				return -1;
			}
		}
	}
	return 0;
}

/*
 * PCI configuration space is little-endian by definition, whatever the host.
 *
 * sysfs hands back the raw bytes, so on a big-endian machine loading them into
 * a uint32_t gives the word reversed. That is not a subtle failure in one
 * place: it makes every capability, BAR and command bit wrong. It showed up as
 *
 *   bus mastering did not enable; COMMAND reads 0x6001000
 *
 * where 0x06001000 reversed is 0x00100006 -- STATUS 0x0010 and COMMAND 0x0006,
 * memory space and bus master both already set. The chip was fine and the
 * reader was not.
 *
 * The Trident2+ BDE does not do this and does not need to: x86 is
 * little-endian and the bytes land in the order they were meant.
 */
static uint32_t cfg_swap(uint32_t v)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	return ((v & 0xffu) << 24) | ((v & 0xff00u) << 8) |
	       ((v >> 8) & 0xff00u) | ((v >> 24) & 0xffu);
#else
	return v;
#endif
}

uint32_t nosaic_tdp_bde_cfg_read(struct nosaic_tdp_bde *b, uint32_t addr)
{
	uint32_t v = 0;

	if (pread(b->cfg_fd, &v, 4, addr) != 4)
		return 0xffffffffu;
	return cfg_swap(v);
}

void nosaic_tdp_bde_cfg_write(struct nosaic_tdp_bde *b, uint32_t addr, uint32_t data)
{
	uint32_t raw = cfg_swap(data);

	/* Reported rather than discarded: a configuration write that does not
	 * land is how bus mastering silently stays off, and the failure then
	 * appears much later as a table operation timing out. */
	if (pwrite(b->cfg_fd, &raw, 4, addr) != 4)
		fprintf(stderr, "nosd-tdp: PCI config write to %#x failed: %s\n",
			addr, strerror(errno));
}

void nosaic_tdp_bde_set_endian(struct nosaic_tdp_bde *b)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	uint32_t es = NOSAIC_ES_BE_HOST;

	nosaic_tdp_bde_wr(b, NOSAIC_CMIC_ENDIAN_SELECT, es);
	nosaic_mmio_barrier();
#else
	(void)b;   /* the chip's default already matches a little-endian host */
#endif
}

int nosaic_tdp_bde_selftest(struct nosaic_tdp_bde *b, uint32_t *raw)
{
	uint32_t v = nosaic_tdp_bde_rd(b, NOSAIC_CMIC_REVID_DEVID);

	if (raw)
		*raw = v;
	/* The device id occupies the low half and the revision sits above it.
	 * Compared against PCI rather than against a constant, so this stays
	 * true for every member of the family. */
	return (v & 0xffffu) == b->device_id ? 0 : -1;
}

void nosaic_tdp_bde_close(struct nosaic_tdp_bde *b)
{
	if (b->bar) {
		munmap((void *)b->bar, b->bar_len);
		b->bar = NULL;
	}
	if (b->dma) {
		munmap(b->dma, b->dma_len);
		b->dma = NULL;
	}
	if (b->bar_fd >= 0)
		close(b->bar_fd);
	if (b->cfg_fd >= 0)
		close(b->cfg_fd);
	if (b->mem_fd >= 0)
		close(b->mem_fd);
	b->bar_fd = b->cfg_fd = b->mem_fd = -1;
}
