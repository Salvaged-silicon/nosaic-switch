/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tdp-probe — bring the Trident+ BDE up far enough to prove it is right.
 *
 * The BDE reads and writes chip registers through an mmap'd BAR from
 * userspace. Two things have to be true before anything above it can be
 * trusted, and both are silent when false:
 *
 *   byte order  the chip must present programmed I/O in the host's order, or
 *               every register read is byte-swapped and the SDK's first
 *               identification of the chip fails for a reason that names
 *               nothing
 *   ordering    PowerPC needs explicit barriers between MMIO accesses, and
 *               without them a register sequence that must happen in order
 *               does not. S-Channel is such a sequence
 *
 * So this checks the first directly, by reading the chip's own identity back
 * and comparing it with what PCI already said. It cannot check the second --
 * ordering shows up under traffic, not at first touch -- which is the honest
 * limit of what a probe can tell you.
 *
 * First run against the AS5610, 2026-09-02, with edged still driving the chip:
 *
 *   device       0001:01:00.0        pci says  0xb846
 *   bar0         262144 bytes mapped
 *   host         big-endian (barriers active)
 *   CMIC_ENDIAN_SELECT (0x174)  0x04040404
 *   CMIC_REVID_DEVID   (0x178)  0x46b80200  -> device 0x0200 rev 0xb8
 *   FAIL
 *
 * Which is the right answer. PCI discovery, the BAR mapping and the barrier
 * accessors all work; the identity does not match because the chip is in
 * little-endian PIO, where edged leaves it so the kernel's ioread32 corrects
 * for it. 0x46b80200 byte-swapped is 0x0002b846 -- device 0xb846 revision 2.
 *
 * So the check does what it exists for: it says the mapping is wrong before
 * anything is built on top of it, rather than letting chip initialisation fail
 * later for a reason that names nothing.
 *
 * The other half, on the same board netbooted into NOSaic so nothing was
 * driving the chip:
 *
 *   CMIC_ENDIAN_SELECT (0x174)  0x00000000  (as found -- power-on default)
 *   CMIC_REVID_DEVID   (0x178)  0x46b80200  -> device 0x0200 rev 0xb8
 *   writing CMIC_ENDIAN_SELECT and retrying
 *   CMIC_ENDIAN_SELECT (0x174)  0x07070707  (after write)
 *   CMIC_REVID_DEVID   (0x178)  0x0002b846  -> device 0xb846 rev 0x02
 *   PASS
 *
 * Which settles it. A userspace BDE over an mmap'd BAR reaches this chip on
 * PowerPC, correctly, with the chip put into big-endian PIO and the barriers
 * in datapath/common/mmio.h supplying the ordering the kernel's ioread32 would
 * otherwise have supplied. No kernel module, and the same BDE model as the
 * 7050SX2.
 *
 * The register reads back with its low byte replicated across all four --
 * 0x07 written, 0x07070707 read -- which also explains what a live EdgeNOS
 * shows: 0x04040404, ES_BIG_ENDIAN_DMA_OTHER alone, PIO left little-endian
 * because its ioctl path swaps for it.
 *
 * Still unproven: ordering under load. This is one register at a time, and
 * S-Channel is a sequence. The barriers are there for it, but only traffic
 * will say.
 *
 * Writing CMIC_ENDIAN_SELECT changes how every subsequent access is
 * interpreted, so --set-endian must not be used on a switch whose datapath is
 * running: forwarding stops immediately. Read-only by default.
 */
#include "bde.h"
#include "sdk.h"
#include "mmio.h"
#include "props.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	/*
	 * Static, not a local, and that is not style.
	 *
	 * Its address is handed to the SDK as the device cookie and kept there
	 * for the life of the device: every register access the SDK makes comes
	 * back through it, from whichever thread the SDK is running -- and with
	 * polled_irq_mode there is a thread reading interrupt status behind us.
	 * A pointer a library keeps has no business living in a stack frame.
	 *
	 * As a local it read back as bar=(nil), bar_len=0 and a nonsense
	 * dma_phys, which turned every SDK register access into "read past
	 * BAR0" and every read into 0xffffffff. What that looks like from above
	 * is a chip that will not answer: parity errors dispatched from status
	 * registers that are all ones, a DMA engine that never completes, and
	 * bcm_attach failing to allocate.
	 */
	static struct nosaic_tdp_bde b;
	const char *bdf = NULL;
	int set_endian = 0, attach = 0, init = 0, ports = 0, stats = 0, serve = 0, bridge = 0, selftest = 0, rx = 0, attach_failed = 0, rc;
	uint32_t raw = 0, es;

	/*
	 * Invoked as `nosd`, be the daemon.
	 *
	 * The service that starts this is generic -- every ASIC's provider
	 * installs a /usr/sbin/nosd and the unit names that and nothing else, so
	 * the exec line carries no arguments and cannot carry any that mean
	 * something to one chip and not another. A flag in the recipe does not
	 * survive that: the image builder writes the unit, and it wrote
	 * `exec /usr/sbin/nosd`, which ran a bare probe that attached to nothing
	 * and exited 2 in a restart loop.
	 *
	 * Deciding from argv[0] puts the choice where the name already is. The
	 * probe modes stay reachable by calling tdp-probe, which is the other
	 * half of what this binary is for.
	 */
	{
		const char *me = strrchr(argv[0], '/');

		me = me ? me + 1 : argv[0];
		if (!strcmp(me, "nosd"))
			serve = ports = init = attach = set_endian = 1;
	}

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--set-endian"))
			set_endian = 1;
		else if (!strcmp(argv[i], "--attach"))
			attach = set_endian = 1;
		else if (!strcmp(argv[i], "--init"))
			init = attach = set_endian = 1;
		else if (!strcmp(argv[i], "--ports"))
			ports = init = attach = set_endian = 1;
		else if (!strcmp(argv[i], "--stats"))
			stats = ports = init = attach = set_endian = 1;
		else if (!strcmp(argv[i], "--serve"))
			serve = ports = init = attach = set_endian = 1;
		/* Bridging every port together in one VLAN is a loop wherever two
		 * of them reach the same neighbour, so it is never implied by
		 * another flag -- it has to be asked for by name. */
		else if (!strcmp(argv[i], "--rx"))
			rx = ports = init = attach = set_endian = 1;
		else if (!strcmp(argv[i], "--selftest"))
			selftest = ports = init = attach = set_endian = 1;
		else if (!strcmp(argv[i], "--bridge"))
			bridge = ports = init = attach = set_endian = 1;
		else if (!strcmp(argv[i], "--help")) {
			printf("usage: tdp-probe [--set-endian] [--attach] [--init] [--ports] [--stats] [--serve] [--bridge] [--selftest] [--rx] [<pci-bdf>]\n");
			return 0;
		} else
			bdf = argv[i];
	}

	/* Every *.conf in /etc/nosaic, in name order: asic.conf carries the SDK
	 * properties for this board model and portmap.conf which lane reaches
	 * which cage. Reported rather than required, so a register probe still
	 * works on a box with no configuration installed. */
	{
		int n = nosaic_props_load_dir("/etc/nosaic");

		if (n > 0)
			printf("config       %d properties from /etc/nosaic\n", n);
		else
			printf("config       none in /etc/nosaic -- an attach will not "
			       "get past the port map\n");
	}

	if (nosaic_tdp_bde_open(&b, bdf) != 0)
		return 1;

	printf("device       %s\n", b.bdf);
	printf("pci says     0x%04x\n", b.device_id);
	printf("bar0         %zu bytes mapped\n", b.bar_len);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	printf("host         big-endian (barriers active)\n");
#else
	printf("host         little-endian\n");
#endif

	es = nosaic_tdp_bde_rd(&b, NOSAIC_CMIC_ENDIAN_SELECT);
	printf("\nCMIC_ENDIAN_SELECT (0x174)  0x%08x  (as found)\n", es);

	rc = nosaic_tdp_bde_selftest(&b, &raw);
	printf("CMIC_REVID_DEVID   (0x178)  0x%08x  -> device 0x%04x rev 0x%02x\n",
	       raw, raw & 0xffff, (raw >> 16) & 0xff);

	if (rc != 0 && set_endian) {
		printf("\nmismatch; writing CMIC_ENDIAN_SELECT and retrying\n");
		nosaic_tdp_bde_set_endian(&b);
		es = nosaic_tdp_bde_rd(&b, NOSAIC_CMIC_ENDIAN_SELECT);
		printf("CMIC_ENDIAN_SELECT (0x174)  0x%08x  (after write)\n", es);
		rc = nosaic_tdp_bde_selftest(&b, &raw);
		printf("CMIC_REVID_DEVID   (0x178)  0x%08x  -> device 0x%04x rev 0x%02x\n",
		       raw, raw & 0xffff, (raw >> 16) & 0xff);
	}

	/*
	 * Does a register write actually land?
	 *
	 * Everything so far proves reads work -- the chip identifies itself --
	 * and proves one write worked, CMIC_ENDIAN_SELECT, whose effect was
	 * visible in the next read. Neither proves the general case, and the
	 * failure being chased is a DMA engine that is enabled by a write and
	 * then never reports done.
	 *
	 * CMIC_SLAM_DMA_ENTRY_COUNT is the safe one to try: it is a scratch
	 * count register for an engine that is not running, adjacent to the
	 * config register the SDK polls, and writing it while nothing is in
	 * flight has no effect on anything.
	 */
	{
		uint32_t save = nosaic_tdp_bde_rd(&b, 0x448);
		uint32_t pat = 0x00000055u, back;

		nosaic_tdp_bde_wr(&b, 0x448, pat);
		back = nosaic_tdp_bde_rd(&b, 0x448);
		nosaic_tdp_bde_wr(&b, 0x448, save);
		printf("\nregister write  0x448: wrote %#010x read %#010x  %s\n",
		       pat, back, back == pat ? "OK" : "MISMATCH");
		if (back != pat)
			printf("  writes are not landing as written; everything above\n"
			       "  this line only ever proved that reads work\n");
	}

	/* The DMA pool. Reported rather than required: a board whose device tree
	 * has not been updated yet still gets a useful register report. */
	printf("\n");
	if (nosaic_tdp_bde_map_dma(&b) == 0) {
		volatile uint32_t *p = (volatile uint32_t *)b.dma;
		uint32_t pat = 0xa5c30f5au;

		printf("dma pool     %zu MiB at %#llx\n",
		       b.dma_len >> 20, (unsigned long long)b.dma_phys);
		/* Written and read back through the mapping, because a region
		 * that maps and does not hold what was put in it is worse than
		 * one that fails to map: the SDK would build descriptors there
		 * and the chip would fetch nothing that means anything. */
		*p = pat;
		printf("dma readback %s\n", *p == pat ? "OK" : "FAILED");
	} else {
		printf("dma pool     unavailable (see above)\n");
	}

	/* --attach hands the chip to the SDK, which is the first thing that
	 * drives S-Channel and therefore the first real test of whether the
	 * barriers hold under a sequence rather than a single register. */
	if (attach) {
		int unit;

		if (rc != 0) {
			printf("\nnot attaching: the identity check has to pass first\n");
		} else {
			printf("\n=== handing the chip to the SDK ===\n");
			unit = nosaic_tdp_sdk_attach(&b, b.device_id, (raw >> 16) & 0xff);
			if (unit < 0) {
				printf("ATTACH FAILED  see above\n");
				attach_failed = 1;
			} else {
				printf("ATTACHED  soc_attach completed\n");
				if (!init) {
					printf("  (--init runs the rest of bring-up)\n");
				} else if (nosaic_tdp_sdk_soc_init(unit) != 0 ||
					   nosaic_tdp_sdk_bcm_init(unit) != 0) {
					printf("INIT FAILED  see above\n");
					attach_failed = 1;
				} else {
					printf("INITIALISED  the chip is up\n");
					/* Only now is there anything to enable.
					 * Ports before bcm_init would be
					 * configuring a chip that has no
					 * forwarding tables yet. */
					if (ports &&
					    nosaic_tdp_sdk_ports_up(unit, bridge) != 0)
						attach_failed = 1;
					else if (stats)
						nosaic_tdp_sdk_stats(unit, 5);
					else if (rx)
						nosaic_tdp_sdk_rx(unit, 45);
					else if (selftest &&
						 nosaic_tdp_sdk_selftest(unit, 100) != 0)
						attach_failed = 1;

					/*
					 * Stay. The chip keeps forwarding after
					 * this process exits -- the tables are
					 * in silicon and nothing tears them down
					 * -- but everything that makes it a
					 * switch rather than a frozen snapshot
					 * lives in the SDK's threads: link scan,
					 * counter collection, and the interrupt
					 * poll that carries both. Exiting takes
					 * those with it, so a port that goes
					 * down afterwards is never noticed.
					 */
					if (serve && !attach_failed) {
						printf("SERVING  datapath up; "
						       "link and counters tracked\n");
						fflush(stdout);
						for (;;)
							pause();
					}
				}
			}
		}
	}

	printf("\n%s\n", rc == 0
	       ? "PASS  the chip identifies itself correctly through this mapping"
	       : "FAIL  the identity read back does not match what PCI reported");
	if (rc != 0 && !set_endian)
		printf("      byte order is the likely cause; --set-endian tests that,\n"
		       "      and must not be run against a live datapath\n");

	nosaic_tdp_bde_close(&b);
	/* Two different results, reported separately: the identity check is
	 * about this mapping, and the attach is about everything the SDK does
	 * behind it. Folding them into one verdict said the identity had failed
	 * when it had passed. */
	if (rc != 0)
		return 2;
	return attach_failed ? 3 : 0;
}
