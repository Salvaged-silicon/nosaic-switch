/* SPDX-License-Identifier: Apache-2.0 */
/*
 * FM6000 Counter Rate Monitor -- hardware memory initialisation.
 *
 * See crm.h for why this exists and where every field came from. This file is
 * our own code; what was recovered is the register layout, which is fact.
 *
 * THE ADDRESS WALK. The datasheet gives the engine's address generation as
 * pseudo code, and getting the parameters wrong does not fail loudly -- it
 * fills the wrong words and leaves the rest uninitialised, which looks exactly
 * like the problem it was meant to solve:
 *
 *	address = base + count0*size0 + count1*stride1 + count2*stride2
 *	count0++; if (count0 == size1) { count0 = 0; count1++;
 *		  if (count1 == size2) { count1 = 0; count2++; } }
 *
 * size0 is the register's width in words -- 1, 2, 4, 4 by Size -- and size1,
 * size2, stride1 and stride2 are all powers of two given as 4-bit shifts, so
 * each is at most 1<<15.
 *
 * For a CONTIGUOUS fill we need stride1 == size1 * size0, so that the step
 * from one block to the next is exactly the length of a block. Setting both
 * shifts to 15 gets that right only for 32-bit registers; for a 64-bit one it
 * would skip every other register and for a 128-bit one three out of four. So
 * size1 absorbs the register width instead:
 *
 *	BlockSize1Shift = 15 - log2(size0)	Stride1Shift = 15
 *
 * which gives stride1 = 1<<15 = size1 * size0 for every width. count1 then
 * counts blocks of 1<<15 words, and count2 cannot advance at all within the
 * documented Count ceiling of 1048575, so stride2 never participates.
 */

#include <stdio.h>
#include <time.h>

#include "crm.h"
#include "pci.h"
#include "regs.h"

/* Same one-millisecond nap boot.c uses; not worth a shared header for one
 * line, and duplicating it keeps this file's only dependency the register
 * map. */
static void nap_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

/* Words per register, by Size. 96-bit and 128-bit both advance four words --
 * the datasheet's pseudo code says size0=4 for both. */
static unsigned words_per_reg(unsigned size)
{
	switch (size) {
	case FM_CRM_SIZE_32:  return 1;
	case FM_CRM_SIZE_64:  return 2;
	case FM_CRM_SIZE_96:  return 4;
	case FM_CRM_SIZE_128: return 4;
	}
	return 0;
}

/* log2 of that, which is what the shift field wants. 1,2,4,4 -> 0,1,2,2. */
static unsigned reg_shift(unsigned size)
{
	return size == FM_CRM_SIZE_32 ? 0 : size == FM_CRM_SIZE_64 ? 1 : 2;
}

/*
 * Write one of the engine's 64-bit registers, low word first.
 *
 * The vendor writes these as a single 64-bit access and we only have 32-bit
 * ones, so the high word lands second. That ordering is deliberate rather than
 * incidental: if either half latches the pair, it is far more likely to be the
 * high one, and a half-written command that is never launched is harmless
 * while a half-written command that launches is not.
 */
static int wr64(struct fm6000 *d, uint32_t word, uint32_t lo, uint32_t hi)
{
	int rv = fm_wr(d, word, lo);

	if (rv != FM_OK)
		return rv;
	return fm_wr(d, word + 1, hi);
}

int fm_crm_running(struct fm6000 *d)
{
	uint32_t v;
	int rv;

	if (d == NULL)
		return FM_ERR;
	rv = fm_rd(d, FM6000_CRM_STATUS, &v);
	if (rv != FM_OK)
		return rv;
	return (v & FM6000_CRM_STATUS_RUNNING) ? 1 : 0;
}

int fm_crm_stop(struct fm6000 *d)
{
	uint32_t v;
	int rv, ms;

	if (d == NULL)
		return FM_ERR;

	rv = fm_rd(d, FM6000_CRM_CTRL, &v);
	if (rv != FM_OK)
		return rv;
	rv = fm_wr(d, FM6000_CRM_CTRL, v & ~FM6000_CRM_CTRL_RUN);
	if (rv != FM_OK)
		return rv;

	/* "Stopping might require some time as the current command has to
	 * complete", and a command walking a large data set holds it. So this
	 * waits rather than assuming. [DS Table 9-1] */
	for (ms = 0; ms < (int)FM_CRM_DONE_MS; ms++) {
		int r = fm_crm_running(d);

		if (r < 0)
			return r;
		if (r == 0)
			return FM_OK;
		nap_ms(1);
	}
	return FM_ETIMEOUT;
}

int fm_crm_memset(struct fm6000 *d, unsigned slot, uint32_t base,
		  uint32_t count, unsigned size, uint32_t value)
{
	uint32_t ip_word, ip_bit, v;
	unsigned size0, shift0, bs1;
	int rv, ms;

	if (d == NULL || slot >= FM_CRM_COMMANDS)
		return FM_ERR;
	if (count == 0 || count > FM_CRM_COUNT_MAX)
		return FM_ERR;
	size0 = words_per_reg(size);
	if (size0 == 0)
		return FM_ERR;
	if (base & ~FM6000_CRM_BASE_MASK)
		return FM_ERR;

	/* Borrowing a slot from a running program would corrupt it. */
	rv = fm_crm_running(d);
	if (rv < 0)
		return rv;
	if (rv == 1)
		return FM_EUNSAFE;

	shift0 = reg_shift(size);
	bs1 = FM6000_CRM_SHIFT_MAX - shift0;

	/* The command: Memory Set, no data section, `count` registers. */
	rv = wr64(d, FM6000_CRM_COMMAND(slot),
		  (FM_CRM_CMD_MEMORY_SET & FM6000_CRM_CMD_MASK) |
		  ((count & FM6000_CRM_COUNT_MASK) << FM6000_CRM_COUNT_SHIFT),
		  (count & FM6000_CRM_COUNT_MASK) >> FM6000_CRM_COUNT_HI_SHIFT);
	if (rv != FM_OK)
		return rv;

	/* The register description: where, how wide, and the block geometry
	 * that makes the walk contiguous -- see the comment at the top. */
	rv = wr64(d, FM6000_CRM_REGISTER(slot),
		  (base & FM6000_CRM_BASE_MASK) |
		  ((size & 0x3u) << FM6000_CRM_SIZE_SHIFT) |
		  ((bs1 & 0xfu) << FM6000_CRM_BS1_SHIFT) |
		  ((unsigned)FM6000_CRM_SHIFT_MAX << FM6000_CRM_ST1_SHIFT),
		  ((unsigned)FM6000_CRM_SHIFT_MAX << FM6000_CRM_BS2_SHIFT_HI) |
		  ((unsigned)FM6000_CRM_SHIFT_MAX << FM6000_CRM_ST2_SHIFT_HI));
	if (rv != FM_OK)
		return rv;

	/* Interval 0 is "as fast as possible", and LastTick starts clean. */
	rv = wr64(d, FM6000_CRM_PERIOD(slot), 0, 0);
	if (rv != FM_OK)
		return rv;

	/* The value. A register wider than 32 bits gets it replicated, which
	 * the datasheet states for this command specifically. */
	rv = fm_wr(d, FM6000_CRM_PARAM(slot), value);
	if (rv != FM_OK)
		return rv;

	/* Clear this command's completion bit before launching, so that what
	 * we poll for afterwards is this run and not a previous one. */
	ip_word = FM6000_CRM_IP + (slot / 32u);
	ip_bit  = 1u << (slot % 32u);
	rv = fm_wr(d, ip_word, ip_bit);
	if (rv != FM_OK)
		return rv;

	/* Run exactly this one slot, exactly once. Continuous clear means the
	 * Run bit clears itself at the end of the sequence. [DS Table 9-1] */
	v = FM6000_CRM_CTRL_RUN |
	    ((slot & 0x3fu) << FM6000_CRM_CTRL_FIRST_SHIFT) |
	    ((slot & 0x3fu) << FM6000_CRM_CTRL_LAST_SHIFT);
	rv = fm_wr(d, FM6000_CRM_STATUS, (slot & 0x3fu) << FM6000_CRM_STATUS_IDX_SHIFT);
	if (rv != FM_OK)
		return rv;
	rv = fm_wr(d, FM6000_CRM_CTRL, v);
	if (rv != FM_OK)
		return rv;

	/* Wait for this command's interrupt, which Table 9-3 says is "set after
	 * initialization completes". */
	for (ms = 0; ms < (int)FM_CRM_DONE_MS; ms++) {
		uint32_t ip;

		rv = fm_rd(d, ip_word, &ip);
		if (rv != FM_OK)
			return rv;
		if (ip & ip_bit) {
			(void)fm_wr(d, ip_word, ip_bit);	/* ack */
			(void)fm_crm_stop(d);
			return FM_OK;
		}
		nap_ms(1);
	}

	(void)fm_crm_stop(d);
	return FM_ETIMEOUT;
}
