/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The scan-chain sequence's shape.
 *
 * This test exists because the sequence is the only part of the vendor's MRL
 * fix we have, and a sequence is exactly the kind of thing that gets quietly
 * broken by a later edit: one write moved out of a loop, one selector hoisted
 * because it looked redundant, one count off by one at a chain boundary. None
 * of that fails on a build host, and on the switch it fails as "the scheduler
 * still does not circulate", which is indistinguishable from the bug we are
 * actually chasing.
 *
 * So the totals are pinned here. The vendor sequence is 6287 shifts split
 * 35 / 5800 / 203 / 249 between prologue, bank chain, core chain and tail,
 * plus one stop word. If our generator stops matching that, this says so.
 */
#include <stdio.h>
#include <string.h>

#include "mrl.h"
#include "pci.h"
#include "regs.h"

#define MAX_SHIFTS 16384
static struct { uint32_t word, val; } log[MAX_SHIFTS];
static unsigned nlog;
static int failures;

int fm_wr(struct fm6000 *d, uint32_t word, uint32_t val)
{
	if (nlog >= MAX_SHIFTS)
		return FM_ERR;
	log[nlog].word = word;
	log[nlog].val = val;
	nlog++;
	return FM_OK;
}

/* Every shift polls the status, and the engine is modelled as always
 * retiring -- this test is about what we emit, not about what the chip says. */
int fm_rd(struct fm6000 *d, uint32_t word, uint32_t *out)
{
	*out = (word == FM6000_SCAN_STATUS) ? FM6000_SCAN_STATUS_RETIRED : 0;
	return FM_OK;
}

int fm_alive(struct fm6000 *d) { return 1; }

static void check(int cond, const char *what)
{
	if (cond)
		return;
	printf("FAIL: %s\n", what);
	failures++;
}

/* Count writes to one address, optionally only those whose selector was a
 * given chain. The log is a flat write stream, so a shift is a selector write
 * followed by a data write. */
static unsigned count_to(uint32_t addr, int chain)
{
	unsigned i, n = 0, sel = 0;

	for (i = 0; i < nlog; i++) {
		if (log[i].word == FM6000_SCAN_CONTROL) {
			sel = log[i].val;
			continue;
		}
		if (log[i].word == addr && (chain < 0 || sel == (unsigned)chain))
			n++;
	}
	return n;
}

int main(void)
{
	struct fm_mrl_report rep;
	struct fm6000 dev;
	unsigned i, shifts, cfg, chain;

	memset(&dev, 0, sizeof dev);
	check(fm_mrl_apply(&dev, NULL, &rep) == FM_OK, "a clean run returns FM_OK");
	check(rep.stalled == 0, "nothing stalls when the engine always retires");
	check(rep.offbus == 0, "the chip stays on the bus");

	cfg   = count_to(FM6000_SCAN_CONFIG_DATA_IN, -1);
	chain = count_to(FM6000_SCAN_CHAIN_DATA_IN, -1);
	shifts = cfg + chain;

	/* 6287 table entries plus the trailing stop word the vendor writes
	 * after its loop. */
	check(shifts == 6288, "6288 shifts in total");
	check(rep.shifted == shifts, "the report counts every shift");

	/* Prologue 35 + tail 246 pumps + the stop word, all to the config
	 * port; 5800 + 203 payload/terminator words to the chain port. */
	check(cfg == 35 + 246 + 1, "282 words to SCAN_CONFIG_DATA_IN");
	check(chain == FM_MRL_BANK_WORDS + FM_MRL_CORE_WORDS + 3,
	      "6006 words to SCAN_CHAIN_DATA_IN, three of them tail terminators");

	check(count_to(FM6000_SCAN_CHAIN_DATA_IN, FM6000_SCAN_CHAIN_BANKS)
	      == FM_MRL_BANK_WORDS, "the bank chain gets exactly its length");

	/* The selector is rewritten for every shift. If someone hoists it out
	 * of a loop as an optimisation this is the test that objects. */
	check(count_to(FM6000_SCAN_CONTROL, -1) == 0, "selector writes are not data writes");
	for (i = 0; i + 1 < nlog; i += 2)
		if (log[i].word != FM6000_SCAN_CONTROL) {
			check(0, "every data write is preceded by a selector write");
			break;
		}

	/* A NULL payload shifts zeros -- the redistributable default. If this
	 * ever stops being true, somebody has put vendor data in the tree. */
	{
		unsigned nonzero = 0, sel = 0;

		for (i = 0; i < nlog; i++) {
			if (log[i].word == FM6000_SCAN_CONTROL) {
				sel = log[i].val;
				continue;
			}
			if (log[i].word == FM6000_SCAN_CHAIN_DATA_IN &&
			    sel == FM6000_SCAN_CHAIN_BANKS && log[i].val != 0)
				nonzero++;
		}
		check(nonzero == 0, "a NULL payload puts nothing but zeros on the bank chain");
	}

	printf("%s: %u shifts, %u config, %u chain\n",
	       failures ? "FAILED" : "ok", shifts, cfg, chain);
	return failures ? 1 : 0;
}
