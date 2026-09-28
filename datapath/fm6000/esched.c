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

/*
 * The block is eight instances of the same four arrays, 0x200 apart, each
 * array holding one word per physical port.
 *
 * Array 2 is left at zero -- a forwarding chip has it zero in all eight
 * instances, and our earlier prior-art tool wrote it as an explicit zero.
 * Arrays 0, 1 and 3 are filled.
 */
#define ESCHED_INSTANCES	8u
#define ESCHED_INSTANCE_STRIDE	0x200u
#define ESCHED_ARRAY_STRIDE	0x080u
#define ESCHED_PORTS		128u
#define ESCHED_CFG(inst, arr, p)					\
	(0x002000u + (inst) * ESCHED_INSTANCE_STRIDE +			\
	 (arr) * ESCHED_ARRAY_STRIDE + (unsigned)(p))

/* The round-robin word is one array, and it covers the switch ports only. */
#define ESCHED_DRR(p)   (0x003800u + (unsigned)(p))
#define ESCHED_DRR_PORTS	76u

/* Strict priority on all twelve classes, all twelve enabled. */
#define ESCHED_ALL_TC       0x00ffffffu
/* The CPU port. Physical port 0, not the CPU number in portmap.h's logical
 * space -- the egress scheduler indexes physical ports throughout. */
#define ESCHED_CPU_PORT     0u
/* Its two, which are the only values that are not ALL_TC. */
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

/* Which arrays of an instance carry values. */
static const unsigned esched_arrays[] = { 0, 1, 3 };

/*
 * A port's configuration word.
 *
 * Every port is strict on all twelve traffic classes with all twelve
 * enabled, except the host port, which is strict on a subset -- and its
 * second array differs from its other two.
 */
static uint32_t esched_cfg_value(unsigned arr, unsigned port)
{
	if (port != ESCHED_CPU_PORT)
		return ESCHED_ALL_TC;
	return arr == 1 ? ESCHED_CPU_CFG_2 : ESCHED_CPU_CFG_1;
}

int fm_esched_init(struct fm6000 *d, unsigned *written, uint32_t *culprit)
{
	unsigned inst, ai, port, n = 0;
	uint32_t dead = 0;
	int rv;

#define WR(w, v) esched_wr(d, (w), (v), &n, &dead)

	if (written != NULL)
		*written = 0;
	if (culprit != NULL)
		*culprit = 0;

	/*
	 * The configuration arrays, every instance, every port.
	 *
	 * ⚠ THE HOST PORT IS WRITTEN LAST IN EACH ARRAY, not first. That is the
	 * order the running switch uses and this block is one where sequences
	 * have already proved to matter, so it is not tidied away.
	 */
	for (inst = 0; inst < ESCHED_INSTANCES; inst++) {
		for (ai = 0; ai < sizeof esched_arrays / sizeof esched_arrays[0]; ai++) {
			unsigned arr = esched_arrays[ai];

			for (port = 1; port < ESCHED_PORTS; port++)
				if ((rv = WR(ESCHED_CFG(inst, arr, port),
					     esched_cfg_value(arr, port))) != FM_OK)
					return esched_fail(rv, n, dead, written, culprit);
			if ((rv = WR(ESCHED_CFG(inst, arr, ESCHED_CPU_PORT),
				     esched_cfg_value(arr, ESCHED_CPU_PORT))) != FM_OK)
				return esched_fail(rv, n, dead, written, culprit);
		}
	}

	/*
	 * The round-robin word, every switch port twice: once with the
	 * inter-frame-gap penalty and once with it cleared.
	 *
	 * ⚠ THE TWO PASSES ARE WHOLE-CHIP, NOT PER-PORT. Every port is given
	 * the penalty before any port has it taken away. Writing each port's
	 * pair back to back touches the same addresses with the same two
	 * values and is not the same thing: the intermediate state the
	 * scheduler latches against is "all ports penalised", and it never
	 * exists if the passes are interleaved.
	 *
	 * ⚠ AND THE INTERNAL PORTS KEEP THE PENALTY. A forwarding chip has
	 * exactly two of its seventy-six round-robin words still at the
	 * penalised value, and they are physical ports 1 and 3 -- the two
	 * ports with no cage. That is not leftover state from bring-up: it is
	 * the same two ports the store-and-forward table singles out, arrived
	 * at from a different direction, and settling them would be
	 * configuring the switch differently from one that works.
	 */
	for (port = 0; port < ESCHED_DRR_PORTS; port++)
		if ((rv = WR(ESCHED_DRR(port), ESCHED_DRR_PENALTY)) != FM_OK)
			return esched_fail(rv, n, dead, written, culprit);

	for (port = 0; port < ESCHED_DRR_PORTS; port++) {
		if (port == FM6000_ALTA_CPU || port == FM6000_ALTA_INTERNAL)
			continue;
		if ((rv = WR(ESCHED_DRR(port), ESCHED_DRR_SETTLED)) != FM_OK)
			return esched_fail(rv, n, dead, written, culprit);
	}

	if (written != NULL)
		*written = n;
	return FM_OK;
#undef WR
}
