/* SPDX-License-Identifier: Apache-2.0 */
/* The SerDes serial bus. See sbus.h for where the numbers come from. */
#include <stddef.h>

#include "regs.h"
#include "sbus.h"

/* SBUS_COMMAND field layout. Confirmed on hardware by writing a command and
 * reading it back: the low three bytes return exactly as written, which is
 * what fixes the boundaries. [DS §9.4 for the shape, RE for the positions] */
#define CMD_REG_SHIFT	0
#define CMD_DEV_SHIFT	8
#define CMD_OP_SHIFT	16
#define CMD_EXECUTE	(1u << 24)
#define CMD_BUSY	(1u << 25)
#define CMD_RC_SHIFT	26
#define CMD_RC_MASK	0x7

/*
 * How long to wait for Busy to clear.
 *
 * The datasheet puts one access at up to 6 µs. This is three orders of
 * magnitude more than that, because the cost of being generous is nothing --
 * the loop exits as soon as the bit clears -- and the cost of being tight is a
 * timeout reported as a hardware fault on a bus the datasheet already calls
 * slow. [OURS]
 */
#define SBUS_POLL_LIMIT 20000

int fm_sbus_start(struct fm6000 *d)
{
	uint32_t v = 0;
	int rv;

	rv = fm_rd(d, FM6000_SBUS_CFG, &v);
	if (rv != FM_OK)
		return rv;
	if (v == 0)
		return FM_OK;
	return fm_wr(d, FM6000_SBUS_CFG, 0);
}

int fm_sbus_txn(struct fm6000 *d, uint8_t op, uint8_t dev, uint8_t reg,
		uint32_t data_in, uint32_t *out, unsigned *rc)
{
	uint32_t cmd, sc = 0;
	unsigned spins;
	int rv;

	if (rc != NULL)
		*rc = 0;

	/* REQUEST carries the data for a write and is harmless for a read;
	 * the datasheet sets it unconditionally and so does this. */
	rv = fm_wr(d, FM6000_SBUS_REQUEST, data_in);
	if (rv != FM_OK)
		return rv;

	/* Clear any stale Execute first. A command register still holding a
	 * finished command ignores the next write, which presents as a
	 * transaction that silently returns the previous one's answer. */
	rv = fm_wr(d, FM6000_SBUS_COMMAND, 0);
	if (rv != FM_OK)
		return rv;

	cmd = CMD_EXECUTE |
	      ((uint32_t)op << CMD_OP_SHIFT) |
	      ((uint32_t)dev << CMD_DEV_SHIFT) |
	      ((uint32_t)reg << CMD_REG_SHIFT);
	rv = fm_wr(d, FM6000_SBUS_COMMAND, cmd);
	if (rv != FM_OK)
		return rv;

	for (spins = 0; spins < SBUS_POLL_LIMIT; spins++) {
		rv = fm_rd(d, FM6000_SBUS_COMMAND, &sc);
		if (rv != FM_OK)
			return rv;
		if (!(sc & CMD_BUSY))
			break;
	}
	if (sc & CMD_BUSY) {
		/* Leave the register clear so the next caller is not reading
		 * this one's wreckage. */
		(void)fm_wr(d, FM6000_SBUS_COMMAND, 0);
		return FM_ETIMEOUT;
	}

	if (rc != NULL)
		*rc = (sc >> CMD_RC_SHIFT) & CMD_RC_MASK;
	if (op == FM_SBUS_OP_READ && out != NULL) {
		rv = fm_rd(d, FM6000_SBUS_RESPONSE, out);
		if (rv != FM_OK)
			return rv;
	}
	return fm_wr(d, FM6000_SBUS_COMMAND, 0);
}

int fm_sbus_present(struct fm6000 *d, uint8_t dev)
{
	unsigned rc = 0;
	uint32_t v = 0;

	/* Register 2 rather than 0: register 0 reads zero on every device here,
	 * so it tells you nothing -- but the result code does, whichever
	 * register is asked for. Register 2 is used because it is known to be
	 * non-zero on a real lane, which makes a positive result visible as
	 * well as reported. */
	if (fm_sbus_txn(d, FM_SBUS_OP_READ, dev, 2, 0, &v, &rc) != FM_OK)
		return 0;
	return rc == FM_SBUS_RC_OK;
}

int fm_sbus_read(struct fm6000 *d, uint8_t dev, uint8_t reg, uint32_t *out)
{
	return fm_sbus_txn(d, FM_SBUS_OP_READ, dev, reg, 0, out, NULL);
}

int fm_sbus_write(struct fm6000 *d, uint8_t dev, uint8_t reg, uint32_t val)
{
	return fm_sbus_txn(d, FM_SBUS_OP_WRITE, dev, reg, val, NULL, NULL);
}

int fm_sbus_device_reset(struct fm6000 *d, uint8_t dev)
{
	return fm_sbus_txn(d, FM_SBUS_OP_RESET, dev, 0, 0, NULL, NULL);
}

/*
 * Table 9-4, transcribed.
 *
 * Each EPL owns four consecutive SBus addresses and the order on the ring is
 * physical, not numerical -- EPL[1] then EPL[3] then EPL[5] -- so there is no
 * arithmetic that produces this and it has to be a table.
 *
 * ⚠ DATASHEET ERRATUM AT SBUS 17. Table 9-4 prints EPL[6] twice, at SBus 17
 * and at SBus 85, and never prints EPL[7]. Every other EPL from 1 to 24
 * appears exactly once, and the one gap is 7, sitting where the first EPL[6]
 * is. So SBus 17 is EPL[7] here. Taking the table literally would map two
 * different EPLs onto one address and send somebody after a dark port.
 */
static const uint8_t epl_sbus[FM_SBUS_EPL_MAX + 1] = {
	[1]  = 5,  [2]  = 77, [3]  = 9,  [4]  = 81,
	[5]  = 13, [6]  = 85, [7]  = 17, [8]  = 89,
	[9]  = 45, [10] = 93, [11] = 49, [12] = 97,
	[13] = 53, [14] = 41, [15] = 57, [16] = 21,
	[17] = 25, [18] = 29, [19] = 33, [20] = 37,
	[21] = 61, [22] = 65, [23] = 69, [24] = 73,
};

uint8_t fm_sbus_epl_base(unsigned epl)
{
	if (epl < FM_SBUS_EPL_MIN || epl > FM_SBUS_EPL_MAX)
		return 0;
	return epl_sbus[epl];
}
