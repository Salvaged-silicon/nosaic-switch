/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The store-and-forward matrix.
 *
 * EOS builds this incrementally -- 34,668 register writes over 111 passes of
 * its port loop, OR-ing one port's bit into a mask at a time. The end state is
 * four distinct three-word patterns spread over the port set, so it can simply
 * be written: **168 writes instead of 34,668**, and the result is
 * indistinguishable from the accumulation.
 *
 * That is the shape every block on this chip should end up in, and it is why
 * a captured sequence is worth understanding rather than replaying: the
 * capture records a loop's 34,668 intermediate steps, and what the hardware
 * actually needs is the answer.
 *
 * PROVENANCE. Ported from EdgeNOS's fm6000_safinit.c -- our own prior work on
 * this chassis, cold-boot validated there on 2026-08-07 (link 0x8c0 -> 0xcc0,
 * OSPF adjacency, routes programmed into silicon). The port set comes from
 * this board's own table in portmap.h, not from the trace.
 */
#include <string.h>

#include "pci.h"
#include "portmap.h"
#include "regs.h"
#include "saf.h"

/* One three-word mask per physical port. */
#define SAF_BASE        0x0a0000
#define SAF_ENTRY(port) (SAF_BASE + 4u * (unsigned)(port))
#define SAF_PORTS       128

/* The four end-state patterns, by the role a port plays. */
static const uint32_t pat_port[3] = { 0x0010000f, 0x00000100, 0x00000000 };
static const uint32_t pat_edge[3] = { 0x00000007, 0x00000000, 0x00000000 };
static const uint32_t pat_all[3]  = { 0xffffffff, 0xffffffff, 0xffffffff };
static const uint32_t pat_cpu[3]  = { 0xffffffff, 0xffffffff, 0x0003ffff };

/* Three physical ports carry the edge pattern rather than the port one. Why
 * these three is not established; they are what the end state has. */
static int is_edge(unsigned alta)
{
	return alta == 3 || alta == 20 || alta == 40;
}

int fm_saf_init(struct fm6000 *d, unsigned *written)
{
	const uint32_t *pat[SAF_PORTS];
	unsigned fp, i;
	int rv;

	if (written != NULL)
		*written = 0;
	memset(pat, 0, sizeof(pat));

	/* Every front-panel port, including the uplink group at 49-52. */
	for (fp = 1; fp <= FM6000_FRONT_PORTS; fp++) {
		unsigned alta = fm6000_alta_of[fp];

		pat[alta] = is_edge(alta) ? pat_edge : pat_port;
	}

	pat[FM6000_ALTA_INTERNAL] = is_edge(FM6000_ALTA_INTERNAL) ? pat_edge : pat_port;
	pat[FM6000_ALTA_CPU] = pat_cpu;

	/* 0 and 2 are not swept by the vendor's loop but do carry an
	 * all-ports mask in the end state. */
	pat[0] = pat_all;
	pat[2] = pat_all;

	/*
	 * Ascending address order, which is the order the cold-boot-validated
	 * sequence used. Ordering within this block has not been shown to be
	 * irrelevant, so it is not varied for tidiness.
	 */
	for (i = 0; i < SAF_PORTS; i++) {
		unsigned k;

		if (pat[i] == NULL)
			continue;
		for (k = 0; k < 3; k++) {
			rv = fm_wr(d, SAF_ENTRY(i) + k, pat[i][k]);
			if (rv != FM_OK)
				return rv;
			if (written != NULL)
				(*written)++;
		}
	}

	/*
	 * Read one of them back.
	 *
	 * ⚠ A WRITE THAT RETURNS OK IS NOT A WRITE THAT LANDED. Two tables in
	 * the congestion-management block on this chip accept 2,560 words and
	 * keep none of them, silently, while the tables beside them store
	 * theirs -- so "168 writes, ok" says nothing on its own about whether
	 * the store-and-forward matrix is configured.
	 *
	 * ⚠ THE WITNESS HAS TO BE A DISTINCTIVE VALUE. The obvious choice, the
	 * CPU port's entry, is 0xffffffff -- which is also what an untouched
	 * register reads, so a block that discarded every write would pass. A
	 * plain front-panel port's first word is 0x0010000f and nothing else
	 * produces that by accident.
	 */
	{
		unsigned witness = 0;
		uint32_t got = 0;

		for (fp = 1; fp <= FM6000_FRONT_PORTS; fp++)
			if (!is_edge(fm6000_alta_of[fp])) {
				witness = fm6000_alta_of[fp];
				break;
			}
		if (witness != 0) {
			rv = fm_rd(d, SAF_ENTRY(witness), &got);
			if (rv != FM_OK)
				return rv;
			if (got != pat_port[0])
				return FM_EUNSAFE;
		}
	}
	return FM_OK;
}
