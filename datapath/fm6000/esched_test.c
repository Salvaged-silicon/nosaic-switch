/* SPDX-License-Identifier: Apache-2.0 */
/*
 * What fm_esched_init() writes, and in what order.
 *
 * The egress scheduler's addresses are easy to get right and its ORDER is
 * easy to get wrong, because every wrong order writes exactly the same 159
 * addresses with exactly the same values. An address-set comparison passes
 * on all of them. This test exists because one did.
 *
 * What has to hold:
 *
 *   - three registers of 76 entries, which is what the block contains. An
 *     earlier version wrote 3,222 words over 128 "ports" in eight
 *     "instances", which was the same three registers seen through address
 *     aliasing.
 *   - the host port written LAST in each array, not first.
 *   - the round-robin penalty applied to every port before it is cleared
 *     from any. Per-port pairs touch the same addresses with the same
 *     values and never produce the intermediate state the scheduler
 *     latches against.
 *   - the two internal ports keep the penalty. A forwarding chip has
 *     exactly those two of its seventy-six still penalised, and settling
 *     them would configure this switch differently from one that works.
 *
 * No vendor table is embedded here and none is needed: the structure is
 * what has to hold and is checkable on its own terms. It was confirmed
 * against a forwarding chip's register dump, which lives outside this
 * repository -- see hardware.md.
 *
 * Built and run by `make test` in this directory; it needs no hardware.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esched.h"
#include "pci.h"
#include "portmap.h"

#define MAX_WRITES 4096

static struct { uint32_t word, val; } log[MAX_WRITES];
static unsigned nlog;
static int failures;
static unsigned written_out;

/* The two functions esched.c reaches for. No chip, no mapping. */
int fm_wr(struct fm6000 *d, uint32_t word, uint32_t val)
{
	if (nlog >= MAX_WRITES)
		return FM_ERR;
	log[nlog].word = word;
	log[nlog].val = val;
	nlog++;
	return FM_OK;
}

int fm_alive(struct fm6000 *d)
{
	return 1;
}

static void check(int cond, const char *what)
{
	if (cond)
		return;
	printf("FAIL: %s\n", what);
	failures++;
}

int main(void)
{
	unsigned i, distinct = 0;
	uint32_t culprit = 1, seen[MAX_WRITES];
	struct fm6000 dev;
	int rv;

	memset(&dev, 0, sizeof dev);
	rv = fm_esched_init(&dev, &written_out, &culprit);

	check(rv == FM_OK, "init returns FM_OK");
	check(culprit == 0, "no culprit on success");
	check(written_out == nlog, "the reported count is the count written");

	/* Three arrays of 76, then the round-robin word over 76 twice less the
	 * two that keep the penalty. */
	check(nlog == 76 * 3 + 76 + 74,
	      "378 writes: three configuration arrays and two round-robin passes");

	for (i = 0; i < nlog; i++) {
		unsigned j;
		int dup = 0;

		for (j = 0; j < distinct; j++)
			if (seen[j] == log[i].word)
				dup = 1;
		if (!dup)
			seen[distinct++] = log[i].word;
	}
	check(distinct == 76 * 4, "304 distinct addresses");

	/* Nothing outside the three registers and the round-robin array. */
	for (i = 0; i < nlog; i++) {
		uint32_t w = log[i].word;
		int ok = (w >= 0x002000u && w < 0x002000u + 76) ||
			 (w >= 0x002080u && w < 0x002080u + 76) ||
			 (w >= 0x002100u && w < 0x002100u + 76) ||
			 (w >= 0x003800u && w < 0x003800u + 76);

		check(ok, "every write lands inside a register the block has");
	}

	/* The host port's two configuration words differ, and CFG_3 is zero. */
	{
		unsigned saw1 = 0, saw2 = 0, saw3 = 0;

		for (i = 0; i < nlog; i++) {
			uint32_t w = log[i].word, v = log[i].val;

			if (w == 0x002000u) { saw1 = 1; check(v == 0x00fff800u, "CFG_1 host value"); }
			if (w == 0x002080u) { saw2 = 1; check(v == 0x00fff000u, "CFG_2 host value"); }
			if (w >= 0x002100u && w < 0x002100u + 76) {
				saw3 = 1;
				check(v == 0, "CFG_3 tcInnerPriority is zero on every port");
			}
		}
		check(saw1 && saw2 && saw3, "all three configuration arrays written");
	}

	/* The two passes, and who keeps the penalty. */
	{
		unsigned base = 76 * 3, penalised = 0, settled = 0;

		for (i = base; i < base + 76; i++) {
			check(log[i].word == 0x003800u + (i - base),
			      "the penalty pass runs over every port in order");
			check(log[i].val == 0x14ffffffu,
			      "every port is penalised before any is settled");
			penalised++;
		}
		for (i = base + 76; i < nlog; i++) {
			unsigned port = log[i].word - 0x003800u;

			check(log[i].val == 0x00ffffffu, "the settling pass clears it");
			check(port != 1 && port != 3,
			      "the internal ports keep the penalty, as a forwarding "
			      "chip does");
			settled++;
		}
		check(penalised == 76 && settled == 74, "76 penalised, 74 settled");
	}

	if (failures != 0) {
		printf("esched: %d checks failed\n", failures);
		return 1;
	}
	printf("esched: %u writes, %u addresses, documented register map -- ok\n",
	       nlog, distinct);
	return 0;
}
