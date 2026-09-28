/* SPDX-License-Identifier: Apache-2.0 */
/*
 * FM6000 scan-chain sequence -- the vendor's "MRL register fix".
 *
 * Everything here is our own code. What was recovered from the vendor binary
 * is the shape of the conversation with the hardware: which register each word
 * goes to, what has to be selected first, and what the chip says back. The
 * payload that conversation carries is not here and is not ours to carry --
 * see mrl.h.
 *
 * THE ENGINE'S CONTROL WORDS. The words written to SCAN_CONFIG_DATA_IN outside
 * the payload are commands, not data. Their encoding appears to be an opcode
 * in [31:24] and an operand in [7:0]:
 *
 *	0x80 op 1	start
 *	0x80 op 0x40	stop
 *	0x84 op 30	\
 *	0x85 op 30	 > three timing or count parameters, set once
 *	0x86 op 200	/
 *	0xbf op n	advance / pump; the operand counts 0..9 in the
 *			prologue and is zero every other time
 *
 * The opcode/operand split is inferred from the bit pattern and from the fact
 * that only those six opcodes ever appear. It is not documented. The values
 * are written as the vendor writes them because they are what the engine
 * accepts, in the same way that BOOT_CTRL's command numbers are.
 *
 * 0xfffffff8 shifted into SCAN_CHAIN_DATA_IN marks the end of a chain. It is
 * the same all-ones word Table 4-1 step 5 writes, with the low three bits
 * clear; what those three bits mean is not known.
 */

#include <stdio.h>
#include <string.h>

#include "mrl.h"
#include "pci.h"
#include "regs.h"

/* Engine commands. [RE] */
#define MRL_START		0x80000001u
#define MRL_STOP		0x80000040u
#define MRL_PARAM_A		0x8400001eu
#define MRL_PARAM_B		0x8500001eu
#define MRL_PARAM_C		0x860000c8u
#define MRL_PUMP(n)		(0xbf000000u | ((n) & 0xffu))
#define MRL_CHAIN_END		0xfffffff8u

/* How many words of each kind the prologue and tail move. Like the chain
 * lengths in mrl.h these are counts the engine expects, not data. [RE] */
#define MRL_PROLOGUE_FLUSH	21u	/* zeros into CONFIG, engine idle */
#define MRL_PROLOGUE_PUMPS	10u	/* MRL_PUMP(0) .. MRL_PUMP(9) */
#define MRL_TAIL_GROUPS		4u
#define MRL_TAIL_PUMPS		60u	/* per group, before its terminator */
#define MRL_TAIL_LAST_PUMPS	66u	/* the last group has more and no end */

/*
 * One shift. Select the chain, put the word in, look at what came back.
 *
 * The selector is rewritten for every single word even when it has not
 * changed. That is what the vendor does, and on a shift-register port a write
 * to the selector may well be what clocks the previous word through, so it is
 * not treated as redundant and is not hoisted out of the loop.
 */
static int shift(struct fm6000 *d, unsigned chain, int to_config, uint32_t word,
		 struct fm_mrl_report *rep)
{
	uint32_t st;
	int rv;

	rv = fm_wr(d, FM6000_SCAN_CONTROL, chain & 0x1fu);
	if (rv != FM_OK)
		goto failed;

	rv = fm_wr(d, to_config ? FM6000_SCAN_CONFIG_DATA_IN
				: FM6000_SCAN_CHAIN_DATA_IN, word);
	if (rv != FM_OK)
		goto failed;

	/*
	 * The vendor reads the status, and on anything but RETIRED reads it a
	 * second time before calling it an error -- so one re-read is part of
	 * the protocol, not a retry loop bolted on. We do the same and no more.
	 */
	rv = fm_rd(d, FM6000_SCAN_STATUS, &st);
	if (rv != FM_OK)
		goto failed;
	if ((st & FM6000_SCAN_STATUS_MASK) != FM6000_SCAN_STATUS_RETIRED) {
		rv = fm_rd(d, FM6000_SCAN_STATUS, &st);
		if (rv != FM_OK)
			goto failed;
	}

	if ((st & FM6000_SCAN_STATUS_MASK) != FM6000_SCAN_STATUS_RETIRED) {
		if (rep->stalled == 0) {
			rep->first_bad = st;
			rep->first_bad_at = rep->shifted;
		}
		rep->stalled++;
	}

	rep->shifted++;
	return FM_OK;

failed:
	if (rv == FM_EOFFBUS)
		rep->offbus = 1;
	return rv;
}

/* A run of identical words down one chain. */
static int shift_n(struct fm6000 *d, unsigned chain, int to_config,
		   uint32_t word, unsigned n, struct fm_mrl_report *rep)
{
	unsigned i;
	int rv;

	for (i = 0; i < n; i++) {
		rv = shift(d, chain, to_config, word, rep);
		if (rv != FM_OK)
			return rv;
	}
	return FM_OK;
}

/* A payload down one chain. A NULL payload is a run of zeros -- see mrl.h for
 * why that is the shipped default and not a stub. */
static int shift_payload(struct fm6000 *d, unsigned chain, const uint32_t *p,
			 unsigned n, struct fm_mrl_report *rep)
{
	unsigned i;
	int rv;

	if (p == NULL)
		return shift_n(d, chain, 0, 0, n, rep);

	for (i = 0; i < n; i++) {
		rv = shift(d, chain, 0, p[i], rep);
		if (rv != FM_OK)
			return rv;
	}
	return FM_OK;
}

int fm_mrl_apply(struct fm6000 *d, const struct fm_mrl_payload *p,
		 struct fm_mrl_report *rep)
{
	static const struct fm_mrl_payload zeros = { NULL, NULL };
	unsigned g;
	int rv;

	if (d == NULL || rep == NULL)
		return FM_ERR;
	if (p == NULL)
		p = &zeros;

	memset(rep, 0, sizeof(*rep));

	/*
	 * Prologue. Twenty-one zero words into the config port with the engine
	 * still stopped, then start it, then its three parameters, then ten
	 * pump words numbered 0 through 9.
	 *
	 * Why twenty-one zeros: unknown. A shift register is flushed by
	 * shifting through it, and twenty-one words is 672 bits, which matches
	 * nothing we have measured. It is done because the engine is evidently
	 * expected to see it.
	 */
	rv = shift_n(d, FM6000_SCAN_CHAIN_CORE, 1, 0, MRL_PROLOGUE_FLUSH, rep);
	if (rv != FM_OK)
		return rv;

	rv = shift(d, FM6000_SCAN_CHAIN_CORE, 1, MRL_START, rep);
	if (rv == FM_OK)
		rv = shift(d, FM6000_SCAN_CHAIN_CORE, 1, MRL_PARAM_A, rep);
	if (rv == FM_OK)
		rv = shift(d, FM6000_SCAN_CHAIN_CORE, 1, MRL_PARAM_B, rep);
	if (rv == FM_OK)
		rv = shift(d, FM6000_SCAN_CHAIN_CORE, 1, MRL_PARAM_C, rep);
	if (rv != FM_OK)
		return rv;

	for (g = 0; g < MRL_PROLOGUE_PUMPS; g++) {
		rv = shift(d, FM6000_SCAN_CHAIN_CORE, 1, MRL_PUMP(g), rep);
		if (rv != FM_OK)
			return rv;
	}

	/* The bank chain, then the core chain. The core chain's last word is
	 * the end marker rather than payload, so one fewer payload word goes
	 * down it than its length. */
	rv = shift_payload(d, FM6000_SCAN_CHAIN_BANKS, p->bank,
			   FM_MRL_BANK_WORDS, rep);
	if (rv != FM_OK)
		return rv;

	rv = shift_payload(d, FM6000_SCAN_CHAIN_CORE, p->core,
			   FM_MRL_CORE_WORDS - 1, rep);
	if (rv == FM_OK)
		rv = shift(d, FM6000_SCAN_CHAIN_CORE, 0, MRL_CHAIN_END, rep);
	if (rv != FM_OK)
		return rv;

	/*
	 * Tail. Three groups of sixty pumps each followed by an end marker,
	 * then a fourth group of sixty-six with no marker after it.
	 */
	for (g = 0; g < MRL_TAIL_GROUPS; g++) {
		int last = (g == MRL_TAIL_GROUPS - 1);

		rv = shift_n(d, FM6000_SCAN_CHAIN_CORE, 1, MRL_PUMP(0),
			     last ? MRL_TAIL_LAST_PUMPS : MRL_TAIL_PUMPS, rep);
		if (rv != FM_OK)
			return rv;
		if (last)
			break;
		rv = shift(d, FM6000_SCAN_CHAIN_CORE, 0, MRL_CHAIN_END, rep);
		if (rv != FM_OK)
			return rv;
	}

	rv = shift(d, FM6000_SCAN_CHAIN_CORE, 1, MRL_STOP, rep);
	if (rv != FM_OK)
		return rv;

	return rep->stalled == 0 ? FM_OK : FM_ETIMEOUT;
}

void fm_mrl_print(const struct fm_mrl_report *rep, int rv)
{
	printf("  words shifted           %u\n", rep->shifted);
	printf("  shifts that stalled     %u%s\n", rep->stalled,
	       rep->stalled == 0 ? "" : "  <-- status was not RETIRED");
	if (rep->stalled != 0)
		printf("  first stall at word %u, SCAN_STATUS 0x%08x\n",
		       rep->first_bad_at, rep->first_bad);
	if (rep->offbus)
		printf("  ⚠ the chip left the PCIe bus during the sequence\n");
	printf("\n%s\n", rv == FM_OK ? "ok" : "did not complete cleanly");
}
