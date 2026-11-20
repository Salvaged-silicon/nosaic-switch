/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The egress scheduler.
 *
 * WRITE ORDER (post-NEXT_CHAIN settle, pre-probe):
 *
 *   1. CFG_1  (0x002000 + port)  front panel, ring-internal, then CPU
 *   2. CFG_2  (0x002080 + port)  NOT written here -- see fm_esched_set_cfg2()
 *
 * DRR registers (0x003000, 0x003800, 0x003c00) are NOT written here.
 * Those addresses are accessible only after the ring circulates; in
 * MGMT_ONLY ring state they are mapped to different hardware.  Writing
 * DRR_Q (0x003000) in MGMT_ONLY mode corrupts chip state and makes
 * subsequent CFG_1 writes fatal (measured install24, 2026-10-16).
 * Writing DRR_CFG (0x003800) in MGMT_ONLY mode directly causes FM6000
 * self-reset regardless of value (measured install22/23, 2026-10-16).
 *
 * CFG_3 (0x002100) and CFG_4 (0x002180) are NOT written.
 * CFG_4 faults the chip asynchronously (measured 2026-10-13).
 *
 * PORT SET:
 *   Front-panel: ports 20-47, 52-75 → CFG_1=0x00ffffff, CFG_2=0x00ffffff
 *   Ring-internal: ports 1-3, 12-19 → CFG_1=0, CFG_2=0 (ECC init only;
 *     these ports are in ssched_ring[] so the ring engine reads their
 *     CFG entries; ECC-bad entries trigger 0xc5 watchdog resets during
 *     NEXT_CHAIN probe.  Value 0 = no traffic classes, safe for internal
 *     ports.  [RE 2026-10-16])
 *   CPU: port 0 → CFG_1=ESCHED_CPU_CFG_1 (CFG_2 handled by fm_esched_set_cfg2)
 *   Internal 48-51 (NOT in ssched_ring[]): not written.
 *
 * ⚠ LBUS (SCD BAR1) CANNOT WRITE THIS BLOCK. Every address in
 * 0x002000-0x003fff is fatal via the LBUS regardless of value or ring state.
 * fm_esched_init() is a no-op when pcie_regs == NULL.
 */
#include <stdio.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include "esched.h"
#include "pci.h"
#include "regs.h"

static void esched_dbg(const char *msg)
{
	int fd;
	fprintf(stderr, "%s\n", msg);
	fflush(stderr);
	fd = open("/dev/kmsg", O_WRONLY | O_NOCTTY);
	if (fd >= 0) {
		write(fd, msg, strlen(msg));
		write(fd, "\n", 1);
		close(fd);
	}
}

#define ESCHED_CFG_1(p)		(0x002000u + (unsigned)(p))
#define ESCHED_CFG_2(p)		(0x002080u + (unsigned)(p))

#define ESCHED_ALL_TC		0x00ffffffu
#define ESCHED_CPU_PORT		0u
#define ESCHED_CPU_CFG_1	0x00fff800u
#define ESCHED_CPU_CFG_2	0x00fff000u

/* Front-panel port ranges (full traffic class mask). */
static const struct { unsigned lo, hi; } ESCHED_FRONT[] = {
	{ 20, 47 },
	{ 52, 75 },
};

/*
 * Ring-internal ports: in ssched_ring[] but not front-panel.
 * CFG_1 and CFG_2 must be ECC-valid before NEXT_CHAIN starts.
 * Without these writes, the ring engine reads ECC-bad entries for
 * ports 1-3 and 12-19 on every pass and triggers 0xc5 watchdog
 * resets (~2/s for 80 s = 154 resets, circ=0).
 * Value 0 = no traffic classes (safe: no user traffic on these ports).
 * [RE 2026-10-16]
 */
static const struct { unsigned lo, hi; } ESCHED_RING_INTERNAL[] = {
	{  1,  3 },
	{ 12, 19 },
};

int fm_esched_init(struct fm6000 *d, unsigned *written, uint32_t *culprit)
{
	unsigned n = 0;
	unsigned i, p;

	if (written != NULL)
		*written = 0;
	if (culprit != NULL)
		*culprit = 0;

	if (d->pcie_regs == NULL)
		return FM_OK;

	/*
	 * install58: skip ALL pre-probe writes. BIST already initialized all
	 * ESCHED SRAM to ECC-valid zero. In c5567d2 (first TX circulation),
	 * ESCHED writes via BAR0 were silently dropped and TX circulated.
	 * With writes landing (Memory Space enabled), TX stalls at ring slot 0
	 * (port 20) regardless of CFG_1 value. Skipping writes reverts to
	 * c5567d2 behavior to confirm BIST ECC-init is sufficient pre-probe.
	 * fm_esched_set_cfg2() still runs post-probe with DRR_Q + CFG_1 + CFG_2.
	 * [RE 2026-10-23]
	 */
	esched_dbg("nosd-fm6000: ESCHED: init skipped (BIST provides ECC-valid zero)");
	if (written != NULL)
		*written = 0;
	return FM_OK;

	/*
	 * 1. CFG_1: ALL ports written to 0 (ECC-valid, no TCs enabled).
	 *
	 * Writing non-zero CFG_1 here (before IC=1) causes the TX ring to stall:
	 * the TX ring engine reads ESCHED CFG_1 on every pass; with CFG_1=0xffffff
	 * (24 TCs enabled) on the first ring slot (port 20), the engine tries to
	 * schedule those TCs but DRR_Q is still 0 (from BIST), which provides
	 * zero quanta and stalls the TX ring permanently.  RX ring does not read
	 * ESCHED and circulates freely, matching the observed TX_FOUND=0/RX_FOUND=1.
	 *
	 * Real CFG_1 values (0x00ffffff for front-panel, ESCHED_CPU_CFG_1 for CPU)
	 * are written by fm_esched_set_cfg2() AFTER the ring circulates and DRR_Q
	 * has valid quanta.  Ring-internal ports (1-3, 12-19) stay 0 permanently.
	 * [RE 2026-10-23, install58]
	 */
	esched_dbg("nosd-fm6000: ESCHED: writing CFG_1");
	for (i = 0; i < 2; i++)
		for (p = ESCHED_FRONT[i].lo; p <= ESCHED_FRONT[i].hi; p++) {
			fm_wr_pcie(d, ESCHED_CFG_1(p), 0u);
			n++;
		}
	for (i = 0; i < 2; i++)
		for (p = ESCHED_RING_INTERNAL[i].lo; p <= ESCHED_RING_INTERNAL[i].hi; p++) {
			fm_wr_pcie(d, ESCHED_CFG_1(p), 0u);
			n++;
		}
	fm_wr_pcie(d, ESCHED_CFG_1(ESCHED_CPU_PORT), 0u); n++;
	esched_dbg("nosd-fm6000: ESCHED: CFG_1 done");

	/*
	 * 2. CFG_2 (0x002080 + port): ring-internal ports ONLY.
	 *
	 * Writing CFG_2 for front-panel or CPU ports via PCIe BAR0 causes a
	 * full-chip reset that bounces the PCIe endpoint and deadlocks the
	 * kernel PCIe error handler (tested install35/36/37, value 0x00ffffff
	 * and 0u both fatal).  The deadlock triggers at port 20 (0x002094),
	 * the first front-panel address.
	 *
	 * Ring-internal ports 1-3 (0x002081-0x002083) and 12-19
	 * (0x00208c-0x002093) are in ssched_ring[] and their CFG_2 is read on
	 * every ring pass.  Without an ECC-valid write here the NEXT_CHAIN probe
	 * triggers ~2 0xc5 watchdog resets/s for 80 s (154 total, circ=0).
	 * Writing value 0 (no traffic classes) is ECC-valid and safe for these
	 * internal ports.  [RE 2026-10-17, install38]
	 *
	 * Front-panel and CPU CFG_2 are written after probe (FOUND) by
	 * fm_esched_set_cfg2().
	 */
	esched_dbg("nosd-fm6000: ESCHED: writing CFG_2 (ring-internal only)");
	for (i = 0; i < 2; i++)
		for (p = ESCHED_RING_INTERNAL[i].lo; p <= ESCHED_RING_INTERNAL[i].hi; p++) {
			fm_wr_pcie(d, ESCHED_CFG_2(p), 0u);
			n++;
		}
	esched_dbg("nosd-fm6000: ESCHED: CFG_2 done");

	/*
	 * ⚠ DO NOT write CFG_4 (0x002180, ESCHED clock enable = 0x2000) here.
	 * Writing it faults the chip asynchronously -- measured 2026-10-13.
	 */

	if (written != NULL)
		*written = n;
	return FM_OK;
}

/*
 * Post-ring ESCHED initialisation.  Must be called AFTER the SSCHED token
 * ring is circulating (probe FOUND) and after PCIe Memory Space is restored.
 *
 * Writes the registers that are unsafe or wrong to write before the ring
 * circulates:
 *   - DRR_Q (0x003000): quanta values -- MUST be first; ring engine reads
 *     these on every ESCHED write, and the fill-zero values from bank-init
 *     give wrong scheduling behaviour.
 *   - CFG_2 (0x002080) for front-panel and CPU ports: writing these before
 *     probe FOUND causes a full-chip reset (tested install35/36/37).
 *   - DRR_Q + CFG_2 only.  CFG_3 (0x002100) and DRR_CFG (0x003800) are NOT
 *     written: CFG_3 causes a full-chip self-reset post-ring (install47,
 *     2026-10-22); DRR_CFG behaviour post-ring is untested.
 *
 * No-op when d->pcie_regs is NULL.  Never writes via LBUS (fatal for all
 * addresses in 0x002000-0x003fff regardless of ring state).
 */
int fm_esched_set_cfg2(struct fm6000 *d, unsigned flags)
{
	static const uint32_t drr_q[12] = {
		0x1450, 0x1450, 0x1450, 0x05c8, 0x1450, 0x05c8,
		0x1450, 0x1450, 0x1450, 0x6590, 0x6590, 0x39d0,
	};
	unsigned i, p;

	if (d->pcie_regs == NULL)
		return FM_OK;

	/* 1. DRR quanta: MUST be written first. */
	/*
	 * DRR_Q (0x003000+p) is indexed by TC (traffic class), not by port.
	 * The FM6000 ESCHED has 12 TCs (indices 0..11).  Writing beyond index
	 * 11 hits reserved or aliased addresses that fault the chip (measured
	 * install79, 2026-10-25: writing indices 12-75 caused ESCHED fault
	 * before SPICO load → AER/PCIe deadlock).  12 entries are correct.
	 */
	esched_dbg("nosd-fm6000: ESCHED post-ring: writing DRR_Q[0..11]");
	for (p = 0; p < 12; p++)
		fm_wr_pcie(d, 0x003000u + p, drr_q[p]);

	/* 2. CFG_1: front-panel and CPU (now that DRR_Q has valid quanta). */
	esched_dbg("nosd-fm6000: ESCHED post-ring: writing CFG_1 (front-panel + CPU)");
	for (i = 0; i < 2; i++)
		for (p = ESCHED_FRONT[i].lo; p <= ESCHED_FRONT[i].hi; p++)
			fm_wr_pcie(d, ESCHED_CFG_1(p), ESCHED_ALL_TC);
	fm_wr_pcie(d, ESCHED_CFG_1(ESCHED_CPU_PORT), ESCHED_CPU_CFG_1);

	/*
	 * install155: CFG_1 writes can trigger a chip FATAL (link bounce).
	 * Clear Root Error Status at the root port (bus=0, safe during bounce).
	 *
	 * install196: replaced fm_wait_link_active() (CF8/portio) with a
	 * LBUS-based FATAL_COUNT stability poll.  fm_wait_link_active reads
	 * LNKSTA via CF8 outl; that outl fires the SB700 SMI trap, causing the
	 * BIOS handler to read bus=2 in SMM.  If FM6000 is still in FATAL from
	 * the CFG_1 bounce, the read CTOs:
	 *  - SERR# enabled → RS780 sends error completion → BIOS exits →
	 *    SERR# latches → BIOS SERR# handler fires → reads bus=2 → CTO →
	 *    BIOS clears SERR# to 0x0007 → BIOS SERR# handler fires again →
	 *    SERR# now 0x0007 → CTO → no error completion → RS780 stall.
	 * Polling FATAL_COUNT via LBUS (SCD BAR1) has no CF8 I/O and is safe
	 * during FATAL; once stable the link is confirmed up before CFG_2.
	 *
	 * install257: use fm_rd_hazardous (LBUS/BAR1) instead of fm_rd (BAR0).
	 * With SERR# disabled, a BAR0 read to an off-bus FM6000 (which is
	 * transiently off-bus during the CFG_1 FATAL recovery) has no CTO
	 * error-completion path on AMD RS780 → CPU stalls permanently.  LBUS
	 * reads always get an error completion (0 return) even with SERR# off.
	 * [install257, 2026-11-20]
	 */
	esched_dbg("nosd-fm6000: ESCHED post-ring: aer_clear after CFG_1");
	fm_pcie_clear_root_aer_errors();
	esched_dbg("nosd-fm6000: ESCHED post-ring: wait-stable after CFG_1 [install196]");
	{
		uint32_t fc0 = 0xFFFFFFFFu, fc1;
		int stable = 0;
		for (int i = 0; i < 40 && stable < 5; i++) {
			struct timespec ts = { 0, 100000000L };	/* 100 ms */
			nanosleep(&ts, NULL);
			if (fm_rd_hazardous(d, FM6000_FATAL_COUNT, &fc1) != FM_OK)
				fc1 = 0xFFFFFFFFu;
			if (fc1 != 0xFFFFFFFFu && fc1 == fc0)
				stable++;
			else { fc0 = fc1; stable = 0; }
		}
	}

	/*
	 * 3. CFG_2: front-panel then CPU.
	 *
	 * install258: skip front-panel ports when FM_ESCHED_CFG2_SKIP_FP_PORTS
	 * is set.  Writing CFG_2 for front-panel ports (port 20+) while the
	 * SSCHED ring is not circulating (TX_RT=0 after tx_reload(SKIP_PLL))
	 * triggers a chip FATAL → PCIe link bounce → kernel IRQ handler grabs
	 * logbuf_lock then stalls on ECAM read with SERR# disabled → deadlock.
	 * CPU-port CFG_2 (port 0, addr 0x002080) is safe and always written.
	 */
	if (flags & FM_ESCHED_CFG2_SKIP_FP_PORTS) {
		esched_dbg("nosd-fm6000: ESCHED post-ring: writing CFG_2 (CPU port only, fp skipped) [install258]");
	} else {
		esched_dbg("nosd-fm6000: ESCHED post-ring: writing CFG_2 (front-panel + CPU)");
		for (i = 0; i < 2; i++)
			for (p = ESCHED_FRONT[i].lo; p <= ESCHED_FRONT[i].hi; p++)
				fm_wr_pcie(d, ESCHED_CFG_2(p), ESCHED_ALL_TC);
	}
	fm_wr_pcie(d, ESCHED_CFG_2(ESCHED_CPU_PORT), ESCHED_CPU_CFG_2);

	/*
	 * CFG_3 (0x002100) and DRR_CFG (0x003800) are NOT written here.
	 * Writing CFG_3 for any port after the ring circulates causes a
	 * full-chip self-reset (measured install47, 2026-10-22).
	 * DRR_Q + CFG_2 (steps 1-2 above) are sufficient for forwarding.
	 */

	esched_dbg("nosd-fm6000: ESCHED post-ring: CFG_1+CFG_2+DRR_Q done [install258]");
	return FM_OK;
}
