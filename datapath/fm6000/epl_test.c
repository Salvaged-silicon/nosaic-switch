/* SPDX-License-Identifier: Apache-2.0 */
/*
 * EPL geometry, against the datasheet rather than against the hardware.
 *
 * These two tables are facts about the part, published in Intel document
 * 331496-002, and neither can be checked on this switch: every port brought
 * up so far is on EPL 14 or EPL 16, which are straight-through and share a
 * reference clock. So a wrong entry for any other EPL would pass every
 * hardware test there is and show up later as a port that configures
 * cleanly and never links.
 *
 * That is what this is for. It is a transcription check of two small
 * tables, and the cheapest possible guard on the one part of the EPL model
 * the bench cannot reach.
 */
#include <stdint.h>
#include <stdio.h>

#include "regs.h"

static int failures;

static void eq(unsigned got, unsigned want, const char *what, unsigned arg)
{
	if (got == want)
		return;
	printf("FAIL: %s for EPL %u: got %u, want %u\n", what, arg, got, want);
	failures++;
}

int main(void)
{
	/* Datasheet Table 6-2: lanes {A,B,C,D} reach channels {3,2,1,0}. */
	static const unsigned reversed[] = { 1, 2, 4, 8, 12, 13, 17, 20, 22, 24 };
	/* Datasheet Table 6-7: one 156.25 MHz input per group of six EPLs. */
	static const unsigned refclk[25] = {
		0,
		1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2,
		3, 4, 3, 4, 3, 4, 3, 4, 3, 4, 3, 4,
	};
	unsigned epl, i;

	for (epl = 1; epl <= 24; epl++) {
		unsigned want = 0;

		for (i = 0; i < sizeof reversed / sizeof reversed[0]; i++)
			if (reversed[i] == epl)
				want = 1;
		eq((unsigned)fm6000_epl_lane_reversed(epl), want,
		   "lane reversal", epl);
		eq(fm6000_epl_refclk(epl), refclk[epl], "reference clock", epl);
	}

	/* And the mapping the reversal produces, in both directions. */
	eq(fm6000_epl_channel(1, 0), 3, "reversed lane A -> channel", 1);
	eq(fm6000_epl_channel(1, 1), 2, "reversed lane B -> channel", 1);
	eq(fm6000_epl_channel(1, 2), 1, "reversed lane C -> channel", 1);
	eq(fm6000_epl_channel(1, 3), 0, "reversed lane D -> channel", 1);
	for (i = 0; i < 4; i++)
		eq(fm6000_epl_channel(14, i), i, "straight lane -> channel", 14);

	/* The blind spot this exists to make visible: the only EPLs the bench
	 * has ever driven are on one of the four reference clocks. */
	eq(fm6000_epl_refclk(14), fm6000_epl_refclk(16),
	   "EPL 14 and 16 share a reference clock, so the bench has only "
	   "ever tested one of the four", 14);

	if (failures != 0) {
		printf("epl: %d checks failed\n", failures);
		return 1;
	}
	printf("epl: lane reversal and reference clocks match the datasheet -- ok\n");
	return 0;
}
