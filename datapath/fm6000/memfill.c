/* SPDX-License-Identifier: Apache-2.0 */

/*
 * FM6000 full memory initialisation.
 *
 * 134 SRAM banks in order. The values for the CM watermark banks
 * (0x112800, 0x113000, 0x115000, 0x115800) are set to 0xffffffff --
 * "no threshold" -- so the fabric admits frames from the start. The
 * STATS bank (0x200000) is also 0xffffffff. Everything else zeros.
 *
 * Addresses and bank geometry derived from our own prior work on the
 * FM6000 register map (re-confirmed on hardware against a running EOS
 * boot; bank boundaries confirmed by the address where fills stopped
 * taking without resetting the chip).
 */

#include <stdio.h>
#include "pci.h"
#include "memfill.h"
#include "boot.h"

static const struct {
	uint32_t base;
	uint32_t words;
	uint32_t val;
} banks[] = {
	/* PARSER */
	{ 0x100200, 14336, 0x00000000 },
	{ 0x108000,   152, 0x00000000 },
	{ 0x108200,   304, 0x00000000 },
	/* MAPPER */
	{ 0x121000,  4096, 0x00000000 },
	{ 0x122000,  4096, 0x00000000 },
	{ 0x123000,   152, 0x00000000 },
	{ 0x123100,    48, 0x00000000 },
	{ 0x123140,    48, 0x00000000 },
	{ 0x123180,    96, 0x00000000 },
	{ 0x123200,    16, 0x00000000 },
	{ 0x123210,    16, 0x00000000 },
	{ 0x123220,    32, 0x00000000 },
	{ 0x123300,    48, 0x00000000 },
	{ 0x123380,    96, 0x00000000 },
	{ 0x123400,    16, 0x00000000 },
	{ 0x123420,    32, 0x00000000 },
	{ 0x123440,    16, 0x00000000 },
	{ 0x123450,    16, 0x00000000 },
	{ 0x123460,    16, 0x00000000 },
	{ 0x123470,    16, 0x00000000 },
	{ 0x123800,   128, 0x00000000 },
	{ 0x123880,   128, 0x00000000 },
	{ 0x123900,   256, 0x00000000 },
	{ 0x123a00,   128, 0x00000000 },
	{ 0x123a80,   128, 0x00000000 },
	{ 0x123b00,   256, 0x00000000 },
	{ 0x123c00,    16, 0x00000000 },
	{ 0x123c10,    16, 0x00000000 },
	{ 0x123c20,    32, 0x00000000 },
	{ 0x123c40,    16, 0x00000000 },
	{ 0x123c50,    16, 0x00000000 },
	{ 0x123c60,    32, 0x00000000 },
	{ 0x123c80,    16, 0x00000000 },
	{ 0x123c90,    16, 0x00000000 },
	{ 0x123ca0,    16, 0x00000000 },
	{ 0x123cb0,    16, 0x00000000 },
	{ 0x123d80,   128, 0x00000000 },
	{ 0x123d00,   128, 0x00000000 },
	{ 0x123cc0,    16, 0x00000000 },
	{ 0x124280,    64, 0x00000000 },
	{ 0x1242c0,    64, 0x00000000 },
	{ 0x124300,    32, 0x00000000 },
	{ 0x124320,    32, 0x00000000 },
	/* FFU / BST -- all four engines + all arrays in one pass */
	{ 0x300000, 262144, 0x00000000 },
	{ 0x381000,  49152, 0x00000000 },
	{ 0x380000,  98304, 0x00000000 },
	{ 0x381800,   1536, 0x00000000 },
	{ 0x381840,   1536, 0x00000000 },
	{ 0x3f8000,     64, 0x00000000 },
	{ 0x3fc400,   1024, 0x00000000 },
	/* NEXTHOP */
	{ 0x160000, 131072, 0x00000000 },
	/* HASH */
	{ 0x00b400,   1024, 0x00000000 },
	{ 0x00b800,   1024, 0x00000000 },
	/* L2 forwarding */
	{ 0x280000, 262144, 0x00000000 },
	{ 0x032000,   8192, 0x00000000 },
	{ 0x034000,   8192, 0x00000000 },
	{ 0x036000,   4096, 0x00000000 },
	{ 0x037000,   4096, 0x00000000 },
	{ 0x00d200,    512, 0x00000000 },
	{ 0x180000,  98304, 0x00000000 },
	{ 0x1a0000,   4096, 0x00000000 },
	/* GLORT */
	{ 0x00e800,   2048, 0x00000000 },
	/* POLICERS -- needs 16384 words each, not 4096 */
	{ 0x130000,  16384, 0x00000000 },
	{ 0x134000,   1024, 0x00000000 },
	{ 0x138000,  16384, 0x00000000 },
	{ 0x13c000,   1024, 0x00000000 },
	/* L2AR */
	{ 0x146200,   128, 0x00000000 },
	{ 0x146300,   128, 0x00000000 },
	{ 0x146400,   128, 0x00000000 },
	{ 0x146500,   128, 0x00000000 },
	{ 0x146600,   128, 0x00000000 },
	{ 0x146700,   128, 0x00000000 },
	{ 0x146800,   128, 0x00000000 },
	{ 0x146900,   128, 0x00000000 },
	{ 0x146a00,   128, 0x00000000 },
	{ 0x146b00,   128, 0x00000000 },
	{ 0x146c00,   128, 0x00000000 },
	{ 0x146d00,   128, 0x00000000 },
	{ 0x146e00,   128, 0x00000000 },
	{ 0x146f00,   128, 0x00000000 },
	{ 0x147000,   128, 0x00000000 },
	{ 0x147100,   128, 0x00000000 },
	{ 0x147200,   128, 0x00000000 },
	{ 0x147300,   128, 0x00000000 },
	/*
	 * CM -- counter manager.
	 *
	 * ⚠ The RXMP_PRIVATE_WM and HOG_WM banks (0x112800, 0x113000,
	 * 0x115000, 0x115800) gate buffer admission at fabric ingress.
	 * Cold, they hold random SRAM values which the fabric interprets as
	 * thresholds -- frames are rejected with no drop counter anywhere.
	 * These MUST be 0xffffffff ("no threshold") for the chip to forward.
	 */
	{ 0x118800,   960, 0x00000000 },
	{ 0x112800,   912, 0xffffffff },
	{ 0x113000,  1216, 0xffffffff },
	{ 0x115000,   912, 0xffffffff },
	{ 0x115800,   912, 0xffffffff },
	{ 0x117000,   912, 0x20001000 },
	/* CMM */
	{ 0x020800,   960, 0x00000000 },
	{ 0x021000,   960, 0x00000000 },
	/* CM continued */
	{ 0x116600,   304, 0x00000000 },
	{ 0x117800,    76, 0x00000000 },
	{ 0x116100,   152, 0x00000000 },
	{ 0x116200,    76, 0x00000000 },
	{ 0x114000,  1280, 0x00003fff },
	/*
	 * 0x240000 and 0x260000 are NOT here.
	 *
	 * These addresses were once modelled as MCAST_MID and MCAST_POST memory
	 * banks. Hardware measurement (2026-09-25, bisected exactly) shows they
	 * are register blocks: word 54 of 0x240000 and word 20 of 0x260000 take
	 * the chip off the bus regardless of chip state. They are not ECC-
	 * protected SRAM and do not need initialisation. See regs.h.
	 */
	/*
	 * 0x003000 (ESCHED_DRR_Q) and 0x003c00 (ESCHED_DRR_DC_INIT) are NOT
	 * here. Both are ESCHED state registers: DRR_Q is the per-class deficit
	 * counter (live runtime state, not a static init value) and they are
	 * inaccessible until the scheduler ring circulates. See esched.c.
	 */
	/*
	 * MOD (Modifier) -- 0x150000 control bank.
	 *
	 * 0x150000-0x157fff is NOT ECC SRAM. It is the MOD block's control and
	 * configuration space. Hardware measurement 2026-10-01 confirmed:
	 *   - Word 65 of BOTH banks (0x150041, 0x154041) is instantly fatal.
	 *   - Writing word 0 of 0x150000 to 0 makes ANY write to 0x154000
	 *     immediately fatal (measured: word 0 of 0x154000 crashes the chip
	 *     after bank 97 is filled, but is safe in isolation).
	 *
	 * The EdgeNOS modinit writes exactly three words here (4, 5, 6) and
	 * skips everything else in the 0x150000-0x157fff range. Words 0-3
	 * are control registers whose power-on defaults must not be zeroed.
	 * We match EdgeNOS exactly: only words 4-6, both banks skipped for
	 * 0x154000 (EdgeNOS modinit writes nothing there).
	 *
	 * The actual modifier action table starts at 0x158000 -- that is ECC
	 * SRAM and is filled further below.
	 */
	{ 0x150004,     3, 0x00000000 },
	/*
	 * 0x15a000-0x15e000 are NOT here.
	 *
	 * Hardware measurement 2026-10-01 confirmed fatal control registers at:
	 *   0x15a01b (word 27), and likely equivalent words in 0x15b000-0x15e000.
	 * EdgeNOS modinit writes nothing in these ranges. Skip them all.
	 */
	/* MGMT2 / CRM_DATA */
	{ 0x01e000,  4096, 0x00000000 },
	/* CM -- second pass (re-zero after earlier writes settle) */
	{ 0x118800,   960, 0x00000000 },
	/*
	 * STATS -- 0xffffffff so that rate monitors read clean
	 * rather than wrapping at random initial offsets.
	 */
	{ 0x200000, 65536, 0xffffffff },
	/* PARSER -- second pass */
	{ 0x100000, 14336, 0x00000000 },
	/* L2 sweeper */
	{ 0x00d080,   128, 0x00000000 },
	/* EACL */
	{ 0x006000,   192, 0x00000000 },
	{ 0x006100,   152, 0x00000000 },
	/* LBS */
	{ 0x014000,    76, 0x00000000 },
	/* MAPPER gap fills -- measured: these words come up dirty and corrupt forwarding */
	{ 0x123098,   232, 0x00000000 },
	{ 0x1231e3,    29, 0x00000000 },
	{ 0x123240,   320, 0x00000000 },
	{ 0x1233e0,    64, 0x00000000 },
	{ 0x123480,   896, 0x00000000 },
	{ 0x123cd0,    48, 0x00000000 },
	{ 0x123e04,   252, 0x00000000 },
	{ 0x123f98,   104, 0x00000000 },
	{ 0x124098,   488, 0x00000000 },
	{ 0x124340,   192, 0x00000000 },
	/* L2AR main table */
	{ 0x144000,  4096, 0x00000000 },
	/* L3AR */
	{ 0x010000,  2560, 0x00000000 },
	/* GLORT second pass */
	{ 0x00e000,  1024, 0x00000000 },
	/* STATS_AR */
	{ 0x018000,  1536, 0x00000000 },
	{ 0x018a00,   512, 0x00000000 },
	{ 0x018c00,   360, 0x00000000 },
	/*
	 * 0x158000 (MOD action table) is NOT here.
	 *
	 * Fatal control register at word 41 (0x158029); probe 2026-10-01.
	 * EdgeNOS modinit writes specific action entries (not a bulk zero),
	 * and the safe range before word 41 is not enough to clear all ECC.
	 * MOD initialisation is deferred to a separate modinit step that
	 * replicates the EdgeNOS action table entries for the ports in use.
	 */
	/* L2L final */
	{ 0x030800,  2048, 0x00000000 },
};

#define NBANKS ((unsigned)(sizeof banks / sizeof banks[0]))

int fm_memfill_all(struct fm6000 *d)
{
	unsigned i;

	for (i = 0; i < NBANKS; i++) {
		int rv = fm_mem_fill(d, banks[i].base, banks[i].words, banks[i].val);

		if (rv != FM_OK)
			return rv;
		if (!fm_alive(d))
			return FM_EOFFBUS;
	}
	return FM_OK;
}

static int memfill_n_verbose_impl(struct fm6000 *d, unsigned n)
{
	unsigned i;

	if (n > NBANKS)
		n = NBANKS;

	for (i = 0; i < n; i++) {
		int rv;

		printf("  bank %3u  0x%06x  %6u words  fill 0x%08x ... ",
		       i, banks[i].base, banks[i].words, banks[i].val);
		fflush(stdout);

		rv = fm_mem_fill(d, banks[i].base, banks[i].words, banks[i].val);
		if (rv != FM_OK) {
			printf("FILL FAILED (rv=%d)\n", rv);
			fflush(stdout);
			return rv;
		}
		if (!fm_alive(d)) {
			printf("CHIP DIED\n");
			fflush(stdout);
			return FM_EOFFBUS;
		}
		printf("ok\n");
		fflush(stdout);
	}
	return FM_OK;
}

int fm_memfill_verbose(struct fm6000 *d)
{
	return memfill_n_verbose_impl(d, NBANKS);
}

int fm_memfill_n_verbose(struct fm6000 *d, unsigned n)
{
	return memfill_n_verbose_impl(d, n);
}
