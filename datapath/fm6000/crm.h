/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_FM6000_CRM_H
#define NOSAIC_FM6000_CRM_H

#include <stdint.h>

struct fm6000;

/*
 * The Counter Rate Monitor -- what Table 4-1 step 12 means by "CRM".
 *
 * WHY THIS EXISTS. Step 12 offers two ways to initialise memory: "Use CRM to
 * setup memory table. Launch CRM execution. Wait for completion." Or:
 * "Software writes memory manually." This port has always done the second,
 * because the CRM was not decoded.
 *
 * That turns out to matter twice over.
 *
 * First, mechanism. A software fill is the CPU writing every word through the
 * management ring, and on this chip an access that cannot complete raises a
 * CRM access timeout, writes FATAL_CODE, and makes the watchdog reset the
 * fabric. Measured: filling the four policer regions by hand took FATAL_COUNT
 * from 8 to 90 while reporting success, because the chip kept coming back
 * between writes. See regs.h.
 *
 * Second, coverage. The vendor initialises 128 memory regions this way before
 * anything else runs -- every parser, mapper, policer, L2AR, MOD, FFU, stats
 * and MAC table on the die. This port initialises exactly one of them, STATS,
 * by hand. That is the far bigger gap, and it is the obvious reason the
 * sweepers fault: they walk tables nobody ever initialised.
 *
 * PROVENANCE. The services, the command set, every field with its width, and
 * the address-generation algorithm are documented [DS 9.3, Tables 9-1 to 9-3
 * and the module pseudo code]. The datasheet does not give bit offsets, so
 * those were inferred from the documented field order -- and then confirmed
 * field by field against the vendor SDK's own fm6000CrmSetMemory, which packs
 * Command at [2:0], Count at [33:14], BaseAddress at [21:0], Size at [23:22],
 * the four shift fields at [27:24], [31:28], [35:32], [39:36], and CRM_CTRL as
 * Run[0], FirstCommandIndex[6:1], LastCommandIndex[12:7]. Nothing is guessed.
 *
 * The addresses are [RE] from the register map, and each block ends exactly
 * where the next named register begins -- four independent confirmations of
 * the two-word stride. The SDK computes CRM_COMMAND's address as
 * (slot + 0xf840) * 2, which is 0x1f080 + slot*2, and CRM_REGISTER's and
 * CRM_PERIOD's the same way from 0xf880 and 0xf8c0:
 *
 *	CRM_DATA     0x1e000  2048 x 2w -> ends 0x1efff, next CRM_CTRL 0x1f000
 *	CRM_COMMAND  0x1f080    64 x 2w -> ends 0x1f0ff, next 0x1f100
 *	CRM_REGISTER 0x1f100    64 x 2w -> ends 0x1f17f, next 0x1f180
 *	CRM_PERIOD   0x1f180    64 x 2w -> ends 0x1f1ff, next 0x1f200
 *	CRM_PARAM    0x1f200    64 x 1w
 */

/* Commands. [DS Table 9-3] Only the two that touch memory are named here; the
 * rest are rate, change and checksum monitors this port has no use for yet. */
#define FM_CRM_CMD_MEMORY_SET	0u	/* initialise a block; PARAM = value */
#define FM_CRM_CMD_MEMORY_COPY	1u	/* PARAM = destination base address */

/* The Size field. The address walk advances 1, 2, 4, 4 words respectively --
 * 96-bit and 128-bit registers both cost four words, which is the datasheet's
 * own pseudo code and not a slip here. [DS 9.3] */
#define FM_CRM_SIZE_32		0u
#define FM_CRM_SIZE_64		1u
#define FM_CRM_SIZE_96		2u
#define FM_CRM_SIZE_128		3u

/* How many command slots the engine has, and the documented ceiling on Count
 * for a command with no data section -- which a memory set is. [DS Table 9-2] */
#define FM_CRM_COMMANDS		64u
#define FM_CRM_COUNT_MAX	1048575u

/* How long to wait for one memory set. The largest region the vendor fills is
 * tens of thousands of registers and the engine runs at the CRM tick, so this
 * is generous on purpose: a timeout here is a finding, and a timeout that was
 * really just impatience would be a bad one. */
#define FM_CRM_DONE_MS		2000u

/*
 * Initialise a memory block in hardware.
 *
 * `base` is the first word address and `count` the number of REGISTERS to walk
 * -- not words and not bytes, which starts to matter as soon as `size` is not
 * 32-bit. Every register in the block is written with `value`; for a register
 * wider than 32 bits the datasheet says this command replicates the value to
 * fill it.
 *
 * `slot` is which of the 64 command registers to borrow. The CRM is not
 * running on this chip -- CRM_CTRL and CRM_STATUS both read 0, measured -- so
 * any slot is free. When that stops being true the caller must pick one the
 * running program does not use, which is what the datasheet means by "possibly
 * by using unused command space".
 *
 * Returns FM_OK once the engine reports the command complete, or FM_ETIMEOUT
 * if it never does. A timeout here is information rather than a bug: a memory
 * the CRM itself cannot walk is a far more interesting fact than a memory the
 * CPU cannot walk.
 *
 * Sample FATAL_COUNT across this call, as with any sequence on this chip. The
 * entire point is that it should not move. If it does, the CRM is hitting the
 * same wall the software fill does, and that is worth knowing at once.
 */
int fm_crm_memset(struct fm6000 *d, unsigned slot, uint32_t base,
		  uint32_t count, unsigned size, uint32_t value);

/* Is the CRM running? 1, 0, or negative if it could not be read. Borrowing a
 * command slot while it runs would corrupt somebody else's program. */
int fm_crm_running(struct fm6000 *d);

/* Stop the engine and wait for it to say it has stopped. The datasheet warns
 * this can take a while: the CRM stops only at a command boundary, and a
 * command walking a large data set holds it. [DS 9.3] */
int fm_crm_stop(struct fm6000 *d);

#endif /* NOSAIC_FM6000_CRM_H */
