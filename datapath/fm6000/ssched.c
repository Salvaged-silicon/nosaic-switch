/* SPDX-License-Identifier: Apache-2.0 */
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
static void ssched_dbg(const char *msg)
{
	int fd;
	size_t n = strlen(msg);

	(void)write(2, msg, n);
	(void)write(2, "\n", 1);

	fd = open("/dev/kmsg", O_WRONLY | O_NOCTTY);
	if (fd >= 0) { (void)write(fd, msg, n); (void)write(fd, "\n", 1); close(fd); }
}
static void nap_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}
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
#include "cmrest.h"
#include "cmwm.h"
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

	ssched_dbg("nosd-fm6000: SSCHED ring_init enter");
	if (circulating != NULL)
		*circulating = 0;
	if (rep != NULL) {
		memset(rep, 0, sizeof(*rep));
		rep->mark = fm_fatal_count(d);
		ssched_dbg("nosd-fm6000: SSCHED fatal_count done");
	}

	/* The SBus master must be running before any JSS SBus transaction.
	 * fm_sbus_start() clears SBUS_CFG (which holds the controller in
	 * reset if non-zero) and takes SPICO out of reset. Safe to call
	 * repeatedly. */
	if ((rv = fm_sbus_start(d)) != FM_OK)
		return rv;
	ssched_dbg("nosd-fm6000: SSCHED sbus_start done");

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
	if (flags & FM_SSCHED_NEXT_CHAIN)
		ssched_dbg("nosd-fm6000: SSCHED NEXT_CHAIN enter");
	/* ⚠ Select SCAN_CHAIN_CORE before any data write.  mrl.c shift() always
	 * sets SCAN_CONTROL before touching a data-in register; omitting it
	 * leaves the write on whatever chain the power-on default selects
	 * (unknown, unmeasured, likely not 0x10).  [RE 2026-10-19, install47] */
	if ((rv = fm_wr(d, FM6000_SCAN_CONTROL, FM6000_SCAN_CHAIN_CORE)) != FM_OK)
		return rv;
	(void)fm_wr(d, FM6000_SCAN_CONFIG_DATA_IN, 0x88800000u);
	(void)fm_wr(d, FM6000_SCAN_CONFIG_DATA_IN, 0x88008000u);
	(void)fm_wr(d, FM6000_SCAN_CONFIG_DATA_IN, 0x80000040u);
	if ((rv = fm_wr(d, FM6000_SCAN_CHAIN_DATA_IN, 0xffffffffu)) != FM_OK)
		return rv;
	{
		uint32_t _ss1 = 0;
		char _scb1[80];
		(void)fm_rd(d, FM6000_SCAN_STATUS, &_ss1);
		snprintf(_scb1, sizeof _scb1,
			"nosd-fm6000: SCAN_STATUS pre-JSS step5: 0x%08x (0x100=retired)",
			_ss1);
		ssched_dbg(_scb1);
	}

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
		/* Each enrolled port points at the next in service order; the
		 * last enrolled port points to port 78 (management), and port
		 * 78 wraps to the first enrolled port: a 65-hop NEXT_PORT
		 * chain with 64 INIT_TOKENs.
		 *
		 * ⚠ Port 78 is in NEXT_PORT but NOT in INIT_TOKEN.
		 *
		 * The INIT_TOKEN FIFO holds exactly 64 entries. Writing 65
		 * tokens (ssched_ring[] + port 78) overflows the FIFO; the
		 * chip uses head-drop, discarding port 20 (the first write).
		 * NEXT_PORT[78]=20 then points to a slot that does not exist,
		 * stalling the ring permanently (measured install41, circ=0).
		 *
		 * Writing only the 64 ssched_ring[] tokens to INIT_TOKEN and
		 * splicing port 78 into NEXT_PORT only (no token) matches the
		 * c5567d2 topology that first achieved TX circulation. The
		 * engine skips the empty port-78 slot and follows
		 * NEXT_PORT[78]=20 on each traversal. [RE 2026-10-23, install60] */
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
	if ((rv = fm_wr(d, FM6000_PLL_CTRL_0, FM6000_PLL_CTRL_0_GOLDEN)) != FM_OK) {
		ssched_dbg("nosd-fm6000: SSCHED PLL_CTRL_0 wr fail");
		return rv;
	}
	if ((rv = fm_wr(d, FM6000_PLL_CTRL_1, FM6000_PLL_CTRL_1_GOLDEN)) != FM_OK) {
		ssched_dbg("nosd-fm6000: SSCHED PLL_CTRL_1 wr fail");
		return rv;
	}
	/*
	 * DLL_CTRL is write-only (reads as 0); re-write to ensure the DLLs
	 * are enabled regardless of what the boot fill reset cleared. [RE]
	 */
	if ((rv = fm_wr(d, FM6000_DLL_CTRL, FM6000_DLL_CTRL_ENABLE)) != FM_OK) {
		ssched_dbg("nosd-fm6000: SSCHED DLL_CTRL wr fail");
		return rv;
	}

	/*
	 * Wait for PLL lock before enabling the clock domain.
	 * PLL_STATUS bits [1:0] = PLL pair, [3:2] = DLLs. [RE, 2026-10-01]
	 */
	{
		unsigned waited;
		uint32_t pll = 0;
		char _pl[96];

		for (waited = 0; waited < FM6000_PLL_LOCK_MAX_MS; waited++) {
			if ((rv = fm_rd(d, FM6000_PLL_STATUS, &pll)) != FM_OK) {
				snprintf(_pl, sizeof _pl,
					"nosd-fm6000: SSCHED PLL_STATUS rd fail waited=%u",
					waited);
				ssched_dbg(_pl);
				return rv;
			}
			if ((pll & FM6000_PLL_STATUS_LOCKED_ALL) ==
			    FM6000_PLL_STATUS_LOCKED_ALL)
				break;
			usleep(1000);
		}
		snprintf(_pl, sizeof _pl,
			"nosd-fm6000: SSCHED PLL lock waited=%u pll=0x%08x",
			waited, pll);
		ssched_dbg(_pl);
		/* Proceed even if the DLLs do not lock -- PLLs in bits [1:0]
		 * are the clock source; a DLL miss will show up later. */
	}

	/*
	 * install128: clear CFG_4 before writing CM_PORT_TXMP_USAGE (0x118800).
	 *
	 * On an initial cold boot CFG_4=0 at this point (SchedPeriod never armed).
	 * On a re-init after a running ring (e.g. install125+ PCIe SerDes re-init),
	 * CFG_4=0x2000 persists through any number of chip FALTALs -- the SchedPeriod
	 * from the prior boot survives.  Writing 0x118800 while CFG_4=0x2000 arms the
	 * CM monitor; the monitor fires immediately against the un-settled 0x118800
	 * values → chip self-reset → fm_alive() fails → circ=0.
	 *
	 * Fix: explicitly clear CFG_4 here so the write below is safe regardless of
	 * prior state.  CFG_4=0x2000 is re-written in the START burst just before
	 * IC=1 (install51). [RE 2026-10-27, install128]
	 */
	if ((rv = fm_wr(d, FM6000_SWEEPER_CFG_4, 0u)) != FM_OK) {
		ssched_dbg("nosd-fm6000: SSCHED CFG_4 clear fail");
		return rv;
	}

	/*
	 * CM_PORT_TXMP_USAGE (0x118800): first fill BEFORE fm_boot_freelists.
	 *
	 * install51 regression (2026-10-20): moving this fill to AFTER
	 * fm_boot_freelists changed the chip state at the time of the post-settle
	 * scan-chain write and produced SCAN_STATUS=0x144 (extra bits 2+6) instead
	 * of 0x100.  MGMT_ONLY probe then never fired FOUND.
	 *
	 * The correct position is HERE, before freelists.  fm_boot_freelists issues
	 * ~42 chip-wide soft-resets that wipe this fill -- but MGMT_ONLY never arms
	 * the CM monitor (CFG_4=0 throughout), so the wipe is harmless for that pass.
	 * NEXT_CHAIN re-fills 0x118800 after settle (see below), just before
	 * CFG_4=0x2000 is written in the START burst. [RE 2026-10-20]
	 *
	 * install131: skip this fill for NEXT_CHAIN.
	 *
	 * On a re-init (NEXT_CHAIN) the previous ring_init left non-zero cell counts
	 * in TXMP_USAGE (traffic flowed: tx_sent=15, some cells lost to tx_ringfull).
	 * Those counts persist through any number of chip FALTALs, through
	 * fm_boot_freelists, and through BIST.  Writing 0 to non-zero counts while
	 * the CM's internal cell tracking still shows allocated cells triggers a CM
	 * consistency check → chip self-reset (FATAL 0x00) → fm_alive() fails →
	 * FM_EOFFBUS.
	 *
	 * For NEXT_CHAIN this fill is unnecessary: the internal fm_boot_freelists
	 * call below (line 430) frees every cell and zeros TXMP_USAGE via its ECC-
	 * init storm.  The second fill (line 758) then writes 0 to a CM that has 0
	 * cells allocated -- the write is consistent with hardware state -- and
	 * succeeds.  [RE 2026-10-27, install131]
	 *
	 * For MGMT_ONLY this fill remains: MGMT_ONLY never runs traffic so the
	 * counts are always 0, and the fill is needed to avoid SCAN_STATUS=0x144
	 * in the post-settle scan-chain write (install51 regression).
	 */
	if (!(flags & FM_SSCHED_NEXT_CHAIN)) {
		if ((rv = fm_mem_fill(d, 0x118800u, 960u, 0u)) != FM_OK) {
			ssched_dbg("nosd-fm6000: SSCHED mem_fill 0x118800 fail");
			return rv;
		}
		ssched_dbg("nosd-fm6000: SSCHED mem_fill 0x118800 done");
		if (!fm_alive(d)) {
			ssched_dbg("nosd-fm6000: SSCHED alive fail after mem_fill");
			return FM_EOFFBUS;
		}
	} else {
		ssched_dbg("nosd-fm6000: SSCHED mem_fill 0x118800 skipped (NEXT_CHAIN, re-fill after settle)");
	}

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
	if ((rv = fm_boot_freelists(d)) != FM_OK) {
		ssched_dbg("nosd-fm6000: SSCHED boot_freelists fail");
		return rv;
	}
	ssched_dbg("nosd-fm6000: SSCHED boot_freelists done");
	/*
	 * install228: 200ms CRM-block settle wait.
	 *
	 * fm_boot_freelists() causes 42 chip self-resets (ECC-init storm, code
	 * 0xd0). The CRM block's LBUS interface needs ~200ms to come out of
	 * reset before its registers respond via LBUS. Without this wait, the
	 * first fm_rd(FM6000_CRM_STATUS) in fm_crm_memset() stalls the SCD's
	 * LBUS bridge indefinitely (no timeout), freezing the CPU.
	 * [Measured 2026-11-13: 42 resets take ≤84ms; 200ms is a safe margin]
	 */
	nap_ms(200);
	if ((rv = fm_crm_memset(d, 0, 0x111000u, 2432u, FM_CRM_SIZE_32, 0u)) != FM_OK) {
		ssched_dbg("nosd-fm6000: SSCHED crm_memset 0x111000 fail");
		return rv;
	}
	ssched_dbg("nosd-fm6000: SSCHED crm_memset 0x111000 done");

	/*
	 * install75: PLL_STATUS readback after freelists.
	 *
	 * fm_boot_freelists() causes 42 chip self-resets (code 0xd0).  PLL_CTRL_0/1
	 * were written above (lines ~321-323) and the comment there says
	 * "the 78+ watchdog resets during step 10's fill clear every register."
	 * That is about step 10's fill resets, not fm_boot_freelists' ECC-init
	 * resets.  Whether 0xd0 ECC-init resets also clear PLL_CTRL is unknown.
	 * If PLL_CTRL is cleared, the clock domain is dead at IC=1 time: the ring
	 * can never advance and TX_FOUND is always 0, with no chip resets (the
	 * ring engine simply does nothing).
	 *
	 * This read is safe: PLL_STATUS (0x1c046) is a management-block register,
	 * not in the ESCHED, EPL, or bank-memory hazard zones. [RE install75]
	 */
	if (flags & FM_SSCHED_NEXT_CHAIN) {
		uint32_t _pll75 = 0;
		char _pb75[96];
		(void)fm_rd(d, FM6000_PLL_STATUS, &_pll75);
		snprintf(_pb75, sizeof _pb75,
			"nosd-fm6000: post-freelists PLL_STATUS=0x%08x (want 0xf=all-locked, 0x3=PLLs-only-ok)",
			_pll75);
		ssched_dbg(_pb75);
	}

	/*
	 * NEXT_CHAIN only: IC=0 clear.
	 *
	 * install49 showed that the MGMT_ONLY ring is already dead (FOUND=0)
	 * even BEFORE fm_boot_freelists runs — the PCIe link-up reset kills the
	 * SSCHED FSM between MGMT_ONLY ring_init and NEXT_CHAIN ring_init.
	 * The chip is in a clean RESET state (IC=0 already) when NEXT_CHAIN
	 * ring_init starts.
	 *
	 * install44 established: writing IC=0 to a cold chip (IC already 0)
	 * disrupts the ring engine FSM — MGMT_ONLY probe failed (circ=0).
	 * install49 writes IC=0 after freelists (which also clear IC to 0 via
	 * chip-wide soft-reset), putting the FSM into a "STOPPED" state from
	 * which IC=1 cannot restart the ring.
	 *
	 * install50: do NOT write IC=0.  The chip is already in RESET state;
	 * writing IC=0 corrupts the FSM.  IC=1 in the START burst fires on
	 * a clean FSM and should start the ring correctly. [RE 2026-10-20]
	 *
	 * NOTE: the "INIT_COMPLETE clear" log message is kept to preserve the
	 * readback diagnostic, but the IC=0 write is removed.
	 */
	if (flags & FM_SSCHED_NEXT_CHAIN) {
		uint32_t ic_rb = 0xffu;
		char _ic[96];

		(void)fm_rd(d, FM6000_SSCHED_RX_INIT_COMPLETE, &ic_rb);
		snprintf(_ic, sizeof _ic,
			"nosd-fm6000: INIT_COMPLETE clear: RX_IC=0x%08x (expect 0 from PCIe reset)",
			ic_rb);
		ssched_dbg(_ic);
	}
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
	 * install73: the NEXT_CHAIN scan chain re-write (install55) is removed.
	 *
	 * install55 moved the scan chain write to here (after fm_boot_freelists,
	 * before JSS enable) because pcie_init had re-applied the chain between
	 * MGMT_ONLY and NEXT_CHAIN ring_init, leaving SCAN_STATUS=0x1cc.  That
	 * was the correct fix at the time.
	 *
	 * PCIe initialisation is now deferred to AFTER ring_init completes.  The
	 * chip enters NEXT_CHAIN ring_init with SCAN_STATUS=0x100 (set by the
	 * PCIe-reset that kills the MGMT_ONLY ring) and fm_boot_freelists()
	 * leaves it at 0x100.  Re-applying the scan chain here takes
	 * SCAN_STATUS 0x100 → 0x144 → 0x188.
	 *
	 * c5567d2 (first confirmed TX circulation) fired IC=1 with SCAN_STATUS
	 * at 0x100 and TX circulated.  Every build since install55 fired IC=1
	 * with SCAN_STATUS at 0x188 and TX did NOT circulate.  The extra scan
	 * chain writes are the prime suspect; removing them restores the
	 * c5567d2 condition. [RE 2026-10-24, install73]
	 */

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
	 *      this is 1 on a running switch. After the 42 chip soft-resets from
	 *      fm_boot_freelists() above, JSS_CFG_8 is 0 at reset default, so
	 *      this is always a fresh 0→1 transition. [RE 2026-10-01]
	 */
	if ((rv = fm_sbus_write(d, 0x45, 0x2a, 0x16u)) != FM_OK) {
		ssched_dbg("nosd-fm6000: SSCHED sbus 0x45 jss-clk fail");
		return rv;
	}
	if ((rv = fm_wr(d, FM6000_JSS_CFG_8, 0x00000001u)) != FM_OK) {
		ssched_dbg("nosd-fm6000: SSCHED JSS_CFG_8 wr fail");
		return rv;
	}

	if ((rv = fm_wr(d, FM6000_SSCHED_TICK_CFG, FM6000_SSCHED_TICK_PERIOD)) != FM_OK) {
		ssched_dbg("nosd-fm6000: SSCHED TICK_CFG wr fail");
		return rv;
	}

	phase(d, rep, FM_SSCHED_PH_TICK);

	/*
	 * Clear CFG_4 so a previous run's SchedPeriod does not fire into
	 * the freshly-enabled clock domain before INIT_COMPLETE. [RE]
	 *
	 * ⚠ install51: this is kept (so a stale SchedPeriod does not trigger
	 * CM-monitor reads against a possibly-stale 0x118800 during JSS startup
	 * before settle), but CFG_4=0x2000 is written in the START burst, just
	 * before IC=1, to re-arm the ring engine.  [RE 2026-10-20]
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
	 * ESCHED (0x2000-0x3fff): not written here or by boot.c.
	 *
	 * This block is not writable via the LBUS.  Measured 2026-10-13:
	 * the first write to any address in 0x002000-0x003fff kills the chip
	 * regardless of ring state, CFG_4, or value (see esched.c).  Without
	 * CFG_4 the bridge queue fills at ~20 writes; with CFG_4 the ring
	 * processes write 1 and immediately faults on ECC-uninit DRR_Q.
	 * fm_esched_init() is a stub until step 9 (PCIe) is implemented.
	 *
	 * The ring circulates with zero scheduling policy (post-BIST
	 * defaults), which is sufficient for basic bring-up testing.
	 */
	phase(d, rep, FM_SSCHED_PH_ESCHED);

	/*
	 * Wait for JSS startup resets to settle.
	 *
	 * install74: NEXT_CHAIN token loading moved to AFTER settle.
	 *
	 * install53 loaded tokens before the settle, claiming settle's ~9
	 * watchdog resets do not flush the INIT_TOKEN FIFO.  In practice, from
	 * install55 through install73, EVERY build with NEXT_CHAIN has seen
	 * RX circulate (RX_FOUND=1) and TX remain stuck (TX_FOUND=0), with
	 * SCAN_STATUS=0x100 and NEXT_PORT/SLOW_PORT written correctly.  The
	 * only remaining asymmetry between RX and TX at IC=1 time is the FIFO
	 * contents loaded before the settle.  If the settle's ~9 resets flush
	 * TX_INIT_TOKEN but not RX_INIT_TOKEN (different hardware domains),
	 * TX has no tokens to carry and will never circulate.
	 *
	 * Moving NEXT_CHAIN token loading to after settle (same pattern as
	 * MGMT_ONLY's in-burst token write) ensures both FIFOs are freshly
	 * loaded on a stable chip, immediately before IC=1. [RE 2026-10-24, install74]
	 *
	 * MGMT_ONLY uses a re-write burst after settle; NEXT_CHAIN tokens now
	 * loaded post-settle (below).
	 */
	{
		uint32_t cnt, prev;
		int stable = 0, n;
		ssched_dbg("nosd-fm6000: SSCHED settle start");
		cnt = (uint32_t)fm_fatal_count(d);
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
		{
			char _sbuf[96];
			int _alive = fm_alive(d);
			snprintf(_sbuf, sizeof _sbuf,
				"nosd-fm6000: SSCHED settle done n=%d stable=%d cnt=%u alive=%d",
				n, stable, cnt, _alive);
			ssched_dbg(_sbuf);
			if (!_alive)
				return FM_EOFFBUS;
		}
	}

	/*
	 * Re-initialise ESCHED after settle.
	 *
	 * The settle resets (~197 in practice) wipe the ESCHED CFG_1/CFG_2
	 * entries written by fm_esched_init() in boot.c Phase 1 (MGMT_ONLY).
	 * Ports 1-3 and 12-19 are in ssched_ring[] but are not front-panel;
	 * without a write after settle, those entries are ECC-bad when the
	 * NEXT_CHAIN probe starts.  The ring engine reads their CFG entries on
	 * every pass, triggering 0xc5 watchdog resets (~2/s × 80 s = ~154
	 * total, circ=0).
	 *
	 * Written HERE (after settle, chip stable) so the writes land on a
	 * chip that will not reset under them.  fm_esched_init() is a no-op
	 * when d->pcie_regs is NULL (BAR0 not mapped).  The self-resets during
	 * settle are scheduler-domain resets; the PCIe endpoint (BAR0) remains
	 * mapped throughout and is valid at this point.  [RE 2026-10-16]
	 */
	(void)fm_esched_init(d, NULL, NULL);

	/*
	 * Re-fill CM_PORT_TXMP_USAGE (0x118800) for NEXT_CHAIN after settle.
	 *
	 * fm_boot_freelists() wiped the initial fill above.  The settle's ~9
	 * resets do NOT re-wipe CM SRAM (only the FREELISTS_ALL ECC-init storm
	 * does).  This fill lands on a stable chip: IC=0 (ring not running) and
	 * CFG_4=0 (CM monitor not yet armed), so no concurrent ring reads of
	 * 0x118800 are possible.  It must be present before CFG_4=0x2000 is
	 * written in the START burst. [RE 2026-10-20, install51 regression]
	 */
	if (flags & FM_SSCHED_NEXT_CHAIN) {
		/*
		 * Re-fill all CM SRAM banks (post-settle, TICK_CFG active).
		 *
		 * fm_boot_freelists() wipes every CM SRAM bank via its ECC-init
		 * storm (~42 soft resets).  The settle's ~9 resets are scheduler-
		 * domain watchdog resets that do NOT re-wipe CM SRAM.  TICK_CFG was
		 * written above (line 508) and esched_init is a NO-OP here because
		 * pcie_regs is NULL; TICK_CFG=2 is therefore still active at this
		 * point.
		 *
		 * This is the only safe window:
		 *   - TICK_CFG=2 active  (needed for CM SRAM writes)
		 *   - IC=0               (ring not yet running)
		 *   - CFG_4=0            (CM monitor not armed)
		 *
		 * The CM monitor is armed in the START burst just before IC=1 (below).
		 * Any CM SRAM bank with uninitialized ECC at that point will trigger
		 * ECC faults on every monitor tick, producing continuous soft resets
		 * that prevent pipeline_init from writing stable values.
		 *
		 * Values match memfill.c: watermark banks 0xffffffff (admit-all)
		 * until pipeline_init overwrites; counter and map banks are zeros.
		 * Established by install101→102 (2026-10-26): CMWM pre-fill in
		 * pipeline_init failed with TICK_CFG=0; moving fills here succeeds.
		 */
		ssched_dbg("nosd-fm6000: SSCHED CM SRAM refill (post-settle)");
		/* CM live-counter banks */
		if ((rv = fm_mem_fill(d, 0x111000u, 2432u, 0u)) != FM_OK)
			return rv;
		if ((rv = fm_mem_fill(d, 0x118800u, 960u, 0u)) != FM_OK)
			return rv;
		/*
		 * CM watermark banks -- 0xffffffff = admit-all until fm_cmwm_init.
		 *
		 * cmwm_port uses CMWM_PORT_STRIDE=16 between ports even when only
		 * CMWM_TC=12 classes are written per port.  The SRAM therefore spans
		 * 76×16=1216 words for the RX banks (not 76×12=912).  Filling only
		 * 912 leaves ports 57-75 with uninitialised ECC; the first fm_wr by
		 * fm_cmwm_init to offset 912 (port 57, class 0) triggers an internal
		 * ECC RMW fault and kills the chip.
		 */
		if ((rv = fm_mem_fill(d, 0x112800u, 1216u, 0xffffffffu)) != FM_OK)
			return rv;
		if ((rv = fm_mem_fill(d, 0x113000u, 1216u, 0xffffffffu)) != FM_OK)
			return rv;
		/* TXMP_PRIVATE (0x113800): 80 ports x 16 classes = 1280 words.
		 * Not in memfill.c (write-only via external bus), but the SRAM
		 * still does an internal ECC RMW on every fm_wr. */
		if ((rv = fm_mem_fill(d, 0x113800u, 1280u, 0x00003fffu)) != FM_OK)
			return rv;
		if ((rv = fm_mem_fill(d, 0x115000u, 1216u, 0xffffffffu)) != FM_OK)
			return rv;
		if ((rv = fm_mem_fill(d, 0x115800u, 1216u, 0xffffffffu)) != FM_OK)
			return rv;
		if ((rv = fm_mem_fill(d, 0x117000u, 912u, 0x20001000u)) != FM_OK)
			return rv;
		/* CM map and threshold banks */
		/*
		 * CM_SHARED_RXMP_PAUSE_ON_WM (0x116000, 12 words) and
		 * CM_SHARED_RXMP_PAUSE_OFF_WM (0x116010, 12 words) must be
		 * ECC-initialised before the per-port PAUSE thresholds are
		 * written by cmwm.c.  Writing 0x4000c000 to any active port's
		 * RXMP_PAUSE_ON entry (0x115000) activates the CM's PAUSE
		 * evaluation circuit, which immediately reads the shared
		 * thresholds.  Without prior ECC init the read triggers an ECC
		 * fault that kills the chip (observed as alive=0 3ms after
		 * CMWM chk5 -- all 6512 per-port writes completed but the chip
		 * died in the fm_alive() check that follows).
		 * 0xffffffff = "no shared limit" until cmrest.c overwrites.
		 *
		 * CM_PAUSE_CFG (0x116400, 76 ports x 4 words = 304 words) has
		 * the same problem: the CM reads it when processing the PAUSE
		 * frame schedule.  Zero is safe here; cmrest.c overwrites with
		 * the parked and running values immediately after pipeline_init.
		 */
		if ((rv = fm_mem_fill(d, 0x116000u, 24u, 0xffffffffu)) != FM_OK)
			return rv;
		if ((rv = fm_mem_fill(d, 0x116400u, 304u, 0u)) != FM_OK)
			return rv;
		if ((rv = fm_mem_fill(d, 0x116600u, 304u, 0u)) != FM_OK)
			return rv;
		if ((rv = fm_mem_fill(d, 0x117800u, 76u, 0u)) != FM_OK)
			return rv;
		if ((rv = fm_mem_fill(d, 0x116100u, 152u, 0u)) != FM_OK)
			return rv;
		if ((rv = fm_mem_fill(d, 0x116200u, 76u, 0u)) != FM_OK)
			return rv;
		if ((rv = fm_mem_fill(d, 0x114000u, 1280u, 0x00003fffu)) != FM_OK)
			return rv;
		/*
		 * Watermarks: written here with TICK_CFG=2 and IC=0 (ring not
		 * started, CM monitor not armed).  Individual fm_wr writes to CM
		 * SRAM while IC=1 compete with the monitor for the SRAM bus;
		 * the monitor can catch SRAM mid-write (ECC transition) and
		 * trigger a soft reset that kills the chip.  The fills above
		 * ensure ECC is valid on every word before the first fm_wr.
		 */
		if ((rv = fm_cmwm_init(d, NULL)) != FM_OK)
			return rv;
		ssched_dbg("nosd-fm6000: SSCHED CMWM done (TICK_CFG=2, IC=0)");
		if ((rv = fm_cmrest_init(d, NULL)) != FM_OK)
			return rv;
		ssched_dbg("nosd-fm6000: SSCHED CMREST done (TICK_CFG=2, IC=0)");
	}

	/*
	 * Post-settle SCAN_STATUS readback (diagnostic only).
	 *
	 * install73: the NEXT_CHAIN scan chain re-write is removed.  SCAN_STATUS
	 * is left at 0x100 by fm_boot_freelists(); the settle resets (~9) are
	 * scheduler-domain watchdog resets that do not affect the scan chain
	 * domain.  Expect 0x100 here, matching c5567d2 at IC=1. [RE install73]
	 */
	{
		uint32_t _ss2 = 0;
		char _scb2[80];
		(void)fm_rd(d, FM6000_SCAN_STATUS, &_ss2);
		snprintf(_scb2, sizeof _scb2,
			"nosd-fm6000: SCAN_STATUS post-settle step5: 0x%08x (0x100=retired)",
			_ss2);
		ssched_dbg(_scb2);
	}
	if (!fm_alive(d))
		return FM_EOFFBUS;

	/* Insert the ring tokens.  Written after the settle.
	 *
	 * MGMT_ONLY:   token written inside the START burst (no write here).
	 * NEXT_CHAIN:  tokens written here, after settle, freshly before IC=1.
	 *              install74: moved from pre-settle; see the comment above
	 *              the settle block for the full rationale.
	 * SMALL_RING:  five locked ports written here before the burst. */
	if (flags & FM_SSCHED_MGMT_ONLY) {
		/*
		 * INIT_TOKEN for MGMT_ONLY is written inside the tight burst,
		 * immediately before INIT_COMPLETE, to ensure exactly one push.
		 * Writing it here and again in the burst causes a fatal bus-lock
		 * (measured 2026-10-05: double-push to non-wiped FIFO → 0x00 crash).
		 */
	} else if (flags & FM_SSCHED_NEXT_CHAIN) {
		unsigned _n = sizeof ssched_ring;
		ssched_dbg("nosd-fm6000: SSCHED NEXT_CHAIN tokens post-settle");
		for (i = 0; i < _n; i++) {
			unsigned port = ssched_ring[i];
			uint32_t tok = SSCHED_TOKEN(port, ssched_locked(port), 0);
			if ((rv = fm_wr(d, FM6000_SSCHED_RX_INIT_TOKEN, tok)) != FM_OK)
				return rv;
			if ((rv = fm_wr(d, FM6000_SSCHED_TX_INIT_TOKEN, tok)) != FM_OK)
				return rv;
		}
		if (!fm_alive(d))
			return FM_EOFFBUS;
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

	/* Diagnostic: read back NEXT_PORT words 4+5 for both RX and TX. */
	if (flags & FM_SSCHED_NEXT_CHAIN) {
		uint32_t rx4=0, rx5=0, tx4=0, tx5=0;
		char _nb[192];
		(void)fm_rd(d, FM6000_SSCHED_RX_NEXT_PORT(5), &rx5);
		(void)fm_rd(d, FM6000_SSCHED_RX_NEXT_PORT(4), &rx4);
		(void)fm_rd(d, FM6000_SSCHED_TX_NEXT_PORT(5), &tx5);
		(void)fm_rd(d, FM6000_SSCHED_TX_NEXT_PORT(4), &tx4);
		snprintf(_nb, sizeof _nb,
			"nosd-fm6000: NEXT_PORT RB: RX w4=0x%08x w5=0x%08x TX w4=0x%08x w5=0x%08x (p20next RX=%u TX=%u, want 24)",
			rx4, rx5, tx4, tx5,
			(unsigned)(rx5 & 0xffu), (unsigned)(tx5 & 0xffu));
		ssched_dbg(_nb);

		/*
		 * install75: full TX vs RX NEXT_PORT comparison.
		 *
		 * Words 4 and 5 are confirmed matching above.  Word 19 covers port 78
		 * (MGMT_PORT, byte 2): NEXT_PORT[78] = ssched_ring[0] = 20 (0x14).
		 * If TX_NEXT_PORT(19) fails to write (e.g., address bug or silent drop),
		 * it stays at reset default 0, so NEXT_PORT[78]=0 for TX.  The TX ring
		 * then follows 78→0→21→…→19→78 (cycle missing ports 20, 24, 28).
		 * REPLACE_TOKEN=20 never fires FOUND.  This would explain RX_FOUND=1
		 * (RX word 19 written correctly, port 20 in RX ring) while TX_FOUND=0.
		 *
		 * Log word 0 (locked ports 0-3) and word 19 (MGMT_PORT) for TX and RX.
		 * Also scan all 20 words and log any TX/RX mismatch. [RE install75]
		 */
		{
			uint32_t rx0=0, rx19=0, tx0=0, tx19=0;
			char _mb[128];
			int _mis = 0, _w;
			unsigned _mw = 0, _mrx = 0, _mtx = 0;

			(void)fm_rd(d, FM6000_SSCHED_RX_NEXT_PORT(0),  &rx0);
			(void)fm_rd(d, FM6000_SSCHED_TX_NEXT_PORT(0),  &tx0);
			(void)fm_rd(d, FM6000_SSCHED_RX_NEXT_PORT(19), &rx19);
			(void)fm_rd(d, FM6000_SSCHED_TX_NEXT_PORT(19), &tx19);
			snprintf(_mb, sizeof _mb,
				"nosd-fm6000: NEXT_PORT w0: RX=0x%08x TX=0x%08x  w19: RX=0x%08x TX=0x%08x (p78next: RX=%u TX=%u want 20)",
				rx0, tx0, rx19, tx19,
				(unsigned)((rx19 >> 16) & 0xffu),
				(unsigned)((tx19 >> 16) & 0xffu));
			ssched_dbg(_mb);

			/* Scan all 20 words for TX/RX mismatch. */
			for (_w = 0; _w < FM6000_SSCHED_NEXT_PORT_WORDS; _w++) {
				uint32_t _rv = 0, _tv = 0;
				(void)fm_rd(d, FM6000_SSCHED_RX_NEXT_PORT(_w), &_rv);
				(void)fm_rd(d, FM6000_SSCHED_TX_NEXT_PORT(_w), &_tv);
				if (_rv != _tv && !_mis) {
					_mis = 1;
					_mw  = (unsigned)_w;
					_mrx = _rv;
					_mtx = _tv;
				}
			}
			if (_mis) {
				char _xb[128];
				snprintf(_xb, sizeof _xb,
					"nosd-fm6000: NEXT_PORT MISMATCH first at w%u: RX=0x%08x TX=0x%08x",
					_mw, _mrx, _mtx);
				ssched_dbg(_xb);
			} else {
				ssched_dbg("nosd-fm6000: NEXT_PORT all 20 words RX==TX ok");
			}
		}
	}

	phase(d, rep, FM_SSCHED_PH_VISIT);

	/*
	 * The slow-port mask, all five words.
	 *
	 * SLOW_PORT is a 5-word × 16-bit bitmask: one bit per ring slot (0-79).
	 * "Slow" marks slots whose ports need extra arbitration time (10G ports).
	 *
	 * Word 0 (slots 0-15): 0x000f -- ports 0-3 (locked internal ports) are slow.
	 * Words 1-4: golden values from the EdgeNOS fm6000_sched_std reference. The
	 * pattern 0xffe0, 0xfeff, 0xfff0, 0x0fff marks the front-panel 10G slots
	 * across the remaining 64 ring positions. [RE]
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
		 * TX_SLOW_PORT: restored to match c5567d2 (first confirmed TX
		 * circulation).  At c5567d2, writing TX_SLOW_PORT[0-4] with the same
		 * golden values as RX_SLOW_PORT was present and TX circulated.
		 * install60 still failed with port-78 restored but TX_SLOW_PORT absent.
		 * [RE 2026-10-23, install61]
		 */
		for (i = 0; i < FM6000_SSCHED_SLOW_PORT_WORDS; i++) {
			if ((rv = fm_wr(d, FM6000_SSCHED_TX_SLOW_PORT(i), slow[i])) != FM_OK)
				return rv;
		}
	}

	phase(d, rep, FM_SSCHED_PH_SLOW);

	/*
	 * SWEEPER_CFG_0/1: ring tick reference clock.
	 *
	 * CFG_0 must be set before INIT_COMPLETE (the ring engine reads it at
	 * INIT_COMPLETE time) and after ring SRAMs hold valid ECC data (writing
	 * it before SLOW_PORT/NEXT_PORT triggers the SWEEPER on uninitialised
	 * memory). [RE]
	 */
	if ((rv = fm_wr(d, FM6000_SWEEPER_CFG_0, 0x0008bb2cu)) != FM_OK)
		return rv;
	if ((rv = fm_wr(d, FM6000_SWEEPER_CFG_1, 0x00000002u)) != FM_OK)
		return rv;

	if (flags & FM_SSCHED_NEXT_CHAIN) {
		uint32_t _jss8 = 0, _tick = 0, _cfg4 = 0;
		char _jb[160];
		(void)fm_rd(d, FM6000_JSS_CFG_8, &_jss8);
		(void)fm_rd(d, FM6000_SSCHED_TICK_CFG, &_tick);
		(void)fm_rd(d, FM6000_SWEEPER_CFG_4, &_cfg4);
		snprintf(_jb, sizeof _jb,
			"nosd-fm6000: pre-START diag: JSS_CFG_8=0x%08x TICK_CFG=0x%08x CFG_4=0x%08x (expect 0x1,0x2,0x0)",
			_jss8, _tick, _cfg4);
		ssched_dbg(_jb);
	}

	/*
	 * START burst: REPLACE_TOKEN → INIT_COMPLETE (and ring SRAMs for MGMT_ONLY).
	 *
	 * MGMT_ONLY re-writes the ring SRAMs in this burst because the settle
	 * phase resets (code 0xd0 from fm_boot_freelists ECC init) can wipe
	 * NEXT_PORT[0] and SLOW_PORT[0] written earlier.  For the full and
	 * small rings the ring SRAMs are written in the first pass above; with
	 * BIST run beforehand the settle produces ~0 resets and those writes
	 * survive intact, so no re-write is needed.
	 *
	 * CFG_4 is NOT written here (either path).  The ring circulates without
	 * the CM monitor enabled (c5567d2 confirmed: CFG_4=0 throughout, ring
	 * circulated).  MGMT_ONLY port 78 is above the ESCHED table (0-75) and
	 * needs no policy write.  NEXT_CHAIN tokens were loaded pre-settle.
	 */
	if (flags & FM_SSCHED_MGMT_ONLY) {
		uint32_t tok = SSCHED_TOKEN(SSCHED_MGMT_PORT, 1, 0);

		/*
		 * 11-write MGMT_ONLY burst: re-write ring SRAMs to survive
		 * any settle resets, push the one management token, prime
		 * REPLACE_TOKEN=78, fire INIT_COMPLETE.
		 *
		 * Port 78 is above the ESCHED table (ports 0-75) so the ring
		 * engine never does an ESCHED lookup; the ring circulates
		 * without CFG_4. [RE 2026-10-05]
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
		/*
		 * install77: TX IC=1 intentionally NOT written for MGMT_ONLY.
		 *
		 * MGMT_ONLY only needs RX to circulate (port 78 probe, sched_ready).
		 * Writing TX IC=1 here puts the TX FSM into RUNNING-but-stuck state
		 * (port 78 token is above the ESCHED table; without ESCHED policy the
		 * TX ring engine stalls immediately after IC=1).  That state survives
		 * the 42 fm_boot_freelists MASTER_RESETs that precede NEXT_CHAIN, and
		 * NEXT_CHAIN's TX IC=1 then fails to restart the ring.
		 *
		 * c5567d2 (first confirmed TX circulation, 2026-10-11) had TX IC=1
		 * never written before NEXT_CHAIN.  Leaving TX FSM in RESET throughout
		 * MGMT_ONLY restores that condition.  fm_esched_init() is currently a
		 * no-op (pcie_regs==NULL), so TX is not needed during MGMT_ONLY.
		 * [RE 2026-10-24, install77]
		 */
	} else if (flags & FM_SSCHED_NEXT_CHAIN) {
		/*
		 * NEXT_CHAIN burst.
		 *
		 * Tokens were loaded post-settle (install74).  CFG_0 was written
		 * post-settle above.
		 *
		 * install78: restore CFG_4=0x2000 (CmMonitorTickPeriod) to the
		 * START burst.  install53 removed it citing c5567d2 "TX circulated
		 * without CFG_4."  install78 analysis shows c5567d2's probe broke
		 * on RX_FOUND=1 at i=0 without ever checking TX_FOUND -- TX has
		 * never been confirmed circulating.  The "TX circulates without
		 * CFG_4" claim is unsubstantiated.  0x118800 fill is complete
		 * (post-settle, line 675) so CM Monitor reads are safe.
		 * CFG_4=0x2000 before IC=1 is safe in nosweep mode (measured,
		 * ssched.c:554-557). [RE 2026-10-25, install78]
		 */
		unsigned first_port = (unsigned)ssched_ring[0];

		ssched_dbg("nosd-fm6000: SSCHED INIT_COMPLETE");
		/* 1 */ if ((rv = fm_wr(d, FM6000_SWEEPER_CFG_4, 0x00002000u)) != FM_OK)
			return rv;
		/* 2 */ if ((rv = fm_wr(d, FM6000_SSCHED_RX_REPLACE_TOKEN, first_port)) != FM_OK)
			return rv;
		/* 3 */ if ((rv = fm_wr(d, FM6000_SSCHED_TX_REPLACE_TOKEN, first_port)) != FM_OK)
			return rv;
		/* 4 */ if ((rv = fm_wr(d, FM6000_SSCHED_RX_INIT_COMPLETE, 1u)) != FM_OK)
			return rv;
		/* 5 */ if ((rv = fm_wr(d, FM6000_SSCHED_TX_INIT_COMPLETE, 1u)) != FM_OK)
			return rv;
		/*
		 * install68: read TX_IC immediately after writing TX IC=1, and also
		 * read REPLACE_TOKEN again to check if the ring found the token.
		 */
		{
			uint32_t _tx_ic2 = 0, _rx_rt2 = 0, _tx_rt2 = 0;
			char _ib2[192];
			(void)fm_rd(d, FM6000_SSCHED_TX_INIT_COMPLETE, &_tx_ic2);
			(void)fm_rd(d, FM6000_SSCHED_RX_REPLACE_TOKEN, &_rx_rt2);
			(void)fm_rd(d, FM6000_SSCHED_TX_REPLACE_TOKEN, &_tx_rt2);
			snprintf(_ib2, sizeof _ib2,
				"nosd-fm6000: post-IC burst: TX_IC=0x%08x RX_RT=0x%08x TX_RT=0x%08x",
				_tx_ic2, _rx_rt2, _tx_rt2);
			ssched_dbg(_ib2);
		}
	} else {
		/*
		 * SMALL_RING (or other mode): 4-write burst only.
		 *
		 * NEXT_PORT, SLOW_PORT, CFG_0/1, and tokens were written after
		 * the settle, so no startup reset can wipe them.
		 *
		 * CFG_4 is NOT written here (see the start-burst comment above).
		 */

		/*
		 * Prime REPLACE_TOKEN to the first enrolled port so the ring
		 * finds it on the first pass (FOUND is set before the FIND
		 * probe even writes its own request). [RE 2026-10-05]
		 */
		unsigned first_port = (flags & FM_SSCHED_SMALL_RING)
					? 0u : (unsigned)ssched_ring[0];
		ssched_dbg("nosd-fm6000: SSCHED INIT_COMPLETE");
		/* 1 */ if ((rv = fm_wr(d, FM6000_SSCHED_RX_REPLACE_TOKEN, first_port)) != FM_OK)
			return rv;
		/* 2 */ if ((rv = fm_wr(d, FM6000_SSCHED_TX_REPLACE_TOKEN, first_port)) != FM_OK)
			return rv;
		/* 3 */ if ((rv = fm_wr(d, FM6000_SSCHED_RX_INIT_COMPLETE, 1u)) != FM_OK)
			return rv;
		/* 4 */ if ((rv = fm_wr(d, FM6000_SSCHED_TX_INIT_COMPLETE, 1u)) != FM_OK)
			return rv;
	}
	if (!fm_alive(d))
		return FM_EOFFBUS;

	phase(d, rep, FM_SSCHED_PH_START);

	fm_sched_mark_ready(d);

	/*
	 * ⚠  REPLACE_TOKEN was primed in the burst above (port 78 for MGMT_ONLY,
	 * first enrolled port for full/small ring).  Do not overwrite it with 0:
	 * that would request a find for port 0, which is not in the MGMT_ONLY
	 * ring, causing 0xc5 watchdog resets on every pass. [RE 2026-10-05]
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

		ssched_dbg("nosd-fm6000: SSCHED probe start");

		/* Diagnostic: read REPLACE_TOKEN and INIT_COMPLETE immediately before
		 * probing.  IC=0 means the clear worked (R/W); IC=1 means W1S latch
		 * and a different approach is needed. */
		if (flags & FM_SSCHED_NEXT_CHAIN) {
			uint32_t rx0 = 0, tx0 = 0, ic0 = 0;
			char _pb[160];
			(void)fm_rd(d, FM6000_SSCHED_RX_REPLACE_TOKEN, &rx0);
			(void)fm_rd(d, FM6000_SSCHED_TX_REPLACE_TOKEN, &tx0);
			(void)fm_rd(d, FM6000_SSCHED_RX_INIT_COMPLETE, &ic0);
			snprintf(_pb, sizeof _pb,
				"nosd-fm6000: PROBE diag: RX_RT=0x%08x TX_RT=0x%08x IC=0x%08x (RX_FOUND=%d TX_FOUND=%d)",
				rx0, tx0, ic0,
				!!(rx0 & FM6000_SSCHED_RX_FOUND),
				!!(tx0 & FM6000_SSCHED_TX_FOUND));
			ssched_dbg(_pb);
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
				if ((flags & FM_SSCHED_NEXT_CHAIN) && i == 0) {
					char _xb[96];
					snprintf(_xb, sizeof _xb,
						"nosd-fm6000: PROBE RX port=%u after wait: RT=0x%08x",
						port, v);
					ssched_dbg(_xb);
				}
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

		/*
		 * install67: delayed TX_FOUND readback.
		 *
		 * The probe breaks immediately on RX_FOUND=1 without ever re-checking
		 * TX_FOUND.  TX_FOUND in the pre-probe diagnostic may be 0 simply
		 * because TX IC=1 was just written and the ring hasn't completed one
		 * pass yet.  Wait 200ms (>> one ring pass) and read TX_REPLACE_TOKEN
		 * directly to see whether TX is actually circulating.
		 * [RE 2026-10-23, install67]
		 */
		if (flags & FM_SSCHED_NEXT_CHAIN) {
			uint32_t tx_post = 0;
			usleep(200000);
			(void)fm_rd(d, FM6000_SSCHED_TX_REPLACE_TOKEN, &tx_post);
			{
				char _tb[128];
				snprintf(_tb, sizeof _tb,
					"nosd-fm6000: TX_FOUND@200ms: TX_RT=0x%08x (%s)",
					tx_post,
					(tx_post & FM6000_SSCHED_TX_FOUND) ? "CIRCULATING" : "STUCK");
				ssched_dbg(_tb);
			}

			/*
			 * install71: TX ring retry.
			 *
			 * TX_FOUND=0 after 200ms.  Hypothesis: the TX INIT_TOKEN
			 * FIFO is cleared by the settle-phase watchdog resets (RX
			 * FIFO may survive while TX does not), leaving TX with zero
			 * enrolled tokens.  A ring with zero tokens does not
			 * circulate but produces no chip resets (NEXT_CHAIN total=0
			 * confirms this).
			 *
			 * ⚠ DO NOT write TX_REPLACE_TOKEN here.  The ring is in
			 * RUNNING state (IC=1 was accepted and self-cleared) but not
			 * circulating.  Writing REPLACE_TOKEN to a RUNNING-but-stuck
			 * ring causes the SSCHED FSM to deadlock the LocalBus, which
			 * propagates as a PCIe completion timeout and freezes the
			 * kernel.  Measured 2026-10-24, install70.
			 *
			 * Recovery: stop TX (IC=0, RUNNING→STOPPED), reload 64 TX
			 * tokens, prime REPLACE_TOKEN=first_port while ring is in
			 * STOPPED state (safe: no ring activity), restart (IC=1).
			 * If TX FIFO was empty, the reload gives the ring 64 enrolled
			 * ports and it will circulate.  If FIFO was full, the reload
			 * overflows it; head-drop discards the earlier 64 and keeps
			 * these 64 -- same ring, identical result. [RE 2026-10-24]
			 */
			if (!(tx_post & FM6000_SSCHED_TX_FOUND)) {
				uint32_t tx_retry = 0;
				unsigned _r;
				char _rb[128];

				ssched_dbg("nosd-fm6000: TX retry: IC=0 stop");
				(void)fm_wr(d, FM6000_SSCHED_TX_INIT_COMPLETE, 0u);

				/*
				 * install76: reload TX tokens in REVERSED order.
				 *
				 * The initial write writes port 20 FIRST and port 19 LAST.
				 * If TX INIT_TOKEN is a head-drop circular buffer (overflow
				 * discards the oldest/first entries), port 20 is dropped and
				 * the ring circulates without it -- TX_FOUND=0 for port 20.
				 *
				 * Writing in reverse (port 19 first, port 20 last) puts port
				 * 20 at the HEAD of the queue (last-written = first-out on a
				 * true FIFO, or last survivor on a tail-drop buffer).  If
				 * TX_FOUND=1 after this retry, the FIFO overflow was the root
				 * cause: the ring has port 20 when it is written last.
				 *
				 * If TX_FOUND=0 even with reversed order, the ring engine is
				 * truly dead (not a FIFO depth issue). [RE install76]
				 */
				ssched_dbg("nosd-fm6000: TX retry: reload 64 TX tokens REVERSED (port20 last)");
				for (_r = sizeof ssched_ring; _r-- > 0; ) {
					unsigned port = ssched_ring[_r];
					uint32_t tok = SSCHED_TOKEN(port, ssched_locked(port), 0);
					if ((rv = fm_wr(d, FM6000_SSCHED_TX_INIT_TOKEN, tok)) != FM_OK)
						return rv;
				}

				ssched_dbg("nosd-fm6000: TX retry: prime RT=port20 + IC=1");
				(void)fm_wr(d, FM6000_SSCHED_TX_REPLACE_TOKEN, (unsigned)ssched_ring[0]);
				(void)fm_wr(d, FM6000_SSCHED_TX_INIT_COMPLETE, 1u);

				usleep(200000); /* 200ms: >> one ring pass */
				(void)fm_rd(d, FM6000_SSCHED_TX_REPLACE_TOKEN, &tx_retry);
				snprintf(_rb, sizeof _rb,
					"nosd-fm6000: TX retry@200ms (reversed): TX_RT=0x%08x (%s)",
					tx_retry,
					(tx_retry & FM6000_SSCHED_TX_FOUND) ? "CIRCULATING" : "STUCK");
				ssched_dbg(_rb);
			}
		}
	}

	/*
	 * install56: explicit RX IC=0 stop after MGMT_ONLY ring confirms circulating.
	 *
	 * pcie_init only asserts FM6000_SOFT_RESET_PCIE (PCIe domain only) and
	 * does NOT reset the SSCHED block.  NEXT_CHAIN ring_init therefore finds
	 * IC functional=1 from MGMT_ONLY, and its IC=1 write is a no-op.
	 *
	 * Writing IC=0 while the ring is known-running is a RUNNING→STOPPED
	 * transition.  Install44/49 showed IC=0 on a never-started chip corrupts
	 * the FSM; this is the opposite: we know IC=1 because the probe
	 * just confirmed it.  NEXT_CHAIN ring_init then writes IC=1 from a clean
	 * STOPPED state, triggering a fresh 0→1 edge that starts the ring.
	 *
	 * ⚠ TX IC=0 is intentionally NOT written here.  The MGMT_ONLY probe checks
	 * RX_FOUND first and breaks immediately (circ=1) without checking TX_FOUND.
	 * TX may never have circulated.  Writing TX IC=0 when TX IC was already 0
	 * (TX ring never started) corrupts the TX FSM in a way that survives the
	 * 42 fm_boot_freelists MASTER_RESETs and prevents NEXT_CHAIN IC=1 from
	 * starting the TX ring.  Measured 2026-10-23: TX_FOUND=0 throughout all
	 * of NEXT_CHAIN, live token reload + IC=1 both rejected.
	 *
	 * fm_boot_freelists (42 MASTER_RESETs) clears TX IC to 0 naturally before
	 * the NEXT_CHAIN IC=1 burst, so no explicit TX IC=0 stop is needed.
	 * This matches the c5567d2 behaviour that first circulated the TX ring.
	 * [RE 2026-10-23, install57]
	 */
	if ((flags & FM_SSCHED_MGMT_ONLY) && circulating != NULL && *circulating) {
		ssched_dbg("nosd-fm6000: SSCHED MGMT_ONLY stop: RX IC=0 written (RUNNING->STOPPED); TX IC never written (install77)");
		(void)fm_wr(d, FM6000_SSCHED_RX_INIT_COMPLETE, 0u);
		/* TX IC was never written in MGMT_ONLY (see install77 comment above). */
	}

	phase(d, rep, FM_SSCHED_PH_FIND);
	if (rep != NULL) {
		struct fm_fatal f;

		if (fm_fatal_read(d, &f) == FM_OK)
			rep->last_fatal = f.last;
	}

	return fm_alive(d) ? FM_OK : FM_EOFFBUS;
}

/*
 * fm_ssched_tx_reload -- post-settle TX ring reload.
 *
 * The pre-DMA FATAL_COUNT stability wait and the post-SPICO PCIe SerDes
 * re-init both fire chip resets that flush the TX INIT_TOKEN FIFO.  The
 * reload in fm_ssched_ring_init() (install71) runs before those resets, so
 * its tokens are gone by the time DMA init starts.
 *
 * This function reloads 64 TX tokens and restarts the ring after all reset
 * storms have settled.  By this point IC has self-cleared to 0 (the ring is
 * in "RUNNING but stuck" state -- no tokens enrolled so nothing circulates).
 * Writing IC=0 to a self-cleared IC is a no-op STOPPED→STOPPED transition.
 *
 * ⚠ Do NOT call before fm_boot_freelists has run and before NEXT_CHAIN
 * ring_init has issued IC=1 at least once -- see install57 for why writing
 * IC=0 on a never-started TX FSM corrupts it.
 * [RE 2026-10-24, install72]
 */
int fm_ssched_tx_reload(struct fm6000 *d)
{
	uint32_t v = 0;
	unsigned i, r;
	int rv;
	char _mb[128];

	/*
	 * Re-write TX and RX NEXT_PORT.
	 *
	 * Chip resets from the pre-DMA SPICO stability wait wipe SSCHED SRAM
	 * including TX_NEXT_PORT and RX_NEXT_PORT.  With NEXT_PORT zeroed the
	 * rings loop at port 0 and never advance.  Re-write the same NEXT_CHAIN
	 * table ring_init wrote for both directions.
	 * [RE 2026-10-25, install79; RX added install117]
	 */
	{
		uint32_t tnp[FM6000_SSCHED_NEXT_PORT_WORDS];
		unsigned n = sizeof ssched_ring;

		for (i = 0; i < FM6000_SSCHED_NEXT_PORT_WORDS; i++)
			tnp[i] = 0;
		for (i = 0; i < n; i++) {
			unsigned here = ssched_ring[i];
			unsigned next = (i + 1 == n) ? (unsigned)SSCHED_MGMT_PORT
						     : (unsigned)ssched_ring[i + 1];
			ring_next(tnp, here, next);
		}
		ring_next(tnp, SSCHED_MGMT_PORT, (unsigned)ssched_ring[0]);

		ssched_dbg("nosd-fm6000: pre-DMA TX reload: re-writing TX+RX NEXT_PORT");
		for (i = 0; i < FM6000_SSCHED_NEXT_PORT_WORDS; i++) {
			if ((rv = fm_wr(d, FM6000_SSCHED_TX_NEXT_PORT(i), tnp[i])) != FM_OK)
				return rv;
			if ((rv = fm_wr(d, FM6000_SSCHED_RX_NEXT_PORT(i), tnp[i])) != FM_OK)
				return rv;
		}
		ssched_dbg("nosd-fm6000: pre-DMA TX reload: TX+RX NEXT_PORT done");
	}

	/*
	 * Stop TX and RX (IC=0) only if IC is currently non-zero.
	 *
	 * install117: SPICO resets clear both RX and TX IC.  ESCHED CFG_2 writes
	 * are safe only when the SSCHED ring is circulating (RX or TX IC=1 in
	 * RUNNING state).  Writing CFG_2 with both rings stopped (IC=0) causes
	 * a chip FATAL, bouncing the PCIe link and risking an MFENCE stall.
	 * Restore both RX and TX here so ESCHED writes are safe.
	 *
	 * ⚠ install57 warning: writing IC=0 when IC is ALREADY 0 (ring never
	 * started or reset to 0 by a chip FATAL) corrupts the TX/RX FSM in a
	 * way that survives chip resets and prevents IC=1 from starting the ring.
	 * Read IC first; only write IC=0 if IC is non-zero (RUNNING state).
	 * [RE 2026-10-26, install117]
	 */
	{
		uint32_t _tx_ic = 0, _rx_ic = 0;
		char _icmsg[128];
		(void)fm_rd(d, FM6000_SSCHED_TX_INIT_COMPLETE, &_tx_ic);
		(void)fm_rd(d, FM6000_SSCHED_RX_INIT_COMPLETE, &_rx_ic);
		snprintf(_icmsg, sizeof _icmsg,
			"nosd-fm6000: pre-DMA TX reload: pre-stop TX_IC=0x%x RX_IC=0x%x",
			_tx_ic, _rx_ic);
		ssched_dbg(_icmsg);
		if (_tx_ic) {
			if ((rv = fm_wr(d, FM6000_SSCHED_TX_INIT_COMPLETE, 0u)) != FM_OK)
				return rv;
		}
		if (_rx_ic) {
			if ((rv = fm_wr(d, FM6000_SSCHED_RX_INIT_COMPLETE, 0u)) != FM_OK)
				return rv;
		}
	}

	/*
	 * install116: JSS clock-domain re-enable.
	 *
	 * Any chip FATAL (including the single pre-DMA reset counted in
	 * FATAL_COUNT) clears all registers to their reset defaults, including
	 * JSS_CFG_8 (→ 0) and PLL_CTRL_0/1 (→ 0).  With JSS_CFG_8=0 the
	 * SSCHED clock domain is dead: IC=1 starts the ring FSM but the domain
	 * never ticks, so the ring never advances and TX_FOUND is always 0.
	 * Confirmed: TX_RT=0x00000014 (STUCK, TX_FOUND=0) in install115 boot
	 * with fc=1, zero new resets after IC=1 — exactly the signature of a
	 * dead JSS clock.
	 *
	 * Re-apply the same sequence ring_init uses (lines ~320-511 of
	 * ring_init): PLL_CTRL re-write → PLL lock wait → SBus JSS enable →
	 * JSS_CFG_8=1 → TICK_CFG.  Freelist SRAM is NOT re-initialised here
	 * (fm_boot_freelists() would wipe SRAM and trigger 42+ resets); a
	 * single FATAL is assumed to be register-only and freelist SRAM is
	 * presumed intact from the earlier ring_init run.
	 * [RE 2026-10-26, install116]
	 */
	/*
	 * install120: conditional JSS re-enable.
	 *
	 * Writing PLL_CTRL_0/1 to their golden values triggers a PLL re-lock
	 * cycle even when the values are unchanged.  The re-lock causes a chip
	 * FATAL, and the subsequent fm_sbus_write polls via fm_rd, which calls
	 * fm_check_offbus() (a sysfs read) on a 0xFFFFFFFF result — deadlocking
	 * the kernel.
	 *
	 * After a normal SPICO run the chip's registers are stable: PLL is
	 * already locked at the golden values, JSS is enabled, TICK is set.
	 * Read the state first and skip the re-init block entirely in that case.
	 * Only re-write registers that were actually cleared by a FATAL.
	 */
	{
		unsigned _w = 0;
		uint32_t _pll = 0, _ctrl0 = 0, _jss = 0, _tick = 0;
		int _need_reinit;
		char _jmsg[128];

		(void)fm_rd_hazardous(d, FM6000_PLL_CTRL_0,        &_ctrl0);
		(void)fm_rd_hazardous(d, FM6000_PLL_STATUS,         &_pll);
		(void)fm_rd_hazardous(d, FM6000_JSS_CFG_8,          &_jss);
		(void)fm_rd_hazardous(d, FM6000_SSCHED_TICK_CFG,    &_tick);

		_need_reinit = (_ctrl0 != FM6000_PLL_CTRL_0_GOLDEN ||
				(_pll & FM6000_PLL_STATUS_LOCKED_ALL) !=
				FM6000_PLL_STATUS_LOCKED_ALL ||
				_jss  != 1u ||
				_tick != FM6000_SSCHED_TICK_PERIOD ||
				_ctrl0 == 0xffffffffu);

		snprintf(_jmsg, sizeof _jmsg,
			"nosd-fm6000: pre-DMA TX reload: PLL re-init + JSS enable"
			" (ctrl0=0x%08x pll=0x%08x jss=%u tick=%u reinit=%d)",
			_ctrl0, _pll, _jss, _tick, _need_reinit);
		ssched_dbg(_jmsg);

		if (_need_reinit) {
			/*
			 * One or more JSS/PLL registers were cleared by a FATAL.
			 * Re-apply the full sequence.  PLL re-write may itself
			 * trigger another FATAL; the fm_rd_hazardous PLL poll and
			 * (void) SBus write below handle that safely.
			 */
			if ((rv = fm_wr(d, FM6000_PLL_CTRL_0, FM6000_PLL_CTRL_0_GOLDEN)) != FM_OK)
				return rv;
			if ((rv = fm_wr(d, FM6000_PLL_CTRL_1, FM6000_PLL_CTRL_1_GOLDEN)) != FM_OK)
				return rv;
			if ((rv = fm_wr(d, FM6000_DLL_CTRL,   FM6000_DLL_CTRL_ENABLE))   != FM_OK)
				return rv;

			/* Wait for PLL lock via hazardous read (no fm_check_offbus). */
			for (_w = 0; _w < FM6000_PLL_LOCK_MAX_MS; _w++) {
				if (fm_rd_hazardous(d, FM6000_PLL_STATUS, &_pll) != FM_OK)
					break;
				if ((_pll & FM6000_PLL_STATUS_LOCKED_ALL) ==
				    FM6000_PLL_STATUS_LOCKED_ALL)
					break;
				usleep(1000);
			}
			snprintf(_jmsg, sizeof _jmsg,
				"nosd-fm6000: pre-DMA TX reload: PLL_STATUS=0x%08x waited=%ums",
				_pll, _w);
			ssched_dbg(_jmsg);

			/* JSS SBus clock enable; ignore failure (chip may be mid-FATAL). */
			(void)fm_sbus_write(d, 0x45, 0x2a, 0x16u);

			if ((rv = fm_wr(d, FM6000_JSS_CFG_8,       0x00000001u)) != FM_OK)
				return rv;
			if ((rv = fm_wr(d, FM6000_SSCHED_TICK_CFG, FM6000_SSCHED_TICK_PERIOD)) != FM_OK)
				return rv;
		}

		ssched_dbg("nosd-fm6000: pre-DMA TX reload: JSS enabled");
	}

	/* Reload 64 TX and RX tokens into their FIFOs. */
	ssched_dbg("nosd-fm6000: pre-DMA TX reload: writing 64 TX+RX tokens");
	for (r = 0; r < sizeof ssched_ring; r++) {
		unsigned port = ssched_ring[r];
		uint32_t tok = SSCHED_TOKEN(port, ssched_locked(port), 0);
		if ((rv = fm_wr(d, FM6000_SSCHED_TX_INIT_TOKEN, tok)) != FM_OK)
			return rv;
		if ((rv = fm_wr(d, FM6000_SSCHED_RX_INIT_TOKEN, tok)) != FM_OK)
			return rv;
	}

	/*
	 * Prime REPLACE_TOKEN while stopped (safe), then start TX+RX (IC=1).
	 * ⚠ Do NOT write REPLACE_TOKEN after IC=1: writing to a running ring
	 * deadlocks the LocalBus. Measured 2026-10-24, install70.
	 */
	ssched_dbg("nosd-fm6000: pre-DMA TX reload: prime RT + TX+RX IC=1");
	if ((rv = fm_wr(d, FM6000_SSCHED_TX_REPLACE_TOKEN, (unsigned)ssched_ring[0])) != FM_OK)
		return rv;
	if ((rv = fm_wr(d, FM6000_SSCHED_RX_REPLACE_TOKEN, (unsigned)ssched_ring[0])) != FM_OK)
		return rv;
	if ((rv = fm_wr(d, FM6000_SSCHED_TX_INIT_COMPLETE, 1u)) != FM_OK)
		return rv;
	if ((rv = fm_wr(d, FM6000_SSCHED_RX_INIT_COMPLETE, 1u)) != FM_OK)
		return rv;

	/*
	 * Poll TX_FOUND and RX_FOUND for up to 1 s (10 × 100 ms).  FOUND fires
	 * when the ring circulates and the engine visits the MGMT_PORT slot,
	 * confirming the clock domain is live and the NEXT_PORT chain is intact.
	 */
	{
		unsigned _p;
		uint32_t _rv = 0;
		for (_p = 0; _p < 10; _p++) {
			usleep(100000); /* 100 ms */
			(void)fm_rd_hazardous(d, FM6000_SSCHED_TX_REPLACE_TOKEN, &v);
			(void)fm_rd_hazardous(d, FM6000_SSCHED_RX_REPLACE_TOKEN, &_rv);
			if (v == 0xffffffffu) v = 0;     /* chip off-bus: ignore */
			if (_rv == 0xffffffffu) _rv = 0; /* chip off-bus: ignore */
			if ((v & FM6000_SSCHED_TX_FOUND) || (_rv & FM6000_SSCHED_RX_FOUND))
				break;
		}
		snprintf(_mb, sizeof _mb,
			"nosd-fm6000: pre-DMA reload result: TX_RT=0x%08x RX_RT=0x%08x (%s)",
			v, _rv,
			((v & FM6000_SSCHED_TX_FOUND) || (_rv & FM6000_SSCHED_RX_FOUND))
				? "CIRCULATING" : "STUCK");
		ssched_dbg(_mb);
	}

	return FM_OK; /* DMA LOOP_TEST will confirm delivery below */
}
