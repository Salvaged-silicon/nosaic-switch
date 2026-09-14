/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (C) the NOSaic authors.
 *
 * The SCD's MDIO accelerator, ported from Arista's GPL-2.0 scd-mdio driver.
 * See scdmdio.h for why this directory is GPL while the datapath around it is
 * not.
 */
#include <stdio.h>
#include <time.h>

#include "scdmdio.h"

/* Offsets from an accelerator's base, per Arista's scd-mdio.h. */
#define MDIO_REQUEST_LO  0x00
#define MDIO_REQUEST_HI  0x10
#define MDIO_CTRL_STATUS 0x20
#define MDIO_RESPONSE    0x30

/* enum mdio_operation */
#define OP_SET   0
#define OP_WRITE 1
#define OP_READ  3

static uint32_t rd(struct nosaic_mdio *m, unsigned long off)
{
	return m->bar[(m->base + off) / 4];
}

static void wr(struct nosaic_mdio *m, unsigned long off, uint32_t v)
{
	m->bar[(m->base + off) / 4] = v;
}

/* cs: res_count:10, fs:3, pbd:3, req_count:10, sp:2, rsvd:2, fe:1, reset:1 */
static uint32_t cs_default(struct nosaic_mdio *m)
{
	return (uint32_t)(m->speed & 3) << 26;
}

static void reset_interrupt(struct nosaic_mdio *m)
{
	wr(m, MDIO_CTRL_STATUS, cs_default(m) | (1u << 30));
}

/*
 * Wait for a response.
 *
 * res_count of 1 is the answer we asked for. Anything above 1 means the
 * accelerator has queued more than was requested, which the reference driver
 * treats as an error rather than reading the first and hoping.
 */
static int wait_response(struct nosaic_mdio *m)
{
	int i;

	for (i = 0; i < 2000; i++) {
		unsigned n = rd(m, MDIO_CTRL_STATUS) & 0x3ff;

		if (n == 1)
			return 0;
		if (n > 1)
			return -1;
		{
			struct timespec ts = { 0, 100000 };  /* 100 us */
			nanosleep(&ts, NULL);
		}
	}
	return -2;
}

/*
 * Take the accelerator through reset before its first transaction.
 *
 * ⚠ WITHOUT THIS THE BUS ANSWERS AND THE PHYs DO NOT. Transactions complete
 * with no error reported and every read returns 0xffff -- an idle MDIO bus,
 * which is indistinguishable from a board whose PHYs are absent.
 */
void nosaic_mdio_init(struct nosaic_mdio *m)
{
	struct timespec ts = { 0, 20000000 };  /* 20 ms */

	wr(m, MDIO_CTRL_STATUS, cs_default(m) | (1u << 31));
	nanosleep(&ts, NULL);
	wr(m, MDIO_CTRL_STATUS, cs_default(m));
	nanosleep(&ts, NULL);
}

static int mdio_request(struct nosaic_mdio *m, int bus, int clause, int prtad,
			int devad, int op, uint16_t data, uint16_t *out)
{
	uint32_t lo, resp;
	int err;

	reset_interrupt(m);

	/* req_lo: d:16, dt:5, pa:5, op:2, t:1, bs:3 */
	lo = (uint32_t)data;
	lo |= ((uint32_t)(devad & 0x1f)) << 16;
	lo |= ((uint32_t)(prtad & 0x1f)) << 21;
	lo |= ((uint32_t)(op & 0x03)) << 26;
	lo |= ((uint32_t)(clause & 0x01)) << 28;
	lo |= ((uint32_t)(bus & 0x07)) << 29;
	wr(m, MDIO_REQUEST_LO, lo);

	/* req_hi: reserved:16, ri:8 -- writing this launches the transaction. */
	wr(m, MDIO_REQUEST_HI, ((uint32_t)(m->req++ & 0xff)) << 16);

	if ((err = wait_response(m)) != 0)
		return err;
	reset_interrupt(m);

	/* resp: d:16, ri:8, rsvd:6, fe:1, ts:1 */
	resp = rd(m, MDIO_RESPONSE);
	if (((resp >> 31) & 1) != 1 || ((resp >> 30) & 1) != 0)
		return -3;
	if (out != NULL)
		*out = (uint16_t)(resp & 0xffff);
	return 0;
}

/*
 * ⚠ EVERY CLAUSE-45 ACCESS IS TWO TRANSACTIONS.
 *
 * A SET carrying the register number, then the real READ or WRITE. That is the
 * clause-45 address/data split, and skipping it is the easiest way to read
 * convincing rubbish from the wrong register.
 */
static int mdio_access(struct nosaic_mdio *m, int bus, int prtad, int devad,
		       int reg, int is_write, uint16_t val, uint16_t *out)
{
	int err = mdio_request(m, bus, 1, prtad, devad, OP_SET,
			       (uint16_t)reg, NULL);
	if (err != 0)
		return err;
	return mdio_request(m, bus, 1, prtad, devad,
			    is_write ? OP_WRITE : OP_READ, val, out);
}

int nosaic_mdio_read(struct nosaic_mdio *m, int bus, int prtad, int devad,
		     int reg, uint16_t *out)
{
	return mdio_access(m, bus, prtad, devad, reg, 0, 0, out);
}

int nosaic_mdio_write(struct nosaic_mdio *m, int bus, int prtad, int devad,
		      int reg, uint16_t val)
{
	return mdio_access(m, bus, prtad, devad, reg, 1, val, NULL);
}
