/*
 * The SDK's view of the chip: soc_cm_device_vectors_t over our userspace BDE.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * WHY THIS IS THE SHAPE IT IS
 *
 * The SDK reaches the device through these vectors and nothing else. That is
 * the property the whole design rests on: fill in fourteen function pointers
 * and everything above them is unmodified vendor code, including the Trident2
 * MMU and LLS initialisation that hand-reproduction repeatedly failed to match
 * on this silicon. Chip initialisation is deliberately not ours to write.
 *
 * The sequence the SDK expects (include/soc/cmext.h:13,75,121):
 *
 *     soc_cm_init()                            once
 *     soc_cm_device_create(dev_id, rev_id, c)  -> unit number
 *     soc_cm_device_init(unit, &vectors)       installs these
 *
 * The cookie passed to device_create comes back in every vector as
 * dev->cookie (include/soc/cmtypes.h:47), which is how a vector with no
 * context of its own finds the BDE it belongs to. That matters because it is
 * the only per-device state these functions get: everything else about the
 * mapping lives in struct nosaic_bde.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bde.h"
#include "props.h"
#include "sdk.h"

/* The SDK's own headers. Included last: they define types with names general
 * enough to collide with anything declared after them. */
#include <sal/types.h>
#include <soc/cm.h>
#include <soc/cmext.h>
#include <soc/cmtypes.h>
#include <soc/error.h>
#include <shared/bsltypes.h>
#include <shared/bslext.h>

/* Bus and device type, from include/sal/types.h:269,283. A PCI-attached switch
 * chip -- stated rather than defaulted, because the SDK selects access paths
 * from it and a wrong value here fails much further downstream. */
#define NOSAIC_BUS_TYPE (SAL_PCI_DEV_TYPE | SAL_SWITCH_DEV_TYPE)

static struct nosaic_bde *bde_of(soc_cm_dev_t *dev)
{
	return (struct nosaic_bde *)dev->cookie;
}

/*
 * Register access.
 *
 * addr is a byte offset into BAR0. It is bounds-checked on every access rather
 * than trusted: this is the path every register read and write in the SDK
 * takes, an out-of-range offset is a wild access into whatever follows the
 * mapping, and the cost of checking is nothing next to the cost of not.
 */
static uint32 nosaic_read(soc_cm_dev_t *dev, uint32 addr)
{
	struct nosaic_bde *b = bde_of(dev);

	if ((size_t)addr + 4 > b->bar_len) {
		fprintf(stderr, "nosd-td2p: read past BAR0: %#x\n", addr);
		return 0xffffffff;
	}
	return *(volatile uint32 *)((volatile char *)b->bar + addr);
}

static void nosaic_write(soc_cm_dev_t *dev, uint32 addr, uint32 data)
{
	struct nosaic_bde *b = bde_of(dev);

	if ((size_t)addr + 4 > b->bar_len) {
		fprintf(stderr, "nosd-td2p: write past BAR0: %#x\n", addr);
		return;
	}
	*(volatile uint32 *)((volatile char *)b->bar + addr) = data;
}

/* PCI configuration space, through sysfs rather than port I/O. */
static uint32 nosaic_pci_conf_read(soc_cm_dev_t *dev, uint32 addr)
{
	return nosaic_bde_cfg_read(bde_of(dev), addr);
}

static void nosaic_pci_conf_write(soc_cm_dev_t *dev, uint32 addr, uint32 data)
{
	nosaic_bde_cfg_write(bde_of(dev), addr, data);
}

/*
 * DMA memory.
 *
 * Drawn from the region reserved on the kernel command line and mapped through
 * /dev/mem, so it is physically contiguous -- which a pagemap walk over
 * ordinary anonymous memory cannot guarantee, and the SDK needs for a pool
 * rather than for one descriptor at a time.
 */
static void *nosaic_salloc(soc_cm_dev_t *dev, int size, const char *name)
{
	return sal_dma_alloc((unsigned int)size, (char *)name);
}

static void nosaic_sfree(soc_cm_dev_t *dev, void *ptr)
{
	sal_dma_free(ptr);
}

/*
 * Cache maintenance: nothing to do.
 *
 * x86 DMA is coherent, so there is no flush or invalidate to issue. Returning
 * success is correct here and would be a silent lie on an architecture where
 * it is not -- which is why it says so rather than being an empty function
 * someone later assumes was a stub.
 */
static int nosaic_sflush(soc_cm_dev_t *dev, void *addr, int length)
{
	return 0;
}

static int nosaic_sinval(soc_cm_dev_t *dev, void *addr, int length)
{
	return 0;
}

/*
 * Address translation.
 *
 * The DMA region sits at a known physical address and is mapped in one piece,
 * so translation is a base plus an offset in both directions. A pointer
 * outside the region is a bug in the caller and is reported rather than
 * translated into a plausible-looking address that would corrupt memory
 * somewhere else.
 */
static sal_paddr_t nosaic_l2p(soc_cm_dev_t *dev, void *addr)
{
	struct nosaic_bde *b = bde_of(dev);
	size_t off;

	if (addr == NULL)
		return 0;
	off = (size_t)((char *)addr - (char *)b->dma);
	if (off >= b->dma_len) {
		fprintf(stderr, "nosd-td2p: l2p of %p, which is not in the DMA region\n", addr);
		return 0;
	}
	return (sal_paddr_t)(b->dma_phys + off);
}

static void *nosaic_p2l(soc_cm_dev_t *dev, sal_paddr_t addr)
{
	struct nosaic_bde *b = bde_of(dev);
	uint64_t off;

	if (addr == 0)
		return NULL;
	off = (uint64_t)addr - b->dma_phys;
	if (off >= b->dma_len) {
		fprintf(stderr, "nosd-td2p: p2l of %#llx, which is not in the DMA region\n",
			(unsigned long long)addr);
		return NULL;
	}
	return (char *)b->dma + off;
}

/*
 * Interrupts: not connected.
 *
 * The SDK polls when no handler is installed, which is what this driver wants
 * during bring-up -- an interrupt path is one more thing that can be wrong
 * while the question is still whether the chip initialises at all. Returning
 * success without connecting anything would be the dangerous version of this;
 * these say plainly that nothing was installed.
 */
static int nosaic_interrupt_connect(soc_cm_dev_t *dev,
				    soc_cm_isr_func_t handler, void *data)
{
	return -1;
}

static int nosaic_interrupt_disconnect(soc_cm_dev_t *dev)
{
	return -1;
}

/*
 * Configuration properties.
 *
 * This is Broadcom's config.bcm mechanism seen from the inside: port maps,
 * per-port settings, feature overrides. NULL means "not set", which the SDK
 * reads as "use the default for this chip".
 *
 * The answers come from a file the board supplies, not from this code. A port
 * map is a fact about how one switch is wired, and every board with this ASIC
 * wires it differently -- compiling one in would make the thing that must vary
 * per board the one thing that cannot.
 */
static char *nosaic_config_var_get(soc_cm_dev_t *dev, const char *name)
{
	/* The SDK's own prototype is not const-correct; the value is not
	 * modified, and casting here keeps that fact in one place. */
	return (char *)nosaic_props_get(name);
}

/*
 * The SDK's own log, sent to stderr.
 *
 * Worth doing before anything else. Every failure inside the SDK reports
 * itself through here first and then returns a small negative number, so
 * without a sink the caller gets the number and none of the sentence that
 * explains it -- which is the difference between "soc_attach returned -4" and
 * being told which parameter it objected to.
 */
static int nosaic_bsl_out(bsl_meta_t *meta, const char *fmt, va_list args)
{
	return vfprintf(stderr, fmt, args);
}

/* Let everything through. This is bring-up: the messages that matter are the
 * ones nobody predicted needing. */
static int nosaic_bsl_check(bsl_packed_meta_t meta)
{
	return 1;
}

static void nosaic_bsl_start(void)
{
	bsl_config_t cfg;

	bsl_config_t_init(&cfg);
	cfg.out_hook = nosaic_bsl_out;
	cfg.check_hook = nosaic_bsl_check;
	if (bsl_init(&cfg) < 0)
		fprintf(stderr, "nosd-td2p: could not start the SDK log; "
			"failures below will be numbers without sentences\n");
}

int nosaic_sdk_attach(struct nosaic_bde *b, uint16 dev_id, uint16 rev_id)
{
	soc_cm_device_vectors_t v;
	int unit, rv;

	/* The SAL DMA hooks carry no device context, so the BDE they draw from
	 * is set before anything in the SDK can call them. */
	nosaic_bde_set_sal_device(b);
	nosaic_bsl_start();

	if (soc_cm_init() < 0) {
		fprintf(stderr, "nosd-td2p: soc_cm_init failed\n");
		return -1;
	}

	unit = soc_cm_device_create(dev_id, rev_id, b);
	if (unit < 0) {
		fprintf(stderr, "nosd-td2p: soc_cm_device_create(%#x, %#x) failed: %d\n"
			"  the SDK does not recognise this device id, or was built "
			"without support for it\n", dev_id, rev_id, unit);
		return -1;
	}

	memset(&v, 0, sizeof(v));
	v.init                 = 1;
	v.bus_type             = NOSAIC_BUS_TYPE;
	v.big_endian_pio       = 0;
	v.big_endian_packet    = 0;
	v.big_endian_other     = 0;
	v.config_var_get       = nosaic_config_var_get;
	v.interrupt_connect    = nosaic_interrupt_connect;
	v.interrupt_disconnect = nosaic_interrupt_disconnect;
	v.read                 = nosaic_read;
	v.write                = nosaic_write;
	v.pci_conf_read        = nosaic_pci_conf_read;
	v.pci_conf_write       = nosaic_pci_conf_write;
	v.salloc               = nosaic_salloc;
	v.sfree                = nosaic_sfree;
	v.sflush               = nosaic_sflush;
	v.sinval               = nosaic_sinval;
	v.l2p                  = nosaic_l2p;
	v.p2l                  = nosaic_p2l;

	/*
	 * This is where the SDK takes over. soc_cm_device_init installs the
	 * vectors and then calls soc_attach (src/soc/common/cm.c), which is chip
	 * initialisation proper -- so a failure here can be a rejected vector
	 * table or anything in the entire bring-up sequence behind it.
	 *
	 * The return code is therefore the whole diagnosis, and throwing it away
	 * to print "failed" leaves nothing to work from. SOC_E_PARAM means a
	 * vector this build requires is missing; anything else came from the
	 * attach.
	 */
	rv = soc_cm_device_init(unit, &v);
	if (rv < 0) {
		fprintf(stderr, "nosd-td2p: soc_cm_device_init(unit %d) returned %d (%s)\n",
			unit, rv, soc_errmsg(rv));
		fprintf(stderr, "  this is either a rejected vector table or a failure "
			"inside soc_attach; the SDK log above says which\n");
		return -1;
	}
	return unit;
}
