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
#include <dirent.h>

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

/*
 * Where the reservation actually is, read off the running kernel.
 *
 * The board states it once, in kernel_params:
 *
 *     memmap=64M$0xb0000000 iomem=relaxed
 *
 * and that statement is sitting on /proc/cmdline by the time this runs.
 * Reading it there rather than keeping a second copy in a constant means the
 * two cannot disagree -- and disagreeing is not a benign failure. A base that
 * is not memory maps through /dev/mem SUCCESSFULLY and reads back all-ones, so
 * the pool's first list head is 0xffffffffffffffff, which is not NULL, passes
 * the pool's own NULL check, and the datapath dies writing through it. That is
 * how a compiled-in 0xd0000000 behaved on a board whose RAM stops at
 * 0xbf791fff.
 *
 * Only the `$` form is ours: memmap=nn@ss marks a range usable, nn#ss ACPI,
 * nn!ss persistent. The first reservation wins; a board wanting a different
 * one of several sets NOSAIC_DMA_BASE, which still overrides this.
 */
static int dma_from_cmdline(uint64_t *base, size_t *len)
{
	char buf[4096];
	const char *p;
	FILE *f = fopen("/proc/cmdline", "r");
	size_t n;

	if (f == NULL)
		return -1;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';

	for (p = buf; (p = strstr(p, "memmap=")) != NULL; p += 7) {
		char *end;
		unsigned long long sz = strtoull(p + 7, &end, 0);

		if (end == p + 7)
			continue;
		switch (*end) {
		case 'G': case 'g': sz <<= 30; end++; break;
		case 'M': case 'm': sz <<= 20; end++; break;
		case 'K': case 'k': sz <<= 10; end++; break;
		default: break;
		}
		if (*end != '$' || sz == 0)
			continue;              /* @ # and ! are somebody else's */
		*base = strtoull(end + 1, &end, 0);
		if (*base == 0)
			continue;
		*len = (size_t)sz;
		return 0;
	}
	return -1;
}

int nosaic_bde_open(struct nosaic_bde *b, const char *bdf)
{
	char path[256];

	memset(b, 0, sizeof(*b));
	b->uio_fd = -1;                 /* no interrupt device until asked for */
	snprintf(b->bdf, sizeof(b->bdf), "%s", bdf);

	/* BAR0: the register window. Mapped shared because the whole point is
	 * that writes reach the device rather than a private copy. */
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource0", bdf);
	b->bar_fd = open(path, O_RDWR | O_SYNC);
	if (b->bar_fd < 0) {
		fprintf(stderr, "nosd-td2: %s: %s\n", path, strerror(errno));
		return -1;
	}
	b->bar_len = nosaic_bde_bar_size(bdf, 0);
	if (b->bar_len == 0) {
		fprintf(stderr, "nosd-td2: BAR0 has zero length; is %s the ASIC?\n", bdf);
		return -1;
	}
	b->bar = mmap(NULL, b->bar_len, PROT_READ | PROT_WRITE, MAP_SHARED, b->bar_fd, 0);
	if (b->bar == MAP_FAILED) {
		fprintf(stderr, "nosd-td2: mapping BAR0: %s\n", strerror(errno));
		b->bar = NULL;
		return -1;
	}

	/* PCI configuration space, read and written as a file. */
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/config", bdf);
	b->cfg_fd = open(path, O_RDWR);
	if (b->cfg_fd < 0) {
		fprintf(stderr, "nosd-td2: %s: %s\n", path, strerror(errno));
		return -1;
	}

	/*
	 * What it actually is. The caller may have found this device by
	 * scanning rather than by name, and even when it did not, the SDK
	 * matches on device and revision -- so read them off the device rather
	 * than trusting a compiled-in pair.
	 */
	{
		uint32_t id = nosaic_bde_cfg_read(b, 0x00);
		b->vendor_id = (uint16_t)(id & 0xffff);
		b->dev_id    = (uint16_t)(id >> 16);
		b->rev_id    = (uint8_t)(nosaic_bde_cfg_read(b, 0x08) & 0xff);
	}

	/*
	 * Bus mastering, so the chip can reach host memory.
	 *
	 * The SDK programs most tables through SBUS DMA: it builds a descriptor
	 * in the DMA pool and has the chip fetch it. A device that cannot master
	 * the bus never fetches anything, and the symptom is not "DMA is off" --
	 * it is a table write that times out and an abort that then also fails:
	 *
	 *   SOURCE_TRUNK_MAP_MODBASE[0].ipipe0 polling timeout
	 *   Fatal error: CMC 0 channel 1 abort failed, cold boot might be needed
	 *
	 * which reads as broken silicon.
	 *
	 * The reset path deliberately leaves this bit clear: it enables memory
	 * decoding only, because letting a chip that has not been initialised
	 * write host memory is not a good default. This is the point at which
	 * something genuinely needs it.
	 *
	 * MEMORY SPACE is set here too, and that is not belt and braces. The
	 * reset path is the SCD driver on the Arista boards, and a board that
	 * has no platform HAL has nothing that does it -- on the Nexus 3172TQ
	 * the chip's COMMAND reads 0x0004 and /sys/.../enable reads 0, so BAR0
	 * is never decoded. A PCI device that is not decoding memory answers
	 * every read with all-ones, and that is not reported as an error
	 * anywhere; it arrives as nonsense much later:
	 *
	 *   Unit 0 CMIC_SBUS_RING_MAP_0_7 mismatch:ffffffff
	 *   nosd-td2: soc_reset_init(0) returned -15 (Invalid configuration)
	 *
	 * which reads as a configuration problem rather than a dark window.
	 * This is the code that maps BAR0 and then reads through it, so this is
	 * where decoding has to be on. Setting a bit that is already set costs
	 * nothing on the boards whose reset path got there first.
	 */
	{
		uint16_t cmd = (uint16_t)nosaic_bde_cfg_read(b, 0x04);
		uint16_t want = cmd | (1 << 1) | (1 << 2);   /* memory space, bus master */

		if (cmd != want) {
			nosaic_bde_cfg_write(b, 0x04, want);
			cmd = (uint16_t)nosaic_bde_cfg_read(b, 0x04);
			if ((cmd & ((1 << 1) | (1 << 2))) != ((1 << 1) | (1 << 2))) {
				fprintf(stderr, "nosd-td2: %s: memory decoding and bus "
					"mastering did not both enable; COMMAND reads %#06x\n",
					b->bdf, (unsigned)cmd);
				return -1;
			}
		}
	}

	/* The DMA pool. Not allocated -- claimed, from a region the kernel was
	 * told to leave alone. Preferring what the kernel was actually told over
	 * a constant, and an explicit override over both. */
	{
		uint64_t cbase = 0;
		size_t   clen  = 0;
		int found = dma_from_cmdline(&cbase, &clen) == 0;

		b->dma_phys = env_u64("NOSAIC_DMA_BASE",
				      found ? cbase : DMA_BASE_DEFAULT);
		b->dma_len  = (size_t)env_u64("NOSAIC_DMA_SIZE",
					      found ? clen : DMA_SIZE_DEFAULT);
		printf("dma        %zu bytes at %#llx (%s)\n",
		       b->dma_len, (unsigned long long)b->dma_phys,
		       found ? "reserved on the kernel command line"
			     : "no memmap= on /proc/cmdline -- built-in default");
	}
	b->mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (b->mem_fd < 0) {
		fprintf(stderr, "nosd-td2: /dev/mem: %s\n", strerror(errno));
		return -1;
	}
	b->dma = mmap(NULL, b->dma_len, PROT_READ | PROT_WRITE, MAP_SHARED,
		      b->mem_fd, (off_t)b->dma_phys);
	if (b->dma == MAP_FAILED) {
		fprintf(stderr,
			"nosd-td2: mapping %zu bytes of DMA at %#llx: %s\n"
			"  the kernel command line must reserve it and allow the mapping:\n"
			"    memmap=64M$%#llx iomem=relaxed\n",
			b->dma_len, (unsigned long long)b->dma_phys, strerror(errno),
			(unsigned long long)b->dma_phys);
		b->dma = NULL;
		return -1;
	}
	if (nosaic_dmapool_init(&b->pool, b->dma, b->dma_len, "nosd-td2") != 0) {
		fprintf(stderr, "nosd-td2: the DMA region at %#llx is too small "
			"to divide (%zu bytes)\n",
			(unsigned long long)b->dma_phys, b->dma_len);
		munmap(b->dma, b->dma_len);
		b->dma = NULL;
		return -1;
	}
	return 0;
}

/* ---- interrupts, through uio_pci_generic ---------------------------------
 *
 * PCI config space, the command register, bit 10.
 *
 * uio_pci_generic's own handler masks INTx with pci_check_and_mask_intx() and
 * offers no irqcontrol, so nothing unmasks it again but us. Miss this and the
 * chip interrupts exactly once and is then silent forever -- which looks like
 * an interrupt path that was never wired up at all, rather than one that fired
 * and was left masked.
 */
#define PCI_COMMAND_REG      0x04
#define PCI_INTX_DISABLE     (1u << 10)

int nosaic_bde_irq_open(struct nosaic_bde *b)
{
	char path[256];
	DIR *d;
	struct dirent *e;
	int n = -1;

	b->uio_fd = -1;

	/* The kernel names the device uioN and links it under the PCI device,
	 * so the number is discovered rather than assumed. */
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/uio", b->bdf);
	d = opendir(path);
	if (d == NULL)
		return -1;                      /* not bound: the board runs polled */
	while ((e = readdir(d)) != NULL) {
		if (sscanf(e->d_name, "uio%d", &n) == 1)
			break;
		n = -1;
	}
	closedir(d);
	if (n < 0)
		return -1;

	snprintf(path, sizeof(path), "/dev/uio%d", n);
	b->uio_fd = open(path, O_RDWR);
	if (b->uio_fd < 0) {
		fprintf(stderr, "bde: %s: %s\n", path, strerror(errno));
		return -1;
	}
	fprintf(stderr, "bde: interrupts via %s\n", path);
	return 0;
}

int nosaic_bde_irq_wait(struct nosaic_bde *b)
{
	uint32_t count;
	ssize_t rv;

	if (b->uio_fd < 0)
		return -1;
	rv = read(b->uio_fd, &count, sizeof(count));
	if (rv != (ssize_t)sizeof(count))
		return -1;
	return 0;
}

void nosaic_bde_irq_arm(struct nosaic_bde *b)
{
	uint32_t cmd;

	if (b->uio_fd < 0)
		return;
	cmd = nosaic_bde_cfg_read(b, PCI_COMMAND_REG);
	if (cmd & PCI_INTX_DISABLE)
		nosaic_bde_cfg_write(b, PCI_COMMAND_REG, cmd & ~PCI_INTX_DISABLE);
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
	if (b->uio_fd >= 0) close(b->uio_fd);
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

/*
 * PCI configuration space, through sysfs.
 *
 * pread/pwrite rather than lseek plus read: the SDK may call these from more
 * than one thread, and a shared file offset would make two accesses interleave
 * into each other's addresses. That failure is intermittent and looks like
 * flaky hardware.
 */
uint32_t nosaic_bde_cfg_read(struct nosaic_bde *b, uint32_t addr)
{
	uint32_t v = 0xffffffff;

	if (pread(b->cfg_fd, &v, sizeof(v), (off_t)addr) != (ssize_t)sizeof(v)) {
		fprintf(stderr, "nosd-td2: PCI config read at %#x failed: %s\n",
			addr, strerror(errno));
		return 0xffffffff;
	}
	return v;
}

void nosaic_bde_cfg_write(struct nosaic_bde *b, uint32_t addr, uint32_t data)
{
	if (pwrite(b->cfg_fd, &data, sizeof(data), (off_t)addr) != (ssize_t)sizeof(data))
		fprintf(stderr, "nosd-td2: PCI config write at %#x failed: %s\n",
			addr, strerror(errno));
}

void nosaic_bde_set_sal_device(struct nosaic_bde *b) { sal_dev = b; }

void *sal_dma_alloc(unsigned int size, char *name)
{
	if (!sal_dev || !sal_dev->dma) {
		fprintf(stderr, "nosd-td2: sal_dma_alloc(%u, %s) before the BDE was opened\n",
			size, name ? name : "?");
		return NULL;
	}
	return nosaic_dmapool_alloc(&sal_dev->pool, (size_t)size, name);
}

void sal_dma_free(void *ptr)
{
	/* This used to do nothing, on the reasoning that the pool outlived
	 * every allocation taken from it. It does not: bcm_tx takes a DMA
	 * vector per transmitted packet and gives it back, and a free that
	 * reclaimed nothing turned that into a leak that emptied 64 MiB and
	 * took the control plane with it. See datapath/common/dmapool.h. */
	if (sal_dev)
		nosaic_dmapool_free(&sal_dev->pool, ptr);
}

void sal_config_init_defaults(void)
{
	/* The SDK's compiled-in configuration is the starting point; NOSaic's
	 * board configuration is applied afterwards through the normal config
	 * interface rather than by editing defaults here. */
}
