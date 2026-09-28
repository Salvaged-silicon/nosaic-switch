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
 *   - eight instances of four arrays, and array 2 is never written. A
 *     forwarding chip has it zero in all eight.
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
	unsigned i, distinct = 0, seen_arr2 = 0;
	uint32_t culprit = 1, seen[MAX_WRITES];
	struct fm6000 dev;
	int rv;

	memset(&dev, 0, sizeof dev);
	rv = fm_esched_init(&dev, &written_out, &culprit);

	check(rv == FM_OK, "init returns FM_OK");
	check(culprit == 0, "no culprit on success");
	check(written_out == nlog, "the reported count is the count written");

	/* 8 instances x 3 arrays x 128 ports, then the round-robin word over
	 * 76 ports twice, less the two that keep the penalty. */
	check(nlog == 8 * 3 * 128 + 76 + 74,
	      "3222 writes: the configuration arrays, then two round-robin passes");

	for (i = 0; i < nlog; i++) {
		unsigned j;
		int dup = 0;

		for (j = 0; j < distinct; j++)
			if (seen[j] == log[i].word)
				dup = 1;
		if (!dup)
			seen[distinct++] = log[i].word;
	}
	check(distinct == 8 * 3 * 128 + 76,
	      "3148 distinct addresses: only the round-robin word repeats");

	/* Array 2 is never touched, in any instance. */
	for (i = 0; i < nlog; i++) {
		uint32_t w = log[i].word;

		if (w >= 0x003800u)
			continue;
		if (((w - 0x002000u) % 0x200u) / 0x80u == 2)
			seen_arr2++;
	}
	check(seen_arr2 == 0, "array 2 is left at zero in every instance");

	/* Every configuration word is one of the three legal values, and the
	 * host port's is the one that differs. */
	for (i = 0; i < nlog; i++) {
		uint32_t w = log[i].word, v = log[i].val;
		unsigned arr, port;

		if (w >= 0x003800u)
			continue;
		arr = ((w - 0x002000u) % 0x200u) / 0x80u;
		port = w & 0x7fu;
		if (port != 0) {
			check(v == 0x00ffffffu, "a front-panel word is all classes");
			continue;
		}
		check(v == (arr == 1 ? 0x00fff000u : 0x00fff800u),
		      "the host port's word differs, and differs by array");
	}

	/* The host port comes last in each array: the write before the next
	 * array starts. */
	{
		unsigned arrays_seen = 0;

		for (i = 0; i + 1 < nlog; i++) {
			uint32_t w = log[i].word, nx = log[i + 1].word;

			if (w >= 0x003800u)
				break;
			/* last write of an array = next write is a different array */
			if ((w & ~0x7fu) != (nx & ~0x7fu)) {
				check((w & 0x7fu) == 0,
				      "the host port is the last write of its array");
				arrays_seen++;
			}
		}
		/* One boundary per array: 23 between arrays, plus the last
		 * array's end, which is where the round-robin word begins. */
		check(arrays_seen == 8 * 3,
		      "every array boundary was preceded by the host port");
	}

	/* The two round-robin passes, and who keeps the penalty. */
	{
		unsigned base = 8 * 3 * 128;
		unsigned penalised = 0, settled = 0;

		for (i = base; i < base + 76; i++) {
			check(log[i].word == 0x003800u + (i - base),
			      "the penalty pass runs over every switch port in order");
			check(log[i].val == 0x14ffffffu,
			      "every port is penalised before any is settled");
			penalised++;
		}
		for (i = base + 76; i < nlog; i++) {
			unsigned port = log[i].word - 0x003800u;

			check(log[i].val == 0x00ffffffu, "the settling pass clears it");
			check(port != 1 && port != 3,
			      "the internal ports are not settled: a forwarding chip "
			      "keeps the penalty on exactly those two");
			settled++;
		}
		check(penalised == 76, "all 76 switch ports are penalised");
		check(settled == 74, "74 are settled, leaving the two internal ports");
	}

	if (failures != 0) {
		printf("esched: %d checks failed\n", failures);
		return 1;
	}
	printf("esched: %u writes, %u addresses, structure as specified -- ok\n",
	       nlog, distinct);
	return 0;
}
