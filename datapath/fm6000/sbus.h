/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The SerDes serial bus.
 *
 * Every one of this chip's 96 Ethernet SerDes hangs off one slow ring, and the
 * SBus controller is the only way to reach them. Bringing a port up means
 * talking to its lane here, so this file sits under everything in M4.
 *
 * ⚠ IT IS SLOW AND IT IS SHARED. The datasheet puts a single access at up to
 * 6 µs, which is why the controller latches a command and frees the management
 * bus rather than stalling it. And there is exactly one response register for
 * the whole chip: §9.4 spells out that two masters reading different SerDes
 * gives one of them the other's answer, with no hardware to prevent it. One
 * command outstanding at a time, chip-wide.
 *
 * WHERE THIS COMES FROM. The command sequence and the field layout are in the
 * public datasheet (331496-002 §9.4), which prints them as pseudocode. What it
 * does not give is the bit positions of Execute, Busy and the result code, or
 * the operation codes; those were confirmed on this board by writing a command
 * and reading it back -- the low three bytes come back exactly as written,
 * which is what identifies the field boundaries.
 *
 * ⚠ The datasheet prints `READ << 16` in BOTH its read and its write example.
 * The write one is a typo.
 */
#ifndef NOSAIC_FM6000_SBUS_H
#define NOSAIC_FM6000_SBUS_H

#include <stdint.h>
#include "pci.h"

/* Operations. [RE, and consistent with the ring being an Avago-style SBus] */
#define FM_SBUS_OP_RESET	0x20
#define FM_SBUS_OP_WRITE	0x21
#define FM_SBUS_OP_READ		0x22

/*
 * Result codes, from the 3 bits the controller returns in SBUS_COMMAND.
 *
 * Established with a control rather than by assumption: reading 64 registers
 * from a real SerDes gives rc 4 and 37 non-zero values, and reading the same
 * from device ids that cannot exist (170, 200) gives rc 6 and nothing.
 * Measured 2026-09-26.
 *
 * ⚠ DO NOT TEST A TRANSACTION BY WHETHER THE DATA IS NON-ZERO. Register 0
 * reads 0 on every device on this ring, real or not, which made a working bus
 * look completely dead until a second register was tried.
 */
#define FM_SBUS_RC_OK		4
#define FM_SBUS_RC_NO_DEVICE	6

/* Reserved device ids the datasheet names. §9.4 -- and both answer here. */
#define FM_SBUS_DEV_SPICO	0xfd
#define FM_SBUS_DEV_CONTROLLER	0xfe

/*
 * Take the SBus controller out of reset.
 *
 * ⚠ SBUS_CFG BIT 0 HOLDS IT IN RESET, and the datasheet's wording hides that:
 * it says the register "defines the reset state of the SBUS controller and the
 * clock ratio", and then separately that "the clock ratio should be set to 4",
 * which reads like an instruction to write 4. It is not. On this board the
 * register holds only bit 0 -- write 2, 3, 4 or 5 and it reads back 0 or 1 --
 * and with bit 0 SET every command hangs with Busy stuck. Clearing it is what
 * makes the bus work. Measured 2026-09-25.
 */
int fm_sbus_start(struct fm6000 *d);

/*
 * One transaction. `out` may be NULL for anything but a read.
 *
 * Returns FM_OK on completion -- which means the controller finished, NOT that
 * the device liked the request. The result code is handed back separately in
 * `rc` (3 bits) precisely so a caller can tell those apart; a read that
 * completes with an unexpected rc is a different fault from one that never
 * completes, and treating them alike is how a dead lane looks like a busy one.
 */
int fm_sbus_txn(struct fm6000 *d, uint8_t op, uint8_t dev, uint8_t reg,
		uint32_t data_in, uint32_t *out, unsigned *rc);

/* True if this device answered. Checks the result code, not the data. */
int fm_sbus_present(struct fm6000 *d, uint8_t dev);

int fm_sbus_read(struct fm6000 *d, uint8_t dev, uint8_t reg, uint32_t *out);
int fm_sbus_write(struct fm6000 *d, uint8_t dev, uint8_t reg, uint32_t val);

/*
 * Reset one device on the ring.
 *
 * ⚠ REQUIRED BEFORE WRITING A SERDES. The prior investigation on this chassis
 * found that SBus writes to a SerDes device do not take without an op-0x20
 * device reset first -- and that the vendor's own 389,809-line boot capture
 * issues exactly two of them, to the two SerDes belonging to the two ports that
 * worked. A capture of a boot where 50 ports are empty does not show you the
 * step the other two needed.
 */
int fm_sbus_device_reset(struct fm6000 *d, uint8_t dev);

/*
 * The SBus address of EPL `epl` (1..24), or 0 if there is no such EPL.
 *
 * Lane L of that EPL is at the returned address + L, for L in 0..3.
 */
uint8_t fm_sbus_epl_base(unsigned epl);

#define FM_SBUS_EPL_MIN 1
#define FM_SBUS_EPL_MAX 24

#endif /* NOSAIC_FM6000_SBUS_H */
