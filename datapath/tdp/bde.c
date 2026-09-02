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

int nosaic_tdp_bde_open(struct nosaic_tdp_bde *b, const char *bdf)
{
	char path[512];

	memset(b, 0, sizeof(*b));
	b->bar_fd = b->cfg_fd = -1;

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
	return 0;
}

void nosaic_tdp_bde_set_endian(struct nosaic_tdp_bde *b)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	uint32_t es = NOSAIC_ES_BIG_ENDIAN_PIO |
		      NOSAIC_ES_BIG_ENDIAN_DMA_PACKET |
		      NOSAIC_ES_BIG_ENDIAN_DMA_OTHER;

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
	if (b->bar_fd >= 0)
		close(b->bar_fd);
	if (b->cfg_fd >= 0)
		close(b->cfg_fd);
	b->bar_fd = b->cfg_fd = -1;
}
