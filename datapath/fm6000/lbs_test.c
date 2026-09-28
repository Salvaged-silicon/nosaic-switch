/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Loopback suppression, and the one entry that is deliberately not a port's
 * own GLORT.
 *
 * Getting this wrong does not fail loudly. Too narrow a match and a frame
 * goes back out of the port it came in on; too broad -- which is what an
 * entry of GLORT 0 would be -- and flooding to the CPU stops. Neither shows
 * up as an error anywhere.
 */
#include <stdio.h>
#include <string.h>

#include "lbs.h"
#include "pci.h"
#include "portmap.h"

#define MAX_WRITES 128
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

int main(void)
{
	unsigned written = 0, i, seen_host = 0;
	struct fm6000 dev;

	memset(&dev, 0, sizeof dev);
	check(fm_lbs_init(&dev, &written) == FM_OK, "init returns FM_OK");

	/* The 52 front-panel ports plus the host port and the two internal
	 * ones: every port that can source a frame, and no others. */
	check(nlog == FM6000_FRONT_PORTS + 3,
	      "one entry per port that carries traffic, and no more");
	check(written == nlog, "the reported count is the count written");

	for (i = 0; i < nlog; i++) {
		unsigned port = log[i].word - 0x014000u;
		uint32_t v = log[i].val;
		unsigned x = (v >> 16) & 0xffff, mask = v & 0xffff;

		check(mask == ((~x) & 0xffff),
		      "the low half is the complement of the high half, so the "
		      "match is exact");

		if (port == FM6000_ALTA_HOST) {
			seen_host = 1;
			/* ⚠ Not its own GLORT. The host port's GLORT is 0 and
			 * an entry matching 0 would suppress flooding to the
			 * CPU entirely. */
			check(x == 0xff00,
			      "the host port matches a GLORT no frame carries");
			check(x != fm6000_glort_of(FM6000_ALTA_HOST),
			      "and that is deliberately not its own GLORT");
			continue;
		}
		check(x == fm6000_glort_of(port),
		      "every other port matches its own GLORT");
		check(x != 0, "a port with no GLORT should not have an entry");
	}
	check(seen_host, "the host port has an entry");

	if (failures != 0) {
		printf("lbs: %d checks failed\n", failures);
		return 1;
	}
	printf("lbs: %u entries, exact GLORT match per port -- ok\n", nlog);
	return 0;
}
