/* SPDX-License-Identifier: Apache-2.0 */
/* The memory BIST and repair march. See bist.h for provenance and hazards. */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bist.h"
#include "regs.h"

static void nap_us(long us)
{
	struct timespec t = { us / 1000000, (us % 1000000) * 1000L };

	nanosleep(&t, NULL);
}

/*
 * The default pace between memory-controller writes.
 *
 * ⚠ NOT ZERO, AND NOT NEGOTIABLE DOWNWARD WITHOUT EVIDENCE. Unpaced writes to
 * this block hard hang the host -- the machine issuing them, not the switch
 * chip -- and that needs a power cycle rather than a reset pulse. 50 us over
 * roughly sixty writes costs three milliseconds, which is nothing against a
 * boot, and buys back the one failure mode here that cannot be recovered
 * remotely. [OURS]
 */
#define BIST_PACE_DEFAULT_US 50

/* Scan-config preamble, and the march op table, both as the SDK issues them. */
#define R_SCAN_CFG_IN   0x01c03a
#define R_SCAN_DATA_OUT 0x01c03c
#define R_BM_MARCH      0x01d080	/* 4 words */
#define R_SRBM_MARCH    0x01d708	/* 4 words */
#define R_BM_IP         0x01d08c
#define R_BM_STATUS     0x01d08e

static int wrp(struct fm6000 *d, uint32_t w, uint32_t v, unsigned pace_us)
{
	int rv = fm_wr(d, w, v);

	if (rv != FM_OK)
		return rv;
	nap_us(pace_us);
	return FM_OK;
}

/* Is the chip still there? Checked between groups rather than after every
 * write, because the point is to localise which GROUP did it without then
 * touching a dead chip -- an MMIO access to one is what hangs the host. */
static int still_there(struct fm6000 *d)
{
	return fm_alive(d) == 1;
}

static int configure(struct fm6000 *d, unsigned pace_us)
{
	static const uint32_t march[4] = {
		0x6529eda9, 0x9b8ed9b1, 0xefca952b, 0x000fca99
	};
	int n, rv;

	/* A. Scan-config preamble. */
	if ((rv = fm_wr(d, R_SCAN_CFG_IN, 0x00000063)) != FM_OK)
		return rv;
	nap_us(1);
	{
		uint32_t junk = 0;

		(void)fm_rd(d, R_SCAN_DATA_OUT, &junk);	/* read advances it */
	}
	if ((rv = fm_wr(d, R_SCAN_CFG_IN, 0x80000063)) != FM_OK)
		return rv;
	nap_us(1);
	if ((rv = fm_wr(d, R_SCAN_CFG_IN, 0x88d55555)) != FM_OK)
		return rv;
	nap_us(1);
	if ((rv = fm_wr(d, R_SCAN_CFG_IN, 0x88009555)) != FM_OK)
		return rv;
	nap_us(2);

	/* The engine must be idle before the march is loaded. */
	{
		uint32_t st = 0;

		if ((rv = fm_rd(d, R_BM_STATUS, &st)) != FM_OK)
			return rv;
		if (st != 0)
			return FM_ERR;
	}

	/* B. Load the march sequence into both engines. */
	for (n = 0; n < 4; n++)
		if ((rv = fm_wr(d, R_BM_MARCH + n, march[n])) != FM_OK)
			return rv;
	for (n = 0; n < 4; n++)
		if ((rv = fm_wr(d, R_SRBM_MARCH + n, march[n])) != FM_OK)
			return rv;

	/*
	 * C. Per-controller configuration, PACED.
	 *
	 * ⚠ This is the KNOWN-GOOD SUBSET, not the full warm geometry. The
	 * complete set read off a warm chip off-buses after the march unless
	 * the SBus has been initialised first, which is a dependency this port
	 * has not established. The subset is what was measured to leave the
	 * banks reading valid ECC, and a smaller thing that works beats a
	 * larger one that needs a condition nobody has checked.
	 */
	for (n = 0; n < 4; n++)
		if ((rv = wrp(d, 0x1d210 + n * 0x80, 0x00200000, pace_us)) != FM_OK)
			return rv;
	for (n = 0; n < 5; n++)
		if ((rv = wrp(d, 0x1d400 + n * 0x80, 0x00200000, pace_us)) != FM_OK)
			return rv;
	if (!still_there(d))
		return FM_EOFFBUS;

	for (n = 0; n < 4; n++)
		if ((rv = wrp(d, 0x1d218 + n * 0x80, 0x000000b4, pace_us)) != FM_OK)
			return rv;
	{
		static const uint32_t four[] = {
			0x1d241, 0x1d2c1, 0x1d261, 0x1d281, 0x1d2a1
		};
		size_t i;

		for (i = 0; i < sizeof(four) / sizeof(four[0]); i++)
			if ((rv = wrp(d, four[i], 4, pace_us)) != FM_OK)
				return rv;
	}
	for (n = 0; n < 4; n++)
		if ((rv = wrp(d, 0x1d404 + n * 0x80, 0x0000000c, pace_us)) != FM_OK)
			return rv;
	if ((rv = wrp(d, 0x1d604, 0x4, pace_us)) != FM_OK)
		return rv;
	if (!still_there(d))
		return FM_EOFFBUS;

	{
		static const uint32_t ones[] = {
			0x1d440, 0x1d4c0, 0x1d4e0, 0x1d540,
			0x1d5c0, 0x1d5e0, 0x1d640, 0x1d660
		};
		static const struct { uint32_t w, v; } sizes[] = {
			{ 0x1d409, 0x0fff }, { 0x1d489, 0x7fff }, { 0x1d509, 0x3fff },
			{ 0x1d589, 0x0fff }, { 0x1d609, 0x03ff },
		};
		static const struct { uint32_t w, v; } widths[] = {
			{ 0x1d441, 4 }, { 0x1d4c1, 4 }, { 0x1d4e1, 4 }, { 0x1d541, 4 },
			{ 0x1d5c1, 6 }, { 0x1d5e1, 6 }, { 0x1d641, 0xa }, { 0x1d661, 0xa },
		};
		size_t i;

		for (i = 0; i < sizeof(ones) / sizeof(ones[0]); i++)
			if ((rv = wrp(d, ones[i], 1, pace_us)) != FM_OK)
				return rv;
		for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
			if ((rv = wrp(d, sizes[i].w, sizes[i].v, pace_us)) != FM_OK)
				return rv;
		for (i = 0; i < sizeof(widths) / sizeof(widths[0]); i++)
			if ((rv = wrp(d, widths[i].w, widths[i].v, pace_us)) != FM_OK)
				return rv;
	}
	if (!still_there(d))
		return FM_EOFFBUS;

	/* The run/kick bit, which self-clears; a warm chip reads 0 here. */
	for (n = 0; n < 4; n++)
		if ((rv = wrp(d, 0x1d220 + n * 0x80, 3, pace_us)) != FM_OK)
			return rv;
	return still_there(d) ? FM_OK : FM_EOFFBUS;
}

int fm_bist_configure_only(struct fm6000 *d, unsigned pace_us,
			   struct fm_bist_report *rep)
{
	int rv;

	memset(rep, 0, sizeof(*rep));
	if (pace_us == 0)
		pace_us = BIST_PACE_DEFAULT_US;
	rv = configure(d, pace_us);
	rep->configured = (rv == FM_OK);
	return rv;
}

int fm_bist_memory_init(struct fm6000 *d, unsigned pace_us,
			struct fm_bist_report *rep)
{
	static const struct { uint32_t w, v; } trigger[] = {
		{ 0x1d40b, 0 }, { 0x1d48b, 2 }, { 0x1d50b, 2 },
		{ 0x1d58b, 2 }, { 0x1d60b, 0 },
	};
	unsigned i;
	int n, rv;

	if ((rv = fm_bist_configure_only(d, pace_us, rep)) != FM_OK)
		return rv;
	if (pace_us == 0)
		pace_us = BIST_PACE_DEFAULT_US;

	/* D. Trigger, per controller. */
	for (i = 0; i < sizeof(trigger) / sizeof(trigger[0]); i++)
		if ((rv = wrp(d, trigger[i].w, trigger[i].v, pace_us)) != FM_OK)
			return rv;

	/* E. Wait for the march. Five seconds, as the vendor allows. */
	for (i = 0; i < 5000; i++) {
		uint32_t v = 0;

		if ((rv = fm_rd(d, R_BM_STATUS, &v)) != FM_OK)
			return rv;
		rep->status = v;
		if (v == 0)
			break;
		nap_us(1000);
	}
	rep->march_ms = i;
	if (i >= 5000)
		return FM_ETIMEOUT;
	rep->marched = 1;

	/* F. Harvest. Everything here wants to read zero. */
	{
		uint32_t v = 0;

		if (fm_rd(d, R_BM_IP, &v) == FM_OK && v != 0)
			rep->defects++;
		for (n = 0; n < 4; n++)
			if (fm_rd(d, 0x1d21b + n * 0x80, &v) == FM_OK && v != 0)
				rep->defects++;
	}
	return FM_OK;
}

/*
 * The per-memory BIST controllers.
 *
 * There are five controller instances at a stride of 0x80, each with an
 * address ceiling and a sequence selector, and a set of chain instances at a
 * stride of 0x20 carrying a general config and a latency. The layout is from
 * the register map -- SPDP_BIST at 0x1d400 names +0x09 MAX_ADDR, +0x0b
 * START_SEQUENCE, +0x40 CHAIN_GENERAL_CONFIG and +0x41 CHAIN_LATENCY, and the
 * instances repeat that.
 *
 * The ceilings differ per instance because the memories differ in size, and
 * the latencies differ because the chains differ in depth. Neither is
 * derivable from anything we can see, so both are the vendor's numbers.
 */
#define BIST_INST_MAX_ADDR(b)	((b) + 0x09u)
#define BIST_INST_START_SEQ(b)	((b) + 0x0bu)
#define BIST_CHAIN_CFG(b)	(b)
#define BIST_CHAIN_LATENCY(b)	((b) + 0x01u)

static const struct { uint32_t base, max_addr, start_seq; } bist_inst[] = {
	{ 0x01d400, 0x0fff, 0 },
	{ 0x01d480, 0x7fff, 2 },
	{ 0x01d500, 0x3fff, 2 },
	{ 0x01d580, 0x0fff, 2 },
	{ 0x01d600, 0x03ff, 0 },
};

static const struct { uint32_t base, latency; } bist_chain[] = {
	{ 0x01d440, 0x4 }, { 0x01d4c0, 0x4 }, { 0x01d4e0, 0x4 }, { 0x01d540, 0x4 },
	{ 0x01d5c0, 0x6 }, { 0x01d5e0, 0x6 }, { 0x01d640, 0xa }, { 0x01d660, 0xa },
};

/* The CDP chains take the general config and nothing else. */
static const uint32_t bist_cdp_chain[] = {
	0x01d241, 0x01d261, 0x01d281, 0x01d2a1, 0x01d2c1,
};

#define BIST_CHAIN_CFG_VALUE	0x4u

int fm_bist_configure_controllers(struct fm6000 *d, unsigned *written)
{
	unsigned i, n = 0;
	int rv;

	if (d == NULL)
		return FM_ERR;

#define WR(a, v) do { rv = fm_wr(d, (a), (v)); if (rv != FM_OK) goto out; n++; } while (0)

	/* Chain general configs first: the CDP chains, then the instance
	 * general config the vendor sets alongside them, then the chains. */
	for (i = 0; i < sizeof bist_cdp_chain / sizeof bist_cdp_chain[0]; i++)
		WR(bist_cdp_chain[i], BIST_CHAIN_CFG_VALUE);
	WR(0x01d604, BIST_CHAIN_CFG_VALUE);
	for (i = 0; i < sizeof bist_chain / sizeof bist_chain[0]; i++)
		WR(BIST_CHAIN_CFG(bist_chain[i].base), BIST_CHAIN_CFG_VALUE);

	/* Then each instance's address ceiling. */
	for (i = 0; i < sizeof bist_inst / sizeof bist_inst[0]; i++)
		WR(BIST_INST_MAX_ADDR(bist_inst[i].base), bist_inst[i].max_addr);

	/* Then the chain latencies. */
	for (i = 0; i < sizeof bist_chain / sizeof bist_chain[0]; i++)
		WR(BIST_CHAIN_LATENCY(bist_chain[i].base), bist_chain[i].latency);

	/* Then the sequence selectors, which is what arms each controller. */
	for (i = 0; i < sizeof bist_inst / sizeof bist_inst[0]; i++)
		WR(BIST_INST_START_SEQ(bist_inst[i].base), bist_inst[i].start_seq);

	/* Finally clear the buffer manager's status and acknowledge its
	 * interrupts, which is how the vendor closes the sequence. */
	WR(R_BM_STATUS, 0);
	WR(R_BM_IP, 0x802);
#undef WR

out:
	if (written != NULL)
		*written = n;
	return rv;
}
