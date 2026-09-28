/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Loopback suppression.
 *
 * A frame flooded to a VLAN must not go back out of the port it came in on.
 * The chip decides that by matching the frame's source GLORT against one
 * entry per port: if they match, that port is dropped from the flood.
 *
 * One word per physical port at 0x014000, and the word is a match/mask pair
 * packed together --
 *
 *     entry = glort << 16 | (~glort & 0xffff)
 *
 * -- so the mask is the complement of the value and the match is exact. A
 * port's GLORT is its logical port number; see portmap.h, where that
 * assignment is derived and where it is the only place written down.
 *
 * ⚠ THE HOST PORT GETS A GLORT THAT NO FRAME CARRIES, NOT ITS OWN. Its
 * GLORT is 0, and an entry matching 0 would match every frame whose source
 * GLORT field is unset -- which would quietly suppress flooding to the CPU.
 * It is given 0xff00 instead, which is outside the assignment and therefore
 * matches nothing.
 *
 * Nothing here is transcribed: every entry is computed from this board's
 * port map and our own GLORT assignment. Checked against the reference
 * table all the same, and 54 of its 55 entries are exactly what this
 * produces -- the 55th being the host port, whose sentinel is the rule
 * above rather than an exception to it.
 *
 * ⚠ WHAT IS NOT WRITTEN. LBS_PROFILE_TABLE at 0x014080 has twelve entries
 * holding 0 or 2, in a pattern (0,2,2,2,2,0,2,2,0,0,0,0) that nothing here
 * explains. Twelve unexplained values are still twelve unexplained values,
 * and writing them because a reference has them is the thing this port does
 * not do. It is left alone until someone can say what a profile is.
 */
#include "lbs.h"
#include "pci.h"
#include "portmap.h"
#include "regs.h"

#define LBS_CAM			0x014000u
#define LBS_CAM_PORTS		76u

/* A GLORT outside the assignment, so it matches no frame. */
#define LBS_GLORT_NEVER		0xff00u

static uint32_t lbs_entry(unsigned glort)
{
	return ((uint32_t)glort << 16) | (~(uint32_t)glort & 0xffffu);
}

int fm_lbs_init(struct fm6000 *d, unsigned *written)
{
	unsigned port, n = 0;
	int rv;

	if (written != NULL)
		*written = 0;

	for (port = 0; port < LBS_CAM_PORTS; port++) {
		unsigned glort = fm6000_glort_of(port);

		/* A port with no GLORT carries no traffic, so there is nothing
		 * to suppress and no entry to write. */
		if (glort == 0 && port != FM6000_ALTA_HOST)
			continue;
		if (port == FM6000_ALTA_HOST)
			glort = LBS_GLORT_NEVER;

		if ((rv = fm_wr(d, LBS_CAM + port, lbs_entry(glort))) != FM_OK)
			return rv;
		n++;
	}

	if (written != NULL)
		*written = n;
	return fm_alive(d) ? FM_OK : FM_EOFFBUS;
}
