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
 *   [OURS]     read by THIS code on the lab 7150S (2026-09-23, chip warm under
 *              EOS 4.16.8M) and found to hold the value the [RE] note
 *              predicted. That is confirmation from a second direction, by a
 *              second implementation -- and it also proves the word addressing
 *              below is right, because a wrong stride would not have produced
 *              five predicted values in a row.
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
/* ⚠ The EPL block is 0x0e0000..0x0effff, a full 64K words. An earlier version
 * of this file had it at 0x0e3000 spanning 0x2000, which was where traffic to
 * it had been *observed* rather than where it starts -- and since reading an
 * uninitialised EPL word takes the chip off the bus, a block bound that is too
 * narrow is a guard with a hole in it. */
#define FM6000_BLK_EPL		0x0e0000
#define FM6000_BLK_EPL_SPAN	0x010000
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
/*
 * ⚠ THE OLD MODEL HERE WAS WRONG, AND MEASURING IT COST A DOZEN RESET PULSES.
 *
 * It said there were three bank memories -- STATS, MCAST_MID, MCAST_POST --
 * each 0x20000 words. Filling them on hardware 2026-09-25 says otherwise:
 *
 *   0x200000 .. 0x23ffff   262144 words, filled in one go, chip fine.
 *                          This IS a memory, and it is TWICE the modelled
 *                          span: the old 0x20000 stopped halfway through it.
 *
 *   0x240000               NOT a memory. 54 words in, writing 0x240036 takes
 *                          the chip off the bus.
 *
 *   0x260000               NOT a memory. 20 words in, writing 0x260014 does
 *                          the same.
 *
 * Both were bisected exactly rather than bounded. So there is one bank memory
 * on this part as far as anything here knows, the two "MCAST" addresses are
 * register blocks that happen to sit where traffic to them was once observed,
 * and treating a register block as fillable memory is how you lose a chip.
 * [OURS, measured]
 */
#define FM6000_BANK_STATS_BASE		FM6000_BLK_STATS
#define FM6000_BANK_STATS_SPAN		0x040000

/*
 * Words whose WRITE is measured to take the chip off the bus.
 *
 * Reading them has not been tried, so the guard refuses both directions: the
 * cost of refusing a read nobody needs is nothing, and the cost of finding out
 * is a reset pulse and a re-boot of the chip.
 */
#define FM6000_FATAL_WRITE_1		0x240036
#define FM6000_FATAL_WRITE_2		0x260014

/*
 * MGMT block registers.
 *
 * BOOT_CTRL carries the boot command in Table 4-1 steps 8-10, and
 * BOOT_STATUS:CommandDone is what you poll. The datasheet names both and
 * documents the command codes; the addresses below are [RE], from watching a
 * working boot.
 */
#define FM6000_BOOT_CTRL	0x01c022	/* [OURS] read 0x00000313 warm, as predicted */

/*
 * PIN_STRAP -- the sampled boot configuration pins.
 *
 * Identified 2026-09-23: this register read 0x00000208 on the warm lab board,
 * and the prior investigation's starting state for a cold bring-up is written
 * "PIN_STRAP=0x208".
 *
 * Confirmed from a second direction 2026-09-25: a cold chip, reached over the
 * SCD local bus with nothing done to it but a reset pulse, reads 0x00000208
 * here too. Straps are latched in hardware and do not depend on configuration,
 * so agreement between a cold chip and a forwarding one is what this register
 * ought to show -- and it makes it the cheapest liveness beacon on the part.
 * [OURS, confirmed]
 *
 * fm_alive() uses exactly that. A chip that has been knocked off the local bus
 * reads 0x00000000 here -- note ZERO, not the 0xffffffff a dead PCIe endpoint
 * answers, because on the local bus there is no PCIe error semantics to give
 * the all-ones. Code that tests for 0xffffffff will not notice a dead chip on
 * this path.
 */
#define FM6000_PIN_STRAP	0x01c021
#define FM6000_PIN_STRAP_VALUE	0x00000208	/* this board, cold and warm */
/* BOOT_STATUS, with the CommandDone bit that every BOOT command is polled on.
 * [DS §4.2 Table 4-1 names it; address UNKNOWN] */
/* #define FM6000_BOOT_STATUS	?? */

/*
 * SOFT_RESET -- Table 4-1 step 7, and it is nowhere near the other boot
 * registers.
 *
 * Word 0x9. Not in the MGMT block at all, which is why sweeping 0x1c000,
 * 0x1a000, 0x1b000, 0x1d000 and 0x1e000 for it found nothing: it sits almost at
 * the bottom of the address space.
 *
 * A SET bit means that block is HELD in reset. Confirmed on this board
 * 2026-09-25: reads 0x00000000 warm, under EOS, with the chip forwarding --
 * which is what "every module released" has to look like.
 *
 * ⚠ ORDER MATTERS FOR MSB. Releasing the core fabric before the boot
 * controller's bank-repair and freelist commands have run drops the CPU into an
 * unconfigured fabric and hangs it. Release it last.
 */
#define FM6000_SOFT_RESET		0x000009
#define FM6000_SOFT_RESET_PCIE		(1u << 0)	/* PCIe controller */
#define FM6000_SOFT_RESET_MSB		(1u << 1)	/* core fabric: parser, FFU, L2AR */
#define FM6000_SOFT_RESET_FIBM		(1u << 2)	/* in-band management mailbox */
#define FM6000_SOFT_RESET_JSS		(1u << 3)	/* SerDes micro (SPICO) and SBus */
#define FM6000_SOFT_RESET_EPL		(1u << 4)	/* Ethernet Port Logic */

/*
 * PLL and DLL. Table 4-1 step 6.
 *
 * 0x0f is everything locked: PLLs in [1:0], DLLs in [3:2]. Measured on this
 * board 2026-09-25 -- 0x3 on a chip that has had nothing but a reset pulse
 * (both PLLs locked, neither DLL), 0x7 warm under EOS. So the PLLs lock on
 * their own and the DLLs are what bring-up has to do something about.
 *
 * DLL_CTRL reads 0 in both states, so it is write-only or its enable does not
 * read back; nothing here depends on reading it. [UNKNOWN]
 */
#define FM6000_PLL_STATUS		0x01c046
#define FM6000_PLL_STATUS_LOCKED_ALL	0x0000000f
#define FM6000_PLL_STATUS_PLL_MASK	0x00000003
#define FM6000_DLL_CTRL			0x01c045
#define FM6000_DLL_CTRL_ENABLE		0x00000003

/*
 * BOOT_STATUS is BOOT_CTRL. There is no separate register: Table 4-1 says to
 * write BOOT_CTRL:Command and poll BOOT_STATUS:CommandDone, and both live at
 * 0x1c022.
 *
 * Measured on this board 2026-09-25, and the two readings decode cleanly under
 * this layout, which is what makes it believable rather than merely asserted:
 *
 *   cold  0x320  = EepromLoadDone, Command 0     (nothing commanded yet)
 *   warm  0x313  = CommandDone, Command 3        (3 is the LAST of the three
 *                                                 Table 4-1 commands)
 *
 * Bits 8 and 9 are set in both and are not identified. [UNKNOWN]
 */
#define FM6000_BOOT_CTRL_CMD_MASK	0x0000000f
#define FM6000_BOOT_STATUS_CMD_DONE	(1u << 4)
#define FM6000_BOOT_CTRL_EEPROM_DONE	(1u << 5)

/* The scan chain. Table 4-1 step 5 is a single write of 0xFFFFFFFF to
 * SCAN_CHAIN_DATA_IN to put the core logic and the EPLs into normal operating
 * mode. [DS §4.2 Table 4-1 step 5 for the operation; RE for the addresses]
 *
 * ⚠ The prior work on this chassis concluded that this single write is NOT
 * sufficient and that a per-block scan program is what actually makes the bank
 * memories writable. The datasheet says otherwise. That conflict is unresolved
 * and is the first experiment this port should run -- see the board's
 * docs/todo.md, M2. */
#define FM6000_SCAN_CONFIG_DATA_IN	0x01c03a	/* [OURS] warm 0xffffffff */
#define FM6000_SCAN_CHAIN_DATA_IN	0x01c03b	/* [OURS] warm 0xffffffff */
#define FM6000_SCAN_FIRST		0x01c039	/* [RE] the window is */
#define FM6000_SCAN_LAST		0x01c03d	/* [RE] 0x1c039..0x1c03d */

#define FM6000_SWEEPER		0x01c048	/* [OURS] read 0x0008bb2c warm, as predicted */

/*
 * A control register whose meaning is unknown and whose warm/cold difference
 * is not. Bit 24 is set on a warm chip and clear on a cold one, so it is one
 * of the few known handles on "has this chip been brought up".
 * [RE for the delta; OURS for the warm value 0x0101e848]
 */
#define FM6000_MGMT_0X1C038	0x01c038
#define FM6000_MGMT_0X1C038_WARM_BIT	(1u << 24)

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
/*
 * HOW TO FIND IT, now that a warm fingerprint of the MGMT block exists.
 *
 * SOFT_RESET reads 0x16 on a cold chip and 0 once bring-up has released every
 * module, so it is one of the MGMT words that is ZERO warm and 0x16 cold. Warm
 * alone cannot pick it out -- most of the block is zero -- but a cold dump of
 * 0x1c000..0x1c07f diffed against the warm one should leave very few
 * candidates, and only one of them holding exactly 0x16.
 *
 * The same diff is the way to find PLL_STATUS and BOOT_STATUS. That single
 * experiment unblocks Table 4-1 steps 6 through 10, which is most of boot.c.
 */

/* PLL_STATUS, polled for lock in Table 4-1 step 6; maximum lock time 80 ms.
 * [DS §4.2 Table 4-1 step 6 names it; address UNKNOWN] */
#define FM6000_PLL_LOCK_MAX_MS		80

/* No documented bound for a boot-controller command. 500 ms is far longer than
 * any of the three should need and short enough that a wedged one is reported
 * rather than waited on. [OURS] */
#define FM6000_BOOT_CMD_MAX_MS		500

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
 * A working chip reads 0x00090003 here, where the low nibble is Port0PcsSel=3.
 * This never appears in a port-bounce capture, because the vendor OS sets the
 * PCS type once at boot -- so a bring-up built from a port-flap trace misses it
 * entirely and the port simply never links. [RE]
 *
 * ⚠⚠ DO NOT READ THIS ADDRESS BEFORE THE COLD BOOT HAS RUN. Measured on the
 * bench 2026-09-25, over the local bus: reading word 0x0e3b02 after nothing but
 * a reset pulse takes the chip off the bus. PIN_STRAP, which is a hardware
 * strap and reads 0x208 on any live chip, reads 0 from that moment on, and
 * stays 0 until the next reset pulse. Reads of MGMT and the blocks either side
 * are harmless -- it is EPL specifically.
 *
 * AND IT IS FIXED BY RUNNING TABLE 4-1. Same word, same board, immediately
 * after fm_boot_cold() succeeds: reads 0x00080000, chip still answering. So
 * fm_hazard() refuses the EPL block until fm_boot_mark_done(), and not after.
 *
 * That also rehabilitates a note this file used to carry and that I removed as
 * wrong: "a cold chip reads 0x00080000". It does -- for a chip that has been
 * through the boot sequence but not yet configured. It is not what a chip that
 * has had only a reset pulse does, because that chip does not survive the read.
 */
#define FM6000_EPL_CFG_B		0x0e3b02	/* [OURS] */
#define FM6000_EPL_CFG_B_10GBASE_R	0x00090003	/* configured, forwarding */
#define FM6000_EPL_CFG_B_POST_BOOT	0x00080000	/* booted, not configured */

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
