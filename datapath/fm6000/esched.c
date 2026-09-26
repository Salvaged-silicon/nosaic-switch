/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The egress scheduler.
 *
 * Three registers per physical port, all indexed by the port number directly:
 *
 *     CFG_1   0x002000 + port    strictPriority[11:0] | tcEnable[23:12]
 *     CFG_2   0x002080 + port    same shape
 *     DRR_CFG 0x003800 + port    zeroLength[11:0] | groupBoundary[23:12]
 *                                | ifgPenalty[31:24]
 *
 * A front-panel port runs strict priority on all twelve traffic classes with
 * all twelve enabled, which is 0xffffff in both CFG registers. The CPU port
 * is the exception and the only one: strict on a subset in CFG_1, none in
 * CFG_2.
 *
 * ⚠ DRR_CFG IS WRITTEN TWICE PER PORT, with an inter-frame-gap penalty of 20
 * and then with it cleared. That is not redundancy to tidy away -- the vendor
 * does it, the intermediate value is presumably what the scheduler latches
 * against, and this block is one where sequences matter.
 *
 * ⚠ PRECONDITION: THE SCHEDULER RING MUST BE CIRCULATING. Until it is, every
 * address in this file is fatal to touch -- see ssched.c -- so fm_hazard()
 * refuses the whole block and this function writes nothing rather than
 * getting twenty registers in and taking the switch down. A refusal here is
 * the guard working, not a bug in the table below.
 *
 * PROVENANCE. The geometry and values are from EdgeNOS's fm6000_esched.c --
 * our own prior work on this chassis -- whose C file is a materialised table
 * from its own generator. Regenerated here from that geometry rather than
 * copied, and the port set comes from this board's map in portmap.h.
 */
#include "esched.h"
#include "pci.h"
#include "portmap.h"
#include "regs.h"

#define ESCHED_CFG_1(p) (0x002000u + (unsigned)(p))
#define ESCHED_CFG_2(p) (0x002080u + (unsigned)(p))
#define ESCHED_DRR(p)   (0x003800u + (unsigned)(p))

/* Strict priority on all twelve classes, all twelve enabled. */
#define ESCHED_ALL_TC       0x00ffffffu
/* The CPU port's two, which are the only values that are not ALL_TC. */
#define ESCHED_CPU_CFG_1    0x00fff800u
#define ESCHED_CPU_CFG_2    0x00fff000u
/* The deficit round-robin word, with and without the IFG penalty. */
#define ESCHED_DRR_PENALTY  0x14ffffffu
#define ESCHED_DRR_SETTLED  0x00ffffffu

/*
 * Write one register and prove the chip survived it.
 *
 * ⚠ OVER THE LOCAL BUS NOTHING ELSE DETECTS A DEAD CHIP. fm_wr() cannot fail
 * -- a posted write to a chip that has stopped answering looks exactly like
 * one that worked -- and fm_check_offbus() reads FM6000 PCI config space,
 * which on this board does not exist: the part is behind the SCD's bridge and
 * is not a PCI device until the datapath has configured it. fm_alive() reads
 * PIN_STRAP as a beacon and is the only detector there is.
 *
 * The check is one register read per write, which against 212 writes is free,
 * and it is the difference between "this block killed the chip" and "word
 * 0x0020xx killed the chip". The first of those cost an evening.
 */
/* Publish how far we got and what stopped us, so a failure localises. */
static int esched_fail(int rv, unsigned n, uint32_t dead,
		       unsigned *written, uint32_t *culprit)
{
	if (written != NULL)
		*written = n;
	if (culprit != NULL)
		*culprit = dead;
	return rv;
}

static int esched_wr(struct fm6000 *d, uint32_t word, uint32_t val,
		     unsigned *n, uint32_t *culprit)
{
	int rv = fm_wr(d, word, val);

	if (rv != FM_OK) {
		*culprit = word;
		return rv;
	}
	(*n)++;
	if (!fm_alive(d)) {
		*culprit = word;
		return FM_EOFFBUS;
	}
	return FM_OK;
}

int fm_esched_init(struct fm6000 *d, unsigned *written, uint32_t *culprit)
{
	unsigned fp, n = 0;
	uint32_t dead = 0;
	int rv;

#define WR(w, v) esched_wr(d, (w), (v), &n, &dead)

	if (written != NULL)
		*written = 0;
	if (culprit != NULL)
		*culprit = 0;

	/* The CPU port first, and it is the special case. */
	if ((rv = WR(ESCHED_CFG_1(0), ESCHED_CPU_CFG_1)) != FM_OK)
		return esched_fail(rv, n, dead, written, culprit);
	if ((rv = WR(ESCHED_CFG_2(0), ESCHED_CPU_CFG_2)) != FM_OK)
		return esched_fail(rv, n, dead, written, culprit);

	for (fp = 1; fp <= FM6000_FRONT_PORTS; fp++) {
		unsigned p = fm6000_alta_of[fp];

		if ((rv = WR(ESCHED_CFG_1(p), ESCHED_ALL_TC)) != FM_OK)
			return esched_fail(rv, n, dead, written, culprit);
		if ((rv = WR(ESCHED_CFG_2(p), ESCHED_ALL_TC)) != FM_OK)
			return esched_fail(rv, n, dead, written, culprit);
	}

	/* Then the round-robin word, twice each, CPU port included. */
	if ((rv = WR(ESCHED_DRR(0), ESCHED_DRR_PENALTY)) != FM_OK)
		return esched_fail(rv, n, dead, written, culprit);
	if ((rv = WR(ESCHED_DRR(0), ESCHED_DRR_SETTLED)) != FM_OK)
		return esched_fail(rv, n, dead, written, culprit);

	for (fp = 1; fp <= FM6000_FRONT_PORTS; fp++) {
		unsigned p = fm6000_alta_of[fp];

		if ((rv = WR(ESCHED_DRR(p), ESCHED_DRR_PENALTY)) != FM_OK)
			return esched_fail(rv, n, dead, written, culprit);
		if ((rv = WR(ESCHED_DRR(p), ESCHED_DRR_SETTLED)) != FM_OK)
			return esched_fail(rv, n, dead, written, culprit);
	}

	if (written != NULL)
		*written = n;
	return FM_OK;
#undef WR
}
