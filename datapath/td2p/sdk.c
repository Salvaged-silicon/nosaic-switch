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
#include <sal/core/boot.h>
#include <sal/appl/sal.h>
#include <sal/core/time.h>
#include <soc/cm.h>
#include <soc/cmext.h>
#include <soc/cmtypes.h>
#include <soc/error.h>
#include <bcm/init.h>
#include <bcm/port.h>
#include <bcm/link.h>
#include <bcm/stat.h>
#include <bcm/error.h>
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

/*
 * 64-bit register access.
 *
 * Some registers are 64 bits wide and the SDK reaches them through these
 * rather than through two 32-bit accesses -- which would not be equivalent on
 * a device where reading the low half latches the high one.
 */
static uint64 nosaic_read64(soc_cm_dev_t *dev, uint32 addr)
{
	struct nosaic_bde *b = bde_of(dev);

	if ((size_t)addr + 8 > b->bar_len) {
		fprintf(stderr, "nosd-td2p: 64-bit read past BAR0: %#x\n", addr);
		return ~(uint64)0;
	}
	return *(volatile uint64 *)((volatile char *)b->bar + addr);
}

static void nosaic_write64(soc_cm_dev_t *dev, uint32 addr, uint64 data)
{
	struct nosaic_bde *b = bde_of(dev);

	if ((size_t)addr + 8 > b->bar_len) {
		fprintf(stderr, "nosd-td2p: 64-bit write past BAR0: %#x\n", addr);
		return;
	}
	*(volatile uint64 *)((volatile char *)b->bar + addr) = data;
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

	/*
	 * The SDK's own abstraction layer, before anything that uses it.
	 *
	 * Nothing above works without this and the failure is not obviously
	 * related: soc_cm_init and soc_attach both complete, and soc_init then
	 * dies on an assertion deep in the lock implementation --
	 *
	 *   Assertion failed: (sl) at src/sal/core/unix/sync.c:972
	 *
	 * because a spinlock it takes was never created. The SDK's own startup
	 * does this first (systems/linux/user/common/socdiag.c:263) and so must
	 * anything else that drives it.
	 */
	if (sal_core_init() < 0) {
		fprintf(stderr, "nosd-td2p: sal_core_init failed\n");
		return -1;
	}
	if (sal_appl_init() < 0) {
		fprintf(stderr, "nosd-td2p: sal_appl_init failed\n");
		return -1;
	}

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
	v.read64               = nosaic_read64;
	v.write64              = nosaic_write64;

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

/*
 * Finish bringing the chip up, and survey its ports.
 *
 * soc_attach leaves the device initialised but not running. The rest of the
 * sequence is the SDK's own, in the order its diagnostic shell uses
 * (src/appl/diag/dev.c:202, src/appl/diag/shell.c:4836):
 *
 *     soc_init(unit)                     the SOC layer
 *     bcm_attach(unit, "esw", NULL, 0)   the BCM layer for a switch device
 *     bcm_init(unit)                     the software layer above it
 *
 * "esw" is the driver family for every Ethernet switch device in this SDK, the
 * Trident2+ included; the alternatives in that switch statement are for
 * Tomahawk3.
 */
/*
 * soc_init is declared here rather than by including <soc/drv.h>.
 *
 * That header is written for the SDK's own translation units and needs the
 * generated per-chip register database -- SOC_MAX_NUM_BLKS, NUM_SOC_REG and
 * the rest -- which only exists once the SDK's full chip-selection defines are
 * in scope. Pulling that in to reach one function would mean replicating the
 * SDK's build configuration here and keeping it in step, which is a larger and
 * more fragile dependency than the declaration itself.
 *
 * The signature is from include/soc/drv.h:6537. If it ever changes the linker
 * will not notice, which is the cost of doing it this way and the reason it is
 * confined to this one function.
 */
extern int soc_init(int unit);
extern int soc_reset_init(int unit);
extern int soc_misc_init(int unit);
extern int soc_mmu_init(int unit);

/*
 * Bring the SOC layer up, resetting the chip on the way.
 *
 * soc_reset_init rather than soc_init, and the difference is the whole
 * problem. Both call soc_do_init (src/soc/common/drv.c:361,489); soc_init
 * passes FALSE for the reset argument and soc_reset_init passes TRUE.
 *
 * soc_init therefore initialises a chip it assumes is already reset. On a
 * switch running the vendor OS, or one where the SDK's kernel BDE loaded, that
 * assumption holds. Here nothing has ever reset this chip: NOSaic released it
 * from the board controller's reset and mapped it, and no kernel driver is
 * bound to it at all. Its pipeline blocks are still held.
 *
 * The symptom was that everything appeared to work and nothing answered.
 * soc_init returned success after twenty-six thousand lines of initialisation,
 * and then the first table write in bcm_attach got no SBUS acknowledgement --
 * which reads as broken silicon or a bad port map. Two independent paths
 * agreeing that blocks were silent is what pointed here: S-Channel found no
 * block answering either, before and after soc_init alike.
 */
int nosaic_sdk_soc_init(int unit)
{
	int rv;

	rv = soc_reset_init(unit);
	if (rv < 0) {
		fprintf(stderr, "nosd-td2p: soc_reset_init(%d) returned %d (%s)\n",
			unit, rv, soc_errmsg(rv));
		return -1;
	}
	return 0;
}

int nosaic_sdk_bcm_init(int unit)
{
	int rv;

	/*
	 * Two SOC-layer steps come between the chip reset and the BCM layer, and
	 * skipping them is why port probing failed with "Feature not initialized":
	 * the ports were probed against a device whose memory bounds and MMU had
	 * never been set up.
	 *
	 *   soc_misc_init  populates the memory-state index bounds
	 *   soc_mmu_init   _soc_trident2_mmu_init and soc_td2_lls_init -- the
	 *                  Trident2 MMU and link-list scheduler, and precisely the
	 *                  sequences that hand-reproduction failed to match on
	 *                  this silicon. Running the vendor's own is the reason
	 *                  the BDE exists.
	 */
	printf("  soc_misc_init...\n");
	rv = soc_misc_init(unit);
	if (rv < 0) {
		fprintf(stderr, "nosd-td2p: soc_misc_init(%d) returned %d (%s)\n",
			unit, rv, soc_errmsg(rv));
		return -1;
	}

	printf("  soc_mmu_init...\n");
	rv = soc_mmu_init(unit);
	if (rv < 0) {
		fprintf(stderr, "nosd-td2p: soc_mmu_init(%d) returned %d (%s)\n",
			unit, rv, soc_errmsg(rv));
		return -1;
	}

	/*
	 * type MUST be NULL. bcm_attach selects the driver family itself from the
	 * SOC_IS_* macros and falls through to "esw" for this chip. Passing a
	 * name here is how it gets rejected -- and "esw" happening to be the right
	 * family did not save it, because the last argument matters too: it is
	 * the remote unit, and it is the unit itself, not zero.
	 */
	printf("  bcm_attach...\n");
	rv = bcm_attach(unit, NULL, NULL, unit);
	if (rv < 0) {
		fprintf(stderr, "nosd-td2p: bcm_attach(%d) returned %d (%s)\n",
			unit, rv, bcm_errmsg(rv));
		return -1;
	}

	printf("  bcm_init...\n");
	rv = bcm_init(unit);
	if (rv < 0) {
		fprintf(stderr, "nosd-td2p: bcm_init(%d) returned %d (%s)\n",
			unit, rv, bcm_errmsg(rv));
		return -1;
	}
	printf("  init complete\n");
	return 0;
}

/*
 * Report every port the chip believes it has, and whether it has link.
 *
 * This is the measurement the port map needs. The configured map satisfies the
 * chip's constraints but says nothing about which physical lane reaches which
 * front-panel cage -- and link is a fact the chip reports rather than one
 * anybody has to be told. A cage with a cable in it lights up; the logical
 * port that reports it is the one wired to that cage.
 */
/* How long to wait for link after enabling ports. */
#define LINK_SETTLE_SECONDS 8

/* How long to count for. Long enough that a neighbour sending periodic
 * protocol traffic -- OSPF hellos every 10 s, say -- is certain to appear. */
#define COUNT_SECONDS 25

static uint64 stat_of(int unit, bcm_port_t port, bcm_stat_val_t t)
{
	uint64 v = 0;

	if (bcm_stat_get(unit, port, t, &v) < 0)
		return 0;
	return v;
}

/*
 * What a linked port has actually received and sent.
 *
 * This is the measurement polarity exists for. A link comes up whether or not
 * the lane is inverted -- inverting a 64b/66b stream turns the sync header 01
 * into 10, which is also legal -- so "UP" says nothing about whether frames
 * arrive intact. Only the counters do:
 *
 *   good packets rising, errors flat   the lane is the right way round
 *   errors rising, or nothing at all   it is not, whatever the link says
 *
 * The far end has to be sending something, which on a live network it always
 * is; a silent neighbour looks the same as a broken one here and the totals
 * being zero is reported rather than glossed.
 */
struct pcounters {
	uint64 rxpkt, rxoct, rxerr, crc, txpkt;
};

static void sample(int unit, bcm_port_t port, struct pcounters *c)
{
	c->rxpkt = stat_of(unit, port, snmpIfInUcastPkts);
	c->rxoct = stat_of(unit, port, snmpIfInOctets);
	c->rxerr = stat_of(unit, port, snmpIfInErrors);
	c->crc   = stat_of(unit, port, snmpEtherStatsCRCAlignErrors);
	c->txpkt = stat_of(unit, port, snmpIfOutUcastPkts);
}

/*
 * What a linked port received over an interval.
 *
 * Deltas rather than totals, because totals taken moments after a chip reset
 * say almost nothing: the link is still coming up, the neighbour has just seen
 * its own port bounce, and a handful of CRC errors during that is ordinary. It
 * is whether errors keep arriving that distinguishes a lane that is the wrong
 * way round from one that merely started badly.
 *
 * This is the measurement polarity exists for. A link comes up whether or not
 * the lane is inverted -- inverting a 64b/66b stream turns the sync header 01
 * into 10, which is also legal -- so "UP" says nothing about whether frames
 * arrive intact. Only the counters do.
 */
static void report_delta(bcm_port_t port, const struct pcounters *a,
			 const struct pcounters *b, int secs)
{
	unsigned long long dpkt = b->rxpkt - a->rxpkt;
	unsigned long long doct = b->rxoct - a->rxoct;
	unsigned long long derr = b->rxerr - a->rxerr;
	unsigned long long dcrc = b->crc - a->crc;
	unsigned long long dtx  = b->txpkt - a->txpkt;

	printf("         over %ds: rx %llu pkts / %llu octets, %llu errors, "
	       "%llu CRC; tx %llu pkts\n", secs, dpkt, doct, derr, dcrc, dtx);

	if (doct == 0)
		printf("         nothing arrived. Either the neighbour is silent, or\n"
		       "         this lane receives nothing intelligible at all.\n");
	else if (derr > 0 || dcrc > 0)
		printf("         STILL ERRORING: frames keep arriving damaged. On this\n"
		       "         board that is what a wrong RX polarity looks like --\n"
		       "         the link is up and the content is not.\n");
	else
		printf("         clean: %llu frames arrived intact, so this lane is the\n"
		       "         right way round.\n", dpkt);
}

int nosaic_sdk_ports(int unit)
{
	bcm_port_config_t cfg;
	bcm_port_t port;
	int rv, up = 0, total = 0, enable_failures = 0;
	bcm_port_t linked[16];

	bcm_port_config_t_init(&cfg);
	rv = bcm_port_config_get(unit, &cfg);
	if (rv < 0) {
		fprintf(stderr, "nosd-td2p: bcm_port_config_get returned %d (%s)\n",
			rv, bcm_errmsg(rv));
		return -1;
	}

	/*
	 * Linkscan, and the ports enabled, before asking anything about link.
	 *
	 * Without these a survey reports every port down and means nothing by it.
	 * A disabled port cannot come up, and link state is not read from the
	 * hardware on demand -- linkscan is the thread that polls the PHYs and
	 * maintains it, so with linkscan stopped the answer is whatever the
	 * software last believed, which after init is "down" for everything.
	 *
	 * It matters beyond this survey: the transmit path ANDs its port bitmap
	 * with the link bitmap that only linkscan populates, and returns success
	 * having built no descriptor when that is empty. Every transmit then
	 * silently vanishes.
	 */
	rv = bcm_linkscan_enable_set(unit, 250000);
	if (rv < 0) {
		fprintf(stderr, "nosd-td2p: bcm_linkscan_enable_set returned %d (%s)\n",
			rv, bcm_errmsg(rv));
		return -1;
	}

	BCM_PBMP_ITER(cfg.port, port) {
		int erv = bcm_port_enable_set(unit, port, 1);

		if (erv < 0 && enable_failures++ == 0)
			fprintf(stderr, "nosd-td2p: bcm_port_enable_set(port %d) returned "
				"%d (%s); further failures not reported\n",
				port, erv, bcm_errmsg(erv));
	}

	/* Give the PHYs time to negotiate. A cage with a cable in it does not
	 * report link the instant it is enabled, and a survey run immediately
	 * finds nothing and looks like a wrong port map. */
	printf("linkscan running, ports enabled; waiting %d s for negotiation\n",
	       LINK_SETTLE_SECONDS);
	sal_sleep(LINK_SETTLE_SECONDS);

	printf("\n%-8s %-8s %-8s %s\n", "port", "link", "speed", "note");
	BCM_PBMP_ITER(cfg.port, port) {
		int status = 0, speed = 0;

		total++;
		if (bcm_port_link_status_get(unit, port, &status) < 0)
			status = -1;
		if (bcm_port_speed_get(unit, port, &speed) < 0)
			speed = -1;

		/* Only linked ports are printed. Fifty-four lines of "down" is
		 * not a survey, it is a haystack -- and what matters here is the
		 * short list of cages that actually have something in them. */
		if (status == BCM_PORT_LINK_STATUS_UP) {
			if (up < (int)(sizeof(linked) / sizeof(linked[0])))
				linked[up] = port;
			up++;
			printf("%-8d %-8s %-8d %s\n", port, "UP", speed,
			       "a cable is in this cage");
		}
	}
	/* Measure over an interval rather than reporting the totals that a chip
	 * reset left behind. */
	if (up > 0) {
		int n = up < (int)(sizeof(linked) / sizeof(linked[0]))
			? up : (int)(sizeof(linked) / sizeof(linked[0]));
		struct pcounters before[16], after[16];
		int i;

		printf("\nsampling the linked ports for %d s...\n", COUNT_SECONDS);
		for (i = 0; i < n; i++)
			sample(unit, linked[i], &before[i]);
		sal_sleep(COUNT_SECONDS);
		for (i = 0; i < n; i++) {
			sample(unit, linked[i], &after[i]);
			printf("port %d\n", linked[i]);
			report_delta(linked[i], &before[i], &after[i], COUNT_SECONDS);
		}
	}

	printf("\n%d of %d ports have link", up, total);
	if (enable_failures)
		printf("  (%d ports refused to enable)", enable_failures);
	printf(".\n");
	if (up == 0)
		printf("No link anywhere. Either nothing is plugged in, or the port map\n"
		       "does not reach the cages that are -- which is exactly what this\n"
		       "survey exists to tell apart, and it cannot until something is\n"
		       "known to be connected.\n");
	return 0;
}
