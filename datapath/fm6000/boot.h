/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The FM6000's documented cold boot, as code.
 *
 * Intel 331496-002 §4.2 Table 4-1 gives the whole thing as twelve ordered
 * steps. This file is that table, in that order, with each step named -- and
 * with the steps whose register addresses are not yet established REFUSING by
 * name rather than doing something approximate.
 *
 * That refusal is the point of the file at this stage. A bring-up that guesses
 * an address does not fail; it writes somewhere else, and the chip carries on
 * looking almost right until something much later makes no sense. Naming the
 * gap means the next hardware session knows exactly what it is looking for.
 *
 * WHAT THIS IS NOT: a replay. Every write here has a documented reason, and
 * the sequence is parameterised by nothing at all because a cold boot is not
 * board-specific -- what follows it is.
 */
#ifndef NOSAIC_FM6000_BOOT_H
#define NOSAIC_FM6000_BOOT_H

#include "pci.h"



enum fm_boot_step {
	FM_STEP_RESET_RELEASED = 1,	/* 1-3: hardware + boot controller */
	FM_STEP_BOOT_METHOD,		/* 4: boot from CPU, chip is stalled */
	FM_STEP_SCAN_CHAIN,		/* 5: normal operating mode */
	FM_STEP_PLL,			/* 6: PLL lock */
	FM_STEP_MODULES,		/* 7: SOFT_RESET release */
	FM_STEP_FFU_SLICES,		/* 8: BOOT command 1 */
	FM_STEP_BANK_REPAIR,		/* 9: BOOT command 2 */
	FM_STEP_FREELISTS,		/* 10: BOOT command 3 */
	FM_STEP_PCIE,			/* 11 */
	FM_STEP_MEMORY_INIT,		/* 12: CRM, or software fill */
	FM_STEP__COUNT
};

struct fm_boot_step_result {
	int         rv;		/* FM_OK, FM_ENOADDR, FM_EOFFBUS, FM_ERR */
	const char *what;	/* what the step does */
	const char *note;	/* why it stopped, when it did */
};

struct fm_boot_report {
	struct fm_boot_step_result step[FM_STEP__COUNT];
	int reached;		/* the last step attempted */
	int ok;			/* every step returned FM_OK */
};

/*
 * Run the documented sequence as far as it will go, filling in `rep`.
 *
 * Stops at the first step that does not return FM_OK, because these are
 * ordered and continuing past a failed one is how a chip ends up in a state
 * nobody can describe. Returns FM_OK only if all twelve succeeded.
 */
int fm_boot_cold(struct fm6000 *d, struct fm_boot_report *rep);

/*
 * The same sequence, with step 12 optional.
 *
 * Step 12 fills a 262144-word memory and there is a live question about what
 * else that disturbs, so it has to be possible to run steps 1-11 and stop --
 * otherwise "did step 12 do this?" cannot be asked at all.
 */
int fm_boot_cold_opt(struct fm6000 *d, struct fm_boot_report *rep, int mem_init);

/*
 * Has the documented cold boot already run on this chip?
 *
 * Asked because nosd restarting must not put a forwarding chip back through
 * Table 4-1. The test is the end state the sequence leaves behind and that a
 * chip which has not been through it cannot have: every module out of soft
 * reset, and the boot controller reporting the last of its three commands
 * complete.
 *
 * Returns 1 for yes, 0 for no, negative if the chip could not be read. On a
 * negative the caller should NOT boot -- a chip that cannot be read cannot be
 * booted either, and the failure belongs in the log rather than in a write.
 */
int fm_boot_already_done(struct fm6000 *d);

/* Print a report in the form a human reads at three in the morning. */
void fm_boot_report_print(const struct fm_boot_report *rep);

const char *fm_boot_step_name(int step);

#endif /* NOSAIC_FM6000_BOOT_H */
