/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The parser's per-port seed, and the GLORT assignment underneath it.
 *
 * A GLORT is how the forwarding path names a port, so getting it wrong does
 * not fail loudly -- it sends frames to the wrong place, or nowhere. The
 * assignment is simple enough to state in one line (a port's GLORT is its
 * logical port number) and that is exactly why it is worth pinning: a
 * one-line rule is easy to "tidy" into a different one.
 *
 * No vendor table is embedded. The rule was confirmed against a forwarding
 * chip's parser seeds, which live outside this repository.
 */
#include <stdio.h>
#include <string.h>

#include "parser.h"
#include "pci.h"
#include "portmap.h"

#define MAX_WRITES 512
static struct { uint32_t word, val; } log[MAX_WRITES];
static unsigned nlog;
static int failures;

int fm_wr(struct fm6000 *d, uint32_t word, uint32_t val)
{
	if (nlog >= MAX_WRITES)
		return FM_ERR;
	log[nlog].word = word;
	log[nlog].val = val;
	nlog++;
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

static uint32_t at(uint32_t word)
{
	unsigned i;

	for (i = 0; i < nlog; i++)
		if (log[i].word == word)
			return log[i].val;
	printf("FAIL: word 0x%06x was never written\n", word);
	failures++;
	return 0xdeadbeef;
}

int main(void)
{
	unsigned written = 0, fp, port;
	struct fm6000 dev;

	memset(&dev, 0, sizeof dev);
	check(fm_parser_fields_init(&dev, &written) == FM_OK, "init returns FM_OK");

	/* The whole array: 76 ports, two entries, two words. Leaving any of it
	 * unwritten leaves a stale seed the parser would act on. */
	check(nlog == 76 * 2 * 2, "the whole seed array is written, 304 words");
	check(written == nlog, "the reported count is the count written");

	/* A front-panel port's GLORT is its panel number. */
	for (fp = 1; fp <= FM6000_FRONT_PORTS; fp++) {
		unsigned alta = fm6000_alta_of[fp];
		uint32_t base = 0x108200u + alta * 4u;

		check(fm6000_glort_of(alta) == fp,
		      "a front-panel port's GLORT is its panel number");
		check(at(base) == (0x00010000u | 0x100u | fp),
		      "word 0 is flags, then 0x100 | GLORT");
		check(at(base + 1) == (((uint32_t)fp << 16) | 1u),
		      "word 1 is GLORT in the high half");
	}

	/* The internal ports continue that numbering, and carry no 0x100|glort. */
	check(fm6000_glort_of(FM6000_ALTA_CPU) == 54, "physical 1 is GLORT 54");
	check(fm6000_glort_of(FM6000_ALTA_INTERNAL) == 53, "physical 3 is GLORT 53");
	check(fm6000_glort_of(FM6000_ALTA_HOST) == 0, "the host port is GLORT 0");
	check(at(0x108200u + FM6000_ALTA_CPU * 4u) == 0x00010000u,
	      "an internal port has no 0x100 | GLORT in its low half");
	check(at(0x108200u + FM6000_ALTA_CPU * 4u + 1) == ((54u << 16) | 1u),
	      "an internal port still carries its GLORT");

	/* A port that carries no traffic is zeroed, and so is entry 1. */
	for (port = 0; port < 76; port++) {
		uint32_t e0 = 0x108200u + port * 4u;

		if (fm6000_glort_of(port) == 0 && port != FM6000_ALTA_HOST) {
			check(at(e0) == 0 && at(e0 + 1) == 0,
			      "a port with no GLORT is left at zero");
		}
		check(at(e0 + 2) == 0 && at(e0 + 3) == 0,
		      "entry 1 is zero on every port");
	}

	if (failures != 0) {
		printf("parser: %d checks failed\n", failures);
		return 1;
	}
	printf("parser: %u words, GLORT = logical port number -- ok\n", nlog);
	return 0;
}
