/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_FM6000_MRL_H
#define NOSAIC_FM6000_MRL_H

#include <stdint.h>

struct fm6000;

/*
 * The scan-chain sequence the vendor calls the "MRL register fix".
 *
 * This is NOT Table 4-1 step 9. That step asks the boot controller to apply
 * bank memory repairs with a single hardware command, which boot.c issues.
 * This is a separate sequence the vendor drives by hand, one 32-bit word at a
 * time, through the five-register scan window at 0x1c039..0x1c03d.
 *
 * ⚠ WHERE IT RUNS, CORRECTED. An earlier version of this comment said it runs
 * after the boot commands. It does not: fm6000PrebootSwitch calls
 * fm6000BistMemoryInit, then this (or MrlRegisterFixVersion2, chosen by an API
 * attribute), then writes three words to SCAN_CONFIG_DATA_IN to stop the scan
 * engine, and only then does Table 4-1 step 5's write of 0xffffffff to
 * SCAN_CHAIN_DATA_IN. So it belongs BEFORE step 5, in the pre-boot.
 *
 * WHY IT IS HERE. The scheduler ring initialises byte-for-byte correctly and
 * still will not circulate. The bank memories the ring walks are exactly what
 * a repair sequence configures, and this is the one documented-adjacent step
 * between a reset chip and a working one that we have never run. See
 * docs/todo.md M2.
 *
 * WHAT WE HAVE AND WHAT WE DO NOT. The sequence -- the order of writes, which
 * register each word goes to, the selector before each one, the status poll
 * after it, and the control words that start and stop the engine -- is
 * recovered and implemented below as our own code. The bulk payload is not.
 * The vendor ships roughly six thousand words of chain data; that is third
 * party data and NOSaic does not carry it. fm_mrl_apply() therefore shifts a
 * payload the caller supplies, and the caller that supplies nothing shifts
 * zeros.
 *
 * ⚠⚠ MEASURED 2026-09-28, AND THE ANSWER IS THAT A ZERO PAYLOAD IS
 * DESTRUCTIVE. The experiment was run on the lab 7150S, twice, with a cold
 * boot in between:
 *
 *	--ssched	ring initialises, does not circulate, chip answering
 *	--mrl		all 6288 shifts retire, chip answering
 *	--ssched	SAME COMMAND TAKES THE CHIP OFF THE BUS
 *
 * So the sequence reaches something the scheduler depends on -- which is the
 * coupling the prior work guessed at and had never shown -- but it moves it
 * the wrong way. The most economical reading is that Table 4-1 step 9 has
 * already installed the real bank repairs from the fusebox, and shifting
 * zeros overwrites them, after which the first access to a repaired bank
 * raises an uncorrectable ECC error and the chip escalates to fatal.
 *
 * The payload is therefore load-bearing and we cannot reach the scheduler by
 * running the sequence empty. Do not call this with a NULL payload expecting
 * a no-op. It is not one, and recovery needs a reset pulse and a full reboot
 * of the chip.
 */

/*
 * Chain lengths, in 32-bit words. These are geometry -- how many bits of shift
 * register sit behind each selector on this die -- so they are the same for
 * every FM6000 and are not anybody's data. Measured from the vendor sequence,
 * which shifts exactly this many words before moving on. [RE]
 */
#define FM_MRL_BANK_WORDS	5800u	/* chain FM6000_SCAN_CHAIN_BANKS */
#define FM_MRL_CORE_WORDS	203u	/* chain FM6000_SCAN_CHAIN_CORE */

struct fm_mrl_payload {
	/* NULL means "shift zeros", which is the redistributable default and
	 * the only payload NOSaic ships. A caller doing bring-up experiments
	 * may point these at its own arrays. */
	const uint32_t *bank;	/* FM_MRL_BANK_WORDS words, or NULL */
	const uint32_t *core;	/* FM_MRL_CORE_WORDS words, or NULL */
};

struct fm_mrl_report {
	unsigned shifted;	/* words successfully shifted */
	unsigned stalled;	/* shifts whose status was not RETIRED */
	uint32_t first_bad;	/* the first status word that was not RETIRED */
	unsigned first_bad_at;	/* which shift it was */
	int	 offbus;	/* the chip left the bus part way through */
};

/*
 * Run the sequence. Returns FM_OK if every word retired.
 *
 * ⚠ ORDER. This must run after boot.c has finished Table 4-1 -- in particular
 * after step 9, because a repair sequence that runs before the boot
 * controller's own repair pass is configuring memories the controller is
 * about to reconfigure. The vendor's own call site is after the boot commands.
 *
 * ⚠ The vendor gates this on a chip-revision check and skips it on every
 * revision but one. We do not know which revision ours is, and we do not know
 * what the sequence does to a part that does not need it. Run it deliberately,
 * not from the boot path, until that is settled.
 */
int fm_mrl_apply(struct fm6000 *d, const struct fm_mrl_payload *p,
		 struct fm_mrl_report *rep);

void fm_mrl_print(const struct fm_mrl_report *rep, int rv);

#endif /* NOSAIC_FM6000_MRL_H */
