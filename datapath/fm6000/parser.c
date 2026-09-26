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
 * ⚠ WHAT THIS FUNCTION DOES NOT DO, AND WHY THAT IS NOT AN OVERSIGHT.
 *
 * It writes only zeros: entry 1 on all 76 ports, and entry 0 on the 21 ports
 * that carry no traffic. It does not seed entry 0 for the ports that do.
 *
 * That is the whole of what the reference block covers. The live seeds are
 * not write-once -- they are accumulated as ports come up -- so the prior
 * work on this chassis left them in its capture replay rather than authoring
 * them, and there is nothing there to port. Writing them means choosing this
 * switch's GLORT assignment, which is a forwarding decision and belongs with
 * the forwarding bring-up, not here. Until then a port's seed stays whatever
 * the boot left it.
 *
 * Clearing is still worth doing on its own: it guarantees that no port which
 * is not carrying traffic, and no second entry anywhere, holds a stale or
 * uninitialised seed that the parser would act on.
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

static int clear_entry(struct fm6000 *d, unsigned port, unsigned entry,
		       unsigned *n)
{
	unsigned w;
	int rv;

	for (w = 0; w < PARSER_ENTRY_STRIDE; w++) {
		rv = fm_wr(d, PARSER_INIT_FIELDS + port * PARSER_PORT_STRIDE +
			      entry * PARSER_ENTRY_STRIDE + w, 0);
		if (rv != FM_OK)
			return rv;
		(*n)++;
	}
	return FM_OK;
}

int fm_parser_fields_clear(struct fm6000 *d, unsigned *written)
{
	unsigned port, n = 0;
	int rv;

	if (written != NULL)
		*written = 0;

	/* Entry 0, but only where no traffic will be parsed. */
	for (port = 0; port < PARSER_PORTS; port++)
		if (!parser_carries_traffic(port))
			if ((rv = clear_entry(d, port, 0, &n)) != FM_OK)
				return rv;

	/* Entry 1, everywhere: it is unused on every port. */
	for (port = 0; port < PARSER_PORTS; port++)
		if ((rv = clear_entry(d, port, 1, &n)) != FM_OK)
			return rv;

	if (written != NULL)
		*written = n;
	return fm_alive(d) ? FM_OK : FM_EOFFBUS;
}
