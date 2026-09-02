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

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
	struct nosaic_tdp_bde b;
	const char *bdf = NULL;
	int set_endian = 0, attach = 0, attach_failed = 0, rc;
	uint32_t raw = 0, es;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--set-endian"))
			set_endian = 1;
		else if (!strcmp(argv[i], "--attach"))
			attach = set_endian = 1;
		else if (!strcmp(argv[i], "--help")) {
			printf("usage: tdp-probe [--set-endian] [--attach] [<pci-bdf>]\n");
			return 0;
		} else
			bdf = argv[i];
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
			printf("%s\n", unit >= 0
			       ? "ATTACHED  soc_attach completed"
			       : "ATTACH FAILED  see above");
			if (unit < 0)
				attach_failed = 1;
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
