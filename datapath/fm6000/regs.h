/* SPDX-License-Identifier: Apache-2.0 */
/*
 * FM6000 register addresses.
 *
 * ⚠ READ THE PROVENANCE MARK ON EVERY LINE BEFORE YOU TRUST IT.
 *
 * This chip has no SDK we may link and no header we may copy, so every address
 * here was either read out of the public Intel datasheet or worked out against
 * a running chip. Those are not the same kind of fact and they are marked
 * differently:
 *
 *   [DS §x.y]  documented in Intel FM5000/FM6000 datasheet 331496-002 rev 3.4.
 *              The datasheet names registers and documents their FIELDS and
 *              their semantics; it does not, in the sections we have needed,
 *              print their addresses.
 *   [RE]       established against the running chip by the reverse engineering
 *              that precedes this port. Believed, not proven from a second
 *              direction.
 *   [UNKNOWN]  we know the register exists and what it does, and we do not
 *              know where it is. Deliberately absent rather than guessed:
 *              a wrong address here does not fail, it writes somewhere else.
 *
 * ADDRESSES ARE 32-BIT WORD ADDRESSES unless the name says BYTE. Multiply by
 * four for a BAR0 offset. The packet DMA block is the one exception in the
 * chip and it is named accordingly.
 */
#ifndef NOSAIC_FM6000_REGS_H
#define NOSAIC_FM6000_REGS_H

/* PCI identity. [live: lspci on DCS-7150S-52, 2026-09-22] */
#define FM6000_PCI_VENDOR	0x8086
#define FM6000_PCI_DEVICE	0x155b

/* BAR0 is 32 MB = 8M words, so this is the last legal word address.
 * [live: lspci -v reports "Memory at e2000000 (64-bit) [size=32M]"] */
#define FM6000_WORD_MAX		0x7fffff

/*
 * Block bases, by word address. [RE]
 *
 * These come from probing a running chip rather than from a register map, so
 * they are where traffic to a block was observed, not necessarily where the
 * block begins.
 */
#define FM6000_BLK_MGMT		0x01c000
#define FM6000_BLK_CRM		0x01f000
#define FM6000_BLK_EPL		0x0e3000
#define FM6000_BLK_CM		0x110000
#define FM6000_BLK_MOD		0x150000
#define FM6000_BLK_MOD_END	0x15ffff
#define FM6000_BLK_L2F		0x180000
#define FM6000_BLK_STATS	0x200000
#define FM6000_BLK_MCAST_MID	0x240000
#define FM6000_BLK_MCAST_POST	0x260000

/*
 * The ECC bank memories.
 *
 * ⚠ THESE ARE NOT ORDINARY REGISTERS. They come out of reset with their ECC
 * uninitialised, and ONE access to an uninitialised word raises an
 * uncorrectable error that the chip escalates to fatal -- at which point the
 * PCIe endpoint stops responding and every subsequent read returns 0xffffffff
 * while the link stays up. The host sees a stall, an RCU warning or a reboot.
 * It never sees an error.
 *
 * fm_rd()/fm_wr() refuse these ranges until fm_bank_mark_initialised() has
 * been called, which is the whole reason that flag exists. See pci.c.
 *
 * [RE for the addresses; DS §4.2 Table 4-1 steps 9 and 12 for the fact that
 *  initialising them is a documented part of boot]
 */
#define FM6000_BANK_STATS_BASE		FM6000_BLK_STATS
#define FM6000_BANK_MCAST_MID_BASE	FM6000_BLK_MCAST_MID
#define FM6000_BANK_MCAST_POST_BASE	FM6000_BLK_MCAST_POST
/* Extent is not established. One word is known dangerous in each; treating the
 * whole 0x20000-word span as dangerous is the conservative reading and costs
 * nothing until something needs a register inside one. [assumed] */
#define FM6000_BANK_SPAN		0x020000

/*
 * MGMT block registers.
 *
 * BOOT_CTRL carries the boot command in Table 4-1 steps 8-10, and
 * BOOT_STATUS:CommandDone is what you poll. The datasheet names both and
 * documents the command codes; the addresses below are [RE], from watching a
 * working boot.
 */
#define FM6000_BOOT_CTRL	0x01c022	/* [RE] warm reads 0x313, cold 0x320 */
/* BOOT_STATUS, with the CommandDone bit that every BOOT command is polled on.
 * [DS §4.2 Table 4-1 names it; address UNKNOWN] */
/* #define FM6000_BOOT_STATUS	?? */

/* The scan chain. Table 4-1 step 5 is a single write of 0xFFFFFFFF to
 * SCAN_CHAIN_DATA_IN to put the core logic and the EPLs into normal operating
 * mode. [DS §4.2 Table 4-1 step 5 for the operation; RE for the addresses]
 *
 * ⚠ The prior work on this chassis concluded that this single write is NOT
 * sufficient and that a per-block scan program is what actually makes the bank
 * memories writable. The datasheet says otherwise. That conflict is unresolved
 * and is the first experiment this port should run -- see the board's
 * docs/todo.md, M2. */
#define FM6000_SCAN_CONFIG_DATA_IN	0x01c03a	/* [RE] */
#define FM6000_SCAN_CHAIN_DATA_IN	0x01c03b	/* [RE] */
#define FM6000_SCAN_FIRST		0x01c039	/* [RE] the window is */
#define FM6000_SCAN_LAST		0x01c03d	/* [RE] 0x1c039..0x1c03d */

#define FM6000_SWEEPER		0x01c048	/* [RE] warm 0x0008bb2c, cold 0 */

/*
 * SOFT_RESET holds every module at reset by default, and each must be released
 * before the platform is accessed: EPLs, PCIe, JSS (SPICO/SBUS) and MSB.
 * [DS §4.2 "SOFT_RESET Register"]
 *
 * The RESET-ALL VALUE is known -- a cold chip reads 0x16 and bring-up drives it
 * to 0 -- but the register's ADDRESS is not. [RE for the value; UNKNOWN address]
 */
#define FM6000_SOFT_RESET_COLD_VALUE	0x16
/* #define FM6000_SOFT_RESET	?? */

/* PLL_STATUS, polled for lock in Table 4-1 step 6; maximum lock time 80 ms.
 * [DS §4.2 Table 4-1 step 6 names it; address UNKNOWN] */
#define FM6000_PLL_LOCK_MAX_MS		80

/*
 * BOOT_CTRL:Command codes. [DS §4.2, Table 4-1 and the BOOT command list]
 *
 * These are the documented commands, and three of them are the ordered spine
 * of steps 8-10. They are worth having even while BOOT_CTRL's field layout is
 * unknown, because they say what the chip can be asked to do for itself --
 * including initialising the bank memories, which is the wall the prior work
 * hit from the other side.
 */
#define FM6000_BOOT_CMD_FFU_SLICE_NUMBERS	1
#define FM6000_BOOT_CMD_BANK_MEMORY_REPAIRS	2
#define FM6000_BOOT_CMD_FREELISTS_ALL		3
#define FM6000_BOOT_CMD_FREELIST_ARRAY		4
#define FM6000_BOOT_CMD_FREELIST_HEAD_STORAGE	5
#define FM6000_BOOT_CMD_FREELIST_TXQ		6
#define FM6000_BOOT_CMD_FREELIST_RXQ		7

/*
 * EPL. EPL_CFG_B selects the PCS type per port, and 10GBASE-R is the one this
 * board's 52 SFP+ cages need.
 *
 * ⚠ A cold chip reads 0x00080000 here and a working one 0x00090003, where the
 * low nibble is Port0PcsSel=3. This never appears in a port-bounce capture,
 * because the vendor OS sets the PCS type once at boot -- so a bring-up built
 * from a port-flap trace misses it entirely and the port simply never links.
 * [RE]
 */
#define FM6000_EPL_CFG_B		0x0e3b02
#define FM6000_EPL_CFG_B_10GBASE_R	0x00090003
#define FM6000_EPL_CFG_B_COLD		0x00080000

/* A port that is up reads 0x8c0 in PORT_STATUS. [RE] The register's address is
 * per-EPL and the per-port stride is not established. [UNKNOWN] */
#define FM6000_PORT_STATUS_UP		0x8c0

/*
 * ⚠ READ HAZARD: ESCHED at word 0x2000.
 *
 * READING this takes a cold chip off the PCIe bus. Not writing -- reading.
 * The prior investigation on this chassis lost a run to exactly this: a
 * post-microcode probe that read it to see how things were going was the thing
 * that killed the chip it was probing.
 *
 * It is listed here so that the guard in pci.c can refuse it, because the
 * natural instinct on a chip that is misbehaving is to go and read more
 * registers, and this is the one that punishes that. [RE]
 */
#define FM6000_ESCHED_READ_HAZARD	0x002000

/*
 * Packet DMA. ⚠ THIS ONE IS A BYTE OFFSET INTO BAR0, not a word address --
 * the only such block found so far. Reading it as a word address lands in a
 * different block entirely and the chip answers plausibly. [RE]
 *
 * The engine itself is documented: TX and RX buffer-descriptor rings, sized to
 * a power of two and 32-byte aligned, with a 16-byte descriptor of
 * Status / Length / BufferAddrLo / BufferAddrHi. [DS §7.11, Table 7-5]
 */
#define FM6000_DMA_BYTE_BASE		0x5000
#define FM6000_DMA_DESC_BYTES		16
#define FM6000_DMA_RING_ALIGN		32

/*
 * The internal frame tag, inline in the frame at L2 offset 12 -- NOT in the
 * descriptor field that looks like it should carry it. The frame's length
 * includes it.
 *
 * ⚠ Table 7-8 documents SEVEN bytes (DGLORT, SGLORT, SWPRI, USER, FTYPE); the
 * prior work on this chassis measured EIGHT when it snooped a working
 * transmit. One is wrong, or the eighth byte is alignment padding. A tag wrong
 * by one byte produces frames the chip accepts and misparses, so this is
 * settled on the bench before the packet path is trusted.
 * [DS Table 7-8 vs RE -- CONFLICT, unresolved]
 */
#define FM6000_F64_OFFSET		12
#define FM6000_F64_LEN_DATASHEET	7
#define FM6000_F64_LEN_OBSERVED		8

#endif /* NOSAIC_FM6000_REGS_H */
