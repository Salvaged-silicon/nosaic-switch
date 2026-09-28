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
#include <string.h>

#include "ssched.h"

/*
 * A ring token: the port to serve, plus two state bits.
 *
 * Locked marks a slot the engine may not reassign. No golden token sets Sync,
 * and setting it on the bootstrap ring is a documented way to get a ring that
 * initialises and never advances, so the argument exists to make the zero
 * explicit rather than to be varied.
 *
 * ⚠ RX AND TX TOKENS ARE NOT THE SAME IN THE VENDOR'S BUILDER, and this code
 * writes one token to both. Decoded from the SDK: the RX token takes the port
 * in [6:0] and one flag into bit 9; the TX token takes the same two and then
 * computes BIT 10 from a second per-entry field the RX path never reads. Our
 * bit 10 is the Sync argument, defaulted off.
 *
 * On the golden capture every token has bit 10 clear, so for this ring the two
 * happen to coincide and writing one to both is not wrong here. It is written
 * down because "the same token to both directions" is an assumption this code
 * makes silently, and it is only true while that field is zero. [RE]
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

/* Same table, but the byte for `port` names the port served after it. */
static void ring_next(uint32_t *visit, unsigned port, unsigned next)
{
	visit[port / 4] |= ((uint32_t)next & 0xffu) << (8 * (port % 4));
}

static const char *phase_names[FM_SSCHED_PH__COUNT] = {
	[FM_SSCHED_PH_TICK]    = "tick",
	[FM_SSCHED_PH_SWEEPER] = "sweeper config",
	[FM_SSCHED_PH_CLEAR1]  = "replace tokens cleared",
	[FM_SSCHED_PH_TOKENS]  = "ring tokens inserted",
	[FM_SSCHED_PH_VISIT]   = "visit table",
	[FM_SSCHED_PH_SLOW]    = "slow-port mask",
	[FM_SSCHED_PH_START]   = "RX/TX INIT_COMPLETE",
	[FM_SSCHED_PH_CLEAR2]  = "replace tokens cleared again",
	[FM_SSCHED_PH_FIND]    = "find probe",
};

const char *fm_ssched_phase_name(int ph)
{
	if (ph < 0 || ph >= FM_SSCHED_PH__COUNT || phase_names[ph] == NULL)
		return "?";
	return phase_names[ph];
}

/* Close off a phase: how many times did the chip reset itself during it. */
static void phase(struct fm6000 *d, struct fm_ssched_report *rep, int ph)
{
	uint32_t now;

	if (rep == NULL)
		return;
	now = fm_fatal_count(d);
	rep->resets[ph] = now > rep->mark ? now - rep->mark : 0;
	rep->total += rep->resets[ph];
	rep->mark = now;
}

int fm_ssched_ring_init(struct fm6000 *d, unsigned flags, int *circulating,
			struct fm_ssched_report *rep)
{
	uint32_t visit[FM6000_SSCHED_NEXT_PORT_WORDS];
	unsigned i;
	int rv;

	if (circulating != NULL)
		*circulating = 0;
	if (rep != NULL) {
		memset(rep, 0, sizeof(*rep));
		rep->mark = fm_fatal_count(d);
	}

	for (i = 0; i < FM6000_SSCHED_NEXT_PORT_WORDS; i++)
		visit[i] = 0;
	if (flags & FM_SSCHED_NEXT_CHAIN) {
		/* Each enrolled port points at the next in service order, and
		 * the last wraps to the first: a closed cycle. The management
		 * port is spliced in at the end so it is served once per lap
		 * rather than left pointing nowhere. */
		unsigned n = sizeof ssched_ring;

		for (i = 0; i < n; i++) {
			unsigned here = ssched_ring[i];
			unsigned next = (i + 1 == n) ? SSCHED_MGMT_PORT
						     : ssched_ring[i + 1];

			ring_next(visit, here, next);
		}
		ring_next(visit, SSCHED_MGMT_PORT, ssched_ring[0]);
	} else {
		for (i = 0; i <= 3; i++)
			ring_slot(visit, i);
		ring_slot(visit, SSCHED_MGMT_PORT);
	}

	/*
	 * The tick first: it is the clock the whole engine runs on, and every
	 * write below lands in a domain that cannot retire it until the tick
	 * exists.
	 */
	if ((rv = fm_wr(d, FM6000_SSCHED_TICK_CFG, FM6000_SSCHED_TICK_PERIOD)) != FM_OK)
		return rv;

	phase(d, rep, FM_SSCHED_PH_TICK);

	/* The sweeper shares that domain and is configured with it -- but see
	 * FM_SSCHED_NO_SWEEPER: word 3 of this register starts a reset storm on
	 * a chip whose swept tables are not initialised yet. */
	if (flags & FM_SSCHED_NO_SWEEPER)
		goto no_sweeper;
	if ((rv = fm_wr(d, FM6000_SWEEPER_CFG_0, 0x0008bb2cu)) != FM_OK)
		return rv;
	if ((rv = fm_wr(d, FM6000_SWEEPER_CFG_1, 0x00000002u)) != FM_OK)
		return rv;
	if ((rv = fm_wr(d, FM6000_SWEEPER_CFG_2, 0x00000000u)) != FM_OK)
		return rv;
	/*
	 * Word 3 is DERIVED, not captured.
	 *
	 * This used to write 0x0030a2c3, a value taken off a running switch,
	 * and writing it put this chip into a permanent self-reset storm. The
	 * vendor does not write a constant here: fmPlatformSwitchPreInitialize,
	 * gated on the api.fm6000.mrlPatch attribute which defaults on, reads
	 * the register, keeps its low sixteen bits and ORs in 0x300000.
	 *
	 * On this chip word 3 reads 0 after Table 4-1, so the vendor's own
	 * formula gives 0x300000 -- and the captured 0xa2c3 in the low half
	 * came from a chip state we never create. Doing the read-modify-write
	 * is both correct and measured safe. [RE]
	 */
	{
		uint32_t w3 = 0;

		if ((rv = fm_rd(d, FM6000_SWEEPER_CFG_3, &w3)) != FM_OK)
			return rv;
		w3 = (w3 & 0xffffu) | 0x300000u;
		if ((rv = fm_wr(d, FM6000_SWEEPER_CFG_3, w3)) != FM_OK)
			return rv;
	}

	/*
	 * The other half of the same patch: FC_MRL_FC_TOKEN_LIMIT, low ten
	 * bits set to 0x190 and bit 30 set, again read-modify-write. Nothing
	 * here wrote this register at all before. [RE]
	 */
	{
		uint32_t tl = 0;

		if ((rv = fm_rd(d, FM6000_FC_MRL_FC_TOKEN_LIMIT, &tl)) != FM_OK)
			return rv;
		tl = (tl & ~0x3ffu) | 0x190u | (1u << 30);
		if ((rv = fm_wr(d, FM6000_FC_MRL_FC_TOKEN_LIMIT, tl)) != FM_OK)
			return rv;
	}

	/*
	 * ⚠ WORD 4 IS LEFT AT ZERO, DELIBERATELY.
	 *
	 * Writing the captured 0x00002000 here still puts the chip into a
	 * permanent self-reset storm, measured alone from a fresh boot, and
	 * writing 0 is free. It is the last sweeper trigger left after the
	 * step-5 fix cured word 3.
	 *
	 * Bit 13 of word 4 most likely arms the L2 lookup sweeper, which walks
	 * the MAC table -- but CRM-initialising the MAC table first does not
	 * make it safe, measured. So what it wants is not known, and a
	 * register whose value we cannot justify is better left alone than
	 * written from a capture. That is what caused word 3.
	 */
	if ((rv = fm_wr(d, FM6000_SWEEPER_CFG_4, 0u)) != FM_OK)
		return rv;
	if (!fm_alive(d))
		return FM_EOFFBUS;

no_sweeper:
	phase(d, rep, FM_SSCHED_PH_SWEEPER);

	/* Clear both replace-token registers before programming. */
	if ((rv = fm_wr(d, FM6000_SSCHED_RX_REPLACE_TOKEN, 0)) != FM_OK)
		return rv;
	if ((rv = fm_wr(d, FM6000_SSCHED_TX_REPLACE_TOKEN, 0)) != FM_OK)
		return rv;

	phase(d, rep, FM_SSCHED_PH_CLEAR1);

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

	phase(d, rep, FM_SSCHED_PH_TOKENS);

	/* The visit table, same in both directions. */
	for (i = 0; i < FM6000_SSCHED_NEXT_PORT_WORDS; i++) {
		if ((rv = fm_wr(d, FM6000_SSCHED_RX_NEXT_PORT(i), visit[i])) != FM_OK)
			return rv;
		if ((rv = fm_wr(d, FM6000_SSCHED_TX_NEXT_PORT(i), visit[i])) != FM_OK)
			return rv;
	}
	if (!fm_alive(d))
		return FM_EOFFBUS;

	phase(d, rep, FM_SSCHED_PH_VISIT);

	/*
	 * The slow-port mask, all five words.
	 *
	 * ⚠ THE COMMENT THAT USED TO BE HERE WAS WRONG. It said the running
	 * switch writes only the first word and the other four are left as the
	 * boot leaves them. The vendor writes all five, in a loop over i = 0..4,
	 * and each takes a SIXTEEN-bit value -- so this is one bit per
	 * scheduler port across 80 bits, not a 32-bit mask in one register.
	 * That matches the 76-port segment scheduler with room to spare. [RE]
	 *
	 * Word 0 keeps the value measured from a running switch. The upper four
	 * are written explicitly as zero rather than left alone: the vendor
	 * computes all five from per-port state we do not have, and leaving a
	 * register to whatever the boot left in it is how this port has been
	 * bitten before. Zero is "no slow ports here", which is the right
	 * default for a chassis whose front panel is uniform.
	 */
	for (i = 0; i < FM6000_SSCHED_SLOW_PORT_WORDS; i++) {
		uint32_t mask = (i == 0) ? 0x0000000fu : 0u;

		if ((rv = fm_wr(d, FM6000_SSCHED_RX_SLOW_PORT(i), mask)) != FM_OK)
			return rv;
	}

	phase(d, rep, FM_SSCHED_PH_SLOW);

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

	phase(d, rep, FM_SSCHED_PH_START);

	/* Clear the replace-token registers again, as the running switch does,
	 * before they are used as a find-probe below. */
	if ((rv = fm_wr(d, FM6000_SSCHED_RX_REPLACE_TOKEN, 0)) != FM_OK)
		return rv;
	if ((rv = fm_wr(d, FM6000_SSCHED_TX_REPLACE_TOKEN, 0)) != FM_OK)
		return rv;

	phase(d, rep, FM_SSCHED_PH_CLEAR2);

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

	phase(d, rep, FM_SSCHED_PH_FIND);
	if (rep != NULL) {
		struct fm_fatal f;

		if (fm_fatal_read(d, &f) == FM_OK)
			rep->last_fatal = f.last;
	}

	return fm_alive(d) ? FM_OK : FM_EOFFBUS;
}
