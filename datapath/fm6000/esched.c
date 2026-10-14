/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The egress scheduler.
 *
 * Three registers per physical port, all indexed by the port number directly:
 *
 *     CFG_1   0x002000 + port    strictPriority[11:0] | tcEnable[23:12]
 *     CFG_2   0x002080 + port    same shape
 *     DRR_CFG 0x003800 + port    zeroLength[11:0] | groupBoundary[23:12]
 *                                | ifgPenalty[31:24]
 *
 * A front-panel port runs strict priority on all twelve traffic classes with
 * all twelve enabled, which is 0xffffff in both CFG registers. The CPU port
 * is the exception and the only one: strict on a subset in CFG_1, none in
 * CFG_2.
 *
 * ⚠ DRR_CFG IS WRITTEN TWICE PER PORT, with an inter-frame-gap penalty of 20
 * and then with it cleared. That is not redundancy to tidy away -- the vendor
 * does it, the intermediate value is presumably what the scheduler latches
 * against, and this block is one where sequences matter.
 *
 * ⚠ PRECONDITION: THE SCHEDULER RING MUST BE CIRCULATING. Until it is, every
 * address in this file is fatal to touch -- see ssched.c -- so fm_hazard()
 * refuses the whole block and this function writes nothing rather than
 * getting twenty registers in and taking the switch down. A refusal here is
 * the guard working, not a bug in the table below.
 *
 * ⚠ LBUS (SCD BAR1) CANNOT WRITE THIS BLOCK. Measured 2026-10-13: every
 * address in 0x002000-0x003fff is fatal via the LBUS regardless of value or
 * ring state. fm_esched_init() is therefore a stub that returns FM_OK without
 * writing anything until the FM6000's PCIe link is brought up. The ring
 * circulates with zero scheduling policy (post-BIST defaults), which is
 * sufficient for bring-up testing. When step 9 (PCIe serdes) is implemented,
 * this stub must be replaced by PCIe-resource0 writes matching EdgeNOS's
 * fm6000_esched.c + fm6000_eschedrr.c. The 42 fm_boot_freelists() resets
 * wipe ESCHED SRAM, so fm6000_eschedrr.c (DRR quanta) must run first.
 *
 * PROVENANCE. The geometry and values are from EdgeNOS's fm6000_esched.c --
 * our own prior work on this chassis -- whose C file is a materialised table
 * from its own generator. Regenerated here from that geometry rather than
 * copied, and the port set comes from this board's map in portmap.h.
 */
#include "esched.h"
#include "pci.h"
#include "regs.h"

/*
 * The block is eight instances of the same four arrays, 0x200 apart, each
 * array holding one word per physical port.
 *
 * Array 2 is left at zero -- a forwarding chip has it zero in all eight
 * instances, and our earlier prior-art tool wrote it as an explicit zero.
 * Arrays 0, 1 and 3 are filled.
 */
/*
 * The register map, from the vendor's own header rather than inferred:
 *
 *   ESCHED_CFG_1   0x2000 + port   prioritySetBoundary[11:0] tcGroupBoundary[23:12]
 *   ESCHED_CFG_2   0x2080 + port   strictPriority[11:0]      tcEnable[23:12]
 *   ESCHED_CFG_3   0x2100 + port   tcInnerPriority[11:0]
 *   ESCHED_DRR_CFG 0x3800 + port
 *
 * ⚠ SEVENTY-SIX ENTRIES EACH, AND ONLY THREE REGISTERS IN THE BLOCK.
 *
 * This file used to write eight "instances" of four arrays over 128 ports --
 * 3,222 writes across the whole of 0x2000-0x2fff -- because that is the
 * shape a forwarding chip's register dump appears to have. It is not a
 * shape. The block holds three registers of 76 entries and decodes only
 * part of the address, so everything above 0x2180 is those same registers
 * seen again. The dump was 24 aliases of one array, and the model built
 * from it was 13 times more writes than the hardware has registers.
 *
 * ⚠ AND THE FIELD NAMES WERE ON THE WRONG REGISTER. The comment here said
 * CFG_1 carried strictPriority and tcEnable. Those are CFG_2's. CFG_1 is
 * the priority-set and traffic-class group boundaries.
 *
 * Diffing against a forwarding chip will now show it holding values at
 * addresses we do not write. Those are the aliases, and writing them once
 * through the canonical address is the same operation.
 */
#define ESCHED_PORTS		76u
#define ESCHED_CFG_1(p)		(0x002000u + (unsigned)(p))
#define ESCHED_CFG_2(p)		(0x002080u + (unsigned)(p))
#define ESCHED_CFG_3(p)		(0x002100u + (unsigned)(p))

/* The round-robin word is one array, and it covers the switch ports only. */
#define ESCHED_DRR(p)   (0x003800u + (unsigned)(p))
#define ESCHED_DRR_PORTS	76u

/* Strict priority on all twelve classes, all twelve enabled. */
#define ESCHED_ALL_TC       0x00ffffffu
/* The CPU port. Physical port 0, not the CPU number in portmap.h's logical
 * space -- the egress scheduler indexes physical ports throughout. */
#define ESCHED_CPU_PORT     0u
/* Its two, which are the only values that are not ALL_TC. */
#define ESCHED_CPU_CFG_1    0x00fff800u
#define ESCHED_CPU_CFG_2    0x00fff000u
/* The deficit round-robin word, with and without the IFG penalty. */
#define ESCHED_DRR_PENALTY  0x14ffffffu
#define ESCHED_DRR_SETTLED  0x00ffffffu


/* Settled DRR quanta per traffic class, from EdgeNOS fm6000_eschedrr.c.
 * TC9/TC10 get five times TC3/TC5; the other eight sit between.
 * ⚠ These are the vendor's settled values after convergence.  A quantum wrong
 * by one shows up as unfairness under load, not a failed ping. */
static const uint32_t drr_q[12] = {
	0x1450, 0x1450, 0x1450, 0x05c8, 0x1450, 0x05c8,
	0x1450, 0x1450, 0x1450, 0x6590, 0x6590, 0x39d0,
};

int fm_esched_init(struct fm6000 *d, unsigned *written, uint32_t *culprit)
{
	unsigned n = 0;
	unsigned p;

	if (written != NULL)
		*written = 0;
	if (culprit != NULL)
		*culprit = 0;

	/*
	 * Stub path: no PCIe BAR0 attached. The ESCHED block (0x002000-0x003fff)
	 * is not writable via the LBUS -- any write there kills the chip.
	 * Return OK; the ring circulates with zero ESCHED policy (post-BIST
	 * defaults), which is sufficient for basic forwarding. Step 9 (PCIe
	 * serdes init in boot.c) must succeed before this can do real work.
	 */
	if (d->pcie_regs == NULL)
		return FM_OK;

	/*
	 * Write order is critical:
	 *
	 * 1. DRR_Q (0x003000, 12 quanta) FIRST. The ring engine reads these on
	 *    every write to the ESCHED block. fm_boot_freelists()'s 42 self-
	 *    resets leave them ECC-uninit, so any write before this causes a
	 *    deferred hard fault. Writes here go via PCIe resource0 (fm_wr_pcie)
	 *    -- they are not reachable via the LBUS.
	 *
	 * 2. CFG_1/CFG_2/CFG_3 per port (priority sets, TC enables, inner prio).
	 *
	 * 3. DRR_CFG per port, written twice: first with IFG penalty, then with
	 *    it cleared. The intermediate value matters -- the scheduler latches
	 *    against it. Skip ports 1 and 3 on the second pass (vendor pattern).
	 *
	 * 4. CFG_4 = 0x2000 (SchedPeriod) LAST. Enables the ESCHED clock domain.
	 *    Writing it before DRR_Q is valid faults the chip asynchronously.
	 */

	/* 1. DRR quanta (SRAM -- no LBUS path). */
	for (p = 0; p < 12; p++) {
		fm_wr_pcie(d, 0x003000u + p, drr_q[p]);
		n++;
	}

	/* 2a. CFG_1: ports 1..75, then 0. */
	for (p = 1; p < ESCHED_PORTS; p++) {
		fm_wr_pcie(d, ESCHED_CFG_1(p), ESCHED_ALL_TC);
		n++;
	}
	fm_wr_pcie(d, ESCHED_CFG_1(ESCHED_CPU_PORT), ESCHED_CPU_CFG_1);
	n++;

	/* 2b. CFG_2: same order. */
	for (p = 1; p < ESCHED_PORTS; p++) {
		fm_wr_pcie(d, ESCHED_CFG_2(p), ESCHED_ALL_TC);
		n++;
	}
	fm_wr_pcie(d, ESCHED_CFG_2(ESCHED_CPU_PORT), ESCHED_CPU_CFG_2);
	n++;

	/* 2c. CFG_3: 0 for all ports (tcInnerPriority). */
	for (p = 0; p < ESCHED_PORTS; p++) {
		fm_wr_pcie(d, ESCHED_CFG_3(p), 0u);
		n++;
	}

	/* 3. DRR_CFG: penalty pass, then settled pass. */
	for (p = 0; p < ESCHED_DRR_PORTS; p++) {
		fm_wr_pcie(d, ESCHED_DRR(p), ESCHED_DRR_PENALTY);
		n++;
	}
	for (p = 0; p < ESCHED_DRR_PORTS; p++) {
		if (p == 1 || p == 3)
			continue;
		fm_wr_pcie(d, ESCHED_DRR(p), ESCHED_DRR_SETTLED);
		n++;
	}

	/*
	 * ⚠ DO NOT write FM6000_SWEEPER_CFG_4 (0x01c04c) here.
	 *
	 * Writing it with 0x2000 (SchedPeriod) faults the chip asynchronously
	 * and takes it off the PCIe bus -- measured 2026-10-13, two confirmed
	 * kernel hangs. The chip reaches the watch loop and forwards without
	 * it. Root cause is unknown; skip until the exact preconditions for a
	 * safe write are determined.
	 */

	if (written != NULL)
		*written = n;
	return FM_OK;
}
