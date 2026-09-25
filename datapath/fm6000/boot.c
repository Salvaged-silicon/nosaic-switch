/* SPDX-License-Identifier: Apache-2.0 */
/* Intel 331496-002 §4.2 Table 4-1, in order. See boot.h. */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "boot.h"
#include "pci.h"
#include "regs.h"

static void nap_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static const char *step_names[FM_STEP__COUNT] = {
	[FM_STEP_RESET_RELEASED] = "chip out of reset and on the bus",
	[FM_STEP_BOOT_METHOD]    = "boot from CPU selected",
	[FM_STEP_SCAN_CHAIN]     = "core logic and EPLs to normal operating mode",
	[FM_STEP_PLL]            = "PLL locked",
	[FM_STEP_MODULES]        = "EPL, PCIe, MSB, SPICO/SBUS out of soft reset",
	[FM_STEP_FFU_SLICES]     = "BOOT 1: initialize FFU slice numbers",
	[FM_STEP_BANK_REPAIR]    = "BOOT 2: apply bank memory repairs",
	[FM_STEP_FREELISTS]      = "BOOT 3: initialize all scheduler freelists",
	[FM_STEP_PCIE]           = "PCIe serdes up and out of reset",
	[FM_STEP_MEMORY_INIT]    = "memory initialised (CRM, or software fill)",
};

const char *fm_boot_step_name(int step)
{
	if (step <= 0 || step >= FM_STEP__COUNT || step_names[step] == NULL)
		return "?";
	return step_names[step];
}

static void set(struct fm_boot_report *rep, int step, int rv, const char *note)
{
	rep->step[step].rv = rv;
	rep->step[step].what = fm_boot_step_name(step);
	rep->step[step].note = note;
	rep->reached = step;
}

/*
 * Poll until (read & mask) == want, or the deadline passes.
 *
 * Every wait in Table 4-1 is of this shape and each one needs a bound: a chip
 * that never sets the bit must produce a named failure rather than a hang, on
 * a box whose only recovery is the watchdog.
 */
static int poll_bits(struct fm6000 *d, uint32_t word, uint32_t mask,
		     uint32_t want, unsigned ms)
{
	unsigned waited;
	uint32_t v = 0;
	int rv;

	for (waited = 0;; waited++) {
		rv = fm_rd(d, word, &v);
		if (rv != FM_OK)
			return rv;
		if ((v & mask) == want)
			return FM_OK;
		if (waited >= ms)
			return FM_ETIMEOUT;
		nap_ms(1);
	}
}

/* Drive SOFT_RESET to `held`, where a set bit means that module stays down. */
static int release_modules(struct fm6000 *d, uint32_t held)
{
	held &= FM6000_SOFT_RESET_PCIE | FM6000_SOFT_RESET_MSB |
		FM6000_SOFT_RESET_FIBM | FM6000_SOFT_RESET_JSS |
		FM6000_SOFT_RESET_EPL;
	return fm_wr(d, FM6000_SOFT_RESET, held);
}

/*
 * One boot-controller command: write it into BOOT_CTRL:Command, then wait for
 * CommandDone in the same register.
 *
 * CommandDone is read back from the register the command was written to, so
 * the read that follows the write is not a formality -- it is the only
 * confirmation that the boot controller saw anything at all.
 */
static int boot_command(struct fm6000 *d, uint32_t cmd)
{
	uint32_t v = 0;
	int rv;

	rv = fm_rd(d, FM6000_BOOT_CTRL, &v);
	if (rv != FM_OK)
		return rv;
	v = (v & ~FM6000_BOOT_CTRL_CMD_MASK) | (cmd & FM6000_BOOT_CTRL_CMD_MASK);
	rv = fm_wr(d, FM6000_BOOT_CTRL, v);
	if (rv != FM_OK)
		return rv;
	return poll_bits(d, FM6000_BOOT_CTRL, FM6000_BOOT_STATUS_CMD_DONE,
			 FM6000_BOOT_STATUS_CMD_DONE, FM6000_BOOT_CMD_MAX_MS);
}

int fm_boot_already_done(struct fm6000 *d)
{
	uint32_t soft = 0, boot = 0;
	int rv;

	rv = fm_rd(d, FM6000_SOFT_RESET, &soft);
	if (rv != FM_OK)
		return rv;
	rv = fm_rd(d, FM6000_BOOT_CTRL, &boot);
	if (rv != FM_OK)
		return rv;

	/* A chip that has been through the sequence reads SOFT_RESET 0x00 and
	 * BOOT_CTRL with CommandDone set and Command 3 -- measured, and the same
	 * on a forwarding EOS chip. A chip that has only been pulsed reads
	 * SOFT_RESET 0x1f and BOOT_CTRL 0x320. There is no overlap. */
	if (soft != 0)
		return 0;
	if (!(boot & FM6000_BOOT_STATUS_CMD_DONE))
		return 0;
	if ((boot & FM6000_BOOT_CTRL_CMD_MASK) != FM6000_BOOT_CMD_FREELISTS_ALL)
		return 0;
	return 1;
}

int fm_boot_cold(struct fm6000 *d, struct fm_boot_report *rep)
{
	int rv;

	memset(rep, 0, sizeof(*rep));

	/*
	 * Steps 1-3. Asserting and releasing CHIP_RESET_N is the board's job,
	 * not ours: on this chassis the SCD holds the FM6000 down from power-on
	 * and the platform HAL releases it. By the time we have a mapped BAR the
	 * boot controller has already transferred the fusebox contents to each
	 * module and sampled the BOOT_MODE pins.
	 *
	 * So the test for "steps 1-3 happened" is simply that the chip answers.
	 */
	if (d->regs == NULL) {
		set(rep, FM_STEP_RESET_RELEASED, FM_ERR,
		    "no BAR mapped -- is the chip still held in reset by the SCD?");
		return FM_ERR;
	}
	if (fm_alive(d) != 1) {
		set(rep, FM_STEP_RESET_RELEASED, FM_EOFFBUS,
		    "chip is not answering. On the local bus that means it has "
		    "had no reset PULSE -- being found with the resets clear is "
		    "not the same thing");
		return FM_EOFFBUS;
	}
	set(rep, FM_STEP_RESET_RELEASED, FM_OK, NULL);

	/*
	 * Step 4. With boot-from-ROM disabled the boot controller stalls until
	 * the CPU drives BOOT_CTRL, which is the mode we are in by construction
	 * -- we are the CPU and we are about to. Nothing to write.
	 */
	set(rep, FM_STEP_BOOT_METHOD, FM_OK, "boot controller waits for us");

	/*
	 * Step 5. "Write 0xFFFFFFFF to SCAN_CHAIN_DATA_IN to put the core logic
	 * and the EPLs into normal operating mode."
	 *
	 * ⚠ THE DATASHEET AND THE PRIOR INVESTIGATION DISAGREE ABOUT THIS STEP.
	 * The datasheet describes one write. The reverse engineering on this
	 * chassis concluded that the single write only loads the scan data
	 * register, and that what actually configures each memory block into a
	 * writable mode is a several-thousand-iteration scan PROGRAM shifted
	 * through 0x1c039..0x1c03d.
	 *
	 * Both can be true if the vendor applies per-block trim from the fusebox
	 * on top of the documented minimum. We do the documented thing, and the
	 * experiment that settles it is: run this sequence in this order and see
	 * whether step 9 then makes the bank memories safe. Nobody has.
	 */
	rv = fm_wr(d, FM6000_SCAN_CHAIN_DATA_IN, 0xffffffff);
	if (rv != FM_OK) {
		set(rep, FM_STEP_SCAN_CHAIN, rv, "write to SCAN_CHAIN_DATA_IN failed");
		return rv;
	}
	set(rep, FM_STEP_SCAN_CHAIN, FM_OK, NULL);

	/*
	 * Step 6. Initialise the PLL and wait for lock, 80 ms maximum.
	 *
	 * The PLLs lock by themselves -- measured, a chip that has had nothing
	 * but a reset pulse already reads 0x3 -- so what this step waits for in
	 * practice is the two DLLs in bits [3:2]. Enabling them is a write to
	 * DLL_CTRL, which does not read back, so the only evidence either way is
	 * PLL_STATUS.
	 */
	rv = fm_wr(d, FM6000_DLL_CTRL, FM6000_DLL_CTRL_ENABLE);
	if (rv != FM_OK) {
		set(rep, FM_STEP_PLL, rv, "write to DLL_CTRL failed");
		return rv;
	}
	rv = poll_bits(d, FM6000_PLL_STATUS, FM6000_PLL_STATUS_LOCKED_ALL,
		       FM6000_PLL_STATUS_LOCKED_ALL, FM6000_PLL_LOCK_MAX_MS);
	if (rv != FM_OK) {
		uint32_t v = 0;

		(void)fm_rd(d, FM6000_PLL_STATUS, &v);
		/* Not fatal by itself: the PLLs are what the rest of the
		 * sequence needs and they are in the low two bits. Report it
		 * and carry on, so one unlocked DLL does not hide whether the
		 * boot commands work. */
		set(rep, FM_STEP_PLL, rv,
		    (v & FM6000_PLL_STATUS_PLL_MASK) == FM6000_PLL_STATUS_PLL_MASK
		    ? "PLLs locked but a DLL did not within 80 ms -- continuing"
		    : "PLLs did not lock within 80 ms");
	} else {
		set(rep, FM_STEP_PLL, FM_OK, NULL);
	}

	/*
	 * Step 7, FIRST HALF. SOFT_RESET holds five modules down and each has to
	 * be released -- but NOT all of them here.
	 *
	 * ⚠ MSB, the core fabric, is released LAST, after the boot controller's
	 * commands in steps 8-10. Releasing it into a fabric whose bank repairs
	 * and freelists have not been applied hangs the CPU. So this releases
	 * everything except MSB, and step 10 finishes the job.
	 */
	rv = release_modules(d, (uint32_t)~FM6000_SOFT_RESET_MSB);
	if (rv != FM_OK) {
		set(rep, FM_STEP_MODULES, rv,
		    "could not release the non-MSB modules in SOFT_RESET");
		return rv;
	}
	set(rep, FM_STEP_MODULES, FM_OK, "MSB deliberately still held");

	/*
	 * Steps 8, 9 and 10. Three boot-controller commands, each written into
	 * BOOT_CTRL:Command and each polled to completion on CommandDone in the
	 * same register.
	 *
	 * Step 9 is the one that matters most: applying the bank memory repairs
	 * is the documented answer to the ECC hazard that makes half this chip
	 * untouchable, and it is what fm_bank_mark_initialised() is waiting for.
	 */
	rv = boot_command(d, FM6000_BOOT_CMD_FFU_SLICE_NUMBERS);
	set(rep, FM_STEP_FFU_SLICES, rv, rv == FM_OK ? NULL : "command 1 did not complete");
	if (rv != FM_OK)
		return rv;

	rv = boot_command(d, FM6000_BOOT_CMD_BANK_MEMORY_REPAIRS);
	set(rep, FM_STEP_BANK_REPAIR, rv, rv == FM_OK ? NULL : "command 2 did not complete");
	if (rv != FM_OK)
		return rv;

	rv = boot_command(d, FM6000_BOOT_CMD_FREELISTS_ALL);
	set(rep, FM_STEP_FREELISTS, rv, rv == FM_OK ? NULL : "command 3 did not complete");
	if (rv != FM_OK)
		return rv;

	/*
	 * Step 7, SECOND HALF. Now the fabric has its repairs and its freelists,
	 * so MSB can come out of reset.
	 */
	rv = release_modules(d, 0);
	if (rv != FM_OK) {
		set(rep, FM_STEP_MODULES, rv, "could not release MSB");
		return rv;
	}
	/* set() also moves rep->reached, and this one moves it backwards to step
	 * 7. Put it back, or the report stops printing at MODULES and hides the
	 * three commands that just succeeded. */
	rep->step[FM_STEP_MODULES].note = "released, MSB last after the boot commands";
	rep->reached = FM_STEP_FREELISTS;

	/*
	 * Step 11. PCIe.
	 *
	 * Nothing to do here and nothing to wait for. We are talking to the chip
	 * over the SCD's local bus, not over PCIe, and the endpoint is expected
	 * to be absent until it is configured -- which is a later job, for
	 * whatever wants packet DMA. Saying "ok" would claim a link that has not
	 * been brought up.
	 */
	set(rep, FM_STEP_PCIE, FM_ENOADDR,
	    "not attempted: the PCIe block is configured separately, and "
	    "nothing before packet DMA needs it");

	/*
	 * Step 12. Initialise memory. The datasheet offers two routes -- program
	 * the CRM and launch it, or "software writes memory manually" -- and
	 * this is the second, because it needs nothing but the bulk writer and
	 * the CRM's command register map is not known.
	 *
	 * ⚠ ONE BANK, NOT THREE. This used to be described as three memories of
	 * 0x20000 words. Filling them on hardware says otherwise: 0x200000 runs
	 * to 0x23ffff and takes a fill fine, and 0x240000 and 0x260000 are
	 * register blocks where the fill dies 54 and 20 words in. See regs.h.
	 *
	 * What the fill achieves is exactly what the step is for: before it,
	 * reading 0x200000 takes the chip off the bus; after it, the read is
	 * safe. What the region then CONTAINS is a separate question and is not
	 * answered -- it does not read back the pattern written, so it is not
	 * plain 32-bit RAM.
	 */
	rv = fm_mem_fill(d, FM6000_BANK_STATS_BASE, FM6000_BANK_STATS_SPAN, 0);
	if (rv != FM_OK) {
		set(rep, FM_STEP_MEMORY_INIT, rv,
		    "the chip stopped answering during the STATS fill");
		return rv;
	}
	fm_bank_mark_initialised(d);
	set(rep, FM_STEP_MEMORY_INIT, FM_OK,
	    "STATS filled and readable; 0x240000 and 0x260000 are register "
	    "blocks, not banks, and are left alone");

	/* The EPL block is now safe to read. Measured: the same word that takes
	 * the chip off the bus before this sequence returns 0x00080000 after it,
	 * with the chip still answering. The ECC bank memories are NOT unlocked
	 * -- they wait for step 12. */
	fm_boot_mark_done(d);
	return FM_OK;
}

void fm_boot_report_print(const struct fm_boot_report *rep)
{
	int i;

	/* ⚠ stdout, and this function is called by a daemon that then runs
	 * forever. Block-buffered into a pipe, every line below sits in the
	 * buffer until an exit that never comes -- so on the boot where the
	 * report matters most it is the one thing missing from the log.
	 * Measured: nosd's log stopped at "running the documented boot
	 * sequence" with the whole report invisible. */

	for (i = 1; i < FM_STEP__COUNT; i++) {
		const char *mark;

		if (i > rep->reached) {
			printf("  %2d  ....  %s\n", i, fm_boot_step_name(i));
			continue;
		}
		switch (rep->step[i].rv) {
		case FM_OK:       mark = " ok "; break;
		case FM_ENOADDR:  mark = "skip"; break;
		case FM_ETIMEOUT: mark = "TIME"; break;
		case FM_EOFFBUS:  mark = "OFF!"; break;
		case FM_EUNSAFE:  mark = "STOP"; break;
		default:          mark = "FAIL"; break;
		}
		printf("  %2d  %s  %s\n", i, mark, fm_boot_step_name(i));
		if (rep->step[i].note != NULL)
			printf("          %s\n", rep->step[i].note);
	}
	fflush(stdout);
}
