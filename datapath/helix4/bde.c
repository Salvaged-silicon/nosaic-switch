/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "bde.h"
#include "mmio.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define TAG "nosd-helix4"

static int read_file(const char *path, char *buf, size_t len)
{
	int fd = open(path, O_RDONLY);
	int n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, len - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = 0;
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		buf[--n] = 0;
	return n;
}

/* A sysfs attribute holding one number, in whatever base it is written in.
 * UIO writes addresses and sizes as 0x-prefixed hex, so strtoull with base 0
 * reads both those and the plain decimals elsewhere. -1 if it is not there. */
static long long sysfs_num(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));

static long long sysfs_num(const char *fmt, ...)
{
	char path[512], buf[64];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(path, sizeof(path), fmt, ap);
	va_end(ap);

	if (read_file(path, buf, sizeof(buf)) <= 0)
		return -1;
	return (long long)strtoull(buf, NULL, 0);
}

/*
 * Find the UIO device.
 *
 * By name rather than by number. /dev/uio0 is whatever probed first, and a
 * board that grows a second UIO device -- which this one may, if the QSPI or
 * the watchdog ever get the same treatment -- would silently renumber the
 * switch chip. The name comes from the device tree node, so it is stable.
 */
static int find_uio(struct nosaic_helix4_bde *b, const char *want)
{
	DIR *d = opendir("/sys/class/uio");
	struct dirent *e;
	int found = 0;

	if (!d) {
		fprintf(stderr, TAG ": no /sys/class/uio: this kernel has no UIO "
			"support, so nothing can reach the chip.\n"
			"  CONFIG_UIO and CONFIG_UIO_PDRV_GENIRQ are what is missing.\n");
		return -1;
	}
	while ((e = readdir(d)) && !found) {
		char path[512], name[64];

		if (strncmp(e->d_name, "uio", 3) != 0)
			continue;
		/* "uio0" and nothing longer than "uio4294967295" is one, so a
		 * name that does not fit is not a device we are looking for --
		 * checked rather than truncated, because a truncated name
		 * opens a different device or none. */
		if (strlen(e->d_name) >= sizeof(b->uio))
			continue;
		snprintf(path, sizeof(path), "/sys/class/uio/%s/name", e->d_name);
		if (read_file(path, name, sizeof(name)) <= 0)
			continue;
		if (want ? strcmp(name, want) != 0 : !strstr(name, "cmic"))
			continue;
		memcpy(b->uio, e->d_name, strlen(e->d_name) + 1);
		snprintf(b->name, sizeof(b->name), "%s", name);
		found = 1;
	}
	closedir(d);

	if (!found) {
		/*
		 * The overwhelmingly likely cause, and it is not a bug in this
		 * file. Say it here rather than leaving somebody to find it in
		 * a device tree that looks perfectly correct.
		 */
		fprintf(stderr, TAG ": no UIO device %s%s%s.\n"
			"  The device tree declares the CMIC at 0x48000000, but mainline's\n"
			"  uio_pdrv_genirq matches only what its of_id parameter names -- its\n"
			"  match table is one empty entry. Without\n"
			"      uio_pdrv_genirq.of_id=brcm,iproc-cmicd\n"
			"  on the kernel command line nothing binds, no /dev/uio* appears, and\n"
			"  the device tree gives no indication that anything is wrong.\n",
			want ? "named \"" : "whose name contains \"cmic\"",
			want ? want : "", want ? "\"" : "");
		return -1;
	}
	return 0;
}

/*
 * Map one of the device's memory regions.
 *
 * UIO's mmap offset is not the region's address: it is the region index
 * multiplied by the page size, which is how a single character device offers
 * several unrelated windows. Passing the physical address here instead --
 * which is the natural mistake, because every other mapping in this tree takes
 * one -- returns a mapping of whatever region that arithmetic lands on, or
 * EINVAL if it lands on none.
 */
static void *map_region(struct nosaic_helix4_bde *b, int idx, size_t *len_out,
			uint64_t *phys_out)
{
	long long addr, size, offs;
	void *p;

	addr = sysfs_num("/sys/class/uio/%s/maps/map%d/addr", b->uio, idx);
	size = sysfs_num("/sys/class/uio/%s/maps/map%d/size", b->uio, idx);
	if (addr < 0 || size <= 0)
		return NULL;
	offs = sysfs_num("/sys/class/uio/%s/maps/map%d/offset", b->uio, idx);
	if (offs < 0)
		offs = 0;

	p = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED,
		 b->uio_fd, (off_t)idx * getpagesize());
	if (p == MAP_FAILED)
		return NULL;

	*len_out  = (size_t)size - (size_t)offs;
	*phys_out = (uint64_t)addr + (uint64_t)offs;
	return (char *)p + offs;
}

int nosaic_helix4_bde_open(struct nosaic_helix4_bde *b, const char *name)
{
	char dev[32];

	memset(b, 0, sizeof(*b));
	b->uio_fd = b->mem_fd = -1;

	if (find_uio(b, name) != 0)
		return -1;

	snprintf(dev, sizeof(dev), "/dev/%s", b->uio);
	b->uio_fd = open(dev, O_RDWR);
	if (b->uio_fd < 0) {
		fprintf(stderr, TAG ": %s: %s\n", dev, strerror(errno));
		return -1;
	}

	b->bar = map_region(b, 0, &b->bar_len, &b->bar_phys);
	if (!b->bar) {
		fprintf(stderr, TAG ": mapping %s map0 (the CMIC window): %s\n",
			b->uio, strerror(errno));
		close(b->uio_fd);
		b->uio_fd = -1;
		return -1;
	}
	return 0;
}

void nosaic_helix4_bde_close(struct nosaic_helix4_bde *b)
{
	if (b->dma && !b->dma_via_uio)
		munmap(b->dma, b->dma_len);
	if (b->bar)
		munmap((void *)b->bar, b->bar_len);
	if (b->mem_fd >= 0)
		close(b->mem_fd);
	if (b->uio_fd >= 0)
		close(b->uio_fd);
	b->bar = NULL;
	b->dma = NULL;
	b->mem_fd = b->uio_fd = -1;
}

uint32_t nosaic_helix4_bde_rd(struct nosaic_helix4_bde *b, uint32_t off)
{
	if ((size_t)off + 4 > b->bar_len) {
		fprintf(stderr, TAG ": read past the CMIC window: %#x\n", off);
		return 0xffffffffu;
	}
	return nosaic_mmio_rd32((volatile char *)b->bar + off);
}

void nosaic_helix4_bde_wr(struct nosaic_helix4_bde *b, uint32_t off, uint32_t v)
{
	if ((size_t)off + 4 > b->bar_len) {
		fprintf(stderr, TAG ": write past the CMIC window: %#x\n", off);
		return;
	}
	nosaic_mmio_wr32((volatile char *)b->bar + off, v);
}

int nosaic_helix4_bde_identify(struct nosaic_helix4_bde *b, uint32_t *raw)
{
	uint32_t v = nosaic_helix4_bde_rd(b, NOSAIC_CMIC_REVID_DEVID);

	if (raw)
		*raw = v;

	/*
	 * All-ones is an unmapped window; all-zeroes is a mapped one behind a
	 * block that is not powered or not out of reset. Neither is a device
	 * id, and both are worth separating from "read a number we did not
	 * expect", because only the last of the three is a chip problem.
	 */
	if (v == 0xffffffffu || v == 0) {
		fprintf(stderr, TAG ": the chip's id register reads %#x, which is not "
			"a device.\n  The window is at %#llx; either it is not the CMIC or "
			"the block is not powered.\n",
			v, (unsigned long long)b->bar_phys);
		return -1;
	}

	/* CMIC_DEV_REV_ID: device id in the low half, revision above it. */
	b->device_id = (uint16_t)(v & 0xffff);
	b->rev_id    = (uint8_t)((v >> 16) & 0xff);
	return 0;
}

int nosaic_helix4_bde_arm_irq(struct nosaic_helix4_bde *b)
{
	uint32_t on = 1;

	if (b->uio_fd < 0)
		return -1;
	if (write(b->uio_fd, &on, sizeof(on)) != (ssize_t)sizeof(on)) {
		fprintf(stderr, TAG ": re-arming the interrupt: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

int nosaic_helix4_bde_wait_irq(struct nosaic_helix4_bde *b, int timeout_ms)
{
	struct pollfd p = { .fd = b->uio_fd, .events = POLLIN };
	uint32_t count = 0;
	int rv;

	if (b->uio_fd < 0)
		return -1;

	rv = poll(&p, 1, timeout_ms);
	if (rv < 0)
		return errno == EINTR ? 0 : -1;
	if (rv == 0)
		return 0;

	if (read(b->uio_fd, &count, sizeof(count)) != (ssize_t)sizeof(count))
		return -1;
	return (int)count;
}

/* ------------------------------------------------------------------ DMA pool */

/* Device tree property data is big-endian whatever the host is. */
static uint64_t be_cells(const unsigned char *p, int cells)
{
	uint64_t v = 0;
	for (int i = 0; i < cells * 4; i++)
		v = (v << 8) | p[i];
	return v;
}

/*
 * The preferred path: a second reg range on the CMIC node, published by
 * uio_pdrv_genirq as map1.
 *
 * Nothing else is needed for this to work -- no /dev/mem, no second file
 * descriptor, and no exemption from CONFIG_STRICT_DEVMEM, which is enabled in
 * NOSaic's kernels and which refuses a mapping of System RAM with EPERM and no
 * explanation of why.
 */
static int map_dma_via_uio(struct nosaic_helix4_bde *b)
{
	size_t len = 0;
	uint64_t phys = 0;
	void *p = map_region(b, 1, &len, &phys);

	if (!p)
		return -1;
	b->dma = p;
	b->dma_len = len;
	b->dma_phys = phys;
	b->dma_via_uio = 1;
	return 0;
}

/*
 * The fallback: find the reserved region in the device tree and map it through
 * /dev/mem, which is what the AS5610 does and the only one of the two that has
 * ever run on hardware.
 *
 * Unlike that board's version this reads #address-cells and #size-cells rather
 * than assuming two of each. The AS5610's device tree is 2/2 because a P2020
 * addresses 36 bits; this one is 1/1. Assuming the wrong shape does not fail,
 * it produces a plausible address made of the wrong words.
 */
static int map_dma_via_devmem(struct nosaic_helix4_bde *b)
{
	const char *base = "/proc/device-tree/reserved-memory";
	DIR *d = opendir(base);
	struct dirent *e;
	int acells, scells, found = 0;

	if (!d)
		return -1;

	acells = (int)sysfs_num("%s/#address-cells", base);
	scells = (int)sysfs_num("%s/#size-cells", base);
	/* Those files hold raw big-endian cells, not text, so sysfs_num cannot
	 * read them. Fall back to the device tree's own defaults, which are
	 * what both boards in this tree actually use. */
	if (acells < 1 || acells > 2)
		acells = 1;
	if (scells < 1 || scells > 2)
		scells = 1;

	while ((e = readdir(d)) && !found) {
		char path[512];
		unsigned char reg[32];
		int fd, n;

		if (strncmp(e->d_name, "nosaic-dma", 10) != 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s/reg", base, e->d_name);
		if ((fd = open(path, O_RDONLY)) < 0)
			continue;
		n = read(fd, reg, sizeof(reg));
		close(fd);
		if (n < (acells + scells) * 4)
			continue;
		b->dma_phys = be_cells(reg, acells);
		b->dma_len  = (size_t)be_cells(reg + acells * 4, scells);
		found = 1;
	}
	closedir(d);
	if (!found)
		return -1;

	b->mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (b->mem_fd < 0) {
		fprintf(stderr, TAG ": /dev/mem: %s\n", strerror(errno));
		return -1;
	}
	b->dma = mmap(NULL, b->dma_len, PROT_READ | PROT_WRITE, MAP_SHARED,
		      b->mem_fd, (off_t)b->dma_phys);
	if (b->dma == MAP_FAILED) {
		/* Almost always the reserved node missing no-map: the region is
		 * then still System RAM, and CONFIG_STRICT_DEVMEM refuses it
		 * with EPERM rather than with anything that names the reason. */
		fprintf(stderr, TAG ": mapping the DMA pool at %#llx through /dev/mem: %s\n"
			"  If this is EPERM, the reserved-memory node is missing no-map.\n",
			(unsigned long long)b->dma_phys, strerror(errno));
		b->dma = NULL;
		return -1;
	}
	b->dma_via_uio = 0;
	return 0;
}

int nosaic_helix4_bde_map_dma(struct nosaic_helix4_bde *b)
{
	if (map_dma_via_uio(b) != 0 && map_dma_via_devmem(b) != 0) {
		fprintf(stderr, TAG ": no DMA pool.\n"
			"  Tried %s map1 (a second reg range on the CMIC node) and the\n"
			"  reserved-memory node nosaic-dma@... through /dev/mem. Without a\n"
			"  pool the SDK cannot build a descriptor the chip can fetch, and\n"
			"  chip initialisation stops at the first table write.\n", b->uio);
		return -1;
	}

	if (nosaic_dmapool_init(&b->pool, b->dma, b->dma_len, TAG) != 0) {
		fprintf(stderr, TAG ": the DMA pool at %#llx is too small to divide "
			"(%zu bytes)\n", (unsigned long long)b->dma_phys, b->dma_len);
		if (!b->dma_via_uio)
			munmap(b->dma, b->dma_len);
		b->dma = NULL;
		return -1;
	}
	return 0;
}
