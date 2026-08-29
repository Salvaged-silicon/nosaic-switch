/*
 * A userspace BDE for the Broadcom SDK.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * WHY THIS EXISTS
 *
 * The SDK ships its own BDE as a pair of kernel modules. Their newest version
 * guard is Linux 4.16 and they use interfaces removed since -- ioremap_nocache
 * went in 5.6 -- while NOSaic runs 6.12. Carrying a patch set for them would
 * mean owning it for as long as these boards live.
 *
 * It is also unnecessary. EOS reaches this same chip with no arbitrating
 * driver at all: several of its agents hold live mappings of the ASIC's PCI
 * BAR simultaneously. The BDE's job is small enough to do the same way, and
 * everything above it is then unmodified SDK -- which matters, because the
 * Trident2 MMU and LLS initialisation are precisely the sequences that
 * hand-reproduction repeatedly failed to match.
 *
 * WHAT IT IMPLEMENTS
 *
 * soc_cm_device_vectors_t, defined at include/soc/cmtypes.h:63 of the SDK.
 * Fourteen function pointers; the mapping is:
 *
 *   read/write            mmap of the ASIC's BAR0 through sysfs resource0
 *   pci_conf_read/write   /sys/bus/pci/devices/<bdf>/config
 *   salloc/sfree          a reserved physically contiguous region via /dev/mem
 *   l2p/p2l               base plus offset, because the region is at a known
 *                         physical address rather than paged
 *   sflush/sinval         nothing: x86 DMA is coherent
 *   interrupt_*           nothing: the SDK polls when none is connected
 *
 * THE DMA REGION
 *
 * The SDK wants a contiguous physical pool. A pagemap lookup gives single
 * pages, which is fine for one descriptor and not for a pool, so the region is
 * reserved on the kernel command line instead:
 *
 *     memmap=64M$0xd0000000 iomem=relaxed      (on the 7050SX2)
 *
 * The dollar marks it reserved, so the kernel never touches it and physical
 * addresses are base plus offset. An image that omits that argument will
 * initialise the chip and then fail at the first DMA, which does not look like
 * a missing kernel argument -- so the board records it and boot0 sets it.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>

#include "bde.h"

/* Defaults matching the reservation the board's boot0 makes. Overridable so a
 * board whose memory map differs does not need a rebuild. */
/* Where the reservation actually is on the first board, read off the running
 * switch. Not 0x100000000: that board has 3844 MB of RAM, so 4 GB is past the
 * end of physical memory and the mapping would simply fail. A board with more
 * memory can put it higher; that is what NOSAIC_DMA_BASE is for. */
#define DMA_BASE_DEFAULT 0xd0000000ULL
#define DMA_SIZE_DEFAULT (64u << 20)

static uint64_t env_u64(const char *name, uint64_t fallback)
{
	const char *v = getenv(name);
	if (!v || !*v)
		return fallback;
	return strtoull(v, NULL, 0);
}

int nosaic_bde_open(struct nosaic_bde *b, const char *bdf)
{
	char path[256];

	memset(b, 0, sizeof(*b));
	snprintf(b->bdf, sizeof(b->bdf), "%s", bdf);

	/* BAR0: the register window. Mapped shared because the whole point is
	 * that writes reach the device rather than a private copy. */
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource0", bdf);
	b->bar_fd = open(path, O_RDWR | O_SYNC);
	if (b->bar_fd < 0) {
		fprintf(stderr, "nosd-td2p: %s: %s\n", path, strerror(errno));
		return -1;
	}
	b->bar_len = nosaic_bde_bar_size(bdf, 0);
	if (b->bar_len == 0) {
		fprintf(stderr, "nosd-td2p: BAR0 has zero length; is %s the ASIC?\n", bdf);
		return -1;
	}
	b->bar = mmap(NULL, b->bar_len, PROT_READ | PROT_WRITE, MAP_SHARED, b->bar_fd, 0);
	if (b->bar == MAP_FAILED) {
		fprintf(stderr, "nosd-td2p: mapping BAR0: %s\n", strerror(errno));
		b->bar = NULL;
		return -1;
	}

	/* PCI configuration space, read and written as a file. */
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/config", bdf);
	b->cfg_fd = open(path, O_RDWR);
	if (b->cfg_fd < 0) {
		fprintf(stderr, "nosd-td2p: %s: %s\n", path, strerror(errno));
		return -1;
	}

	/* The DMA pool. Not allocated -- claimed, from a region the kernel was
	 * told to leave alone. */
	b->dma_phys = env_u64("NOSAIC_DMA_BASE", DMA_BASE_DEFAULT);
	b->dma_len  = (size_t)env_u64("NOSAIC_DMA_SIZE", DMA_SIZE_DEFAULT);
	b->mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (b->mem_fd < 0) {
		fprintf(stderr, "nosd-td2p: /dev/mem: %s\n", strerror(errno));
		return -1;
	}
	b->dma = mmap(NULL, b->dma_len, PROT_READ | PROT_WRITE, MAP_SHARED,
		      b->mem_fd, (off_t)b->dma_phys);
	if (b->dma == MAP_FAILED) {
		fprintf(stderr,
			"nosd-td2p: mapping %zu bytes of DMA at %#llx: %s\n"
			"  the kernel command line must reserve it and allow the mapping:\n"
			"    memmap=64M$%#llx iomem=relaxed\n",
			b->dma_len, (unsigned long long)b->dma_phys, strerror(errno),
			(unsigned long long)b->dma_phys);
		b->dma = NULL;
		return -1;
	}
	b->dma_used = 0;
	return 0;
}

void nosaic_bde_close(struct nosaic_bde *b)
{
	/* Cast away volatile: it describes how the mapping is accessed, not
	 * what munmap does to it. */
	if (b->bar) munmap((void *)b->bar, b->bar_len);
	if (b->dma) munmap(b->dma, b->dma_len);
	if (b->bar_fd > 0) close(b->bar_fd);
	if (b->cfg_fd > 0) close(b->cfg_fd);
	if (b->mem_fd > 0) close(b->mem_fd);
	memset(b, 0, sizeof(*b));
}

/* BAR size comes from sysfs rather than from the config space BAR probe,
 * because probing writes all-ones to the register and a mistake there is a
 * device that stops responding. */
size_t nosaic_bde_bar_size(const char *bdf, int bar)
{
	char path[256];
	unsigned long long start, end, flags;
	FILE *f;
	size_t len = 0;
	int i;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource", bdf);
	f = fopen(path, "r");
	if (!f)
		return 0;
	for (i = 0; i <= bar; i++) {
		if (fscanf(f, "%llx %llx %llx", &start, &end, &flags) != 3) {
			len = 0;
			break;
		}
		if (i == bar && end >= start)
			len = (size_t)(end - start + 1);
	}
	fclose(f);
	return len;
}

/*
 * The SAL hooks the SDK leaves to the platform.
 *
 * Three symbols, all declared SAL_ATTR_WEAK by the SDK so a platform that does
 * not need them still links. We do need them: the SDK allocates its DMA
 * descriptors and packet buffers through sal_dma_alloc, and on a userspace BDE
 * there is no kernel allocator behind it.
 *
 * A bump allocator over the reserved region, because that is what the SDK's
 * use actually needs: allocation happens during initialisation and lives for
 * the life of the process. Freeing individual blocks would buy nothing and
 * cost a free list to get wrong.
 */

/* The one device this daemon drives. A pointer rather than a parameter because
 * the SAL signatures are fixed by the SDK and carry no context. */
static struct nosaic_bde *sal_dev;

void nosaic_bde_set_sal_device(struct nosaic_bde *b) { sal_dev = b; }

void *sal_dma_alloc(unsigned int size, char *name)
{
	size_t aligned;
	void *p;

	if (!sal_dev || !sal_dev->dma) {
		fprintf(stderr, "nosd-td2p: sal_dma_alloc(%u, %s) before the BDE was opened\n",
			size, name ? name : "?");
		return NULL;
	}

	/* Cache-line aligned. The SDK hands these addresses to the chip, and an
	 * unaligned descriptor is a class of failure that shows up as corrupt
	 * traffic rather than as an error. */
	aligned = ((size_t)size + 63u) & ~(size_t)63u;
	if (sal_dev->dma_used + aligned > sal_dev->dma_len) {
		fprintf(stderr,
			"nosd-td2p: DMA pool exhausted: %s wanted %u, %zu of %zu bytes used\n"
			"  raise the reservation on the kernel command line\n",
			name ? name : "?", size, sal_dev->dma_used, sal_dev->dma_len);
		return NULL;
	}
	p = (char *)sal_dev->dma + sal_dev->dma_used;
	sal_dev->dma_used += aligned;
	memset(p, 0, aligned);
	return p;
}

void sal_dma_free(void *ptr)
{
	/* Deliberately nothing. See above: the pool outlives every allocation
	 * taken from it, and pretending to reclaim would be less honest than
	 * not trying. */
	(void)ptr;
}

void sal_config_init_defaults(void)
{
	/* The SDK's compiled-in configuration is the starting point; NOSaic's
	 * board configuration is applied afterwards through the normal config
	 * interface rather than by editing defaults here. */
}
