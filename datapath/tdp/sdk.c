/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
/*
 * The SDK's view of the Trident+ chip: soc_cm_device_vectors_t over our
 * userspace BDE.
 *
 * The sequence, which is the SDK's own and not negotiable:
 *
 *     sal_core_init(), sal_appl_init()         locks, before anything uses one
 *     soc_cm_init()                            once
 *     soc_cm_device_create(dev_id, rev_id, c)  -> unit number
 *     soc_cm_device_init(unit, &vectors)       installs these, then soc_attach
 *
 * Two things differ from the Trident2+ version and both are the architecture
 * rather than the chip. Register access goes through datapath/common/mmio.h so
 * that PowerPC gets the barriers a plain volatile access does not emit; and
 * the chip is put into big-endian programmed I/O before the SDK reads a single
 * register, because the SDK for this platform is built with SYS_BE_PIO=1 and
 * that is a statement about the chip which something has to make true.
 */
#include "bde.h"
#include "sdk.h"
#include "mmio.h"
#include "props.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef NOSAIC_WITH_SDK

#include <sal/types.h>
#include <sal/core/boot.h>
#include <sal/appl/sal.h>
#include <soc/cm.h>
#include <soc/cmext.h>
#include <soc/cmtypes.h>
#include <soc/error.h>

/* Bus and device type, from include/sal/types.h. A PCI-attached switch chip --
 * stated rather than defaulted, because the SDK selects access paths from it
 * and a wrong value fails much further downstream. */
#define NOSAIC_BUS_TYPE (SAL_PCI_DEV_TYPE | SAL_SWITCH_DEV_TYPE)

static struct nosaic_tdp_bde *sal_dev;

static struct nosaic_tdp_bde *bde_of(soc_cm_dev_t *dev)
{
	return (struct nosaic_tdp_bde *)dev->cookie;
}

static uint32 nosaic_read(soc_cm_dev_t *dev, uint32 addr)
{
	struct nosaic_tdp_bde *b = bde_of(dev);

	if ((size_t)addr + 4 > b->bar_len) {
		fprintf(stderr, "nosd-tdp: read past BAR0: %#x\n", addr);
		return 0xffffffff;
	}
	return nosaic_mmio_rd32((volatile char *)b->bar + addr);
}

static void nosaic_write(soc_cm_dev_t *dev, uint32 addr, uint32 data)
{
	struct nosaic_tdp_bde *b = bde_of(dev);

	if ((size_t)addr + 4 > b->bar_len) {
		fprintf(stderr, "nosd-tdp: write past BAR0: %#x\n", addr);
		return;
	}
	nosaic_mmio_wr32((volatile char *)b->bar + addr, data);
}

static uint32 nosaic_pci_conf_read(soc_cm_dev_t *dev, uint32 addr)
{
	return nosaic_tdp_bde_cfg_read(bde_of(dev), addr);
}

static void nosaic_pci_conf_write(soc_cm_dev_t *dev, uint32 addr, uint32 data)
{
	nosaic_tdp_bde_cfg_write(bde_of(dev), addr, data);
}

/*
 * The DMA pool, handed out by bumping a pointer.
 *
 * There is no free: the SDK takes what it needs during initialisation and
 * keeps it for the life of the process, so a real allocator would be
 * complexity for a case that does not arise. Exhausting the pool is reported
 * with what asked for it, because "out of memory" on its own does not say
 * which table was being built.
 */
static void *nosaic_salloc(soc_cm_dev_t *dev, int size, const char *name)
{
	struct nosaic_tdp_bde *b = bde_of(dev);
	size_t aligned;
	void *p;

	if (!b->dma) {
		fprintf(stderr, "nosd-tdp: salloc(%d, %s) with no DMA pool mapped\n",
			size, name ? name : "?");
		return NULL;
	}
	/* Cache-line aligned: these addresses are handed to the chip, and an
	 * unaligned descriptor shows up as corrupt traffic rather than an error. */
	aligned = ((size_t)size + 63u) & ~(size_t)63u;
	if (b->dma_used + aligned > b->dma_len) {
		fprintf(stderr,
			"nosd-tdp: DMA pool exhausted: %s wanted %d, %zu of %zu bytes used\n"
			"  enlarge nosaic-dma in the board's device tree\n",
			name ? name : "?", size, b->dma_used, b->dma_len);
		return NULL;
	}
	p = (char *)b->dma + b->dma_used;
	b->dma_used += aligned;
	memset(p, 0, aligned);
	return p;
}

static void nosaic_sfree(soc_cm_dev_t *dev, void *ptr)
{
	(void)dev;
	(void)ptr;   /* see above: the pool outlives every allocation from it */
}

/* Physical address of a pointer into the pool. The chip is given these, so
 * getting it wrong is a DMA to somewhere else in RAM. */
static sal_paddr_t nosaic_l2p(soc_cm_dev_t *dev, void *addr)
{
	struct nosaic_tdp_bde *b = bde_of(dev);
	size_t off = (char *)addr - (char *)b->dma;

	if (!b->dma || off >= b->dma_len)
		return 0;
	return (sal_paddr_t)(b->dma_phys + off);
}

static void *nosaic_p2l(soc_cm_dev_t *dev, sal_paddr_t addr)
{
	struct nosaic_tdp_bde *b = bde_of(dev);
	uint64_t off = (uint64_t)addr - b->dma_phys;

	if (!b->dma || addr < b->dma_phys || off >= b->dma_len)
		return NULL;
	return (char *)b->dma + off;
}

/*
 * Cache maintenance around DMA.
 *
 * No-ops, and the reason is the mapping rather than an assumption that
 * coherency is free: the DMA pool is reserved with no-map and opened through
 * /dev/mem with O_SYNC, which on this architecture gives an uncached guarded
 * mapping. There is no dirty line to flush and none to invalidate.
 *
 * Written out rather than left unset because the SDK requires them, and
 * because if the mapping ever becomes cacheable these are the two functions
 * that have to grow bodies -- and the failure without them would be corrupt
 * descriptors rather than an error.
 */
static int nosaic_sflush(soc_cm_dev_t *dev, void *addr, int length)
{
	(void)dev; (void)addr; (void)length;
	return 0;
}

static int nosaic_sinval(soc_cm_dev_t *dev, void *addr, int length)
{
	(void)dev; (void)addr; (void)length;
	return 0;
}

/*
 * Chip configuration lookup — what the SDK would otherwise read from
 * config.bcm.
 *
 * NULL means "not set" and the SDK falls back to its compiled-in default,
 * which is right for most properties and fatal for one: without a port map it
 * walks its own port table looking for a terminator that was never written.
 * That is not a hypothetical, it is where soc_attach segfaulted before these
 * files existed -- soc_counter_attach, counter.c:7974.
 */
static char *nosaic_config_var_get(soc_cm_dev_t *dev, const char *name)
{
	(void)dev;
	/* The SDK's prototype is not const-correct; the value is not modified,
	 * and casting here keeps that fact in one place. */
	return (char *)nosaic_props_get(name);
}

/*
 * Interrupts. Declared and doing nothing, which leaves the SDK in polled mode.
 *
 * The Trident2+ board delivers interrupts through uio_pci_generic and gets a
 * core back for it. That work does not carry over unexamined: this chip has a
 * CMICe rather than a CMICm and its interrupt path differs, so it is left for
 * after the datapath works at all. Polled is slow, not wrong.
 */
static int nosaic_interrupt_connect(soc_cm_dev_t *dev,
				    soc_cm_isr_func_t handler, void *data)
{
	(void)dev; (void)handler; (void)data;
	return 0;
}

static int nosaic_interrupt_disconnect(soc_cm_dev_t *dev)
{
	(void)dev;
	return 0;
}

static uint64 nosaic_read64(soc_cm_dev_t *dev, uint32 addr)
{
	struct nosaic_tdp_bde *b = bde_of(dev);

	if ((size_t)addr + 8 > b->bar_len) {
		fprintf(stderr, "nosd-tdp: 64-bit read past BAR0: %#x\n", addr);
		return ~(uint64)0;
	}
	/* Two 32-bit accesses rather than one 64-bit: the barriers are defined
	 * for words, and a 64-bit load to this window is not something the CMIC
	 * is documented to accept. High word first, matching big-endian order. */
	{
		uint32 hi = nosaic_mmio_rd32((volatile char *)b->bar + addr);
		uint32 lo = nosaic_mmio_rd32((volatile char *)b->bar + addr + 4);
		return ((uint64)hi << 32) | lo;
	}
}

static void nosaic_write64(soc_cm_dev_t *dev, uint32 addr, uint64 data)
{
	struct nosaic_tdp_bde *b = bde_of(dev);

	if ((size_t)addr + 8 > b->bar_len) {
		fprintf(stderr, "nosd-tdp: 64-bit write past BAR0: %#x\n", addr);
		return;
	}
	nosaic_mmio_wr32((volatile char *)b->bar + addr, (uint32)(data >> 32));
	nosaic_mmio_wr32((volatile char *)b->bar + addr + 4, (uint32)data);
}

int nosaic_tdp_sdk_attach(struct nosaic_tdp_bde *b, uint16_t dev_id, uint8_t rev_id)
{
	soc_cm_device_vectors_t v;
	int unit, rv;

	sal_dev = b;

	/*
	 * Byte order first, before the SDK reads anything.
	 *
	 * This build sets SYS_BE_PIO=1, which tells the SDK that programmed I/O
	 * to this chip is big-endian. Nothing makes that true except writing
	 * CMIC_ENDIAN_SELECT, and if it is not written the SDK's very first
	 * register read -- the one that identifies the chip -- comes back
	 * byte-swapped and it concludes the device is not one it knows.
	 */
	nosaic_tdp_bde_set_endian(b);

	if (sal_core_init() < 0) {
		fprintf(stderr, "nosd-tdp: sal_core_init failed\n");
		return -1;
	}
	if (sal_appl_init() < 0) {
		fprintf(stderr, "nosd-tdp: sal_appl_init failed\n");
		return -1;
	}
	if (soc_cm_init() < 0) {
		fprintf(stderr, "nosd-tdp: soc_cm_init failed\n");
		return -1;
	}

	unit = soc_cm_device_create(dev_id, rev_id, b);
	if (unit < 0) {
		fprintf(stderr, "nosd-tdp: soc_cm_device_create(%#x, %#x) failed: %d\n"
			"  the SDK does not recognise this device id, or was built "
			"without support for it\n", dev_id, rev_id, unit);
		return -1;
	}

	memset(&v, 0, sizeof(v));
	v.init            = 1;
	v.bus_type        = NOSAIC_BUS_TYPE;

	/*
	 * The three that make this board different from the 7050SX2.
	 *
	 * They tell the SDK the byte order the chip is presenting, and they must
	 * agree with what was just written to CMIC_ENDIAN_SELECT -- PIO, packet
	 * DMA and everything else, all big-endian, matching the host. The
	 * Trident2+ sets all three to 0 because x86 is little-endian and the
	 * chip's default suits it.
	 *
	 * Disagreement here is not an error the SDK reports: it byte-swaps or
	 * fails to, and every register and descriptor is wrong in the same
	 * direction.
	 */
	v.big_endian_pio    = 1;
	v.big_endian_packet = 1;
	v.big_endian_other  = 1;

	v.config_var_get       = nosaic_config_var_get;
	v.interrupt_connect    = nosaic_interrupt_connect;
	v.interrupt_disconnect = nosaic_interrupt_disconnect;
	v.read            = nosaic_read;
	v.write           = nosaic_write;
	v.read64          = nosaic_read64;
	v.write64         = nosaic_write64;
	v.pci_conf_read   = nosaic_pci_conf_read;
	v.pci_conf_write  = nosaic_pci_conf_write;
	v.salloc          = nosaic_salloc;
	v.sfree           = nosaic_sfree;
	v.sflush          = nosaic_sflush;
	v.sinval          = nosaic_sinval;
	v.l2p             = nosaic_l2p;
	v.p2l             = nosaic_p2l;

	/*
	 * soc_cm_device_init installs the vectors and then calls soc_attach,
	 * which is chip initialisation proper -- so a failure here is either a
	 * rejected vector table or anything in the bring-up behind it. The
	 * return code is the whole diagnosis and printing "failed" throws it
	 * away: SOC_E_PARAM means a vector this build requires is missing,
	 * anything else came from the attach.
	 */
	rv = soc_cm_device_init(unit, &v);
	if (rv < 0) {
		fprintf(stderr, "nosd-tdp: soc_cm_device_init(unit %d) returned %d (%s)\n",
			unit, rv, soc_errmsg(rv));
		fprintf(stderr, "  either a rejected vector table or a failure inside "
			"soc_attach; the SDK log above says which\n");
		return -1;
	}
	return unit;
}

/*
 * soc_init and friends are declared here rather than by including <soc/drv.h>.
 *
 * That header is written for the SDK's own translation units and needs the
 * generated per-chip register database, which only exists once the SDK's full
 * chip-selection defines are in scope. Pulling it in to reach four functions
 * would mean replicating the SDK's build configuration here and keeping it in
 * step -- a larger and more fragile dependency than the declarations.
 *
 * If a signature ever changes the linker will not notice, which is the cost of
 * doing it this way and the reason it is confined to this one place.
 */
extern int soc_reset_init(int unit);
extern int soc_misc_init(int unit);
extern int soc_mmu_init(int unit);
extern int bcm_attach(int unit, char *type, char *subtype, int remunit);
extern int bcm_init(int unit);

/*
 * Bring the SOC layer up, resetting the chip on the way.
 *
 * soc_reset_init rather than soc_init, and the difference is the whole
 * problem. Both call soc_do_init; soc_init passes FALSE for the reset argument
 * and soc_reset_init passes TRUE.
 *
 * So soc_init initialises a chip it assumes is already reset. On a switch
 * running the vendor OS, or one where the SDK's kernel BDE loaded, that holds.
 * Here nothing has ever reset this chip -- NOSaic mapped it and no driver is
 * bound to it at all -- and its pipeline blocks are still held.
 *
 * The 7050SX2 found this the expensive way: soc_init returned success after
 * twenty-six thousand lines of initialisation and the first table write then
 * got no SBUS acknowledgement, which reads as broken silicon or a bad port
 * map. Taking the finding rather than repeating the discovery.
 */
int nosaic_tdp_sdk_soc_init(int unit)
{
	int rv = soc_reset_init(unit);

	if (rv < 0) {
		fprintf(stderr, "nosd-tdp: soc_reset_init(%d) returned %d (%s)\n",
			unit, rv, soc_errmsg(rv));
		return -1;
	}
	return 0;
}

/*
 * The rest of bring-up, in the order the SDK's own diagnostic shell uses.
 *
 * Each step is announced before it runs rather than after: any of them can sit
 * for a long time or not return at all, and on a serial console the last line
 * printed is the only evidence of where it stopped.
 */
int nosaic_tdp_sdk_bcm_init(int unit)
{
	int rv;

	printf("  soc_misc_init...\n");
	fflush(stdout);
	rv = soc_misc_init(unit);
	if (rv < 0) {
		fprintf(stderr, "nosd-tdp: soc_misc_init(%d) returned %d (%s)\n",
			unit, rv, soc_errmsg(rv));
		return -1;
	}

	printf("  soc_mmu_init...\n");
	fflush(stdout);
	rv = soc_mmu_init(unit);
	if (rv < 0) {
		fprintf(stderr, "nosd-tdp: soc_mmu_init(%d) returned %d (%s)\n",
			unit, rv, soc_errmsg(rv));
		return -1;
	}

	/* NULL type: the SDK picks the driver family from the device it already
	 * knows about, which for every Ethernet switch in this SDK is "esw". */
	printf("  bcm_attach...\n");
	fflush(stdout);
	rv = bcm_attach(unit, NULL, NULL, unit);
	if (rv < 0) {
		fprintf(stderr, "nosd-tdp: bcm_attach(%d) returned %d (%s)\n",
			unit, rv, soc_errmsg(rv));
		return -1;
	}

	printf("  bcm_init...\n");
	fflush(stdout);
	rv = bcm_init(unit);
	if (rv < 0) {
		fprintf(stderr, "nosd-tdp: bcm_init(%d) returned %d (%s)\n",
			unit, rv, soc_errmsg(rv));
		return -1;
	}
	return 0;
}

/*
 * A hook the SDK's SAL expects the application to supply. It is called during
 * initialisation whether or not there is anything to do, so it has to exist:
 * without it the link fails naming a symbol that appears nowhere in this tree.
 */
void sal_config_init_defaults(void)
{
	/* The SDK's compiled-in configuration is the starting point; the board's
	 * configuration is applied afterwards through the normal config
	 * interface rather than by editing defaults here. */
}

#else  /* built without the SDK staged */

int nosaic_tdp_sdk_attach(struct nosaic_tdp_bde *b, uint16_t dev_id, uint8_t rev_id)
{
	(void)b; (void)dev_id; (void)rev_id;
	fprintf(stderr, "nosd-tdp: built without the SDK; rebuild with SDK_DIR set\n");
	return -1;
}

int nosaic_tdp_sdk_soc_init(int unit)
{
	(void)unit;
	return -1;
}

int nosaic_tdp_sdk_bcm_init(int unit)
{
	(void)unit;
	return -1;
}

#endif
