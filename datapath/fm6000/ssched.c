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
#include "sbus.h"
#include "spico.h"
#include <string.h>

#include "ssched.h"
#include "boot.h"
#include "crm.h"
#include "esched.h"

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
	[FM_SSCHED_PH_ESCHED]  = "ESCHED pre-init",
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

	/* The SBus master must be running before any JSS SBus transaction.
	 * fm_sbus_start() clears SBUS_CFG (which holds the controller in
	 * reset if non-zero) and takes SPICO out of reset. Safe to call
	 * repeatedly. */
	if ((rv = fm_sbus_start(d)) != FM_OK)
		return rv;

	/*
	 * SPICO is NOT loaded here.
	 *
	 * EdgeNOS established (SPICO-RE.md, 2026-08-21) that fibre ports link and
	 * forward with zero Intel code; only DAC copper needs the SerDes DFE
	 * tuning that SPICO enables. The JSS tick clock is generated by the clock
	 * synthesiser at SBus device 0x45 (enabled below), not by SPICO.
	 *
	 * An earlier version of this file loaded SPICO unconditionally and
	 * attributed ~112 ring-init resets to its absence. Those resets were
	 * from the blind MMIO write bug in the JSS clock enable (0x45 write),
	 * not from missing SPICO. Loading SPICO triggers ~39 chip self-resets
	 * during the 30,000-write download, which corrupt subsequent SBus state
	 * (SBUS_CFG resets to non-zero after each reset), causing every step
	 * after the download to operate on a half-initialised SBus controller.
	 * The symptom: chip goes permanently off-bus after phase(TICK).
	 *
	 * When copper/DFE support is needed: load SPICO AFTER the ring is
	 * confirmed circulating, with a fresh fm_sbus_start() first to guarantee
	 * SBUS_CFG=0 after any download-phase chip resets. The retry loop in
	 * fm_spico_load() handles the case where a reset corrupted the download.
	 * [RE 2026-10-05]
	 */

	/*
	 * Scan-chain step 5, repeated.
	 *
	 * boot.c writes these three config words and then 0xffffffff to
	 * SCAN_CHAIN_DATA_IN (Table 4-1 step 5) to put the core logic and
	 * EPLs into normal operating mode. The 76+ watchdog resets during the
	 * fill (boot step 12) clear every register and undo that write.
	 * Without a second application here the ring engine starts in an
	 * abnormal core-logic state and never advances: INIT_COMPLETE fires,
	 * FATAL_COUNT does not move, and the FOUND probe never fires.
	 *
	 * The re-write is safe at any time once modules are out of reset.
	 * boot.c documents: "The same write on a settled chip (all modules
	 * out of reset, ring running) is free -- FATAL_COUNT does not move."
	 * [RE 2026-10-02]
	 */
	(void)fm_wr(d, FM6000_SCAN_CONFIG_DATA_IN, 0x88800000u);
	(void)fm_wr(d, FM6000_SCAN_CONFIG_DATA_IN, 0x88008000u);
	(void)fm_wr(d, FM6000_SCAN_CONFIG_DATA_IN, 0x80000040u);
	if ((rv = fm_wr(d, FM6000_SCAN_CHAIN_DATA_IN, 0xffffffffu)) != FM_OK)
		return rv;

	for (i = 0; i < FM6000_SSCHED_NEXT_PORT_WORDS; i++)
		visit[i] = 0;
	if (flags & FM_SSCHED_MGMT_ONLY) {
		/* Single-port bootstrap ring: only port 78 (management).
		 *
		 * NEXT_PORT[i] = 0 for all i: ring loops at slot 0. The INIT_TOKEN
		 * FIFO places the first token at slot 0 (sequential FIFO order).
		 * NEXT_PORT[0]=0 sends the engine back to slot 0 on every tick.
		 * [RE 2026-10-05] */
		(void)i; /* visit[] is already 0 — all NEXT_PORT = 0 */
	} else if (flags & FM_SSCHED_NEXT_CHAIN) {
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
	 * PLL_CTRL_0/1 first: the SSCHED clock domain runs off these, so they
	 * must be stable before JSS enables it. Boot writes them at step 6,
	 * but the 78+ watchdog resets during step 10's fill clear every register.
	 * Re-writing them here, and waiting for lock, ensures the domain is
	 * properly clocked when JSS fires. Writing PLL_CTRL AFTER JSS causes
	 * a clock glitch on the newly-enabled domain → FATAL_CODE 0x00 resets.
	 * Measured 2026-10-02. [RE]
	 */
	if ((rv = fm_wr(d, FM6000_PLL_CTRL_0, FM6000_PLL_CTRL_0_GOLDEN)) != FM_OK)
		return rv;
	if ((rv = fm_wr(d, FM6000_PLL_CTRL_1, FM6000_PLL_CTRL_1_GOLDEN)) != FM_OK)
		return rv;
	/*
	 * DLL_CTRL is write-only (reads as 0); re-write to ensure the DLLs
	 * are enabled regardless of what the boot fill reset cleared. [RE]
	 */
	if ((rv = fm_wr(d, FM6000_DLL_CTRL, FM6000_DLL_CTRL_ENABLE)) != FM_OK)
		return rv;

	/*
	 * Wait for PLL lock before enabling the clock domain.
	 * PLL_STATUS bits [1:0] = PLL pair, [3:2] = DLLs. [RE, 2026-10-01]
	 */
	{
		unsigned waited;
		uint32_t pll = 0;

		for (waited = 0; waited < FM6000_PLL_LOCK_MAX_MS; waited++) {
			if ((rv = fm_rd(d, FM6000_PLL_STATUS, &pll)) != FM_OK)
				return rv;
			if ((pll & FM6000_PLL_STATUS_LOCKED_ALL) ==
			    FM6000_PLL_STATUS_LOCKED_ALL)
				break;
			usleep(1000);
		}
		/* Proceed even if the DLLs do not lock -- PLLs in bits [1:0]
		 * are the clock source; a DLL miss will show up later. */
	}

	/*
	 * CM_PORT_TXMP_USAGE (0x118800): fill BEFORE JSS enable.
	 *
	 * The CM monitor reads 0x118800 on every ring tick once the ring is
	 * running (armed by CFG_4=0x2000 below).  ECC-bad entries trigger a
	 * 0xc5 watchdog reset.  Boot step 10 fills this bank, but the
	 * 400+ watchdog resets during boot wipe it.
	 *
	 * Safe window: before the JSS SBus enable below.  Once JSS
	 * is live, SSCHED owns the 0x118800 range; concurrent MMIO writes to
	 * it cause 39 0x85/0xc5 self-resets (measured 2026-10-05).  No resets
	 * occur here because no scheduler block is active yet. [RE 2026-10-05]
	 */
	if ((rv = fm_mem_fill(d, 0x118800u, 960u, 0u)) != FM_OK)
		return rv;
	if (!fm_alive(d))
		return FM_EOFFBUS;

	/*
	 * Initialize freelist SRAM and CM_PORT_RXMP_USAGE BEFORE enabling JSS.
	 *
	 * When the JSS clock domain wakes up (writes below), the SSCHED hardware
	 * immediately reads the scheduler freelist SRAM to prime its internal
	 * state.  Boot step 10's 100+ watchdog resets wipe freelist SRAM, leaving
	 * every entry ECC-bad.  Each ECC-bad read triggers a chip self-reset.
	 * Measured: 42 self-resets (last code 0x00) all attributed to the JSS
	 * enable writes (phase PH_SWEEPER checkpoint), zero from anything after.
	 *
	 * fm_boot_freelists() (FREELISTS_ALL) also resets the INIT_TOKEN FIFO, so
	 * it MUST precede the token loads below. [Measured 2026-10-02]
	 *
	 * 0x111000 (CM_PORT_RXMP_USAGE) is filled here too -- it is not the
	 * immediate cause of the 42 resets, but it is also SRAM that boot resets
	 * wipe and that the CM block reads once JSS is live. [RE 2026-10-05]
	 */
	if ((rv = fm_boot_freelists(d)) != FM_OK)
		return rv;
	if ((rv = fm_crm_memset(d, 0, 0x111000u, 2432u, FM_CRM_SIZE_32, 0u)) != FM_OK)
		return rv;
	/*
	 * Re-anchor the phase baseline after the boot fills.
	 *
	 * fm_boot_freelists() issues the FREELISTS_ALL hardware command, which
	 * re-initialises the freelist SRAMs and causes ~42 chip self-resets
	 * (ECC init side-effect -- measured consistently, last code 0xd0).
	 * These resets are a normal hardware artefact of first-boot fill and
	 * should not be counted against ring init quality. Resetting rep->mark
	 * here means every subsequent phase() call measures from a stable chip,
	 * not from before the boot fills. [RE 2026-10-05]
	 */
	if (rep != NULL)
		rep->mark = fm_fatal_count(d);

	/*
	 * JSS clock-domain enable.
	 *
	 * Two writes:
	 *   1. SBus write to device 0x45 (JSS-internal clock synthesiser),
	 *      register 0x2a, data 0x16. This enables the SSCHED clock domain
	 *      inside the chip. Without it TICK_CFG and SchedPeriod tick into a
	 *      dead domain and the ring never advances -- confirmed 2026-10-02.
	 *      Must be a proper SBus transaction, NOT a blind MMIO write to 0xF001
	 *      (SBUS_COMMAND): a blind write sends whatever stale value is in
	 *      SBUS_REQUEST (0x00 after the preceding SPICO download), writing
	 *      0x00 instead of 0x16 and leaving the clock domain disabled.
	 *      [RE from 7150S-52 live capture 2026-10-01; fixed 2026-10-02]
	 *   2. Direct write of 1 to JSS_CFG_8 (0xF008). The live capture shows
	 *      this is 1 on a running switch. [RE 2026-10-01]
	 */
	if ((rv = fm_sbus_write(d, 0x45, 0x2a, 0x16u)) != FM_OK)
		return rv;
	if ((rv = fm_wr(d, FM6000_JSS_CFG_8, 0x00000001u)) != FM_OK)
		return rv;

	if ((rv = fm_wr(d, FM6000_SSCHED_TICK_CFG, FM6000_SSCHED_TICK_PERIOD)) != FM_OK)
		return rv;

	phase(d, rep, FM_SSCHED_PH_TICK);

	/*
	 * Clear CFG_4 so a previous run's SchedPeriod does not fire into
	 * the freshly-enabled clock domain before INIT_COMPLETE. [RE]
	 */
	if ((rv = fm_wr(d, FM6000_SWEEPER_CFG_4, 0u)) != FM_OK)
		return rv;

	/*
	 * Skip CFG_3 for MGMT_ONLY or when explicitly requested.
	 *
	 * CFG_3=0x300000 starts the MRL sweeper; on a cold chip it fires a
	 * self-reset storm (~145 resets in CLEAR1 measured 2026-10-02). The
	 * sweeper is not needed to get the ring circulating: MGMT_ONLY only
	 * needs the clock domain enabled, which CFG_0/1 in the START phase
	 * provides. Full sweeper can be added once the ring is confirmed
	 * running. [RE 2026-10-02]
	 */
	if ((flags & FM_SSCHED_NO_SWEEPER) || (flags & FM_SSCHED_MGMT_ONLY))
		goto no_sweeper;
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
	 *
	 * NOTE: CFG_3=0x300000 causes a MRL sweeper reset storm. Combining it
	 * with CFG_4=0x2000 before INIT_COMPLETE produced 524 resets because
	 * the CFG_3 storm ran throughout the sequence -- not because CFG_4=0x2000
	 * before INIT_COMPLETE is unsafe. In nosweep mode (CFG_3 not written)
	 * CFG_4=0x2000 before INIT_COMPLETE is safe. [RE 2026-10-02]
	 */
	{
		uint32_t w3 = 0;

		if ((rv = fm_rd(d, FM6000_SWEEPER_CFG_3, &w3)) != FM_OK)
			return rv;
		w3 = (w3 & 0xffffu) | 0x300000u;
		if ((rv = fm_wr(d, FM6000_SWEEPER_CFG_3, w3)) != FM_OK)
			return rv;
	}

no_sweeper:
	phase(d, rep, FM_SSCHED_PH_SWEEPER);

	/* Clear both replace-token registers before programming. */
	if ((rv = fm_wr(d, FM6000_SSCHED_RX_REPLACE_TOKEN, 0)) != FM_OK)
		return rv;
	if ((rv = fm_wr(d, FM6000_SSCHED_TX_REPLACE_TOKEN, 0)) != FM_OK)
		return rv;

	phase(d, rep, FM_SSCHED_PH_CLEAR1);

	/*
	 * ESCHED registers (0x2000-0x3fff) are clock-gated off until CFG_4
	 * bit 13 is written. Writing CFG_4=0x2000 enables the ESCHED clock
	 * domain; port 78's ESCHED entries are initialised in the START phase
	 * after CFG_4 is written. Once sched_ready=1, fm_esched_init writes
	 * all 76 front-panel entries.
	 */
	phase(d, rep, FM_SSCHED_PH_ESCHED);

	/* Insert the ring tokens. MGMT_ONLY uses a single management port token
	 * to bootstrap without touching ESCHED. SMALL_RING uses five locked
	 * ports matching the EdgeNOS reference. The full 64-token ring risks
	 * overflowing the INIT_TOKEN FIFO; probe checks first+last 4. */
	if (flags & FM_SSCHED_MGMT_ONLY) {
		/*
		 * INIT_TOKEN for MGMT_ONLY is written inside the tight burst,
		 * immediately before INIT_COMPLETE, to ensure exactly one push.
		 * Writing it here and again in the burst causes a fatal bus-lock
		 * (measured 2026-10-05: double-push to non-wiped FIFO → 0x00 crash).
		 */
	} else if (flags & FM_SSCHED_SMALL_RING) {
		static const unsigned char small[] = { 0, 1, 2, 3, SSCHED_MGMT_PORT };

		for (i = 0; i < sizeof small; i++) {
			unsigned port = small[i];
			uint32_t tok = SSCHED_TOKEN(port, 1, 0);

			if ((rv = fm_wr(d, FM6000_SSCHED_RX_INIT_TOKEN, tok)) != FM_OK)
				return rv;
			if ((rv = fm_wr(d, FM6000_SSCHED_TX_INIT_TOKEN, tok)) != FM_OK)
				return rv;
		}
	} else {
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
	/*
	 * SLOW_PORT is a 5-word × 16-bit bitmask: one bit per ring slot (0-79).
	 * "Slow" marks slots whose ports need extra arbitration time (10G ports).
	 *
	 * Word 0 (slots 0-15): 0x000f -- ports 0-3 (locked internal ports) are slow.
	 * Words 1-4: golden values from the EdgeNOS fm6000_sched_std reference. The
	 * pattern 0xffe0, 0xfeff, 0xfff0, 0x0fff marks the front-panel 10G slots
	 * across the remaining 64 ring positions. Writing zeros here (as we did before)
	 * marks all those ports as "fast", which is wrong and may prevent ring advancement
	 * on a cold chip where the scheduler engine relies on correct slow-port timing. [RE]
	 */
	{
		static const uint32_t slow[FM6000_SSCHED_SLOW_PORT_WORDS] = {
			0x0000000fu, 0x0000ffe0u, 0x0000feffu, 0x0000fff0u, 0x00000fffu,
		};

		for (i = 0; i < FM6000_SSCHED_SLOW_PORT_WORDS; i++) {
			if ((rv = fm_wr(d, FM6000_SSCHED_RX_SLOW_PORT(i), slow[i])) != FM_OK)
				return rv;
		}
		/*
		 * TX_SLOW_PORT: the TX ring engine's counterpart to RX_SLOW_PORT.
		 *
		 * RX_SLOW_PORT is at RX base + 0x30 = 0x8070.  The TX block mirrors
		 * the RX block with base 0x8000, so TX_SLOW_PORT should be at
		 * TX base + 0x30 = 0x8030.  No regs.h define existed for this; it sits
		 * in the gap 0x8023..0x803f that was never mapped.
		 *
		 * With RX_SLOW_PORT written but TX_SLOW_PORT left at ECC-uninitialised
		 * defaults, the TX ring engine reads bad ECC on every tick after
		 * INIT_COMPLETE, causing 77 CRM-timeout self-resets (code 0x83) before
		 * the FOUND bit is ever set.  Writing the same values here fixes that.
		 * [RE 2026-10-05]
		 */
		for (i = 0; i < FM6000_SSCHED_SLOW_PORT_WORDS; i++) {
			if ((rv = fm_wr(d, FM6000_SSCHED_TX_SLOW_PORT(i), slow[i])) != FM_OK)
				return rv;
		}
	}

	phase(d, rep, FM_SSCHED_PH_SLOW);

	/*
	 * SWEEPER_CFG_0/1: ring tick reference clock, written AFTER ring SRAMs.
	 *
	 * CFG_0 must be set before INIT_COMPLETE: the ring engine reads it at
	 * INIT_COMPLETE time and commits to it.  Writing CFG_0 after
	 * INIT_COMPLETE is safe but inert -- the engine no longer reacts.
	 *
	 * CFG_0 must be written AFTER the ring SRAMs (NEXT_PORT, INIT_TOKEN,
	 * SLOW_PORT) hold valid data.  Writing CFG_0 before the ring SRAMs are
	 * initialised triggers the SWEEPER on ECC-uninitialised memory and
	 * produces a self-reset storm.  Once the SRAMs carry valid ECC data
	 * (done above) the sweeper can access them safely.
	 *
	 * In nosweep mode write the full golden value including bit 19
	 * (0x0008bb2c).  The non-sweep path skips CFG_2/CFG_3 which would
	 * start the MRL table walk; CFG_0 alone only enables the ring tick
	 * reference, which is safe.
	 *
	 * CFG_1 = 0x2 from the live capture; its role is not fully understood.
	 *
	 * Golden values from the 7150S-52 live capture 2026-10-01. [RE]
	 */
	if ((rv = fm_wr(d, FM6000_SWEEPER_CFG_0, 0x0008bb2cu)) != FM_OK)
		return rv;
	if ((rv = fm_wr(d, FM6000_SWEEPER_CFG_1, 0x00000002u)) != FM_OK)
		return rv;

	/* (freelist and 0x111000 re-written below in PH_START after the settle
	 * wait, because the 0x85 settle-wait resets wipe them) */

	/*
	 * Wait for JSS startup resets to settle.
	 *
	 * JSS_CFG_8=1 and TICK_CFG arm the ring clock domain.  While the domain
	 * stabilises the chip fires self-resets from ECC-uninitialised SRAMs
	 * touched during boot.  Poll fm_fatal_count until 10 consecutive 1 ms
	 * samples agree before writing ring SRAMs and INIT_COMPLETE. [RE 2026-10-05]
	 */
	{
		uint32_t start_cnt = (uint32_t)fm_fatal_count(d);
		uint32_t cnt = start_cnt, prev;
		int stable = 0, n;
		for (n = 0; n < 200; n++) {
			usleep(1000);
			prev = cnt;
			cnt = (uint32_t)fm_fatal_count(d);
			if (cnt == prev) {
				if (++stable >= 10)
					break;
			} else {
				stable = 0;
			}
		}
		if (!fm_alive(d))
			return FM_EOFFBUS;
	}

	if (!fm_alive(d))
		return FM_EOFFBUS;

	/*
	 * Tight burst: ring SRAMs → CFG_0 → INIT_TOKEN → REPLACE_TOKEN → INIT_COMPLETE.
	 *
	 * All ring SRAMs are written immediately before INIT_COMPLETE so any
	 * late JSS startup reset cannot wipe them between write and fire.
	 * MGMT_ONLY deliberately omits CFG_4=0x2000 (ESCHED clock enable):
	 * enabling ESCHED starts an ECC scanner that fires a 0x85 reset ~1.8 μs
	 * later on every ECC-bad entry (all 76 after boot), clearing INIT_COMPLETE
	 * and looping.  Port 78 is above the ESCHED table so the ring engine never
	 * does an ESCHED lookup; the ring circulates without CFG_4. [RE 2026-10-05]
	 */
	{
		uint32_t tok = SSCHED_TOKEN(SSCHED_MGMT_PORT, 1, 0);

		/*
		 * MGMT_ONLY burst: start ring WITHOUT enabling ESCHED (no CFG_4=0x2000).
		 *
		 * CFG_4=0x2000 enables the ESCHED clock domain, which immediately
		 * starts a background ECC scanner that reads all 76 ESCHED entries
		 * (ports 0-75 are ECC-bad after boot).  The scanner fires a 0x85
		 * reset ~1.8μs after CFG_4, which clears INIT_COMPLETE and stops
		 * the ring before FOUND can be observed.  Repeating the burst loops
		 * infinitely: scanner fires on every attempt.
		 *
		 * ESCHED is NOT required for token ring circulation -- it controls
		 * per-port bandwidth allocation (scheduling policy), not the ring's
		 * token advance logic.  Omitting CFG_4 here means the ring ticks
		 * with no ESCHED clock domain, no scanner, no reset storm.  Once
		 * FOUND is confirmed the full ESCHED init (all 76 ports) can be done
		 * in a subsequent step after the ring is known-good. [RE 2026-10-05]
		 *
		 * Ring SRAMs (NEXT_PORT, SLOW_PORT, CFG_0) are ECC-bad after settle
		 * resets and must be written here; they were written in the first
		 * pass but wiped by the settle-phase resets. [RE 2026-10-05]
		 */

		/* 1 */ if ((rv = fm_wr(d, FM6000_SSCHED_RX_NEXT_PORT(0), 0u)) != FM_OK)
			return rv;
		/* 2 */ if ((rv = fm_wr(d, FM6000_SSCHED_TX_NEXT_PORT(0), 0u)) != FM_OK)
			return rv;
		/* 3 */ if ((rv = fm_wr(d, FM6000_SSCHED_RX_SLOW_PORT(0), 0u)) != FM_OK)
			return rv;
		/* 4 */ if ((rv = fm_wr(d, FM6000_SSCHED_TX_SLOW_PORT(0), 0u)) != FM_OK)
			return rv;
		/* 5 */ if ((rv = fm_wr(d, FM6000_SWEEPER_CFG_0, 0x0008bb2cu)) != FM_OK)
			return rv;
		/* 6 */ if ((rv = fm_wr(d, FM6000_SSCHED_RX_INIT_TOKEN, tok)) != FM_OK)
			return rv;
		/* 7 */ if ((rv = fm_wr(d, FM6000_SSCHED_TX_INIT_TOKEN, tok)) != FM_OK)
			return rv;
		/* 8 */ if ((rv = fm_wr(d, FM6000_SSCHED_RX_REPLACE_TOKEN, SSCHED_MGMT_PORT)) != FM_OK)
			return rv;
		/* 9 */ if ((rv = fm_wr(d, FM6000_SSCHED_TX_REPLACE_TOKEN, SSCHED_MGMT_PORT)) != FM_OK)
			return rv;
		/* 10 */ if ((rv = fm_wr(d, FM6000_SSCHED_RX_INIT_COMPLETE, 1u)) != FM_OK)
			return rv;
		/* 11 */ if ((rv = fm_wr(d, FM6000_SSCHED_TX_INIT_COMPLETE, 1u)) != FM_OK)
			return rv;
	}
	if (!fm_alive(d))
		return FM_EOFFBUS;

	phase(d, rep, FM_SSCHED_PH_START);

	fm_sched_mark_ready(d);

	/*
	 * ⚠  Do NOT write REPLACE_TOKEN = 0 here.
	 *
	 * The REPLACE_TOKEN register doubles as the find-request register: writing
	 * a value N with the "found" bit clear asks the running ring engine to
	 * search for port N's token.  Writing 0 therefore requests a find for
	 * port 0 — which is not in the MGMT_ONLY ring.  The engine walks the
	 * ring repeatedly looking for port 0, never finds it, and on each pass
	 * reads an ECC-uninitialised ESCHED entry, triggering a 0xc5 watchdog
	 * reset.  That loops 100+ times until the ring state is fully wiped.
	 *
	 * The FIND phase below issues its own REPLACE_TOKEN write for port 78,
	 * which both starts a new find and clears any stale FOUND state.
	 */
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
	 *
	 * ⚠ PROBE ORDER. In the full 64-token mode we probe four ports from
	 * the START of ssched_ring[] AND four from the END. The INIT_TOKEN
	 * path is a hardware FIFO of unknown capacity; if it overflows, the
	 * early tokens are discarded and the late ones survive. A probe that
	 * only checked early tokens would falsely conclude "not circulating"
	 * when the ring is actually running with the later tokens.
	 */
	{
		/* Pick probe ports: just port 78 for MGMT_ONLY, the five enrolled
		 * ports for SMALL_RING, or first+last 4 for the full ring. */
		unsigned n = sizeof ssched_ring;
		static const unsigned char small_ports[] = { 0, 1, 2, 3, SSCHED_MGMT_PORT };
		unsigned nports;
		uint8_t probe_ports[8];

		if (flags & FM_SSCHED_MGMT_ONLY) {
			nports = 1;
			probe_ports[0] = SSCHED_MGMT_PORT;
		} else if (flags & FM_SSCHED_SMALL_RING) {
			nports = sizeof small_ports;
			for (i = 0; i < nports; i++)
				probe_ports[i] = small_ports[i];
		} else {
			nports = 8;
			for (i = 0; i < 4; i++) {
				probe_ports[i]     = ssched_ring[i];
				probe_ports[i + 4] = ssched_ring[n - 4 + i];
			}
		}

		for (i = 0; i < nports && (circulating == NULL || !*circulating); i++) {
			uint32_t v = 0;
			unsigned port = probe_ports[i];

			/*
			 * Read REPLACE_TOKEN before writing.  When we pre-prime
			 * REPLACE_TOKEN = probe_port before INIT_COMPLETE, the ring
			 * finds its own token on the first visit (slot 0 for
			 * MGMT_ONLY) and sets FOUND before we even reach here.
			 * Writing REPLACE_TOKEN again while the ring is running
			 * clears FOUND, starts a new find, and may conflict with
			 * live ring-engine reads of the same register, causing 0x83
			 * self-resets.  Check FOUND first; only write if it is not
			 * already set. [RE 2026-10-05]
			 */
			if ((rv = fm_rd(d, FM6000_SSCHED_RX_REPLACE_TOKEN, &v)) != FM_OK)
				return rv;
			if (!(v & FM6000_SSCHED_RX_FOUND)) {
				if ((rv = fm_wr(d, FM6000_SSCHED_RX_REPLACE_TOKEN, port)) != FM_OK)
					return rv;
				usleep(FM6000_SSCHED_FIND_US);
				if ((rv = fm_rd(d, FM6000_SSCHED_RX_REPLACE_TOKEN, &v)) != FM_OK)
					return rv;
			}
			if (v & FM6000_SSCHED_RX_FOUND) {
				fm_sched_mark_ready(d);
				if (circulating != NULL)
					*circulating = 1;
				break;
			}

			if ((rv = fm_rd(d, FM6000_SSCHED_TX_REPLACE_TOKEN, &v)) != FM_OK)
				return rv;
			if (!(v & FM6000_SSCHED_TX_FOUND)) {
				if ((rv = fm_wr(d, FM6000_SSCHED_TX_REPLACE_TOKEN, port)) != FM_OK)
					return rv;
				usleep(FM6000_SSCHED_FIND_US);
				if ((rv = fm_rd(d, FM6000_SSCHED_TX_REPLACE_TOKEN, &v)) != FM_OK)
					return rv;
			}
			if (v & FM6000_SSCHED_TX_FOUND) {
				fm_sched_mark_ready(d);
				if (circulating != NULL)
					*circulating = 1;
				break;
			}
		}
	}

	phase(d, rep, FM_SSCHED_PH_FIND);
	if (rep != NULL) {
		struct fm_fatal f;

		if (fm_fatal_read(d, &f) == FM_OK)
			rep->last_fatal = f.last;
	}

	return fm_alive(d) ? FM_OK : FM_EOFFBUS;
}
