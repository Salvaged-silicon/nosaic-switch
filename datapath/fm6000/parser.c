/* SPDX-License-Identifier: Apache-2.0 */
/*
 * PARSER_INIT_FIELDS -- the parser's per-port seed.
 *
 * One register at 0x108200, shaped [2 entries] x [76 ports], two words per
 * entry, so a word is at
 *
 *     0x108200 + port * 4 + entry * 2 + word
 *
 * Entry 0 is the live one. Its 64 bits are the port's source GLORT in
 * [63:48], configured flags in [47:16], and 0x100 | GLORT in [15:0]. Entry 1
 * is unused and is zero on every port.
 *
 * Entry 0 carries the port's GLORT -- its logical port number, which is its
 * panel number for a front-panel port and 53 or 54 for the two internal
 * ones. See portmap.h for why that assignment is ours rather than borrowed.
 * The layout is
 *
 *     word 0   flags << 16 | 0x100 | glort     (0x100|glort is zero on an
 *                                               internal port)
 *     word 1   glort << 16 | 1
 *
 * with flags 0x0001 on a port that has not come up. A chip that is
 * forwarding carries something else there on a port whose link is up, which
 * is link state rather than seed, so it is not written here.
 *
 * A port that carries no traffic is given zero, and so is the unused second
 * entry of every port. That is not tidiness: it guarantees no port which is
 * not forwarding, and no second entry anywhere, holds a stale seed the
 * parser would act on.
 *
 * PROVENANCE. Ported from EdgeNOS's fm6000_parserfields.c -- our own prior
 * work on this chassis -- and relicensed. The address set is regenerated from
 * the geometry above and this board's port map.
 */
#include "parser.h"
#include "pci.h"
#include "portmap.h"
#include "regs.h"

#define PARSER_INIT_FIELDS	0x108200u
#define PARSER_PORT_STRIDE	4u
#define PARSER_ENTRY_STRIDE	2u
#define PARSER_PORTS		76u

static int parser_carries_traffic(unsigned port)
{
	unsigned i;

	if (port == FM6000_ALTA_HOST || port == FM6000_ALTA_CPU ||
	    port == FM6000_ALTA_INTERNAL)
		return 1;
	for (i = 1; i <= FM6000_FRONT_PORTS; i++)
		if (fm6000_alta_of[i] == port)
			return 1;
	return 0;
}

/* A port that has not come up. Link state goes in the same field. */
#define PARSER_FLAGS_DOWN	0x0001u

static int write_entry(struct fm6000 *d, unsigned port, unsigned entry,
		       uint32_t w0, uint32_t w1, unsigned *n)
{
	uint32_t base = PARSER_INIT_FIELDS + port * PARSER_PORT_STRIDE +
			entry * PARSER_ENTRY_STRIDE;
	int rv;

	if ((rv = fm_wr(d, base, w0)) != FM_OK)
		return rv;
	(*n)++;
	if ((rv = fm_wr(d, base + 1, w1)) != FM_OK)
		return rv;
	(*n)++;
	return FM_OK;
}

int fm_parser_fields_init(struct fm6000 *d, unsigned *written)
{
	unsigned port, n = 0;
	int rv;

	if (written != NULL)
		*written = 0;

	/*
	 * Entry 0: a seed for the ports that carry traffic, zero for the rest.
	 *
	 * The host port and the two internal ports have a GLORT but no
	 * 0x100|glort in the low half -- only the front panel does.
	 */
	for (port = 0; port < PARSER_PORTS; port++) {
		uint32_t w0 = 0, w1 = 0;

		if (parser_carries_traffic(port)) {
			unsigned glort = fm6000_glort_of(port);
			unsigned front = port != FM6000_ALTA_HOST &&
					 port != FM6000_ALTA_CPU &&
					 port != FM6000_ALTA_INTERNAL;

			w0 = PARSER_FLAGS_DOWN << 16;
			if (front)
				w0 |= 0x100u | glort;
			w1 = ((uint32_t)glort << 16) | 1u;
		}
		if ((rv = write_entry(d, port, 0, w0, w1, &n)) != FM_OK)
			return rv;
	}

	/* Entry 1 is unused on every port. */
	for (port = 0; port < PARSER_PORTS; port++)
		if ((rv = write_entry(d, port, 1, 0, 0, &n)) != FM_OK)
			return rv;

	if (written != NULL)
		*written = n;
	return fm_alive(d) ? FM_OK : FM_EOFFBUS;
}
