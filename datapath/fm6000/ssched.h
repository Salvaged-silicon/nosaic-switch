/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_FM6000_SSCHED_H
#define NOSAIC_FM6000_SSCHED_H

#include <stdint.h>

struct fm6000;

/*
 * Bring up the scheduler ring.
 *
 * On success the scheduler engine is circulating, which is the precondition
 * for the egress scheduler block being reachable at all -- see ssched.c.
 *
 * `circulating` is set to 1 if the running engine was observed to walk the
 * ring and find a token, 0 if it did not. A ring that initialises without
 * circulating is a failure that reports success everywhere else, so the
 * caller is given it separately rather than folded into the return value.
 */
/*
 * Set the Sync bit on the management port's token.
 *
 * ⚠ OUR TWO PRIOR GENERATIONS DISAGREE ABOUT THIS BIT. The earlier one sets
 * it and says the golden ring captured from running EOS has it set; the later
 * one clears it and says the SDK's own ring builder never sets Sync on any
 * token. They cannot both be right, and this has cost a day before, so the
 * bit is a parameter and the answer is whichever one circulates.
 */
#define FM_SSCHED_SYNC_MGMT	(1u << 0)

/*
 * Do not program SWEEPER_CFG.
 *
 * SWEEPER_CFG is one eight-word register at 0x1c048 that programs the
 * manageability module's reference timers -- PAUSE, POLICERS, the L2 lookup
 * sweepers and FRAME TIMEOUT [DS §9.2]. Starting those timers sets background
 * engines walking the MAC table and the policer banks, and on a chip whose
 * tables have not been initialised those accesses time out. A CRM access
 * timeout writes FATAL_CODE, and the watchdog answers that by resetting the
 * management module and the core fabric [DS §4.2].
 *
 * Measured on this board, 2026-09-28, and it is one write: with FATAL_COUNT
 * stable at 8 after a clean boot, writing word 3 of SWEEPER_CFG (0x1c04b)
 * starts a reset storm that never stops. Words 0, 1, 2 and 4 are harmless.
 *
 * That is very likely why this ring has never circulated. It is programmed
 * correctly into a chip that is being reset several times a second, so nothing
 * written survives long enough to run.
 */
#define FM_SSCHED_NO_SWEEPER	(1u << 1)

/*
 * Where the ring init was when the chip reset itself.
 *
 * ⚠ THIS IS THE POINT OF THE REPORT, not a nicety. Measured on this board: a
 * ring init that reports success resets the chip THIRTY-SEVEN times while it
 * runs. Each reset puts the fabric back to defaults, so every token written
 * before it is gone -- which is the obvious candidate for why a ring that is
 * programmed byte-perfectly has never once circulated. See struct fm_fatal.
 *
 * `rep` may be NULL if the caller genuinely does not care.
 */
enum fm_ssched_phase {
	FM_SSCHED_PH_TICK = 0,	/* the tick, the clock everything else needs */
	FM_SSCHED_PH_SWEEPER,	/* SWEEPER_CFG 0..4 */
	FM_SSCHED_PH_CLEAR1,	/* replace-token registers cleared */
	FM_SSCHED_PH_TOKENS,	/* the ring itself, in service order */
	FM_SSCHED_PH_VISIT,	/* the visit table */
	FM_SSCHED_PH_SLOW,	/* the slow-port mask */
	FM_SSCHED_PH_START,	/* RX/TX INIT_COMPLETE strobes */
	FM_SSCHED_PH_CLEAR2,	/* replace-token registers cleared again */
	FM_SSCHED_PH_FIND,	/* walking the ring looking for a token */
	FM_SSCHED_PH__COUNT
};

struct fm_ssched_report {
	unsigned resets[FM_SSCHED_PH__COUNT];
	unsigned total;
	uint32_t last_fatal;
	uint32_t mark;		/* running FATAL_COUNT, internal to the run */
};

const char *fm_ssched_phase_name(int ph);

int fm_ssched_ring_init(struct fm6000 *d, unsigned flags, int *circulating,
			struct fm_ssched_report *rep);

#endif /* NOSAIC_FM6000_SSCHED_H */
