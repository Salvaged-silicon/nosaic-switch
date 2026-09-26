/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Front-panel port to physical ("alta") port, for the DCS-7150S-52.
 *
 * Recovered from this chassis itself: the vendor's FocalPointV2 agent prints
 * its whole mapping at bring-up, one line per physical port, and this is that
 * mapping. It is not arithmetic -- odd front-panel ports come off one set of
 * physical ports and even off another, in blocks of four, which is what a
 * four-lane EPL feeding two rows of cages looks like once board routing is
 * taken into account.
 *
 * ⚠ THE BOARD'S FDL IS A FAMILY TABLE AND DISAGREES ABOVE PORT 48. It gives
 * the same values for 1-48 exactly, and then numbers four more ports before
 * reaching the ones this SKU calls 49-52:
 *
 *      this board, measured      49->44  50->45  51->46  52->47
 *      the FDL                   53->44  54->45  55->46  56->47
 *                                49->48  50->49  51->50  52->51
 *
 * So the FDL is written for a larger member of the family and this chassis
 * uses a subset, offset by four in that range. Where they differ, the agent
 * log wins: it came off this switch.
 */
#ifndef NOSAIC_FM6000_PORTMAP_H
#define NOSAIC_FM6000_PORTMAP_H

#define FM6000_FRONT_PORTS 52

/* Index by front-panel port, 1-based; [0] is unused. */
static const unsigned char fm6000_alta_of[FM6000_FRONT_PORTS + 1] = {
	0,
	40, 20, 41, 21, 42, 22, 43, 23,		/*  1.. 8 */
	36, 64, 37, 65, 38, 66, 39, 67,		/*  9..16 */
	72, 28, 73, 29, 74, 30, 75, 31,		/* 17..24 */
	68, 24, 69, 25, 70, 26, 71, 27,		/* 25..32 */
	32, 60, 33, 61, 34, 62, 35, 63,		/* 33..40 */
	52, 56, 53, 57, 54, 58, 55, 59,		/* 41..48 */
	44, 45, 46, 47,				/* 49..52, the uplink group */
};

/*
 * The non-front-panel ports, by physical number.
 *
 * Four physical ports carry no cage: 0, 1, 2 and 3. The agent log numbers two
 * of them 53 and 54 in its own logical space.
 *
 * ⚠ THE CHIP'S OWN TABLES DISAGREE ABOUT WHICH OF THESE IS "THE CPU PORT",
 * and both readings come off this running switch, so neither can be waved
 * away:
 *
 *   - the store-and-forward table gives its CPU pattern to physical **1**,
 *     and gives 0 and 2 a plain all-ports mask
 *   - the egress scheduler, the scheduler ring and the congestion watermarks
 *     all single out physical **0** -- the ring's own notes call it the PCIe
 *     DMA port and schedule it first, which is what punt to the host needs
 *
 * The most likely reconciliation is that they are different ports doing
 * different jobs -- 0 is where frames reach the host, 1 is what the
 * forwarding path calls the CPU -- but that is a hypothesis and nothing here
 * has tested it. Until something does, each table gets the port its own
 * measurement wanted, under a name that says which is which, and no code
 * assumes the two are the same port.
 */
#define FM6000_ALTA_HOST     0	/* scheduler, ring and watermarks */
#define FM6000_ALTA_CPU      1	/* store-and-forward */
#define FM6000_ALTA_INTERNAL 3

#endif /* NOSAIC_FM6000_PORTMAP_H */
