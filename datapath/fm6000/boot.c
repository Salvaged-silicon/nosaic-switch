/* SPDX-License-Identifier: Apache-2.0 */
/* Intel 331496-002 §4.2 Table 4-1, in order. See boot.h. */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "boot.h"
#include "memfill.h"
#include "pci.h"
#include "regs.h"
/* ssched.h not needed: TICK_CFG written directly via regs.h (see boot_steps) */

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

/*
 * Record a step, and with it how many times the chip reset itself while the
 * step ran.
 *
 * The count is sampled here rather than around the whole sequence because the
 * interesting question is never "did the boot reset the chip" -- it does --
 * but which step did it. A step that returns FM_OK with a non-zero reset count
 * did not happen: the watchdog put the fabric back to defaults underneath it,
 * and everything it wrote is gone.
 */
/*
 * Close the report off: the last fatal code, and whether the run was clean.
 *
 * `ok` and `clean` are deliberately different. `ok` is what the steps said.
 * `clean` also requires that the chip never reset itself, and it is the one
 * to believe -- a boot with resets in it has written its registers into a
 * fabric that was subsequently put back to defaults.
 */
static void finish(struct fm6000 *d, struct fm_boot_report *rep)
{
	struct fm_fatal f;
	int i, all_ok = 1;

	if (fm_fatal_read(d, &f) == FM_OK)
		rep->last_fatal = f.last;

	for (i = 1; i < FM_STEP__COUNT; i++)
		if (i <= rep->reached && rep->step[i].rv != FM_OK &&
		    rep->step[i].rv != FM_ENOADDR)
			all_ok = 0;

	rep->ok = all_ok;
	rep->clean = all_ok && rep->resets == 0;
}

static void set(struct fm6000 *d, struct fm_boot_report *rep, int step, int rv,
		const char *note)
{
	uint32_t now = fm_fatal_count(d);

	rep->step[step].rv = rv;
	rep->step[step].what = fm_boot_step_name(step);
	rep->step[step].note = note;
	rep->step[step].resets = now > rep->mark ? now - rep->mark : 0;
	rep->resets += rep->step[step].resets;
	rep->mark = now;
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
	return fm_boot_cold_opt(d, rep, 1);
}

static int boot_steps(struct fm6000 *d, struct fm_boot_report *rep, int mem_init)
{
	int rv;

	memset(rep, 0, sizeof(*rep));
	rep->mark = fm_fatal_count(d);

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
		set(d, rep, FM_STEP_RESET_RELEASED, FM_ERR,
		    "no BAR mapped -- is the chip still held in reset by the SCD?");
		return FM_ERR;
	}
	if (fm_alive(d) != 1) {
		set(d, rep, FM_STEP_RESET_RELEASED, FM_EOFFBUS,
		    "chip is not answering. On the local bus that means it has "
		    "had no reset PULSE -- being found with the resets clear is "
		    "not the same thing");
		return FM_EOFFBUS;
	}
	set(d, rep, FM_STEP_RESET_RELEASED, FM_OK, NULL);

	/*
	 * Step 4. With boot-from-ROM disabled the boot controller stalls until
	 * the CPU drives BOOT_CTRL, which is the mode we are in by construction
	 * -- we are the CPU and we are about to. Nothing to write.
	 */
	set(d, rep, FM_STEP_BOOT_METHOD, FM_OK, "boot controller waits for us");

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
	/*
	 * ⚠ QUIESCE THE SCAN ENGINE FIRST. The datasheet describes step 5 as a
	 * single write and this port did exactly that -- and measured eight
	 * watchdog self-resets under it, every time, all eight of the boot's
	 * total. The vendor's own pre-boot writes three words to
	 * SCAN_CONFIG_DATA_IN immediately before the step-5 write, the last of
	 * which is the scan engine's stop command (opcode 0x80, operand 0x40 --
	 * the same word mrl.c ends its sequence with).
	 *
	 * Reading that as "the engine is left running by reset and writing the
	 * chain while it runs is what faults" is inference, not documentation.
	 * What is measured is the reset count either side of this. [RE]
	 */
	rv = fm_wr(d, FM6000_SCAN_CONFIG_DATA_IN, 0x88800000u);
	if (rv == FM_OK)
		rv = fm_wr(d, FM6000_SCAN_CONFIG_DATA_IN, 0x88008000u);
	if (rv == FM_OK)
		rv = fm_wr(d, FM6000_SCAN_CONFIG_DATA_IN, 0x80000040u);
	if (rv != FM_OK) {
		set(d, rep, FM_STEP_SCAN_CHAIN, rv, "quiescing the scan engine failed");
		return rv;
	}

	rv = fm_wr(d, FM6000_SCAN_CHAIN_DATA_IN, 0xffffffff);
	if (rv != FM_OK) {
		set(d, rep, FM_STEP_SCAN_CHAIN, rv, "write to SCAN_CHAIN_DATA_IN failed");
		return rv;
	}
	set(d, rep, FM_STEP_SCAN_CHAIN, FM_OK, NULL);

	/*
	 * Step 6. Initialise the PLL and wait for lock, 80 ms maximum.
	 *
	 * The PLLs lock by themselves -- measured, a chip that has had nothing
	 * but a reset pulse already reads 0x3 -- so what this step waits for in
	 * practice is the two DLLs in bits [3:2]. Enabling them is a write to
	 * DLL_CTRL, which does not read back, so the only evidence either way is
	 * PLL_STATUS.
	 *
	 * PLL_CTRL_0/1 must be written first. They select and enable the clock
	 * source for the EPL and SSCHED domains. Without them the scheduler ring
	 * cannot advance regardless of how TICK_CFG and INIT_COMPLETE are
	 * programmed. The values are golden from the running-switch capture.
	 * [RE, confirmed as root cause 2026-10-01; see regs.h]
	 */
	rv = fm_wr(d, FM6000_PLL_CTRL_0, FM6000_PLL_CTRL_0_GOLDEN);
	if (rv == FM_OK)
		rv = fm_wr(d, FM6000_PLL_CTRL_1, FM6000_PLL_CTRL_1_GOLDEN);
	if (rv != FM_OK) {
		set(d, rep, FM_STEP_PLL, rv, "write to PLL_CTRL failed");
		return rv;
	}
	rv = fm_wr(d, FM6000_DLL_CTRL, FM6000_DLL_CTRL_ENABLE);
	if (rv != FM_OK) {
		set(d, rep, FM_STEP_PLL, rv, "write to DLL_CTRL failed");
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
		set(d, rep, FM_STEP_PLL, rv,
		    (v & FM6000_PLL_STATUS_PLL_MASK) == FM6000_PLL_STATUS_PLL_MASK
		    ? "PLLs locked but a DLL did not within 80 ms -- continuing"
		    : "PLLs did not lock within 80 ms");
	} else {
		set(d, rep, FM_STEP_PLL, FM_OK, NULL);
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
		set(d, rep, FM_STEP_MODULES, rv,
		    "could not release the non-MSB modules in SOFT_RESET");
		return rv;
	}
	set(d, rep, FM_STEP_MODULES, FM_OK, "MSB deliberately still held");

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
	set(d, rep, FM_STEP_FFU_SLICES, rv, rv == FM_OK ? NULL : "command 1 did not complete");
	if (rv != FM_OK)
		return rv;

	rv = boot_command(d, FM6000_BOOT_CMD_BANK_MEMORY_REPAIRS);
	set(d, rep, FM_STEP_BANK_REPAIR, rv, rv == FM_OK ? NULL : "command 2 did not complete");
	if (rv != FM_OK)
		return rv;

	rv = boot_command(d, FM6000_BOOT_CMD_FREELISTS_ALL);
	set(d, rep, FM_STEP_FREELISTS, rv, rv == FM_OK ? NULL : "command 3 did not complete");
	if (rv != FM_OK)
		return rv;

	/*
	 * Step 11. PCIe.
	 *
	 * Nothing to do here and nothing to wait for. We are talking to the chip
	 * over the SCD's local bus, not over PCIe, and the endpoint is expected
	 * to be absent until it is configured -- which is a later job, for
	 * whatever wants packet DMA. Saying "ok" would claim a link that has not
	 * been brought up.
	 */
	set(d, rep, FM_STEP_PCIE, FM_ENOADDR,
	    "not attempted: the PCIe block is configured separately, and "
	    "nothing before packet DMA needs it");

	/*
	 * Step 12. Initialise memory. The datasheet offers two routes -- program
	 * the CRM and launch it, or "software writes memory manually" -- and
	 * this is the second, because it needs nothing but the bulk writer and
	 * the CRM's command register map is not known.
	 *
	 * One prerequisite: MSB released (SSCHED block is in the MSB domain).
	 * FC_MRL tokens are set below but for the sweeper's sake, not the fill.
	 *
	 * 0x240000/0x260000 are register blocks (not ECC SRAM), and 0x003000/
	 * 0x003c00 are ESCHED runtime state (inaccessible without ring). Neither
	 * set is in the fill table. See regs.h and esched.c.
	 *
	 * The ring is NOT started here (no tokens, no INIT_COMPLETE). The
	 * scheduler clock domain (TICK_CFG=2) IS activated before the fill --
	 * without it the PARSER SRAM crashes. --ssched loads tokens and fires
	 * INIT_COMPLETE after the fill completes.
	 */

	/*
	 * Step 7, SECOND HALF. MSB (core fabric) released after the boot
	 * controller's bank-repair and freelist commands. The SSCHED block is
	 * in the MSB domain and its registers are not reachable until MSB is out
	 * of reset. Coldreplay order: boot commands -> MSB -> ring init -> fill.
	 */
	rv = release_modules(d, 0);
	if (rv != FM_OK) {
		set(d, rep, FM_STEP_MEMORY_INIT, rv,
		    mem_init ? "could not release MSB" : "could not release MSB (skip path)");
		return rv;
	}
	rep->step[FM_STEP_MODULES].note = "released, MSB last after the boot commands";

	/*
	 * Activate the scheduler clock domain and zero the sweeper config.
	 *
	 * TICK_CFG=2 must be written before the fill: without it the PARSER
	 * SRAM (bank 0, 0x100200) crashes on its first write (FATAL_COUNT
	 * 4→0, chip off bus). Measured 2026-10-01. TICK_CFG=2 is the clock
	 * the chip's SRAM ECC domain runs on; the boot-controller write that
	 * initialises the bank-repair fuses does NOT activate it.
	 *
	 * INIT_COMPLETE is NOT fired here. Firing it (which also requires
	 * TICK_CFG=2 active and tokens in the FIFO) starts the ring engine
	 * against uninitialized ESCHED tables and produces ~107 self-resets
	 * during the fill. The tokens and INIT_COMPLETE are deferred to
	 * --ssched, which runs after the fill is complete.
	 *
	 * SWEEPER_CFG_0-4 are all zeroed (sweeper disabled during fill).
	 */
	(void)fm_wr(d, FM6000_SSCHED_TICK_CFG, FM6000_SSCHED_TICK_PERIOD);
	(void)fm_wr(d, FM6000_SWEEPER_CFG_0, 0u);
	(void)fm_wr(d, FM6000_SWEEPER_CFG_1, 0u);
	(void)fm_wr(d, FM6000_SWEEPER_CFG_2, 0u);
	(void)fm_wr(d, FM6000_SWEEPER_CFG_3, 0u);
	(void)fm_wr(d, FM6000_SWEEPER_CFG_4, 0u);

	/*
	 * FC_MRL flow-control tokens (prerequisite 3). ESCHED writes stall
	 * without these. Values from coldreplay, validated against EOS boot.
	 */
	rv = fm_wr(d, FM6000_FC_MRL_RATE_LIMITER,  0x500u);
	if (rv == FM_OK)
		rv = fm_wr(d, FM6000_FC_MRL_FC_TOKEN_LIMIT, 0x40000100u);
	if (rv != FM_OK) {
		set(d, rep, FM_STEP_MEMORY_INIT, rv, "could not set FC_MRL before fill");
		return rv;
	}

	/*
	 * Step 5, AGAIN. The first scan chain write (step 3) is reset out from
	 * under itself by the boot's self-resets and leaves the core logic NOT
	 * in normal operating mode. The same write on a settled chip (all
	 * modules out of reset, ring running) is free -- FATAL_COUNT does not
	 * move. Done here so both the mem_init=1 and mem_init=0 (skip) paths
	 * leave the chip in normal operating mode for any subsequent fill.
	 */
	rv = fm_wr(d, FM6000_SCAN_CONFIG_DATA_IN, 0x88800000u);
	if (rv == FM_OK)
		rv = fm_wr(d, FM6000_SCAN_CONFIG_DATA_IN, 0x88008000u);
	if (rv == FM_OK)
		rv = fm_wr(d, FM6000_SCAN_CONFIG_DATA_IN, 0x80000040u);
	if (rv == FM_OK)
		rv = fm_wr(d, FM6000_SCAN_CHAIN_DATA_IN, 0xffffffff);
	if (rv != FM_OK) {
		set(d, rep, FM_STEP_MEMORY_INIT, rv,
		    "settled-chip scan chain write failed before fill");
		return rv;
	}
	rep->step[FM_STEP_SCAN_CHAIN].note =
	    "done twice: once in order (reset out from under itself), "
	    "and again before the fill once the chip had settled";

	if (!mem_init) {
		set(d, rep, FM_STEP_MEMORY_INIT, FM_ENOADDR, "skipped by request");
		fm_boot_mark_done(d);
		return FM_OK;
	}

	/*
	 * Fill all ECC-protected SRAM banks in order. Every bank must be
	 * written before the sweeper runs -- a cold chip holds uninitialised
	 * ECC syndromes that the sweeper treats as fatal errors, resetting the
	 * fabric before the scheduler ring can ever start.
	 *
	 * The CM watermark banks (RXMP_PRIVATE_WM / HOG_WM) are written with
	 * 0xffffffff so the fabric admits frames from the start. Everything
	 * else is zeroed.
	 */
	rv = fm_memfill_all(d);
	if (rv != FM_OK) {
		set(d, rep, FM_STEP_MEMORY_INIT, rv,
		    "the chip stopped answering during the full memory fill");
		return rv;
	}
	fm_bank_mark_initialised(d);

	set(d, rep, FM_STEP_MEMORY_INIT, FM_OK,
	    "all ECC-protected SRAM banks filled; "
	    "CM watermarks set to 0xffffffff (admit-all)");

	fm_boot_mark_done(d);
	return FM_OK;
}

/*
 * Every one of boot_steps()' thirteen exits has to be followed by the same
 * accounting, and the failure paths are the ones where it matters most -- a
 * sequence that stopped early is exactly when you want to know whether it
 * stopped because the chip reset itself. So the steps are an inner function
 * and this is the only way out.
 */
int fm_boot_cold_opt(struct fm6000 *d, struct fm_boot_report *rep, int mem_init)
{
	int rv = boot_steps(d, rep, mem_init);

	finish(d, rep);
	return rv;
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
		if (rep->step[i].resets != 0)
			printf("          ⚠ the chip reset ITSELF %u time%s under "
			       "this step -- whatever it wrote is gone\n",
			       rep->step[i].resets,
			       rep->step[i].resets == 1 ? "" : "s");
		if (rep->step[i].note != NULL)
			printf("          %s\n", rep->step[i].note);
	}

	if (rep->resets != 0)
		printf("\n  ⚠ %u self-reset%s during this sequence, last code "
		       "0x%02x. Steps marked ok above ran, but the watchdog put "
		       "the fabric back to defaults under them.\n",
		       rep->resets, rep->resets == 1 ? "" : "s",
		       rep->last_fatal);
	fflush(stdout);
}
