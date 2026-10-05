/* SPDX-License-Identifier: Apache-2.0 */
/* The SerDes serial bus. See sbus.h for where the numbers come from. */
#include <stddef.h>
#include <unistd.h>

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
	if (v != 0) {
		rv = fm_wr(d, FM6000_SBUS_CFG, 0);
		if (rv != FM_OK)
			return rv;
	}

	/*
	 * And the SerDes micro-controller out of reset -- here, not in the
	 * boot.
	 *
	 * Table 4-1 step 7 says to take SPICO out of reset along with the
	 * other modules, and SOFT_RESET does not do it: SBUS_SPICO has its own
	 * Reset bit and it still reads 1 after the documented boot.
	 *
	 * ⚠ BUT CLEARING IT DURING STEP 7 TAKES THE CHIP OFF THE BUS. Measured
	 * twice, reproducibly, from a verified-recovered chip each time. Doing
	 * it here -- after the SBus controller itself is out of reset -- is
	 * survivable, which is the order the dependency implies anyway: SPICO
	 * sits on the bus this function has just started.
	 *
	 * Reset cleared, Enable left alone. §9.4.1's middle state, "out of
	 * reset but the micro-processor is not running", is the only one in
	 * which code can be downloaded, and enabling a micro-controller with
	 * no code in it is not something to do because a chip that has
	 * firmware reads Enable set.
	 */
	rv = fm_rd(d, FM6000_SBUS_SPICO, &v);
	if (rv != FM_OK)
		return rv;
	if ((v & FM6000_SBUS_SPICO_RESET) == 0)
		return FM_OK;
	return fm_wr(d, FM6000_SBUS_SPICO, v & ~(uint32_t)FM6000_SBUS_SPICO_RESET);
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
 * ⚠ THE EPL NUMBERS HERE ARE THE DATASHEET'S, NOT THE BOARD'S. The FDL
 * numbers EPLs in the register-block space and this table is indexed in the
 * datasheet's, and the two orderings are not the same: the board's "EPL 14"
 * takes SBus 0x49, which this table calls EPL[24]. Measured -- see
 * fm_port_sbus_dev() in serdes.c, where using this table with an FDL EPL
 * number was tried and left the transmitter dark.
 *
 * So this is the right answer to "where is datasheet EPL[n] on the ring"
 * and the wrong answer to "which SerDes belongs to this port". For the
 * second, ask the port table.
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

/*
 * Load nwords 16-bit instruction words into the SPICO.
 *
 * The SPICO download protocol (cross-checked against EdgeNOS fm6000_load_spico,
 * our own prior live-validated work on this chassis):
 *
 *   Pre:  WriteSBus(0xFD, 0x0C, 0x03)       -- reset SPICO processor
 *         WriteSBus(0xFD, 0x0C, 0x01)       -- enable SPICO for IMEM writes
 *         WriteSBus(0xFD, 0x06, 0x08)       -- IMEM write enable
 *   Per word:
 *         WriteSBus(0xFD, 0x04, addr >> 8)  -- address high byte
 *         WriteSBus(0xFD, 0x05, addr & 0xFF) -- address low byte
 *         WriteSBus(0xFD, 0x07, word & 0xFF) -- data low byte (10-bit word)
 *         WriteSBus(0xFD, 0x06, (word>>8)|0xC) -- data[9:8] + strobe
 *         WriteSBus(0xFD, 0x06, (word>>8)|0x8) -- data[9:8] + strobe release
 *   Post: WriteSBus(0xFD, 0x06, 0x00)       -- IMEM write disable
 *         (run command issued by fm_spico_load, not here)
 *
 * Pre-condition: fm_sbus_start() was called (SPICO out of reset at host level).
 * Post-condition: firmware in IMEM, SPICO enabled but not yet running.
 *
 * ⚠ The run command (0x0C=0x08) is NOT issued here. During the 30,000 SBus
 * writes the chip may self-reset ~39 times, resetting SBUS_CFG to its default
 * (non-zero = SBus master held in reset). If the reset happens just before or
 * during the run command, the command is dropped by the idle master and SPICO
 * never starts. fm_spico_load() calls fm_sbus_start() after the download to
 * restore SBUS_CFG=0, then issues 0x0C=0x08 on a known-good controller. [RE]
 *
 * IMEM words are 10-bit (all words in the firmware are <= 0x3FF). The
 * word>>8 field never exceeds 0x3, so (word>>8)|0xC lands in {0xC..0xF}
 * and (word>>8)|0x8 lands in {0x8..0xB}. [RE confirmed 2026-10-05]
 */
int fm_spico_download(struct fm6000 *d, const uint16_t *code, int nwords)
{
	int i, rv;

	/* Reset then enable the SPICO processor before writing IMEM. */
	if ((rv = fm_sbus_write(d, FM_SBUS_DEV_SPICO, 0x0C, 0x03)) != FM_OK)
		return rv;
	if ((rv = fm_sbus_write(d, FM_SBUS_DEV_SPICO, 0x0C, 0x01)) != FM_OK)
		return rv;
	if ((rv = fm_sbus_write(d, FM_SBUS_DEV_SPICO, 0x06, 0x08)) != FM_OK)
		return rv;

	for (i = 0; i < nwords; i++) {
		uint16_t w = code[i];

		if ((rv = fm_sbus_write(d, FM_SBUS_DEV_SPICO, 0x04,
					(uint32_t)(i >> 8))) != FM_OK)
			return rv;
		if ((rv = fm_sbus_write(d, FM_SBUS_DEV_SPICO, 0x05,
					(uint32_t)(i & 0xFF))) != FM_OK)
			return rv;
		if ((rv = fm_sbus_write(d, FM_SBUS_DEV_SPICO, 0x07,
					(uint32_t)(w & 0xFF))) != FM_OK)
			return rv;
		if ((rv = fm_sbus_write(d, FM_SBUS_DEV_SPICO, 0x06,
					(uint32_t)((w >> 8) | 0xC))) != FM_OK)
			return rv;
		if ((rv = fm_sbus_write(d, FM_SBUS_DEV_SPICO, 0x06,
					(uint32_t)((w >> 8) | 0x8))) != FM_OK)
			return rv;
	}

	if ((rv = fm_sbus_write(d, FM_SBUS_DEV_SPICO, 0x06, 0x00)) != FM_OK)
		return rv;
	/* Leave SPICO in "enabled, IMEM loaded, not yet running" state.
	 * The run command (0x0C=0x08) is issued by fm_spico_load() AFTER
	 * a fm_sbus_start() call, so the SBus controller is known active
	 * regardless of any chip self-resets during the IMEM upload. */
	return FM_OK;
}

/*
 * Set the host-side ENABLE bit and wait for SPICO to initialise.
 *
 * fm_spico_download() already wrote 0x0C=0x08 (run) via SBus, starting the
 * processor.  This sets the host-side ENABLE bit so the chip allows SBus
 * interrupt transactions (needed by --spico N).  The EOS driver polls a SPICO
 * internal register for the ready signal; we use a 200 ms empirical delay as
 * a first step. [RE 2026-10-05 — cross-checked against EdgeNOS]
 *
 * ⚠ Do NOT write 0x0C=0x00 here.  0x0C=0 is the "done" signal for the
 * CRC-verify handshake (fm6000InterruptSpico cmd 4), which comes AFTER the
 * processor has been verified running.  Writing it unconditionally stops
 * SPICO before it can establish its tick clock. [RE 2026-10-05]
 */
int fm_spico_enable(struct fm6000 *d)
{
	uint32_t v;
	int rv;

	if ((rv = fm_rd(d, FM6000_SBUS_SPICO, &v)) != FM_OK)
		return rv;
	if ((rv = fm_wr(d, FM6000_SBUS_SPICO, v | FM6000_SBUS_SPICO_ENABLE)) != FM_OK)
		return rv;

	/* Wait for SPICO to complete its internal initialisation. */
	usleep(200000);
	return FM_OK;
}
