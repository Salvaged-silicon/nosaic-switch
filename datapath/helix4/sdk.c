/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The SDK's view of the Helix4 chip: soc_cm_device_vectors_t over our UIO BDE.
 *
 * The sequence is the same one every Broadcom datapath in this tree follows:
 *
 *     sal_core_init(), sal_appl_init()
 *     soc_cm_init()                            once
 *     soc_cm_device_create(dev_id, rev_id, c)  -> unit number
 *     soc_cm_device_init(unit, &vectors)       installs these, then soc_attach
 *
 * What differs is underneath, and it differs in three ways that are worth
 * naming before the code rather than after.
 *
 * ── 1. THERE IS NO PCI CONFIGURATION SPACE, AND THE SDK ASKS FOR ONE ─────────
 *
 * pci_conf_read and pci_conf_write are vectors this build calls. On the other
 * boards they are a file in sysfs. Here there is no bus, so they are answered
 * from a small synthesised structure — see conf_space below. That is what the
 * vendor's own BDE does for iProc devices too; it is not a shortcut.
 *
 * ── 2. THE SDK ALSO REACHES THE SoC'S OWN REGISTERS ──────────────────────────
 *
 * iproc_read and iproc_write take an absolute address in iProc address space
 * (soc/cmtypes.h), which is the 0x18000000 region — the same peripheral space
 * the kernel's i2c, UART and USB drivers live in.
 *
 * Mapping all of it into this process would let a bug here write a register
 * the kernel owns, on the bus the disk is behind. So nothing is mapped by
 * default and every access is recorded and refused by address. That is
 * deliberate and it is the fastest way to learn what this chip actually needs:
 * one boot prints the exact set, and the board can then map precisely those.
 * Guessing the set from vendor source would take longer and be less certain.
 *
 * ── 3. THE INTERRUPT IS REAL ─────────────────────────────────────────────────
 *
 * The AS5610 polls, because nothing delivers that chip's interrupt to
 * userspace; it costs a core and puts a floor under punt latency. Here the
 * interrupt arrives on a file descriptor, so interrupt_connect starts a thread
 * that waits on it and calls the SDK's handler. This is the first NOSaic board
 * where that is possible.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "bde.h"
#include "sdk.h"
#include "mmio.h"
#include "props.h"

#define TAG "nosd-helix4"

/*
 * Every iProc address the SDK asked for and did not get, so that one boot
 * produces the list instead of an afternoon with the vendor's source.
 * Deduplicated, because a register the SDK reads in a loop would otherwise be
 * the only one in the report.
 */
#define IPROC_MISS_MAX 32
static uint32_t iproc_miss[IPROC_MISS_MAX];
static int      iproc_miss_n;

int nosaic_helix4_sdk_iproc_misses(uint32_t *first, int max)
{
	int n = iproc_miss_n < max ? iproc_miss_n : max;

	for (int i = 0; i < n; i++)
		first[i] = iproc_miss[i];
	return iproc_miss_n;
}

#ifdef NOSAIC_WITH_SDK

static void iproc_miss_record(uint32_t addr, const char *how)
{
	for (int i = 0; i < iproc_miss_n; i++)
		if (iproc_miss[i] == addr)
			return;
	if (iproc_miss_n < IPROC_MISS_MAX)
		iproc_miss[iproc_miss_n++] = addr;
	fprintf(stderr, TAG ": the SDK tried to %s iProc register %#010x, which is "
		"not mapped.\n", how, addr);
	if (iproc_miss_n == 1)
		fprintf(stderr,
			"  Nothing in the SoC's peripheral space is mapped into this process "
			"on purpose:\n"
			"  the kernel's i2c, UART and USB drivers live in the same region, and "
			"the disk\n"
			"  is behind one of them. Collect the addresses this reports, then map "
			"exactly\n"
			"  those by adding a reg range to the CMIC node in the board's device "
			"tree.\n");
}

#include <sal/types.h>
#include <sal/core/boot.h>
#include <sal/appl/sal.h>
#include <soc/cm.h>
#include <soc/cmext.h>
#include <soc/cmtypes.h>
#include <soc/error.h>
#include <shared/bsltypes.h>
#include <shared/bslext.h>
#include <bcm/port.h>
#include <bcm/stg.h>
#include <bcm/vlan.h>
#include <bcm/link.h>
#include <bcm/error.h>
#include <stdarg.h>
#include <pthread.h>
#include <errno.h>

#include "tapbridge.h"
#include "l3sync.h"
#include "query.h"

/*
 * Bus and device type.
 *
 * ⚠ THIS IS THE SINGLE MOST LIKELY LINE IN THIS FILE TO BE WRONG, and it is
 * one word, so it is worth stating the reasoning rather than the conclusion.
 *
 * SAL_AXI_DEV_TYPE exists in include/sal/types.h and describes exactly what
 * this is: a switch device on the SoC's own bus rather than behind PCI. What
 * makes it uncertain is that the SDK never references it — a grep of the whole
 * 6.5.24 tree finds the definition and no use. The vendor's own BDE carries an
 * equivalent in its private vocabulary (BDE_AXI_DEV_TYPE) and does treat it
 * specially, so the concept is real; whether the SAL-side constant reaches
 * anything is not established.
 *
 * The alternative, if attach behaves as though a PCI-only path was skipped, is
 *
 *     SAL_PCI_DEV_TYPE | SAL_SWITCH_DEV_TYPE
 *
 * which is what every other board here uses and which the synthesised
 * configuration space below is already able to serve. Try that second, and
 * write down which one worked.
 */
#define NOSAIC_BUS_TYPE (SAL_AXI_DEV_TYPE | SAL_SWITCH_DEV_TYPE)

static struct nosaic_helix4_bde *sal_dev;

static struct nosaic_helix4_bde *bde_of(soc_cm_dev_t *dev)
{
	return (struct nosaic_helix4_bde *)dev->cookie;
}

/* ------------------------------------------------------- register access */

static uint32 nosaic_read(soc_cm_dev_t *dev, uint32 addr)
{
	return nosaic_helix4_bde_rd(bde_of(dev), addr);
}

static void nosaic_write(soc_cm_dev_t *dev, uint32 addr, uint32 data)
{
	nosaic_helix4_bde_wr(bde_of(dev), addr, data);
}

static void nosaic_write64(soc_cm_dev_t *dev, uint32 addr, uint64 data)
{
	/* Two 32-bit stores, low word first, which is the order the CMIC latches
	 * a 64-bit register in. The barrier between them is not optional even on
	 * a little-endian host: the compiler is free to merge or reorder two
	 * stores to adjacent addresses, and the chip latches on the second. */
	nosaic_helix4_bde_wr(bde_of(dev), addr, (uint32)(COMPILER_64_LO(data)));
	nosaic_mmio_barrier();
	nosaic_helix4_bde_wr(bde_of(dev), addr + 4, (uint32)(COMPILER_64_HI(data)));
}

static uint64 nosaic_read64(soc_cm_dev_t *dev, uint32 addr)
{
	uint64 v;
	uint32 lo = nosaic_helix4_bde_rd(bde_of(dev), addr);
	uint32 hi = nosaic_helix4_bde_rd(bde_of(dev), addr + 4);

	COMPILER_64_SET(v, hi, lo);
	return v;
}

/*
 * iProc address space. Nothing is mapped; see the header comment.
 *
 * Returning 0 rather than 0xffffffff for a read is deliberate: all-ones is
 * what a real unmapped bus returns and the SDK has code that treats it as a
 * missing device, which would send the failure somewhere unrelated. Zero plus
 * a line on stderr naming the address keeps the cause and the symptom
 * together.
 */
static uint32 nosaic_iproc_read(soc_cm_dev_t *dev, uint32 addr)
{
	iproc_miss_record(addr, "read");
	return 0;
}

static void nosaic_iproc_write(soc_cm_dev_t *dev, uint32 addr, uint32 data)
{
	iproc_miss_record(addr, "write");
}

/*
 * A synthesised PCI configuration space.
 *
 * The chip is not on a bus and the SDK asks anyway. Rather than return zeroes
 * — which reads as a device that is present, has no capabilities, and has bus
 * mastering disabled — this answers the handful of fields that mean something
 * for an on-die device and says so for the rest.
 *
 * COMMAND is the interesting one. On a PCI board its Bus Master Enable bit is
 * what lets the chip reach memory, and on the AS5610 a bridge above it with
 * that bit clear cost days. Here there is nothing between the chip and the
 * memory controller, so the bit has nothing to gate; it reads set because that
 * is the truth about this hardware, not to satisfy a check.
 */
#define PCI_VENDOR_BROADCOM 0x14e4

static uint32 nosaic_pci_conf_read(soc_cm_dev_t *dev, uint32 addr)
{
	struct nosaic_helix4_bde *b = bde_of(dev);

	switch (addr & ~3u) {
	case 0x00:      /* vendor and device */
		return ((uint32)b->device_id << 16) | PCI_VENDOR_BROADCOM;
	case 0x04:      /* command and status: memory space and bus master */
		return 0x00000006;
	case 0x08:      /* class code (network, other) and revision */
		return 0x02800000u | b->rev_id;
	case 0x10:      /* BAR0: where the register window is */
		return (uint32)b->bar_phys;
	default:
		return 0;
	}
}

static void nosaic_pci_conf_write(soc_cm_dev_t *dev, uint32 addr, uint32 data)
{
	/* Nothing to write to. Reported once rather than per access: the SDK
	 * writes COMMAND during attach on every board, and a line per write
	 * would bury everything else in the bring-up log. */
	static int said;

	if (!said) {
		said = 1;
		fprintf(stderr, TAG ": the SDK wrote PCI config %#x = %#x; this chip is "
			"not on a bus, so it went nowhere.\n"
			"  Harmless for COMMAND, which is what attach writes. Worth reading "
			"twice for anything else.\n", addr, data);
	}
}

/* --------------------------------------------------------------- DMA pool */

static void *nosaic_salloc(soc_cm_dev_t *dev, int size, const char *name)
{
	struct nosaic_helix4_bde *b = bde_of(dev);

	/* The SDK's salloc takes a signed size. A negative one is a bug above us,
	 * and passing it to something that takes size_t turns it into a request
	 * for most of the address space. */
	if (size < 0) {
		fprintf(stderr, TAG ": salloc(%d, %s): negative size\n",
			size, name ? name : "?");
		return NULL;
	}
	return nosaic_dmapool_alloc(&b->pool, (size_t)size, name);
}

static void nosaic_sfree(soc_cm_dev_t *dev, void *ptr)
{
	nosaic_dmapool_free(&bde_of(dev)->pool, ptr);
}

/*
 * Physical address of a pointer into the pool. The chip is given these, so
 * getting it wrong is a DMA to somewhere else in RAM.
 *
 * A pointer outside the pool is a bug above us, and returning 0 for it hands
 * the chip physical address zero and waits for a completion that cannot come —
 * indistinguishable from a dead DMA engine. Said once, because if it happens
 * at all it happens for every entry in a table.
 */
static sal_paddr_t nosaic_l2p(soc_cm_dev_t *dev, void *addr)
{
	struct nosaic_helix4_bde *b = bde_of(dev);
	size_t off = (char *)addr - (char *)b->dma;
	static int complained;

	if (!b->dma || (char *)addr < (char *)b->dma || off >= b->dma_len) {
		if (!complained) {
			complained = 1;
			fprintf(stderr, TAG ": l2p(%p) is outside the DMA pool (%p..%p) -- "
				"the chip would be told to read physical address 0\n",
				addr, b->dma, (char *)b->dma + b->dma_len);
		}
		return 0;
	}
	return (sal_paddr_t)(b->dma_phys + off);
}

static void *nosaic_p2l(soc_cm_dev_t *dev, sal_paddr_t addr)
{
	struct nosaic_helix4_bde *b = bde_of(dev);
	uint64_t off = (uint64_t)addr - b->dma_phys;

	if (!b->dma || addr < b->dma_phys || off >= b->dma_len)
		return NULL;
	return (char *)b->dma + off;
}

/*
 * Cache maintenance around DMA.
 *
 * ⚠ NOT NO-OPS, AND THE AS5610 IS WHY.
 *
 * The tempting argument is that the pool is reserved no-map and mapped with
 * O_SYNC, so it is uncached and there is nothing to flush. That argument was
 * made on the AS5610, and with DMA enabled soc_misc_init timed out: a
 * descriptor the CPU had written was sitting in cache and the chip fetched the
 * memory behind it. The failure names a timeout, never a cache.
 *
 * Here the pool may also arrive through a UIO map, and uio_pdrv_genirq maps
 * UIO_MEM_PHYS with pgprot_noncached — but "may" is the operative word, and an
 * ARM host with a write-allocate L2 in front of it is not the place to bet on
 * it. __builtin___clear_cache is the portable form and compiles to nothing on
 * architectures that do not need it.
 */
static int nosaic_sflush(soc_cm_dev_t *dev, void *addr, int length)
{
	if (addr && length > 0)
		__builtin___clear_cache((char *)addr, (char *)addr + length);
	return 0;
}

static int nosaic_sinval(soc_cm_dev_t *dev, void *addr, int length)
{
	if (addr && length > 0)
		__builtin___clear_cache((char *)addr, (char *)addr + length);
	return 0;
}

/* ------------------------------------------------------------- properties */

static char *nosaic_config_var_get(soc_cm_dev_t *dev, const char *name)
{
	const char *v = nosaic_props_get_unit(name, 0);

	/* The SDK takes a non-const char *. It does not write to it. */
	return (char *)(uintptr_t)v;
}

/* ------------------------------------------------------------- interrupts */

/*
 * The interrupt, which this board has and the others do not.
 *
 * uio_pdrv_genirq masks the interrupt at the controller each time it fires and
 * will not deliver another until the device is re-armed by writing to it. So
 * the loop is: arm, wait, call the SDK's handler, arm again. Getting the
 * re-arm wrong gives exactly one interrupt and then a switch that looks hung
 * with no error anywhere.
 */
static struct {
	pthread_t         thread;
	soc_cm_isr_func_t handler;
	void             *data;
	soc_cm_dev_t     *dev;
	volatile int      stop;
	int               running;
} isr;

static void *isr_thread(void *arg)
{
	struct nosaic_helix4_bde *b = bde_of(isr.dev);

	while (!isr.stop) {
		int n;

		if (nosaic_helix4_bde_arm_irq(b) != 0)
			break;
		/* A timeout rather than an indefinite wait, so that stopping the
		 * daemon does not depend on the chip raising one more interrupt. */
		n = nosaic_helix4_bde_wait_irq(b, 200);
		if (n < 0)
			break;
		if (n > 0 && isr.handler)
			isr.handler(isr.data);
	}
	isr.running = 0;
	return NULL;
}

static int nosaic_interrupt_connect(soc_cm_dev_t *dev,
				    soc_cm_isr_func_t handler, void *data)
{
	if (isr.running)
		return -1;
	isr.handler = handler;
	isr.data    = data;
	isr.dev     = dev;
	isr.stop    = 0;
	isr.running = 1;
	if (pthread_create(&isr.thread, NULL, isr_thread, NULL) != 0) {
		isr.running = 0;
		fprintf(stderr, TAG ": could not start the interrupt thread: %s\n",
			strerror(errno));
		return -1;
	}
	printf("  interrupts: delivered through /dev/%s\n", bde_of(dev)->uio);
	return 0;
}

static int nosaic_interrupt_disconnect(soc_cm_dev_t *dev)
{
	if (!isr.running)
		return 0;
	isr.stop = 1;
	pthread_join(isr.thread, NULL);
	isr.handler = NULL;
	return 0;
}

/* ------------------------------------------------------------------ attach */

static int bsl_vfprintf(void *file, const char *format, va_list args)
{
	return vfprintf(file ? file : stdout, format, args);
}

static int bsl_out_hook(bsl_meta_t *meta, const char *format, va_list args)
{
	return vfprintf(stdout, format, args);
}

/* Without this the SDK's own diagnostics go nowhere and a failure is a return
 * code with no sentence attached to it. */
static void nosaic_bsl_start(void)
{
	bsl_config_t cfg;

	bsl_config_t_init(&cfg);
	cfg.out_hook   = bsl_out_hook;
	cfg.check_hook = NULL;
	cfg.vfprintf   = bsl_vfprintf;
	if (bsl_init(&cfg) < 0)
		fprintf(stderr, TAG ": could not start the SDK log; failures below "
			"will be numbers without sentences\n");
}

int nosaic_helix4_sdk_attach(struct nosaic_helix4_bde *b, uint16_t dev_id, uint8_t rev_id)
{
	soc_cm_device_vectors_t v;
	int unit, rv;

	sal_dev = b;
	nosaic_bsl_start();

	if (sal_core_init() < 0) {
		fprintf(stderr, TAG ": sal_core_init failed\n");
		return -1;
	}
	if (sal_appl_init() < 0) {
		fprintf(stderr, TAG ": sal_appl_init failed\n");
		return -1;
	}
	if (soc_cm_init() < 0) {
		fprintf(stderr, TAG ": soc_cm_init failed\n");
		return -1;
	}

	unit = soc_cm_device_create(dev_id, rev_id, b);
	if (unit < 0) {
		fprintf(stderr, TAG ": soc_cm_device_create(%#x, %#x) failed: %d\n"
			"  the SDK does not recognise this device id, or was built without\n"
			"  support for it. BCM56340 support is src/soc/esw/helix4.c and\n"
			"  src/soc/mcm/bcm56340_a0.c; if those were not compiled in, the\n"
			"  openbcm package's chip selection is what to look at.\n",
			dev_id, rev_id, unit);
		return -1;
	}

	memset(&v, 0, sizeof(v));
	v.init     = 1;
	v.bus_type = NOSAIC_BUS_TYPE;

	/*
	 * Little-endian, all three, which is the x86 answer and not the AS5610's.
	 * This host is little-endian and the chip's default matches it, so unlike
	 * that board nothing has to be written to CMIC_ENDIAN_SELECT first — and
	 * the SDK is built here with SYS_BE_PIO=0 to say the same thing, by way
	 * of platform=iproc-4_14 in the openbcm recipe.
	 */
	v.big_endian_pio    = 0;
	v.big_endian_packet = 0;
	v.big_endian_other  = 0;

	v.config_var_get       = nosaic_config_var_get;
	v.interrupt_connect    = nosaic_interrupt_connect;
	v.interrupt_disconnect = nosaic_interrupt_disconnect;
	v.read            = nosaic_read;
	v.write           = nosaic_write;
	v.read64          = nosaic_read64;
	v.write64         = nosaic_write64;
	v.pci_conf_read   = nosaic_pci_conf_read;
	v.pci_conf_write  = nosaic_pci_conf_write;
	v.iproc_read      = nosaic_iproc_read;
	v.iproc_write     = nosaic_iproc_write;
	v.salloc          = nosaic_salloc;
	v.sfree           = nosaic_sfree;
	v.sflush          = nosaic_sflush;
	v.sinval          = nosaic_sinval;
	v.l2p             = nosaic_l2p;
	v.p2l             = nosaic_p2l;

	/*
	 * soc_cm_device_init installs the vectors and then calls soc_attach, so a
	 * failure here is either a rejected vector table or anything in the
	 * bring-up behind it. The return code is the whole diagnosis:
	 * SOC_E_PARAM means a vector this build requires is missing, anything
	 * else came from the attach.
	 */
	rv = soc_cm_device_init(unit, &v);
	if (rv < 0) {
		fprintf(stderr, TAG ": soc_cm_device_init(unit %d) returned %d (%s)\n"
			"  either a rejected vector table or a failure inside soc_attach;\n"
			"  the SDK log above says which.\n", unit, rv, soc_errmsg(rv));
		return -1;
	}
	return unit;
}

/*
 * Declared here rather than by including <soc/drv.h>: that header is written
 * for the SDK's own translation units and needs the generated per-chip
 * register database, which only exists once the SDK's full chip-selection
 * defines are in scope. Pulling it in to reach four functions would mean
 * replicating the SDK's build configuration here and keeping it in step.
 */
extern int soc_reset_init(int unit);
extern int soc_misc_init(int unit);
extern int soc_mmu_init(int unit);
extern int bcm_attach(int unit, char *type, char *subtype, int remunit);
extern int bcm_init(int unit);

int nosaic_helix4_sdk_soc_init(int unit)
{
	/* soc_reset_init rather than soc_init: the difference is the reset. Both
	 * end in soc_init_done, and only soc_reset_init passes TRUE, which is
	 * what puts a chip left running by a previous daemon back into a state
	 * this one can reason about. */
	int rv = soc_reset_init(unit);

	if (rv < 0) {
		fprintf(stderr, TAG ": soc_reset_init(%d) returned %d (%s)\n",
			unit, rv, soc_errmsg(rv));
		return -1;
	}
	return 0;
}

int nosaic_helix4_sdk_bcm_init(int unit)
{
	int rv;

	printf("  soc_misc_init...\n");
	fflush(stdout);
	rv = soc_misc_init(unit);
	if (rv < 0) {
		fprintf(stderr, TAG ": soc_misc_init(%d) returned %d (%s)\n",
			unit, rv, soc_errmsg(rv));
		return -1;
	}

	printf("  soc_mmu_init...\n");
	fflush(stdout);
	rv = soc_mmu_init(unit);
	if (rv < 0) {
		fprintf(stderr, TAG ": soc_mmu_init(%d) returned %d (%s)\n",
			unit, rv, soc_errmsg(rv));
		return -1;
	}

	printf("  bcm_attach...\n");
	fflush(stdout);
	rv = bcm_attach(unit, "bcm", "bcm", unit);
	if (rv < 0) {
		fprintf(stderr, TAG ": bcm_attach(%d) returned %d (%s)\n",
			unit, rv, soc_errmsg(rv));
		return -1;
	}

	printf("  bcm_init...\n");
	fflush(stdout);
	rv = bcm_init(unit);
	if (rv < 0) {
		fprintf(stderr, TAG ": bcm_init(%d) returned %d (%s)\n"
			"  bcm_init rolls the whole unit back on any module's failure, so "
			"one\n  module failing reads as total failure. The SDK log above "
			"names it.\n", unit, rv, soc_errmsg(rv));
		return -1;
	}
	return 0;
}

int nosaic_helix4_sdk_ports_up(int unit, int forward)
{
	bcm_port_config_t cfg;
	bcm_port_t port;
	int rv, up = 0;

	if ((rv = bcm_port_config_get(unit, &cfg)) < 0) {
		fprintf(stderr, TAG ": bcm_port_config_get: %s\n", bcm_errmsg(rv));
		return -1;
	}

	if (forward)
		printf("  bridging every port in VLAN 1 -- this is a loop wherever two "
		       "ports reach the same neighbour\n");

	BCM_PBMP_ITER(cfg.port, port) {
		if ((rv = bcm_port_enable_set(unit, port, 1)) < 0) {
			fprintf(stderr, TAG ": port %d: enable: %s\n", port, bcm_errmsg(rv));
			continue;
		}
		if (!forward) {
			int vid = NOSAIC_HELIX4_SERVICE_VLAN_BASE + port;
			bcm_pbmp_t pbmp, ubmp;

			BCM_PBMP_CLEAR(pbmp);
			BCM_PBMP_CLEAR(ubmp);
			BCM_PBMP_PORT_ADD(pbmp, port);
			BCM_PBMP_PORT_ADD(ubmp, port);
			BCM_PBMP_PORT_ADD(pbmp, CMIC_PORT(unit));

			if ((rv = bcm_vlan_create(unit, vid)) < 0 && rv != BCM_E_EXISTS) {
				fprintf(stderr, TAG ": vlan %d: create: %s\n", vid, bcm_errmsg(rv));
				continue;
			}
			if ((rv = bcm_vlan_port_add(unit, vid, pbmp, ubmp)) < 0) {
				fprintf(stderr, TAG ": vlan %d: adding port %d: %s\n",
					vid, port, bcm_errmsg(rv));
				continue;
			}
			if ((rv = bcm_port_untagged_vlan_set(unit, port, vid)) < 0) {
				fprintf(stderr, TAG ": port %d: untagged vlan: %s\n",
					port, bcm_errmsg(rv));
				continue;
			}
		}
		if ((rv = bcm_port_stp_set(unit, port, BCM_STG_STP_FORWARD)) < 0) {
			fprintf(stderr, TAG ": port %d: forwarding: %s\n", port, bcm_errmsg(rv));
			continue;
		}
		up++;
	}

	/*
	 * VLAN 1 is emptied, and it is not tidiness. EdgeNOS's note on this
	 * hardware family is specific: leaving it populated lets the chip's L2
	 * lookup pick the wrong egress when the CPU injects a tagged frame on a
	 * service VID, and they watched it happen.
	 */
	if (!forward) {
		bcm_pbmp_t all, none;

		BCM_PBMP_ASSIGN(all, cfg.port);
		BCM_PBMP_CLEAR(none);
		if ((rv = bcm_vlan_port_remove(unit, 1, all)) < 0 && rv != BCM_E_NOT_FOUND)
			fprintf(stderr, TAG ": emptying VLAN 1: %s\n", bcm_errmsg(rv));
	}

	printf("  %d port(s) enabled and forwarding\n", up);
	return up > 0 ? 0 : -1;
}

/*
 * Which ports reach Linux, and under what names.
 *
 * From properties rather than from a table here, because which of 54 ports a
 * particular switch routes on is that switch's configuration and not a fact
 * about Helix4:
 *
 *     tap_swp1=1:3301:1600      port : vlan : mtu
 *
 * The vlan is what stops the chip tagging what it sends on a routed port; the
 * mtu has to match the neighbour, because OSPF carries it in its database
 * description packets and refuses an adjacency when the two disagree — which
 * presents as ExStart and no message.
 */
int nosaic_helix4_sdk_run(int unit)
{
	struct tap_spec specs[64];
	char names[64][32];
	int ntap = 0, i;

	for (i = 0; i < nosaic_props_count() && ntap < 64; i++) {
		const char *name = nosaic_props_name(i);
		const char *val = nosaic_props_value(i);
		const char *colon;

		if (name == NULL || strncmp(name, "tap_", 4) != 0)
			continue;
		snprintf(names[ntap], sizeof(names[ntap]), "%s", name + 4);
		specs[ntap].name = names[ntap];
		specs[ntap].port = atoi(val);
		specs[ntap].vlan = 0;
		specs[ntap].mtu = 0;
		colon = strchr(val, ':');
		if (colon != NULL) {
			specs[ntap].vlan = atoi(colon + 1);
			colon = strchr(colon + 1, ':');
			if (colon != NULL)
				specs[ntap].mtu = atoi(colon + 1);
		}
		ntap++;
	}

	if (ntap == 0) {
		printf("taps         none declared; no port is on the Linux stack\n"
		       "             (add tap_<name>=<port>:<vlan> to have one)\n");
		return 0;
	}

	if (nosaic_tap_start(unit, specs, ntap) < 0) {
		fprintf(stderr, TAG ": could not bridge ports to Linux\n");
		return -1;
	}

	/* A router interface per tap, with the tap's own MAC read back rather
	 * than the one we think we asked for: an interface whose MAC differs
	 * from the tap's answers ARP and then drops everything addressed to the
	 * reply. */
	for (i = 0; i < nosaic_tap_count(); i++) {
		const char *name;
		unsigned char mac[6];
		int port, vlan, mtu;

		if (nosaic_tap_info(i, &name, &port, &vlan, &mtu, mac) != 0)
			continue;
		if (vlan <= 0) {
			printf("l3           %s has no vlan, so it gets no router interface\n",
			       name);
			continue;
		}
		nosaic_l3_add_intf(unit, name, port, vlan, mac, mtu);
	}

	printf("taps         %d port(s) on the Linux stack\n", nosaic_tap_count());
	fflush(stdout);

	/* The diagnostic socket, before the pump takes the thread for good. Not
	 * fatal if it fails: a switch that forwards without a way to ask it
	 * questions is better than one that refuses to start without one. */
	if (sal_dev != NULL)
		nosaic_query_set_dmapool(&sal_dev->pool);
	nosaic_query_start(unit, NOSAIC_QUERY_SOCKET);

	/*
	 * The FIB mirror runs on its own thread and this one does nothing but
	 * move packets.
	 *
	 * That split is not stylistic. On the AS5610 the periodic work ran on
	 * the packet thread and the pump called its tick on every poll wakeup
	 * rather than on a timer, so the FIB mirror ran once per received frame
	 * and a full counter sync every thirtieth. A NULL tick here makes the
	 * pump block in poll() until a frame arrives.
	 */
	{
		pthread_t t;
		extern void *nosaic_helix4_periodic(void *arg);

		if (pthread_create(&t, NULL, nosaic_helix4_periodic, NULL) != 0)
			fprintf(stderr, TAG ": no periodic thread: the routing table will "
				"not be mirrored into the chip\n");
	}
	nosaic_tap_pump(NULL, 0);
	return -1;
}

/* Mirroring the kernel FIB into the chip, on a timer rather than on traffic. */
void *nosaic_helix4_periodic(void *arg)
{
	(void)arg;
	for (;;) {
		nosaic_l3_poll();
		usleep(200 * 1000);
	}
	return NULL;
}

#else  /* !NOSAIC_WITH_SDK */

/*
 * Built without the vendor tree staged.
 *
 * A stub rather than a link error, so that a contributor without the SDK can
 * still build, test and change everything else here — and so that the probe's
 * argument handling is identical either way. It says which binary it is
 * rather than which symbol is missing, because the second is not a useful
 * thing to tell somebody.
 */
static int no_sdk(const char *what)
{
	fprintf(stderr, TAG ": %s needs the OpenBCM SDK, and this binary was built "
		"without it.\n  Build the openbcm package first, then rebuild this one:\n"
		"      make pkg PKG=openbcm ARCH=armhf\n"
		"      make pkg PKG=nosd-helix4 ARCH=armhf\n", what);
	return -1;
}

int nosaic_helix4_sdk_attach(struct nosaic_helix4_bde *b, uint16_t dev_id, uint8_t rev_id)
{
	(void)b; (void)dev_id; (void)rev_id;
	return no_sdk("attaching the chip");
}
int nosaic_helix4_sdk_soc_init(int unit) { (void)unit; return no_sdk("chip initialisation"); }
int nosaic_helix4_sdk_bcm_init(int unit) { (void)unit; return no_sdk("chip initialisation"); }
int nosaic_helix4_sdk_ports_up(int unit, int forward)
{
	(void)unit; (void)forward;
	return no_sdk("bringing ports up");
}
int nosaic_helix4_sdk_stats(int unit, int seconds)
{
	(void)unit; (void)seconds;
	return no_sdk("reading counters");
}
int nosaic_helix4_sdk_run(int unit) { (void)unit; return no_sdk("running the datapath"); }

#endif /* NOSAIC_WITH_SDK */
