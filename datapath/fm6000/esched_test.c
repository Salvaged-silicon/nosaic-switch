/* SPDX-License-Identifier: Apache-2.0 */
/*
 * What fm_esched_init() writes, and in what order.
 *
 * The egress scheduler's addresses are easy to get right and its ORDER is
 * easy to get wrong, because every wrong order writes exactly the same 159
 * addresses with exactly the same values. An address-set comparison passes
 * on all of them. This test exists because one did.
 *
 * Three orderings matter and none of them is arbitrary:
 *
 *   - physical port ascending, not front-panel ascending. The map is not
 *     monotonic, so the two differ.
 *   - the CPU port's configuration after every front-panel port's, not
 *     before.
 *   - the round-robin penalty applied to every port before it is cleared
 *     from any. Per-port pairs touch the same addresses with the same
 *     values and never produce the intermediate state the scheduler
 *     latches against.
 *
 * No vendor table is embedded here, and none is needed: the structure is
 * what has to hold, and the structure is checkable on its own terms.
 *
 * Built and run by `make test` in this directory; it needs no hardware.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esched.h"
#include "pci.h"
#include "portmap.h"

#define MAX_WRITES 512

static struct { uint32_t word, val; } log[MAX_WRITES];
static unsigned nlog;
static int failures;

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
	unsigned written = 0, i, distinct = 0, sorted[FM6000_FRONT_PORTS];
	uint32_t culprit = 1, seen[MAX_WRITES];
	struct fm6000 dev;
	int rv;

	memset(&dev, 0, sizeof dev);
	rv = fm_esched_init(&dev, &written, &culprit);

	check(rv == FM_OK, "init returns FM_OK");
	check(culprit == 0, "no culprit on success");

	/* 52 front ports and the CPU port: two configuration words each, and
	 * a round-robin word written in two passes. */
	check(written == nlog, "the reported count is the count written");
	check(nlog == (FM6000_FRONT_PORTS + 1) * 4,
	      "212 writes: two config words and two round-robin passes per port");

	for (i = 0; i < nlog; i++) {
		unsigned j;
		int dup = 0;

		for (j = 0; j < distinct; j++)
			if (seen[j] == log[i].word)
				dup = 1;
		if (!dup)
			seen[distinct++] = log[i].word;
	}
	check(distinct == (FM6000_FRONT_PORTS + 1) * 3,
	      "159 distinct addresses: only the round-robin word repeats");

	/* The front-panel ports, by physical number. */
	for (i = 0; i < FM6000_FRONT_PORTS; i++)
		sorted[i] = fm6000_alta_of[i + 1];
	for (i = 1; i < FM6000_FRONT_PORTS; i++) {
		unsigned k = i, v = sorted[i];

		while (k > 0 && sorted[k - 1] > v) {
			sorted[k] = sorted[k - 1];
			k--;
		}
		sorted[k] = v;
	}

	/* Section 1: CFG_1 for every front-panel port, physical ascending. */
	for (i = 0; i < FM6000_FRONT_PORTS; i++) {
		check(log[i].word == 0x002000u + sorted[i],
		      "CFG_1 runs over the front-panel ports in physical order");
		check(log[i].val == 0x00ffffffu, "front-panel CFG_1 is all classes");
	}
	/* Section 2: CFG_2, same order. */
	for (i = 0; i < FM6000_FRONT_PORTS; i++) {
		unsigned k = FM6000_FRONT_PORTS + i;

		check(log[k].word == 0x002080u + sorted[i],
		      "CFG_2 follows, in the same order");
		check(log[k].val == 0x00ffffffu, "front-panel CFG_2 is all classes");
	}
	/* Section 3: the CPU port, last and different. */
	{
		unsigned k = 2 * FM6000_FRONT_PORTS;

		check(log[k].word == 0x002000u, "the CPU port's CFG_1 comes after every front-panel port");
		check(log[k].val == 0x00fff800u, "the CPU port's CFG_1 is not all classes");
		check(log[k + 1].word == 0x002080u, "the CPU port's CFG_2 follows it");
		check(log[k + 1].val == 0x00fff000u, "the CPU port's CFG_2 is not all classes");
	}
	/* Sections 4 and 5: the round-robin word, penalised then settled. */
	{
		unsigned base = 2 * FM6000_FRONT_PORTS + 2;
		unsigned span = FM6000_FRONT_PORTS + 1;

		check(log[base].word == 0x003800u, "the penalty pass starts at the CPU port");
		for (i = 0; i < span; i++) {
			check(log[base + i].val == 0x14ffffffu,
			      "every port is penalised before any is settled");
			check(log[base + span + i].val == 0x00ffffffu,
			      "the settling pass comes after the whole penalty pass");
			check(log[base + i].word == log[base + span + i].word,
			      "the two passes visit the ports in the same order");
		}
	}

	if (failures != 0) {
		printf("esched: %d checks failed\n", failures);
		return 1;
	}
	printf("esched: %u writes, %u addresses, order as specified -- ok\n",
	       nlog, distinct);
	return 0;
}
