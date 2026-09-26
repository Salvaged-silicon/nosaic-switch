/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The scheduler ring -- and why the egress scheduler is unreachable without it.
 *
 * The FM6000 schedules by circulating a token around a ring of 80 slots. A
 * tick advances the ring one slot; each slot names the port to be served.
 * The engine is not a passive register file: once started it walks the ring
 * continuously, and the blocks it serves are clocked off that walk.
 *
 * ⚠ THIS IS WHY ESCHED (0x2000-0x3fff) OFF-BUSES A CHIP THAT HAS ONLY BEEN
 * COLD-BOOTED. Measured on this board 2026-09-26, after the documented Table
 * 4-1 boot and with every other block in the low register space answering
 * normally:
 *
 *   - ONE read of any esched word (0x2020, 0x2080, 0x3800 all tried) takes
 *     the chip off the bus immediately. A read has to complete, and nothing
 *     is clocked to complete it.
 *   - Writes appear to work and then kill the chip on roughly the twentieth.
 *     A posted write does not wait, so the first ~19 sit in the bridge's
 *     write queue; the one that finds the queue full blocks, and the local
 *     bus is wedged. The count is not a timing artefact: it is the same
 *     whether the writes are microseconds or a third of a second apart.
 *   - Running the memory BIST first drops the budget from 21 writes to 3,
 *     which is the same queue being partly consumed elsewhere.
 *
 * So the rule is: start the ring, then configure the egress scheduler. Doing
 * it the other way round does not produce a misconfigured switch, it produces
 * a switch that has to be power-pulsed.
 *
 * PROVENANCE. Ported from EdgeNOS's fm6000_sched_std.c -- our own prior work
 * on this chassis -- and relicensed. The token layout and the golden ring
 * membership were established there against the vendor SDK's ring builder;
 * the geometry below is re-derived and commented rather than transcribed.
 */
#include <unistd.h>

#include "pci.h"
#include "regs.h"
#include "ssched.h"

/*
 * A ring token: the port to serve, plus two state bits.
 *
 * Locked marks a slot the engine may not reassign. No golden token sets Sync,
 * and setting it on the bootstrap ring is a documented way to get a ring that
 * initialises and never advances, so the argument exists to make the zero
 * explicit rather than to be varied.
 */
#define SSCHED_TOKEN(port, locked, sync)             \
	(((uint32_t)(port) & 0x7fu) |                \
	 (((uint32_t)(locked) & 1u) << 9) |          \
	 (((uint32_t)(sync) & 1u) << 10))

/*
 * The ring, as the running switch programs it.
 *
 * Sixty-four tokens in service order. Four of them -- physical ports 0 to 3,
 * the ports with no cage -- are Locked, which keeps their slot even when the
 * port is idle; the other sixty are not. No token sets Sync.
 *
 * ⚠ THE ORDER IS THE RING'S SERVICE ORDER AND IS NOT ARBITRARY. Tokens are
 * inserted in the order the engine will visit them, so this list is the
 * schedule. Its shape is clearly physical -- runs of four at stride four,
 * which is what four lanes of an EPL looks like -- but the rule that
 * generates it has not been derived, so it is written out rather than
 * computed, and it is written out in full rather than sorted into something
 * tidier.
 *
 * ⚠ IT ENROLS MORE THAN THE FRONT PANEL. Ports 12 to 19 carry tokens and
 * have no cage on this SKU. They are enrolled by the running switch and the
 * ring is a fixed-size schedule, so they are enrolled here.
 *
 * This is the golden ring recovered from the running switch, and it is a
 * different thing from the five-token bootstrap ring our earlier prior-art
 * tool programs -- which is what this file first implemented, and which does
 * not circulate.
 */
static const unsigned char ssched_ring[] = {
	20, 24, 28,  0,   21, 25, 29,  1,   22, 26, 30,  3,
	23, 27, 31, 32,   36, 40, 44, 33,   37, 41, 45, 34,
	38, 42, 46, 35,   39, 43, 47, 64,   52, 56, 60, 65,
	53, 57, 61, 66,   54, 58, 62, 67,   55, 59, 63,  2,
	68, 72, 12, 16,   69, 73, 13, 17,   70, 74, 14, 18,
	71, 75, 15, 19,
};

/* The four that keep their slot when idle. */
static int ssched_locked(unsigned port)
{
	return port <= 3;
}

/*
 * The visit table: 80 slots, one byte each, naming the port to serve.
 *
 * Only the four internal ports and the management port appear. The
 * front-panel ports are enrolled as tokens above but are not given a fixed
 * slot -- the engine places them -- which is why this table stays sparse
 * while the ring carries sixty-four tokens.
 */
#define SSCHED_MGMT_PORT 78

static void ring_slot(uint32_t *visit, unsigned port)
{
	visit[port / 4] |= (uint32_t)port << (8 * (port % 4));
}

int fm_ssched_ring_init(struct fm6000 *d, unsigned flags, int *circulating)
{
	uint32_t visit[FM6000_SSCHED_NEXT_PORT_WORDS];
	unsigned i;
	int rv;

	if (circulating != NULL)
		*circulating = 0;

	for (i = 0; i < FM6000_SSCHED_NEXT_PORT_WORDS; i++)
		visit[i] = 0;
	for (i = 0; i <= 3; i++)
		ring_slot(visit, i);
	ring_slot(visit, SSCHED_MGMT_PORT);

	/*
	 * The tick first: it is the clock the whole engine runs on, and every
	 * write below lands in a domain that cannot retire it until the tick
	 * exists.
	 */
	if ((rv = fm_wr(d, FM6000_SSCHED_TICK_CFG, FM6000_SSCHED_TICK_PERIOD)) != FM_OK)
		return rv;

	/* The sweeper shares that domain and is configured with it. */
	if ((rv = fm_wr(d, FM6000_SWEEPER_CFG_0, 0x0008bb2cu)) != FM_OK)
		return rv;
	if ((rv = fm_wr(d, FM6000_SWEEPER_CFG_1, 0x00000002u)) != FM_OK)
		return rv;
	if ((rv = fm_wr(d, FM6000_SWEEPER_CFG_2, 0x00000000u)) != FM_OK)
		return rv;
	if ((rv = fm_wr(d, FM6000_SWEEPER_CFG_3, 0x0030a2c3u)) != FM_OK)
		return rv;
	if ((rv = fm_wr(d, FM6000_SWEEPER_CFG_4, 0x00002000u)) != FM_OK)
		return rv;
	if (!fm_alive(d))
		return FM_EOFFBUS;

	/* Clear both replace-token registers before programming. */
	if ((rv = fm_wr(d, FM6000_SSCHED_RX_REPLACE_TOKEN, 0)) != FM_OK)
		return rv;
	if ((rv = fm_wr(d, FM6000_SSCHED_TX_REPLACE_TOKEN, 0)) != FM_OK)
		return rv;

	/* Insert the ring, in service order, both directions. */
	for (i = 0; i < sizeof ssched_ring; i++) {
		unsigned port = ssched_ring[i];
		unsigned sync = (port == SSCHED_MGMT_PORT &&
				 (flags & FM_SSCHED_SYNC_MGMT)) ? 1 : 0;
		uint32_t tok = SSCHED_TOKEN(port, ssched_locked(port), sync);

		if ((rv = fm_wr(d, FM6000_SSCHED_RX_INIT_TOKEN, tok)) != FM_OK)
			return rv;
		if ((rv = fm_wr(d, FM6000_SSCHED_TX_INIT_TOKEN, tok)) != FM_OK)
			return rv;
	}
	if (!fm_alive(d))
		return FM_EOFFBUS;

	/* The visit table, same in both directions. */
	for (i = 0; i < FM6000_SSCHED_NEXT_PORT_WORDS; i++) {
		if ((rv = fm_wr(d, FM6000_SSCHED_RX_NEXT_PORT(i), visit[i])) != FM_OK)
			return rv;
		if ((rv = fm_wr(d, FM6000_SSCHED_TX_NEXT_PORT(i), visit[i])) != FM_OK)
			return rv;
	}
	if (!fm_alive(d))
		return FM_EOFFBUS;

	/* One slow-port mask, not five: the running switch writes only the
	 * first, and the other four are left as the boot leaves them. */
	if ((rv = fm_wr(d, FM6000_SSCHED_RX_SLOW_PORT(0), 0x0000000fu)) != FM_OK)
		return rv;

	/*
	 * Start it. These are write-1 strobes, and they are checked separately
	 * because RX and TX start independently and only one of them needs to
	 * be wrong for the ring to sit still.
	 */
	if ((rv = fm_wr(d, FM6000_SSCHED_RX_INIT_COMPLETE, 1)) != FM_OK)
		return rv;
	if (!fm_alive(d))
		return FM_EOFFBUS;
	if ((rv = fm_wr(d, FM6000_SSCHED_TX_INIT_COMPLETE, 1)) != FM_OK)
		return rv;
	if (!fm_alive(d))
		return FM_EOFFBUS;

	/* Clear the replace-token registers again, as the running switch does,
	 * before they are used as a find-probe below. */
	if ((rv = fm_wr(d, FM6000_SSCHED_RX_REPLACE_TOKEN, 0)) != FM_OK)
		return rv;
	if ((rv = fm_wr(d, FM6000_SSCHED_TX_REPLACE_TOKEN, 0)) != FM_OK)
		return rv;

	/*
	 * Ask the running engine to find a token.
	 *
	 * Writing a port number with the read/write bit clear is a find, not a
	 * modification: the engine walks the ring looking for that port and
	 * sets a Found bit if it got there. That is the only direct evidence
	 * that the ring is advancing rather than merely programmed, and it
	 * stays inside the scheduler block, so it is safe on a chip whose
	 * egress scheduler is still untouchable.
	 */
	for (i = 0; i < sizeof ssched_ring; i++) {
		uint32_t v = 0;
		unsigned port = ssched_ring[i];

		if ((rv = fm_wr(d, FM6000_SSCHED_RX_REPLACE_TOKEN, port)) != FM_OK)
			return rv;
		usleep(FM6000_SSCHED_FIND_US);
		if ((rv = fm_rd(d, FM6000_SSCHED_RX_REPLACE_TOKEN, &v)) != FM_OK)
			return rv;
		if (v & FM6000_SSCHED_RX_FOUND) {
			fm_sched_mark_ready(d);
			if (circulating != NULL)
				*circulating = 1;
			break;
		}

		if ((rv = fm_wr(d, FM6000_SSCHED_TX_REPLACE_TOKEN, port)) != FM_OK)
			return rv;
		usleep(FM6000_SSCHED_FIND_US);
		if ((rv = fm_rd(d, FM6000_SSCHED_TX_REPLACE_TOKEN, &v)) != FM_OK)
			return rv;
		if (v & FM6000_SSCHED_TX_FOUND) {
			fm_sched_mark_ready(d);
			if (circulating != NULL)
				*circulating = 1;
			break;
		}

		/* Four probes is enough to tell a running ring from a stopped
		 * one, and sixty-four at 50 ms each is six seconds of nothing. */
		if (i >= 3)
			break;
	}

	return fm_alive(d) ? FM_OK : FM_EOFFBUS;
}
