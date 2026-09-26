/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The built-in memory BIST and repair march.
 *
 * This configures the chip's memory CONTROLLERS and runs the defect/repair
 * march over them. It is not the same thing as Table 4-1 step 9, which asks
 * the boot controller to apply bank repairs: that command uses the fusebox's
 * recorded repairs, and this establishes the controllers those memories are
 * reached through in the first place.
 *
 * ⚠ WITHOUT IT, LATER MEMORY FILLS TAKE THE CHIP OFF THE BUS. Measured here:
 * a vendor boot capture replayed onto a chip that has had Table 4-1 and
 * nothing else dies 164 words into a 304-word zero-fill at 0x145e00, having
 * completed an identical fill at 0x145c00 immediately before. The prior
 * investigation found the same and named the cause: the fills off-bus without
 * the memory-controller configuration this routine programs.
 *
 * PROVENANCE. Ported from EdgeNOS's fm6000_bist.c -- our own prior work on
 * this chassis, an independent reimplementation whose behaviour was derived
 * by disassembling the vendor SDK for interoperability and which contains no
 * third-party code. Apache-2.0 here by the copyright holder's decision.
 *
 * ⚠⚠ THE MEMORY-CONTROLLER WRITES MUST BE PACED, AND THIS ONE HANGS THE HOST
 * RATHER THAN THE CHIP. Writing the 0x1d200-0x1d6ff block back to back hard
 * hangs the machine doing the writing -- not an off-bus chip that a reset
 * pulse recovers, but a box that needs its power cycled. Pacing is therefore
 * ON by default here, unlike in the original, where it defaulted to none and
 * was turned on by an environment variable. A default that can wedge the host
 * is not a default.
 */
#ifndef NOSAIC_FM6000_BIST_H
#define NOSAIC_FM6000_BIST_H

#include "pci.h"

struct fm_bist_report {
	int configured;		/* controller config completed */
	int marched;		/* the march ran and completed */
	unsigned march_ms;	/* how long it took */
	unsigned defects;	/* non-zero result registers */
	uint32_t status;	/* BM_ENGINE_STATUS at the end */
};

/*
 * Configure the memory controllers and run the march.
 *
 * `pace_us` is the delay after each controller write; 0 asks for the default,
 * which is not zero. See the warning above before considering otherwise.
 */
int fm_bist_memory_init(struct fm6000 *d, unsigned pace_us,
			struct fm_bist_report *rep);

/* Configure the controllers and stop, without running the march. The march is
 * the part that takes seconds and the part that can fail; separating them
 * makes it possible to find out whether the configuration alone is what later
 * fills need. */
int fm_bist_configure_only(struct fm6000 *d, unsigned pace_us,
			   struct fm_bist_report *rep);

#endif /* NOSAIC_FM6000_BIST_H */
