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
/*
 * SSCHED -- the scheduler ring engine.
 *
 * Distinct from ESCHED (0x2000), which is the per-port egress scheduler
 * configuration and is only reachable once this engine is circulating. See
 * ssched.c for the measurements behind that.
 */
#define FM6000_BLK_SSCHED		0x008000
#define FM6000_SSCHED_TX_NEXT_PORT(i)	(0x008000u + (i))
#define FM6000_SSCHED_TX_INIT_TOKEN	0x008020
#define FM6000_SSCHED_TX_INIT_COMPLETE	0x008021
#define FM6000_SSCHED_TX_REPLACE_TOKEN	0x008022
#define FM6000_SSCHED_RX_NEXT_PORT(i)	(0x008040u + (i))
#define FM6000_SSCHED_RX_INIT_TOKEN	0x008060
#define FM6000_SSCHED_RX_INIT_COMPLETE	0x008061
#define FM6000_SSCHED_RX_REPLACE_TOKEN	0x008062
#define FM6000_SSCHED_RX_SLOW_PORT(i)	(0x008070u + (i))

/* The scheduler's own freelist init triggers, each with a done bit beside it.
 * Table 4-1 step 10 asks the boot controller to do all of this with one
 * command and boot.c issues it, and CommandDone goes high -- but every one of
 * these four done registers still reads 0 afterwards, measured 2026-09-28.
 * Whether that is a real discrepancy or these simply are not the bits the
 * command sets is not established: writing 1 to FREELIST_INIT does not stick
 * and does not move FREELIST_INIT_DONE either, which is equally consistent
 * with a self-clearing trigger in a block that is not being clocked. [RE] */
#define FM6000_SSCHED_RXQ_FREELIST_INIT		0x0080f0
#define FM6000_SSCHED_RXQ_FREELIST_INIT_DONE	0x0080f1
#define FM6000_SSCHED_TXQ_FREELIST_INIT		0x0080f4
#define FM6000_SSCHED_TXQ_FREELIST_INIT_DONE	0x0080f5
#define FM6000_SSCHED_HS_FREELIST_INIT		0x0080f8
#define FM6000_SSCHED_HS_FREELIST_INIT_DONE	0x0080f9
#define FM6000_SSCHED_FREELIST_INIT		0x0080fc
#define FM6000_SSCHED_FREELIST_INIT_DONE	0x0080fd

/* The metering rate limiter -- the policer sweeper, which is what MRL stands
 * for throughout this chip. [DS §5.13.5 for the two the datasheet names, RE
 * for the addresses and the rest of the block]
 *
 * Worth knowing because the sweeper's own timing is quoted in segment
 * scheduler port tokens, so this block and the ring are neighbours. Read on a
 * booted chip 2026-09-28: MGMT_CYCLES 0x8208, SWEEP_CYCLES 8, SWEEP_PERIOD
 * 0x50, RATE_LIMITER 0x128 -- and UNROLL_ITER 0, where the datasheet calls
 * 4095 nominal. Nothing is concluded from that yet; no policer is configured. */
#define FM6000_FC_MRL_MGMT_CYCLES	0x028000
#define FM6000_FC_MRL_SWEEP_CYCLES	0x028010
#define FM6000_FC_MRL_SWEEP_PERIOD	0x028014
#define FM6000_FC_MRL_UNROLL_ITER	0x028015
#define FM6000_FC_MRL_TOKEN_LIMIT	0x028018
#define FM6000_FC_MRL_FC_TOKEN_LIMIT	0x028020
#define FM6000_FC_MRL_ALIGN_TX_STATS	0x028021
#define FM6000_FC_MRL_RATE_LIMITER	0x028022

/* The egress scheduler's deficit round robin state, and the congestion
 * manager's view of the egress scheduler. All three read as refused or zero
 * until the ring circulates, which is the point -- CM_ESCHED_STATE is the
 * first register we have that reports on the scheduler from outside the
 * block it is stuck in. [RE] */
#define FM6000_ESCHED_DRR_Q		0x003000
#define FM6000_ESCHED_DRR_CFG		0x003800
#define FM6000_ESCHED_DRR_DC_INIT	0x003c00
#define FM6000_CM_ESCHED_STATE		0x116c00

/* 80 ring slots, one byte each; five 16-bit slow-port masks. */
#define FM6000_SSCHED_NEXT_PORT_WORDS	20
#define FM6000_SSCHED_SLOW_PORT_WORDS	5

/* The Found bits the engine sets when a find-probe located the token. */
#define FM6000_SSCHED_RX_FOUND		(1u << 21)
#define FM6000_SSCHED_TX_FOUND		(1u << 30)
/* A find is asynchronous: the engine has to reach the slot. */
#define FM6000_SSCHED_FIND_US		50000

/*
 * The SerDes reference clock, datasheet §6.4 Table 6-7: one 156.25 MHz
 * input per group of six EPLs.
 *
 *   ETH_REFCLK1  EPL  1  3  5  7  9 11
 *   ETH_REFCLK2  EPL  2  4  6  8 10 12
 *   ETH_REFCLK3  EPL 13 15 17 19 21 23
 *   ETH_REFCLK4  EPL 14 16 18 20 22 24
 *
 * ⚠ EVERY PORT THIS PORT HAS EVER BROUGHT UP IS ON ETH_REFCLK4. Panels 1-8
 * are EPL 14 and EPL 16, which share a reference clock, so "the receiver
 * never locks" has only ever been observed on one of the four. That is a
 * blind spot rather than a finding: a dead reference would look exactly
 * like this. Bringing a port up on an EPL in another group is the cheap way
 * to rule it out, and it needs a module moved to a cage on one.
 */
static inline unsigned fm6000_epl_refclk(unsigned epl)
{
	return ((epl - 1) % 2) + (epl >= 13 ? 3 : 1);
}

/* The scheduler's tick -- the clock the whole engine, and ESCHED, runs on. */
#define FM6000_SSCHED_TICK_CFG		0x00f010
#define FM6000_SSCHED_TICK_PERIOD	0x2

/* The sweeper shares the scheduler's clock domain. */
#define FM6000_SWEEPER_CFG_0		0x01c048
#define FM6000_SWEEPER_CFG_1		0x01c049
#define FM6000_SWEEPER_CFG_2		0x01c04a
#define FM6000_SWEEPER_CFG_3		0x01c04b
#define FM6000_SWEEPER_CFG_4		0x01c04c

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
 * The SerDes serial bus controller. [DS §9.4]
 *
 * SBUS_CFG's bit 0 holds the controller in reset; see fm_sbus_start() for why
 * that is not obvious from the datasheet's wording.
 */
/*
 * SBUS_SPICO, JSS + 0x04: the SerDes micro-controller's own reset.
 *
 * ⚠ SOFT_RESET IS NOT THE WHOLE OF STEP 7. Table 4-1 step 7 says "take all
 * modules out of reset (EPL, PCIe, MSB, SPICO/SBUS)", and driving SOFT_RESET
 * to zero does not clear this bit: after the documented boot it still reads
 * 1, which by §9.4.1 is "SPICO controller is in reset and all internal
 * circuits reset to their default state".
 *
 * Reset is cleared here and Enable is left alone. §9.4.1's three states are
 * Reset, Disabled (out of reset, not running, and the only state in which
 * code can be downloaded) and Enabled. Disabled is what step 7 asks for, and
 * enabling a micro-controller that has had no code loaded is not something
 * to do because a forwarding chip happens to read Enable set -- that chip
 * has firmware in it.
 */
#define FM6000_SBUS_SPICO		0x00f004
#define FM6000_SBUS_SPICO_RESET		(1u << 0)
#define FM6000_SBUS_SPICO_ENABLE	(1u << 1)

#define FM6000_SBUS_CFG			0x00f000
#define FM6000_SBUS_COMMAND		0x00f001
#define FM6000_SBUS_REQUEST		0x00f002
#define FM6000_SBUS_RESPONSE		0x00f003

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
 * The five-register window is one shift-register port, not five independent
 * registers. Every access is a pair: select a chain in SCAN_SELECT, write one
 * 32-bit word into whichever data-in register that chain expects, then read
 * SCAN_STATUS to see the shift retire. See mrl.c for the sequence.
 *
 * The conflict this block used to record -- datasheet says one write, prior
 * work says a several-thousand-step scan program -- is resolved: they are two
 * different operations. Step 5 really is one write. The scan program is a
 * separate erratum workaround, applied after the boot commands and gated on a
 * chip-revision check, not part of Table 4-1 at all.
 *
 * ⚠ We do not write SCAN_SELECT before the step-5 write, so that write lands
 * on whichever chain the reset default selects. That is a gap, not a decision:
 * the vendor sequence never touches a data-in register without setting the
 * selector in the same breath. Which selector step 5 wants is not documented
 * and has not been measured here, so it is not guessed.
 */
#define FM6000_SCAN_CONTROL		0x01c039	/* [RE] chain selector, [4:0] */
#define FM6000_SCAN_CONFIG_DATA_IN	0x01c03a	/* [OURS] warm 0xffffffff */
#define FM6000_SCAN_CHAIN_DATA_IN	0x01c03b	/* [OURS] warm 0xffffffff */
#define FM6000_SCAN_SPARE		0x01c03c	/* [RE] in the window, never accessed */
#define FM6000_SCAN_STATUS		0x01c03d	/* [RE] shift status, [9:8] */
#define FM6000_SCAN_FIRST		0x01c039	/* [RE] the window is */
#define FM6000_SCAN_LAST		0x01c03d	/* [RE] 0x1c039..0x1c03d */

/* SCAN_STATUS[9:8] after a shift. The vendor sequence treats 01b as "the word
 * retired" and anything else as an error, re-reading once before giving up.
 * The other three encodings have never been observed, so they are not named.
 * [RE] */
#define FM6000_SCAN_STATUS_MASK		0x00000300
#define FM6000_SCAN_STATUS_RETIRED	0x00000100

/* Chain selectors seen in use. These two are the only ones the vendor erratum
 * sequence ever selects; the field is five bits wide, so there are thirty more
 * that nothing here has touched. The names describe what each one is shifted
 * full of, which is all we know about them. [RE] */
#define FM6000_SCAN_CHAIN_CORE		0x10
#define FM6000_SCAN_CHAIN_BANKS		0x14

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
/*
 * EPL ADDRESSING, derived by sweeping the block. [OURS, measured]
 *
 * The block is completely regular once you see it. Every EPL owns eight slots
 * of 0x80 words -- 0x400 words in all -- laid out as:
 *
 *     slot 0..3   the four lanes, per-lane registers
 *     slot 4,5    empty
 *     slot 6      the per-EPL registers, EPL_CFG_A and EPL_CFG_B among them
 *     slot 7      empty
 *
 * The sweep found exactly 96 per-lane structures and 24 per-EPL ones, at
 * instance indices 0-3,8-11,16-... and 6,14,22,...,190 respectively, which is
 * that layout and nothing else.
 *
 * ⚠ THE INDEX IS THE FDL'S eplId, NOT THE DATASHEET'S EPL NUMBER. Those two
 * disagree -- see the SBus section -- and it is this one the register block
 * uses: EPL 14, which is what the FDL gives for front-panel port 1, computes
 * to 0x0e3b00, and EPL_CFG_A and EPL_CFG_B were independently found at
 * 0x0e3b01 and 0x0e3b02. The prior investigation's SERDES_IP at 0x0e3841 also
 * falls inside EPL 14 lane 0's slot, which is a second agreement.
 */
#define FM6000_EPL_BASE			0x0e0400
#define FM6000_EPL_STRIDE		0x000400	/* per EPL */
#define FM6000_EPL_LANE_STRIDE		0x000080
#define FM6000_EPL_CFG_SLOT		0x000300	/* slot 6 */

/* Per-lane register block for lane 0..3 of EPL n, n counted as the FDL does. */
#define FM6000_EPL_LANE(n, lane) \
	(FM6000_EPL_BASE + ((n) - 1) * FM6000_EPL_STRIDE + \
	 (lane) * FM6000_EPL_LANE_STRIDE)

/* Per-EPL register block for EPL n. */
#define FM6000_EPL_CFG(n) \
	(FM6000_EPL_BASE + ((n) - 1) * FM6000_EPL_STRIDE + FM6000_EPL_CFG_SLOT)

#define FM6000_EPL_CFG_A_OFF		1
#define FM6000_EPL_CFG_B_OFF		2

/*
 * EPL_CFG_B carries a PCS type selector per port. 3 is 10GBASE-R and 0 is
 * PCS_DISABLE, and a port left at 0 does not transmit -- the prior
 * investigation on this chassis spent two days on a dark port that turned out
 * to be exactly this, with every other register matching a working lane.
 *
 * ⚠ The field WIDTH is not established. A forwarding chip reads 0x00090003
 * and one that has only been booted reads 0x00080000, which is consistent
 * with the selector in the low nibble and something else in bit 16, but four
 * ports share this register and nothing here has seen two of them set at once.
 * [UNKNOWN]
 */
#define FM6000_EPL_CFG_B		0x0e3b02	/* EPL 14: the FIRST one */
/*
 * ⚠ TEN OF THE TWENTY-FOUR EPLs HAVE THEIR LANES REVERSED INSIDE THE
 * PACKAGE. Datasheet §6.2.3, Table 6-2: for these, external lanes
 * {A,B,C,D} reach internal channels {3,2,1,0} rather than {0,1,2,3}.
 *
 * Nothing in this tree depends on it yet, and that is exactly why it is
 * written down. The only ports brought up so far are on EPL 14 and EPL 16,
 * both of which are straight-through, so a lane index that ignored the
 * reversal would work on every port tested and fail on the other
 * forty-four -- as a port that configures cleanly and never links.
 *
 * ⚠ AND IT IS NOT YET KNOWN WHICH INDEX THE BOARD'S FDL GIVES US. The port
 * table in serdes.c takes its lane from the FDL; whether that is the
 * external lane or the internal channel cannot be told from EPL 14 and 16,
 * where the two are the same. Settle it on the first port brought up on a
 * reversed EPL, and use fm6000_epl_lane_reversed() to do the conversion --
 * do not discover it by wondering why the port is dark.
 */
static inline int fm6000_epl_lane_reversed(unsigned epl)
{
	switch (epl) {
	case 1: case 2: case 4: case 8: case 12:
	case 13: case 17: case 20: case 22: case 24:
		return 1;
	default:
		return 0;
	}
}

/* The internal channel an external lane reaches on a given EPL. */
static inline unsigned fm6000_epl_channel(unsigned epl, unsigned lane)
{
	return fm6000_epl_lane_reversed(epl) ? 3u - lane : lane;
}

#define FM6000_EPL_PCS_10GBASE_R		3
#define FM6000_EPL_PCS_DISABLE			0
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
/*
 * ESCHED -- the per-port egress scheduler configuration.
 *
 * ⚠ THE WHOLE BLOCK IS UNREACHABLE UNTIL THE SCHEDULER RING IS CIRCULATING,
 * and the failure is not an error return, it is the chip leaving the bus.
 * Measured 2026-09-26 on a chip that had completed the Table 4-1 boot and
 * whose every other low block answered normally: one read of 0x2020, 0x2080
 * or 0x3800 killed it outright, and writes killed it on about the twentieth.
 * This was recorded for months as "reading 0x2000 off-buses a cold chip",
 * which named one word of a 8192-word block and the wrong direction.
 *
 * ssched.c has the measurements and the mechanism.
 */
#define FM6000_BLK_ESCHED		0x002000
#define FM6000_BLK_ESCHED_SPAN		0x002000

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
