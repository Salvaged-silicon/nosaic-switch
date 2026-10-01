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
 * Measured on this board, 2026-09-28, each word written alone from a fresh
 * boot with FATAL_COUNT stable at 8: words 0, 1 and 2 leave it stable, and
 * word 3 (0x1c04b) and word 4 (0x1c04c) EACH start a reset storm that never
 * stops. Two triggers, not one -- an earlier note here said word 4 was
 * harmless, which was measured after word 3 had already started the storm.
 *
 * That is very likely why this ring has never circulated. It is programmed
 * correctly into a chip that is being reset several times a second, so nothing
 * written survives long enough to run.
 */
#define FM_SSCHED_NO_SWEEPER	(1u << 1)

/*
 * Build the visit table as a CYCLE over the ring rather than as self-pointers.
 *
 * The registers are named NEXT_PORT, and the table is 80 bytes indexed by
 * port. Read that way an entry says which port the engine serves after this
 * one, and the enrolled ports should form a closed cycle in service order.
 *
 * What this file has always written instead is entry[p] = p, for five ports
 * only -- every enrolled port pointing at itself. If the name means what it
 * says, that is 64 tokens in a ring whose every next-pointer is a self-loop,
 * which would look exactly like what we see: a ring that programs cleanly,
 * reports no error, and never advances to the port the find-probe asks for.
 *
 * ⚠ TESTED 2026-09-28 AND IT IS NOT THE ANSWER. The cycle builds correctly --
 * read back, byte[0]=21, byte[1]=22, byte[2]=68, byte[3]=23, exactly the ring
 * order -- and the ring still does not circulate. Neither reading of this
 * table starts it.
 *
 * The flag stays because the semantics of NEXT_PORT are genuinely ambiguous
 * and both interpretations are now implemented and measured, which is worth
 * more than one of them being quietly deleted. Default is the original.
 */
#define FM_SSCHED_NEXT_CHAIN	(1u << 2)

/*
 * Push only the five locked tokens (ports 0, 1, 2, 3, and the management
 * port) rather than all 64.
 *
 * ⚠ WHY THIS EXISTS. INIT_TOKEN is a hardware write-only FIFO of unknown
 * depth. If its capacity is less than 64, writing all 64 tokens silently
 * discards the EARLIEST entries. The find probe checks ports 20, 24, 28 and
 * 0 (the first four from the full ring order), which would all be gone if
 * the FIFO held fewer than 64. The ring might be advancing with the later
 * tokens and we would never know.
 *
 * This flag matches the "golden ring" in the EdgeNOS reference implementation
 * exactly: five Locked tokens with self-pointer NEXT entries. If this
 * circulates when the 64-token variant does not, the FIFO depth is the cause.
 *
 * ⚠ NEITHER VARIANT EVER CIRCULATED. Root cause found 2026-10-01: PLL_CTRL_0/1
 * (0x1c042/0x1c043) were not written, leaving the SSCHED clock domain without
 * a source. See regs.h FM6000_PLL_CTRL_0. boot.c now writes PLL_CTRL in step 6
 * and uses the full 65-token ring (this flag no longer used in that path).
 */
#define FM_SSCHED_SMALL_RING	(1u << 3)

/*
 * If set, write the captured CmMonitorTickPeriod value (0x20) to
 * SWEEPER_CFG word 4 instead of leaving it at zero.  Safe only after
 * --boot has run a full CRM bulk-init: the CM Monitor will fault on
 * uninitialised counter tables.  EdgeNOS says the sweeper drives the
 * scheduler tick; if the ring still does not circulate with this flag,
 * the clock source is not the issue and something else blocks the engine.
 */
#define FM_SSCHED_CM_TICK	(1u << 4)

/*
 * Management-port-only bootstrap ring.
 *
 * Loads a single token for port 78 (the CPU/management port). Port 78 is
 * above the 76-entry ESCHED table, so the ring engine does not perform an
 * ESCHED lookup for its token. This lets the ring circulate with uninitialized
 * ESCHED, setting sched_ready=1 so that fm_esched_init can run.
 *
 * Intended use:
 *   1. --ssched nosweep mgmt_only  → ring circulates, sched_ready=1
 *   2. --esched                    → writes all 76 ESCHED entries
 *   3. --ssched nosweep            → reload ring with full token set
 */
#define FM_SSCHED_MGMT_ONLY	(1u << 5)


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
	FM_SSCHED_PH_ESCHED,	/* ESCHED pre-init (valid ECC before INIT_COMPLETE) */
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
