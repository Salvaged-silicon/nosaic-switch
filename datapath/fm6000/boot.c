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
	if (fm_check_offbus(d) == 1) {
		set(rep, FM_STEP_RESET_RELEASED, FM_EOFFBUS,
		    "chip is not answering config space");
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
	 * We cannot poll PLL_STATUS because we do not know where it is, and we
	 * cannot initialise the PLL for the same reason. Waiting the documented
	 * maximum is the correct conservative behaviour for the wait; it is NOT
	 * a substitute for the initialisation, so this step is honest about
	 * being incomplete rather than reporting success for a sleep.
	 */
	nap_ms(FM6000_PLL_LOCK_MAX_MS);
	set(rep, FM_STEP_PLL, FM_ENOADDR,
	    "PLL_STATUS address not established; waited the documented 80 ms "
	    "maximum but did not initialise or confirm lock");

	/*
	 * Everything below is unreachable today and is left in place because it
	 * is the specification, and because the next person's job is to delete
	 * these refusals one at a time as the addresses are found.
	 *
	 * Step 7   SOFT_RESET holds EPL, PCIe, MSB and SPICO/SBUS at reset by
	 *          default and each must be released. A cold chip reads 0x16 and
	 *          bring-up drives it to 0 -- we know the VALUE and not the
	 *          ADDRESS.
	 *
	 * Steps 8-10  BOOT_CTRL:Command = 1, then 2, then 3, each polled on
	 *          BOOT_STATUS:CommandDone. We have a candidate address for
	 *          BOOT_CTRL (0x1c022, from watching a working boot) but not its
	 *          field layout, and no address at all for BOOT_STATUS. Writing
	 *          a command into the wrong field of the right register is
	 *          exactly the kind of near-miss this file refuses to make.
	 *
	 *          Step 9 is the one that matters most: "apply bank memory
	 *          repairs" is the documented answer to the wall the prior work
	 *          spent twenty-five phases on.
	 *
	 * Step 11  PCIe is already up -- we are talking to the chip over it.
	 *
	 * Step 12  Initialise memory, either by programming the CRM and
	 *          launching it or, in the datasheet's own words, "software
	 *          writes memory manually". The second is what this port will
	 *          do: it is a bulk writer, so it turns the per-write bus check
	 *          off and owes one at the end (see fm_set_write_check), and
	 *          when it completes it is the ONLY thing entitled to call
	 *          fm_bank_mark_initialised().
	 */
	return FM_ENOADDR;
}

void fm_boot_report_print(const struct fm_boot_report *rep)
{
	int i;

	for (i = 1; i < FM_STEP__COUNT; i++) {
		const char *mark;

		if (i > rep->reached) {
			printf("  %2d  ....  %s\n", i, fm_boot_step_name(i));
			continue;
		}
		switch (rep->step[i].rv) {
		case FM_OK:       mark = " ok "; break;
		case FM_ENOADDR:  mark = "GAP "; break;
		case FM_EOFFBUS:  mark = "OFF!"; break;
		case FM_EUNSAFE:  mark = "STOP"; break;
		default:          mark = "FAIL"; break;
		}
		printf("  %2d  %s  %s\n", i, mark, fm_boot_step_name(i));
		if (rep->step[i].note != NULL)
			printf("          %s\n", rep->step[i].note);
	}
}
