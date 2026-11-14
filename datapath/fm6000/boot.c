/* SPDX-License-Identifier: Apache-2.0 */
/* Intel 331496-002 §4.2 Table 4-1, in order. See boot.h. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "bist.h"
#include "boot.h"
#include "memfill.h"
#include "pci.h"
#include "pcie_init.h"
#include "regs.h"
#include "esched.h"
#include "ssched.h"

static void nap_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static void dbg(const char *msg)
{
	int kmsg_fd;

	fprintf(stderr, "%s\n", msg);
	fflush(stderr);
	kmsg_fd = open("/dev/kmsg", O_WRONLY | O_NOCTTY);
	if (kmsg_fd >= 0) {
		write(kmsg_fd, msg, strlen(msg));
		write(kmsg_fd, "\n", 1);
		close(kmsg_fd);
	}
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
	[FM_STEP_BIST]           = "BIST memory march and freelist restore",
	[FM_STEP_SCHED]          = "scheduler token ring circulating",
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

	/* Guard the BAR1 read: ESCHED writes trigger ~1-2 FATALs that bounce
	 * the PCIe link.  On AMD RS780 with SERR# disabled, any BAR1 MMIO
	 * access while the link is re-training stalls the CPU permanently.
	 * Wait here for DLLLA=1 (safe sysfs read, no bus=2 access) before
	 * touching BAR1 via fm_fatal_read().  [install153, 2026-10-27] */
	if (fm_wait_link_active(10000) == FM_OK && fm_fatal_read(d, &f) == FM_OK)
		rep->last_fatal = f.last;

	for (i = 1; i < FM_STEP__COUNT; i++)
		if (rep->step[i].what != NULL &&
		    rep->step[i].rv != FM_OK &&
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
	if (step > rep->reached)
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
		/*
		 * Chip is not answering. On the LBUS transport this means no
		 * reset pulse has been issued -- the SCD leaves the resets
		 * deasserted at power-on but PIN_STRAP reads 0 until the chip
		 * has been pulsed. Issue the pulse now and retry once.
		 *
		 * pci.h: "After a successful call, wait ~100 ms for the chip
		 * to come up, then call fm_clear_offbus() before trying again."
		 */
		if (d->xport == FM_XPORT_LBUS &&
		    fm_scd_reset_pulse(d) == FM_OK) {
			nap_ms(100);
			fm_clear_offbus(d);
		}
		if (fm_alive(d) != 1) {
			set(d, rep, FM_STEP_RESET_RELEASED, FM_EOFFBUS,
			    "chip is not answering after reset pulse; "
			    "SCD not holding the chip or pulse failed");
			return FM_EOFFBUS;
		}
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

	{
		uint32_t ss = 0;
		char lbuf[80];

		fm_rd(d, FM6000_SCAN_STATUS, &ss);
		snprintf(lbuf, sizeof(lbuf),
			 "nosd-fm6000: boot step5: SCAN_STATUS pre=0x%08x", ss);
		dbg(lbuf);
	}
	rv = fm_wr(d, FM6000_SCAN_CONTROL, FM6000_SCAN_CHAIN_CORE);
	if (rv != FM_OK) {
		set(d, rep, FM_STEP_SCAN_CHAIN, rv, "SCAN_CONTROL chain select failed");
		return rv;
	}
	rv = fm_wr(d, FM6000_SCAN_CHAIN_DATA_IN, 0xffffffff);
	if (rv != FM_OK) {
		set(d, rep, FM_STEP_SCAN_CHAIN, rv, "write to SCAN_CHAIN_DATA_IN failed");
		return rv;
	}
	{
		uint32_t ss = 0;
		char lbuf[80];

		fm_rd(d, FM6000_SCAN_STATUS, &ss);
		snprintf(lbuf, sizeof(lbuf),
			 "nosd-fm6000: boot step5: SCAN_STATUS post=0x%08x (0x100=retired)", ss);
		dbg(lbuf);
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
	 * Step 9 (PCIe) runs after Phase 1 ring init (MGMT_ONLY) -- see below.
	 * The 42 fm_boot_freelists() self-resets would wipe any SerDes
	 * configuration written here, and PCIe needs the ring running for
	 * link stability during enumeration.
	 */

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
	 * The ring is started after the fill (step 14 below). The scheduler
	 * clock domain (TICK_CFG=2) IS activated before the fill -- without it
	 * the PARSER SRAM crashes. BIST runs after the fill to make ESCHED ECC
	 * valid, then INIT_COMPLETE fires.
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
	{
		uint32_t ss = 0;
		char lbuf[80];

		fm_rd(d, FM6000_SCAN_STATUS, &ss);
		snprintf(lbuf, sizeof(lbuf),
			 "nosd-fm6000: boot step5-again: SCAN_STATUS pre=0x%08x", ss);
		dbg(lbuf);
	}
	rv = fm_wr(d, FM6000_SCAN_CONFIG_DATA_IN, 0x88800000u);
	if (rv == FM_OK)
		rv = fm_wr(d, FM6000_SCAN_CONFIG_DATA_IN, 0x88008000u);
	if (rv == FM_OK)
		rv = fm_wr(d, FM6000_SCAN_CONFIG_DATA_IN, 0x80000040u);
	if (rv == FM_OK)
		rv = fm_wr(d, FM6000_SCAN_CONTROL, FM6000_SCAN_CHAIN_CORE);
	if (rv == FM_OK)
		rv = fm_wr(d, FM6000_SCAN_CHAIN_DATA_IN, 0xffffffff);
	if (rv != FM_OK) {
		set(d, rep, FM_STEP_MEMORY_INIT, rv,
		    "settled-chip scan chain write failed before fill");
		return rv;
	}
	{
		uint32_t ss = 0;
		char lbuf[80];

		fm_rd(d, FM6000_SCAN_STATUS, &ss);
		snprintf(lbuf, sizeof(lbuf),
			 "nosd-fm6000: boot step5-again: SCAN_STATUS post=0x%08x", ss);
		dbg(lbuf);
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

	/*
	 * Step 13. BIST memory march.
	 *
	 * The BIST march writes every SRAM entry with a known pattern, making
	 * each ECC syndrome valid.  ESCHED (the egress scheduler's per-port
	 * configuration) cannot be written through the PCI bridge without a
	 * valid ECC syndrome in its SRAM -- any write attempt before BIST
	 * causes a burst of CRM access timeouts that self-reset the chip.
	 *
	 * BIST also wipes the scheduler freelist SRAM that boot step 8
	 * initialised.  fm_boot_freelists() re-runs that boot command
	 * immediately after so the freelist is valid before any SSCHED write.
	 *
	 * ⚠ PREREQUISITE FOR THE RING: fm_bist_memory_init() MUST be called
	 * before fm_ssched_ring_init() for a full or small ring.  Without it
	 * the settle phase (from fm_boot_freelists) produces resets from ECC
	 * init (code 0xd0) that wipe NEXT_PORT/SLOW_PORT before INIT_COMPLETE.
	 */
	{
		struct fm_bist_report brep;

		rv = fm_bist_memory_init(d, 0, &brep);
		if (rv != FM_OK) {
			set(d, rep, FM_STEP_BIST, rv,
			    "BIST march failed or chip stopped answering");
			return rv;
		}
		/*
		 * SAF SRAM is not reachable by BIST on this chip: fm_bist_memory_init
		 * does not initialise SAF ECC.  Any write to SAF (via fm_wr or direct
		 * MMIO) causes an internal ECC read-modify-write on uninitialised ECC,
		 * which faults the chip (measured: install96-install99, every attempt
		 * at fc=0 through fc=242 all produced alive=0 fc=0 immediately after
		 * the write loop). SAF init is therefore intentionally omitted here and
		 * in pipeline_init.  The chip runs with BIST march residue in SAF.
		 */
		rv = fm_boot_freelists(d);
		if (rv != FM_OK) {
			set(d, rep, FM_STEP_BIST, rv,
			    "freelist re-init after BIST failed");
			return rv;
		}
	}
	set(d, rep, FM_STEP_BIST, FM_OK,
	    "BIST march complete; freelists restored");

	/*
	 * Phase 1 (MGMT_ONLY) is intentionally omitted.
	 *
	 * SSCHED's IC flip-flop lives in a persistent power domain that only
	 * resets at power-on.  Firing MGMT_ONLY's IC=1 puts SSCHED into RUNNING
	 * state; the engine then stays RUNNING through fm_boot_freelists (42
	 * resets), and any number of scheduler-domain watchdog resets.
	 * NEXT_CHAIN's IC=1 on a RUNNING engine is a no-op -- the ring
	 * continues looping the MGMT_ONLY token.
	 *
	 * Fix: leave SSCHED in RESET (IC never fired) so NEXT_CHAIN's IC=1 is
	 * the first strobe on a fresh engine and the 64-token ring circulates.
	 */

	/*
	 * Step 9 (PCIe SerDes init) is deferred to AFTER ring_init.
	 *
	 * c5567d2 (first confirmed TX circulation, 2026-10-11) ran ring_init
	 * without PCIe Memory Space enabled (pcie_regs was NULL; the standalone
	 * --ssched tool never called fm_pcie_enable_command).  TX ring
	 * circulated.  All subsequent boots have had Memory Space enabled
	 * BEFORE ring_init and TX never circulates (TX_REPLACE_TOKEN stuck at
	 * ring slot 0, RX_FOUND=1, TX_FOUND=0 every time).
	 *
	 * install59 hypothesis (2026-10-23): enabling PCIe Memory Space before
	 * IC=1 stalls the TX ring engine.  The fix is to run ring_init with
	 * pcie_regs == NULL (BAR0 not attached).  PCIe SerDes init runs once,
	 * post-ring, below.
	 *
	 * fm_esched_init() is a no-op when pcie_regs == NULL (guard at the
	 * top of the function).  BIST provides ECC-valid zero for all ESCHED
	 * SRAM; no writes are needed pre-probe.
	 */

	/*
	 * d->pcie_regs is NULL here; fm_esched_init() inside
	 * fm_ssched_ring_init() will skip all writes via the pcie_regs == NULL
	 * guard.  BIST provides ECC-valid zero for all ESCHED SRAM.
	 */
	fm_sched_mark_ready(d);

	/*
	 * Step 14: Start the scheduler token ring (NEXT_CHAIN mode).
	 *
	 * All 64 front-panel/ring-internal ports enrolled in a NEXT_CHAIN
	 * visit table (port 78 excluded -- FIFO capacity is 64).  NO_SWEEPER
	 * skips SWEEPER_CFG word 3 (the manageability reference timer), which
	 * arms itself over uninitialised tables and fires 315 self-resets on
	 * every attempt.
	 *
	 * fm_ssched_ring_init() writes ESCHED post-settle (fm_esched_init),
	 * before the ring ticks any ESCHED slot.  The NEXT_CHAIN settle resets
	 * affect only SSCHED SRAM; ESCHED SRAM remains valid after they clear.
	 *
	 * FOUND bit in RX_REPLACE_TOKEN confirms the ring engine is walking
	 * the full ring.
	 *
	 * install67: log PCI Command register for 0000:02:00.0 immediately before
	 * ring_init to check whether the kernel already enabled Memory Space.
	 * If MemSpc=1 here, the hypothesis that Memory Space before IC=1 stalls
	 * TX is testable; if MemSpc=0, the stall has a different cause.
	 */
	{
		int _cfd = open("/sys/bus/pci/devices/0000:02:00.0/config", O_RDONLY);
		if (_cfd >= 0) {
			uint16_t _cmd = 0;
			if (pread(_cfd, &_cmd, sizeof _cmd, 4) == (ssize_t)sizeof _cmd) {
				char _pb[128];
				snprintf(_pb, sizeof _pb,
					"nosd-fm6000: pre-ring PCI CMD(02:00.0)=0x%04x MemSpc=%d BusMaster=%d",
					_cmd, !!(_cmd & 0x0002), !!(_cmd & 0x0004));
				dbg(_pb);
			}
			close(_cfd);
		}
	}
	{
		struct fm_ssched_report srep;
		int circ = 0;

		rv = fm_ssched_ring_init(d, FM_SSCHED_NEXT_CHAIN |
					    FM_SSCHED_NO_SWEEPER,
					 &circ, &srep);
		/* Always log phase reset counts for diagnostics. */
		{
			char buf[128];
			int k;
			for (k = 0; k < FM_SSCHED_PH__COUNT; k++) {
				if (srep.resets[k] == 0)
					continue;
				snprintf(buf, sizeof buf,
					 "nosd-fm6000: NEXT_CHAIN phase %-24s: %u resets",
					 fm_ssched_phase_name(k), srep.resets[k]);
				dbg(buf);
			}
			snprintf(buf, sizeof buf,
				 "nosd-fm6000: NEXT_CHAIN total=%u circ=%d last_fatal=0x%02x",
				 srep.total, circ, srep.last_fatal);
			dbg(buf);
		}
		if (rv != FM_OK || !circ) {
			set(d, rep, FM_STEP_SCHED,
			    rv != FM_OK ? rv : FM_ERR,
			    !circ ? "NEXT_CHAIN ring programmed but token not found"
				  : "NEXT_CHAIN ring init function failed");
			return rv != FM_OK ? rv : FM_ERR;
		}
	}
	set(d, rep, FM_STEP_SCHED, FM_OK,
	    "FOUND -- full ring circulating");

	/*
	 * Step 9: PCIe SerDes init (post-ring).
	 *
	 * PCIe init is deferred to here so ring_init runs with pcie_regs==NULL.
	 * See the comment above ring_init for why this matters.
	 *
	 * Load fm6000_kdrv first so the driver is registered before the PCIe
	 * endpoint appears.  pci_enable_device() and the AER handler deadlock
	 * during link training; fm_attach_pcie uses /dev/mem for BAR0
	 * (confirmed fix, 2026-10-13) and fm_reopen_lbus_devmem uses /dev/mem
	 * for BAR1 (deadlock confirmed 2026-10-14).
	 *
	 * The ~57 SPICO FATALs caused by serdes_init fire AER uncorrectable
	 * events recovered by fm6000_kdrv's handlers (error_detected →
	 * CAN_RECOVER, slot_reset → re-enable, resume), ~1 ms each.
	 */
	(void)system("modprobe fm6000_kdrv 2>/dev/null");
	{
		char saved_slot2[FM_SLOT_LEN];
		int banks_ready2 = d->banks_ready;
		int rv_s2;

		memcpy(saved_slot2, d->slot, sizeof(saved_slot2));

		dbg("nosd-fm6000: post-SSCHED: PCIe SerDes init (link → x4)");
		rv_s2 = fm_pcie_serdes_init(d);

		/* Release LBUS -- chip is resetting during SPICO loads. */
		munmap((void *)d->regs, d->bar_bytes);
		close(d->bar_fd);
		d->regs = NULL;
		d->bar_fd = -1;

		if (rv_s2 != FM_OK) {
			dbg("nosd-fm6000: post-SSCHED: PCIe SerDes init FAILED"
			    " -- DMA will not work");
		} else {
			dbg("nosd-fm6000: post-SSCHED: PCIe SerDes init done;"
			    " waiting 5s for x4 link to stabilise");
			nap_ms(5000);

			/*
			 * install205: if pcie_bar_phys is already known (second
			 * boot_cold, called from ring-reinit), skip the sysfs
			 * rescan write — it can freeze under BIOS SMI handling
			 * during PCIe link training.  Use CF8 portio to enable
			 * Memory Space directly instead.
			 *
			 * On the FIRST boot_cold, pcie_bar_phys==0 so we take
			 * the original sysfs rescan path unchanged.
			 *
			 * fm_reopen_lbus_devmem() calls memset(d,0,sizeof(*d))
			 * which would zero pcie_bar_phys and pcie_bar_bytes.
			 * Save them here and restore after the call so that
			 * fm_attach_pcie()'s cached path can mmap /dev/mem.
			 * [install205, 2026-10-29]
			 */
			unsigned long saved_pcie_phys  = d->pcie_bar_phys;
			size_t        saved_pcie_bytes = d->pcie_bar_bytes;

			if (saved_pcie_phys != 0) {
				/* Portio path: BAR address known; skip sysfs rescan */
				int rv_p = fm_pcie_enable_portio(2, 0, 0);
				char msg205[128];
				snprintf(msg205, sizeof msg205,
				    "nosd-fm6000: post-SSCHED: portio-rescan"
				    " rv=%d bar=0x%lx [install205]",
				    rv_p, saved_pcie_phys);
				dbg(msg205);
				nap_ms(200);
			} else {
				/* Original path: first boot_cold; sysfs rescan */
				int rfd = open(
				    "/sys/class/pci_bus/0000:02/rescan",
				    O_WRONLY);
				if (rfd < 0)
					rfd = open(
					    "/sys/devices/pci0000:00"
					    "/0000:00:04.0/pci0000:02/rescan",
					    O_WRONLY);
				if (rfd >= 0) {
					(void)write(rfd, "1\n", 2);
					close(rfd);
					dbg("nosd-fm6000: post-SSCHED: PCIe rescan done");
					nap_ms(1000);
				}
			}

			/* Reopen LBUS and re-attach PCIe. */
			if (fm_reopen_lbus_devmem(d, saved_slot2) == FM_OK) {
				d->banks_ready = banks_ready2;
				/* Restore PCIe BAR cache wiped by memset above */
				if (saved_pcie_phys != 0) {
					d->pcie_bar_phys  = saved_pcie_phys;
					d->pcie_bar_bytes = saved_pcie_bytes;
				}
				if (fm_attach_pcie(d) == FM_OK) {
					dbg("nosd-fm6000: post-SSCHED: PCIe attached");
					(void)fm_pcie_enable_command(d);
					dbg("nosd-fm6000: post-SSCHED: Memory Space enabled");
					/*
					 * Disable FM6000 error reporting while the endpoint is
					 * on the bus.  SERR# is NOT disabled here; see the
					 * comment before fm_pcie_serr_disable() below.
					 * [install191, 2026-10-28 — moves serr_disable to after
					 * stability wait; reverts install188 first serr_disable]
					 */
					fm_pcie_ep_disable_error_reporting(2, 0, 0);
					fm_pcie_clear_root_aer_errors();
					/*
					 * Remove FM6000 from PCIe bus before the SSCHED
					 * stability wait and the subsequent fm_spico_load.
					 *
					 * Both operations cause chip self-resets (link
					 * bounces).  pciehp just re-enumerated the endpoint
					 * via the rescan above and is now monitoring the
					 * slot.  Any link-up during a pciehp Config Space
					 * access window hangs the AMD RS780 CF8/CFC loop
					 * forever (no Completion Timeout).
					 *
					 * BAR0 (/dev/mem mapping, d->pcie_regs) stays valid
					 * for the duration: the physical range does not
					 * change.  start_forwarding() calls fm_detach_pcie()
					 * + fm_attach_pcie() after SerDes re-init to
					 * reprogram the BAR register before DMA starts.
					 *
					 * The explicit rescan in start_forwarding() Phase 2
					 * re-enumerates the endpoint with working Config
					 * Space after link training completes.
					 * [2026-10-25, install83]
					 */
					{
						int rfd = open(
						    "/sys/bus/pci/devices"
						    "/0000:02:00.0/remove",
						    O_WRONLY);
						if (rfd >= 0) {
							(void)write(rfd, "1\n", 2);
							close(rfd);
							dbg("nosd-fm6000: post-SSCHED: FM6000 removed from PCIe bus");
						} else {
							dbg("nosd-fm6000: post-SSCHED: WARNING: could not open remove path");
						}
					}
				} else {
					dbg("nosd-fm6000: post-SSCHED: PCIe attach failed"
					    " -- DMA will not work");
				}
			} else {
				dbg("nosd-fm6000: post-SSCHED: LBUS reopen failed"
				    " -- DMA will not work");
			}
		}
	}

	/*
	 * Wait for FATAL_COUNT to fully settle before writing ESCHED registers.
	 * The ~57 SPICO FATALs from the second serdes_init are quickly recovered
	 * by fm6000_kdrv's AER handlers, but we want 10 s of zero-FATAL
	 * stability before any BAR0 write to guarantee no intermediate reset
	 * during the ESCHED write sequence (which would ECC-corrupt DRR_Q and
	 * make subsequent CFG_3/DRR_CFG writes fatal).
	 */
	{
		uint32_t fc = 0, fc2 = 0;
		int n_stable = 0;
		char stab_buf[128];
		fm_rd(d, 0x8, &fc);
		snprintf(stab_buf, sizeof stab_buf,
			 "nosd-fm6000: post-SSCHED: waiting for stability fc=%u", fc);
		dbg(stab_buf);
		for (int i = 0; i < 60 && n_stable < 20; i++) {
			struct timespec ts = { 0, 500000000L };
			nanosleep(&ts, NULL);
			fm_rd(d, 0x8, &fc2);
			if (fc2 == fc)
				n_stable++;
			else {
				fc = fc2;
				n_stable = 0;
			}
		}
		snprintf(stab_buf, sizeof stab_buf,
			 "nosd-fm6000: post-SSCHED: chip %s fc=%u n_stable=%d",
			 n_stable >= 20 ? "stable" : "unstable(timeout)", fc, n_stable);
		dbg(stab_buf);
	}

	/*
	 * Clear HPIE/DLLSCE/PDCE on the upstream port before ESCHED writes.
	 *
	 * The FM6000 removal above caused pciehp to transition to EMPTY state
	 * and re-arm the three slot-interrupt enables.  ESCHED CFG_2 triggers
	 * ~1–2 chip FALTALs (confirmed install83/install115) on the running ring;
	 * each FATAL bounces the PCIe link.  With HPIE live, pciehp's IRQ handler
	 * fires on the link-up edge and issues a CF8/CFC read to FM6000 config
	 * space (bus 2) while the link is still training → AMD RS780 stalls the
	 * CF8/CFC access indefinitely (SERR# is disabled at this point; the root
	 * complex cannot release the stall without SERR#) → kernel deadlock.
	 *
	 * Clearing the three bits via the upstream port's config file (bus 0,
	 * always accessible, never stalls) quiesces pciehp without any CF8/CFC
	 * access to bus 2.  Distinct from a PCIe rescan, which enumerates bus 2
	 * and ITSELF issues CF8/CFC reads — unsafe while SERR# is disabled.
	 * [install137, 2026-10-27]
	 */
	if (fm_pcie_sltctl_clear_hpie() == FM_OK)
		dbg("nosd-fm6000: post-SSCHED: SLTCTL HPIE/DLLSCE/PDCE cleared (pciehp quiesced)");
	else
		dbg("nosd-fm6000: post-SSCHED: WARNING: SLTCTL clear failed (HPIE may be live)");

	/*
	 * Clear AER Root Error Status and disable Root Error Command before
	 * ESCHED writes.  SERR# is disabled at this point (disabled before
	 * FM6000 removal above); the HPIE comment above explains why SERR#
	 * must be off before pciehp-quiescing and before ESCHED link bounces.
	 * [install188, 2026-10-28 — reverts install186 comment]
	 */
	dbg("nosd-fm6000: post-SSCHED: pre-ESCHED: aer_clear");
	fm_pcie_clear_root_aer_errors();
	/* install166: disable Root Error Command before any link-bouncing ESCHED
	 * write to prevent aer_isr() from firing a CF8/CFC to training bus=2. */
	fm_pcie_disable_root_aer_cmd();
	dbg("nosd-fm6000: post-SSCHED: pre-ESCHED: aer_clear done");

	/*
	 * Disable SERR# before bridge_clear and ESCHED writes.
	 *
	 * We intentionally keep SERR# enabled throughout the stability wait
	 * above so that any transient FATAL link bounce during the wait causes
	 * the BIOS periodic SMI timer to receive an error completion
	 * (RS780 sends 0xFFFFFFFF) rather than stalling the CF8/CFC loop.
	 * With SERR# enabled the BIOS exits SMM normally even if the FM6000
	 * link is momentarily down; the subsequent FM6000 recovery (typically
	 * < 100 ms) ensures the next timer fire ~3.5 s later finds the link up.
	 *
	 * SERR# is disabled NOW — after the stability wait — to protect the
	 * bridge_clear CF8 sequence and the ESCHED CFG_2 link bounces that
	 * follow.  SERR# is immediately re-enabled (serr_reenable below) so
	 * CFG_2 CTOs also get error completions.
	 * [install175, 2026-10-27; install191 merges two calls into one here]
	 */
	fm_pcie_serr_disable();
	fm_pcie_clear_bridge_status();
	fm_pcie_serr_reenable();

	/*
	 * [install196] Pre-fill ECC-protected SRAM tables via LBUS before
	 * ESCHED writes.  After RESET_N (ring-reinit path) or after many
	 * self-resets during SPICO load, these cells may have uninitialized
	 * ECC; when CFG_1 enables all TCs, ring tokens traverse them and
	 * each ECC-bad read triggers a chip self-reset.  Sustained self-resets
	 * widen the FATAL window and make the BIOS periodic-timer CF8 cascade
	 * (bus=2 read during FATAL → SERR# double-fire → RS780 stall) far more
	 * likely.  Writing zeros via LBUS gives all cells valid ECC.  Values
	 * are correct: pipeline_init re-programs them after boot_cold returns.
	 * Running in both initial-boot and ring-reinit paths is harmless
	 * (BIST already wrote zeros; pipeline_init overwrites regardless).
	 */
	fm_mem_fill(d, 0x100200u, 14336u, 0u);	/* parser */
	fm_mem_fill(d, 0x108000u,   152u, 0u);
	fm_mem_fill(d, 0x108200u,   304u, 0u);
	fm_mem_fill(d, 0x014000u,    76u, 0u);	/* LBS_CAM */
	fm_mem_fill(d, 0x00e000u,  1024u, 0u);	/* GLORT_CAM */
	fm_mem_fill(d, 0x00e800u,  2048u, 0u);	/* GLORT_RAM */
	fm_mem_fill(d, 0x180000u, 16384u, 0u);	/* L2F_TABLE_4K */
	fm_mem_fill(d, 0x1a0000u,  4096u, 0u);	/* L2F_TABLE_256 */
	dbg("nosd-fm6000: post-SSCHED: SRAM pre-fill done [install196]");

	dbg("nosd-fm6000: post-SSCHED: writing ESCHED post-ring config");
	fm_esched_set_cfg2(d);
	dbg("nosd-fm6000: post-SSCHED: ESCHED post-ring config done");
	/* Leave BAR0 mapped (d->pcie_regs) -- the DMA engine needs it. */

	/* SERR# is re-enabled above; clear Root Error Status accumulated
	 * during ESCHED-triggered FATALs so the BIOS timer sees a clean
	 * slate on its next poll.  [install154, install175, 2026-10-27] */
	fm_pcie_clear_root_aer_errors();

	/*
	 * Now wait for the PCIe link to re-train before finish() reads BAR1.
	 * Also guarded inside finish() for defence-in-depth.
	 * [install150/153/154, 2026-10-27]
	 */
	fm_wait_link_active(5000);

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

/*
 * Re-run BOOT command 3 (FREELISTS_ALL) on an already-booted chip.
 *
 * The BIST memory march overwrites ALL SRAM banks, including the scheduler
 * freelist SRAM that boot step 8 initialised. Writing both a TX and an RX
 * NEXT_PORT entry after a BIST march crashes the chip because the freelist
 * data is invalid march residue. Calling this after fm_bist_memory_init()
 * restores the freelist to a valid state.
 */
int fm_boot_freelists(struct fm6000 *d)
{
	return boot_command(d, FM6000_BOOT_CMD_FREELISTS_ALL);
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
