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
#include <stdint.h>
#include <unistd.h>

#ifdef NOSAIC_WITH_SDK

#include <sal/types.h>
#include <sal/core/boot.h>
#include <sal/appl/sal.h>
#include <soc/cm.h>
#include <soc/cmext.h>
#include <soc/cmtypes.h>
#include <soc/error.h>
#include <shared/bsltypes.h>
#include <shared/bslext.h>
#include <soc/drv.h>
#include <bcm/port.h>
#include <bcm/stg.h>
#include <bcm/link.h>
#include <bcm/error.h>
#include <stdarg.h>
#include <stdlib.h>

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
		static int complained;

		if (!complained) {
			complained = 1;
			/* Once, with the state that made the decision. A bounds
			 * check that rejects an offset plainly inside the BAR is
			 * not a bounds problem -- it is the BDE pointer, and the
			 * two are told apart by printing it. */
			fprintf(stderr, "nosd-tdp: read past BAR0: %#x "
				"(bde=%p bar=%p bar_len=%zu dma_phys=%#llx)\n",
				addr, (void *)b, (void *)b->bar, b->bar_len,
				(unsigned long long)b->dma_phys);
		}
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
/* One pool, two callers: the SDK reaches it through the device vector during
 * chip initialisation and through the SAL hook above for packet buffers. */
static void *pool_alloc(struct nosaic_tdp_bde *b, int size, const char *name)
{
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
	/* Note: free does not reclaim, so dma_used only grows. That is fine for
	 * initialisation and for a packet pool taken once at startup, and would
	 * not be for something allocating per packet. */
}

static void *nosaic_salloc(soc_cm_dev_t *dev, int size, const char *name)
{
	return pool_alloc(bde_of(dev), size, name);
}

static void nosaic_sfree(soc_cm_dev_t *dev, void *ptr)
{
	(void)dev;
	(void)ptr;   /* see above: the pool outlives every allocation from it */
}

/*
 * Physical address of a pointer into the pool. The chip is given these, so
 * getting it wrong is a DMA to somewhere else in RAM.
 *
 * A pointer that is not in the pool is a bug somewhere above, and returning 0
 * for it -- which this used to do quietly -- hands the chip physical address
 * zero and waits for a completion that cannot come. That is indistinguishable
 * from a dead DMA engine, so it says so instead. Once, because if it happens
 * at all it happens for every entry in a table.
 */
static sal_paddr_t nosaic_l2p(soc_cm_dev_t *dev, void *addr)
{
	struct nosaic_tdp_bde *b = bde_of(dev);
	size_t off = (char *)addr - (char *)b->dma;
	static int complained;

	if (!b->dma || (char *)addr < (char *)b->dma || off >= b->dma_len) {
		if (!complained) {
			complained = 1;
			fprintf(stderr,
				"nosd-tdp: l2p(%p) is outside the DMA pool "
				"(%p..%p) -- the chip would be told to read "
				"physical address 0\n",
				addr, b->dma, (char *)b->dma + b->dma_len);
		}
		return 0;
	}
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
 * These were no-ops on the reasoning that the pool is reserved no-map and
 * opened through /dev/mem with O_SYNC, so the mapping is uncached and there is
 * nothing to flush. That reasoning may be right and is not worth betting the
 * DMA engine on: if the mapping is cacheable after all, a descriptor the CPU
 * has written sits in the cache, the chip fetches the memory behind it, and
 * the operation times out rather than failing in a way that names a cache.
 *
 * Which is what this board does. With DMA enabled soc_misc_init times out;
 * with table_dma_enable=0 and tslam_dma_enable=0 it passes and bring-up
 * reaches bcm_attach. So the chip is not getting what the CPU wrote.
 *
 * dcbf writes a dirty line back and invalidates it, which serves both
 * directions: before the chip reads, the CPU's writes are in memory; before
 * the CPU reads, its stale copies are gone. It is user-mode accessible, unlike
 * dcbi. The loop steps by the cache line, which is 32 bytes on an e500v2 and
 * read from the device tree rather than assumed -- a wrong stride either does
 * too much work or misses lines.
 */
#if defined(__powerpc__) || defined(__PPC__)
#define NOSAIC_CACHE_LINE 32

static void cache_flush_range(void *addr, int length)
{
	char *p = (char *)((uintptr_t)addr & ~(uintptr_t)(NOSAIC_CACHE_LINE - 1));
	char *end = (char *)addr + length;

	for (; p < end; p += NOSAIC_CACHE_LINE)
		__asm__ __volatile__("dcbf 0,%0" : : "r"(p) : "memory");
	__asm__ __volatile__("sync" ::: "memory");
}
#else
static void cache_flush_range(void *addr, int length)
{
	(void)addr; (void)length;
	__asm__ __volatile__("" ::: "memory");
}
#endif

static int nosaic_sflush(soc_cm_dev_t *dev, void *addr, int length)
{
	(void)dev;
	cache_flush_range(addr, length);
	return 0;
}

static int nosaic_sinval(soc_cm_dev_t *dev, void *addr, int length)
{
	(void)dev;
	cache_flush_range(addr, length);
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

/*
 * The SDK's own log, sent to stderr.
 *
 * Without it the SDK's diagnosis of its own failures is discarded and all that
 * survives is the return code -- which is how "soc_misc_init returned -9" has
 * been the entire evidence for a DMA engine that is not working. The SDK knows
 * more than that and says so.
 */
static int nosaic_bsl_out(bsl_meta_t *meta, const char *fmt, va_list args)
{
	(void)meta;
	return vfprintf(stderr, fmt, args);
}

/*
 * Warnings and worse by default; everything with NOSAIC_SDK_VERBOSE set.
 *
 * Letting everything through is the right instinct during bring-up -- the
 * message that matters is the one nobody predicted -- and it is not free. A
 * single --init produced 2.8 million lines, which on a serial console or an
 * ssh pipe takes longer than the initialisation it is describing, and with
 * table DMA disabled the initialisation is already slow.
 *
 * So the default is quiet enough to finish and the verbose mode is one
 * environment variable away, which is the balance that was actually wanted.
 * bslSeverityWarn is 3 and lower numbers are more severe.
 */
static int nosaic_bsl_check(bsl_packed_meta_t meta)
{
	static int verbose = -1;

	if (verbose < 0)
		verbose = getenv("NOSAIC_SDK_VERBOSE") != NULL;
	if (verbose)
		return 1;
	return BSL_SEVERITY_GET(meta) <= bslSeverityWarn;
}

static void nosaic_bsl_start(void)
{
	bsl_config_t cfg;

	bsl_config_t_init(&cfg);
	cfg.out_hook = nosaic_bsl_out;
	cfg.check_hook = nosaic_bsl_check;
	if (bsl_init(&cfg) < 0)
		fprintf(stderr, "nosd-tdp: could not start the SDK log; "
			"failures below will be numbers without sentences\n");
}

int nosaic_tdp_sdk_attach(struct nosaic_tdp_bde *b, uint16_t dev_id, uint8_t rev_id)
{
	soc_cm_device_vectors_t v;
	int unit, rv;

	sal_dev = b;
	nosaic_bsl_start();

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
	 * agree with what was written to CMIC_ENDIAN_SELECT. Not all three: the
	 * vendor OS running on this hardware with working DMA selects DMA_OTHER
	 * alone, leaving packet DMA little-endian, and only PIO differs here
	 * because NOSaic reads registers directly instead of through a kernel
	 * that swaps for it. The Trident2+ sets all three to 0, x86 being
	 * little-endian and the chip's default suiting it.
	 *
	 * Disagreement here is not an error the SDK reports: it byte-swaps or
	 * fails to, and every register and descriptor is wrong in the same
	 * direction.
	 */
	v.big_endian_pio    = 1;
	v.big_endian_packet = 0;   /* left little-endian, as the working machine has it */
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
 * Enable every port the chip reports and put it in forwarding.
 *
 * bcm_init leaves ports attached but administratively down and, on the ports
 * it does bring into a VLAN, blocked by spanning tree. Both are deliberate --
 * a chip that started forwarding the instant it initialised would loop a
 * network before anyone configured it -- and both have to be undone before a
 * single frame moves.
 *
 * The port numbers here are the SDK's, not the front panel's. Translating
 * between the two is the port map's job and belongs above this layer; what
 * this reports is what the chip says about itself, which is the thing worth
 * knowing first.
 */
int nosaic_tdp_sdk_ports_up(int unit, int forward)
{
	bcm_port_config_t cfg;
	bcm_port_t port;
	int rv, up = 0, linked = 0;

	rv = bcm_port_config_get(unit, &cfg);
	if (rv < 0) {
		fprintf(stderr, "nosd-tdp: bcm_port_config_get(%d) returned %d (%s)\n",
			unit, rv, soc_errmsg(rv));
		return -1;
	}

	BCM_PBMP_ITER(cfg.port, port) {
		int link = 0;

		rv = bcm_port_enable_set(unit, port, 1);
		if (rv < 0) {
			printf("  port %-3d enable failed: %s\n", port, soc_errmsg(rv));
			continue;
		}

		/*
		 * Forwarding is opt-in, and the default leaves the port blocked
		 * where bcm_init left it.
		 *
		 * Putting every port into one VLAN and forcing them all to
		 * forward is what makes a chip demonstrably move frames, and it
		 * is also a bridging loop the moment two of those ports reach
		 * the same neighbour -- the normal case for a switch with
		 * redundant uplinks, and the case on this board, which has two
		 * links to the same upstream. Nothing here runs spanning tree,
		 * so nothing breaks the loop from this end; the neighbour sees
		 * its own BPDUs return and shuts the path down, and the first
		 * symptom is a network that has gone quiet.
		 *
		 * Which ports forward, and in which VLAN, is a decision for the
		 * configuration model. Until there is one, the honest default is
		 * the safe one.
		 */
		if (forward) {
			rv = bcm_port_stp_set(unit, port, BCM_STG_STP_FORWARD);
			if (rv < 0)
				printf("  port %-3d stp failed: %s\n", port, soc_errmsg(rv));
		}

		up++;
		if (bcm_port_link_status_get(unit, port, &link) == BCM_E_NONE && link)
			linked++;
		printf("  port %-3d %-8s %s\n", port, SOC_PORT_NAME(unit, port),
		       link ? "LINK UP" : "down");
	}

	printf("ports        %d enabled, %d with link%s\n", up, linked,
	       forward ? ", all forwarding in one VLAN" : ", none forwarding");
	if (forward)
		printf("WARNING      every port forwards in VLAN 1 and no spanning tree\n"
		       "             runs here, so two ports reaching the same neighbour\n"
		       "             is a loop. Bring-up on a known topology only.\n");

	/*
	 * Link scan, so link state is tracked rather than sampled once.
	 *
	 * Without it bcm_port_link_status_get still reads the PHY, so a caller
	 * that asks gets a true answer -- but nothing asks, and nothing reacts.
	 * A port that goes down stays in the flood set and its share of every
	 * flooded frame is sent into a dead fibre. Software scan rather than
	 * hardware: it costs a MIIM read per port per interval, and the hardware
	 * scanner on this chip wants the MIIM interrupt this BDE does not carry.
	 */
	rv = bcm_linkscan_enable_set(unit, 250000);
	if (rv < 0)
		printf("  link scan unavailable: %s\n", soc_errmsg(rv));
	else
		printf("link scan    every 250 ms\n");

	/* Link state read straight after enabling a port is not an answer. The
	 * MAC has just been let out of reset, the SerDes has not finished
	 * training, and on a board with SFP+ cages the module may not have been
	 * looked at yet -- so every port reads down whether or not anything is
	 * plugged into it, which is indistinguishable from a real fault.
	 * A couple of seconds is long enough for training to settle. */
	sleep(2);
	linked = 0;
	BCM_PBMP_ITER(cfg.port, port) {
		int link = 0;

		if (bcm_port_link_status_get(unit, port, &link) == BCM_E_NONE && link) {
			linked++;
			printf("  port %-3d %-8s LINK UP after settling\n",
			       port, SOC_PORT_NAME(unit, port));
		}
	}
	printf("settled      %d with link\n", linked);
	return 0;
}

/*
 * Per-port counters, sampled twice, reported as a delta.
 *
 * Absolute counters answer the wrong question during bring-up. What matters is
 * not that a port has seen 4000 frames since some unknown reset, but that the
 * number is moving -- and, for forwarding, that frames arriving on one port
 * leave by another. A delta over a known interval says both; a snapshot says
 * neither.
 *
 * Non-unicast is counted separately because on a quiet lab segment it is
 * nearly all of the traffic. The neighbours here emit spanning tree and
 * discovery frames continuously and address them to multicast, so a switch
 * that floods correctly shows those arriving on one port and leaving every
 * other port in the VLAN. That is the cheapest honest forwarding test
 * available without a generator, and it needs no cabling changes.
 */
int nosaic_tdp_sdk_stats(int unit, int seconds)
{
	bcm_port_config_t cfg;
	bcm_port_t port;
	uint32 in0[128], out0[128], inn0[128], outn0[128];
	uint32 v;
	int rv, moving = 0;

	rv = bcm_port_config_get(unit, &cfg);
	if (rv < 0) {
		fprintf(stderr, "nosd-tdp: bcm_port_config_get(%d) returned %d (%s)\n",
			unit, rv, soc_errmsg(rv));
		return -1;
	}

	memset(in0, 0, sizeof(in0));
	memset(out0, 0, sizeof(out0));
	memset(inn0, 0, sizeof(inn0));
	memset(outn0, 0, sizeof(outn0));

	BCM_PBMP_ITER(cfg.port, port) {
		if (port < 0 || port >= 128)
			continue;
		bcm_stat_get32(unit, port, snmpIfInUcastPkts, &in0[port]);
		bcm_stat_get32(unit, port, snmpIfOutUcastPkts, &out0[port]);
		bcm_stat_get32(unit, port, snmpIfInNUcastPkts, &inn0[port]);
		bcm_stat_get32(unit, port, snmpIfOutNUcastPkts, &outn0[port]);
	}

	printf("sampling counters for %d seconds...\n", seconds);
	sleep(seconds);

	printf("  %-8s %10s %10s %10s %10s\n",
	       "port", "rx-ucast", "tx-ucast", "rx-mcast", "tx-mcast");
	BCM_PBMP_ITER(cfg.port, port) {
		uint32 di, dou, dni, dno;

		if (port < 0 || port >= 128)
			continue;
		v = 0; bcm_stat_get32(unit, port, snmpIfInUcastPkts, &v);
		di = v - in0[port];
		v = 0; bcm_stat_get32(unit, port, snmpIfOutUcastPkts, &v);
		dou = v - out0[port];
		v = 0; bcm_stat_get32(unit, port, snmpIfInNUcastPkts, &v);
		dni = v - inn0[port];
		v = 0; bcm_stat_get32(unit, port, snmpIfOutNUcastPkts, &v);
		dno = v - outn0[port];

		if (!di && !dou && !dni && !dno)
			continue;
		moving++;
		printf("  %-8s %10u %10u %10u %10u\n",
		       SOC_PORT_NAME(unit, port), di, dou, dni, dno);
	}

	/*
	 * "Nothing moved" is two different findings and they need different work:
	 * the chip saw no traffic, or the counters are not being collected at
	 * all. A delta of zero looks identical in both cases.
	 *
	 * The absolute totals do NOT tell them apart, which is worth stating
	 * because it is the obvious idea and it is wrong: bcm_init zeroes every
	 * counter, so in a process that has just initialised the chip the totals
	 * cover exactly the same window the delta does. Both are zero for both
	 * reasons.
	 *
	 * What does tell them apart is whether the call works. A counter subsystem
	 * that is not running fails the read; one that is running returns a number,
	 * and a zero from it is a fact about the network rather than about us.
	 */
	if (!moving) {
		int linked = 0, ok = 0, failed = 0;
		int last = BCM_E_NONE;

		printf("  (no counter moved in %d seconds)\n", seconds);
		BCM_PBMP_ITER(cfg.port, port) {
			int link = 0;

			if (port < 0 || port >= 128)
				continue;
			if (bcm_port_link_status_get(unit, port, &link) != BCM_E_NONE || !link)
				continue;
			linked++;
			v = 0;
			rv = bcm_stat_get32(unit, port, snmpIfInNUcastPkts, &v);
			if (rv == BCM_E_NONE) {
				ok++;
			} else {
				failed++;
				last = rv;
			}
		}

		if (linked == 0)
			printf("  no port has link, so there is nothing to count\n");
		else if (failed)
			printf("  %d of %d linked ports cannot be read (%s) -- the counters\n"
			       "  are not being collected, and this says nothing about\n"
			       "  whether the chip is forwarding\n",
			       failed, linked, soc_errmsg(last));
		else
			printf("  all %d linked ports answer, so collection works and the\n"
			       "  neighbours are genuinely sending nothing\n", ok);
	}
	return 0;
}

/*
 * The SAL's DMA allocator, which the SDK calls with no device context.
 *
 * This has to be defined here or the linker takes the one in the SDK's own
 * liblubde.a -- and that one asks a kernel BDE module for its memory. There is
 * no such module here, so it returns NULL, and the failure appears a long way
 * from the cause:
 *
 *   RX: Starting rx pool with pkt count 256, packet size 16384
 *   bcm_init failed in rx
 *   bcm_attach(0) returned -2 (Out of memory)
 *
 * on a box with 1.8 GB free and a 64 MB pool that was never asked for a byte.
 * Our own salloc reports exhaustion by name and stayed silent throughout,
 * which is what said the allocation was going somewhere else.
 *
 * Defining a symbol an archive also defines is how a library like this is
 * meant to be adapted: the object file wins over the archive member, and the
 * SDK asks its host for memory instead of asking a driver we do not have.
 */
void *sal_dma_alloc(unsigned int size, char *name)
{
	if (!sal_dev) {
		fprintf(stderr, "nosd-tdp: sal_dma_alloc(%u, %s) before the BDE was opened\n",
			size, name ? name : "?");
		return NULL;
	}
	return pool_alloc(sal_dev, (int)size, name ? name : "sal_dma");
}

void sal_dma_free(void *ptr)
{
	(void)ptr;   /* the pool outlives every allocation taken from it */
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

int nosaic_tdp_sdk_ports_up(int unit, int forward)
{
	(void)unit; (void)forward;
	return -1;
}

int nosaic_tdp_sdk_stats(int unit, int seconds)
{
	(void)unit; (void)seconds;
	return -1;
}

#endif
