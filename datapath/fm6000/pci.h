/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Reaching the FM6000's registers, and surviving it.
 *
 * There is no BDE here because there is no CMIC. This file maps the chip's
 * register window and hands out two accessors over it.
 *
 * THERE ARE TWO WINDOWS, and which one you want depends on whether the chip has
 * been brought up yet.
 *
 *   FM_XPORT_PCIE   the FM6000's own BAR0, as an 8086:155b endpoint.
 *   FM_XPORT_LBUS   the same registers, seen through the Arista SCD's BAR1.
 *
 * ⚠ ON A COLD BOARD ONLY THE SECOND ONE EXISTS. The FM6000 does not enumerate
 * on PCIe until it has been configured, so a bring-up that waits for the
 * endpoint to appear waits forever -- which is exactly how this port lost
 * several days. The SCD is a PCI-to-LocalBus bridge and the chip's whole
 * register space is mapped into its 16 MB BAR1 at offset 0, word-addressed:
 * register word w is at byte 4*w. That window is live as soon as the SCD
 * enumerates, which it does unconditionally at power-on.
 *
 * The vendor OS does the same thing and says so in its own log -- it reports
 * "localBusHam for alta0" throughout bring-up and only switches to "pciHam for
 * alta0" at the very end, after the chip is configured. PCIe on this part is
 * for packet DMA, not for bring-up.
 *
 * Verified on the bench 2026-09-25 by reading four registers both ways on a
 * forwarding chip -- PIN_STRAP, BOOT_CTRL, SWEEPER and an EPL word all agreed
 * exactly -- and then reading PIN_STRAP over the local bus on a cold board with
 * the FM6000 absent from lspci.
 *
 * It is not a thin wrapper, and it should not become one. Two properties of
 * this silicon make unguarded access a liability rather than a convenience:
 *
 * 1. AN ILLEGAL ACCESS TAKES THE CHIP OFF THE PCIe BUS. Touching an
 *    ECC-uninitialised bank memory word raises an uncorrectable error the chip
 *    escalates to fatal. The endpoint then answers 0xffffffff to everything --
 *    config space included -- while the PCIe link stays up. Nothing raises an
 *    exception, nothing logs, and the failure surfaces minutes later as an MMIO
 *    stall or an RCU warning somewhere unrelated. Every accessor here checks
 *    for it and latches, so the first bad access is reported at the point it
 *    happened and the ten thousand after it never leave the host.
 *
 * 2. SOME ADDRESSES ARE ONLY SAFE AFTER PART OF BOOT HAS RUN. The bank
 *    memories are refused outright until something calls
 *    fm_bank_mark_initialised(). That is a deliberate obstacle: the whole
 *    reason it is awkward to read STATS on a cold chip is that doing so kills
 *    the chip.
 */
#ifndef NOSAIC_FM6000_PCI_H
#define NOSAIC_FM6000_PCI_H

#include <stddef.h>
#include <stdint.h>

#define FM_SLOT_LEN 16

enum fm_xport {
	FM_XPORT_PCIE,	/* the FM6000's own BAR0 -- only once it has enumerated */
	FM_XPORT_LBUS,	/* the SCD's BAR1 -- available cold, and the bring-up path */
};

struct fm6000 {
	/* The device whose BAR is mapped. For FM_XPORT_PCIE that is the
	 * FM6000; for FM_XPORT_LBUS it is the SCD, and the FM6000 has no PCI
	 * address of its own yet. */
	char               slot[FM_SLOT_LEN];  /* "0000:02:00.0" */
	enum fm_xport      xport;
	volatile uint32_t *regs;
	size_t             bar_bytes;
	int                bar_fd;

	/* Sticky. Once the chip has left the bus it does not come back without
	 * a reset, so there is nothing to be gained by trying again -- and a
	 * great deal of log noise to be had. */
	int                offbus;
	/* Set only when the bank memories have actually been initialised. */
	int                banks_ready;
	/* Set once the documented cold boot has run. Unlocks the EPL block,
	 * which is hazardous to read before it and safe after -- measured. It
	 * does NOT unlock the ECC bank memories; those wait for banks_ready,
	 * which is a later and separate thing. */
	int                boot_done;
	/* Whether every write is followed by a config-space check. See
	 * fm_set_write_check(). Defaults on. */
	int                check_writes;

	unsigned long long reads, writes, refused;
};

/*
 * Return values. Negative is failure; the distinction between them matters
 * because one of them means "the chip is gone" and the callers that must stop
 * immediately can only tell from this.
 */
#define FM_OK		0
#define FM_ERR		-1	/* ordinary failure: bad argument, no device */
#define FM_EOFFBUS	-2	/* the chip has left the PCIe bus */
#define FM_EUNSAFE	-3	/* refused: not safe to touch this yet */
/* A step we know we need and cannot do yet -- an address never established,
 * or a routine never decoded. Distinct from FM_ERR so a report can separate
 * "not implemented" from "went wrong", which are different kinds of news. */
#define FM_ENOADDR	-4
/* A wait that did not finish. Distinct from FM_ERR because it says the address
 * was right and the chip answered -- it just never set the bit, which points
 * at the step before rather than at this one. */
#define FM_ETIMEOUT	-5

/*
 * Find the chip and map its BAR0.
 *
 * `slot` may be a PCI address such as "0000:02:00.0", or NULL to search for
 * the first 8086:155b on the bus. Searching is the normal case; naming a slot
 * is for a board that somehow has two.
 *
 * ⚠ IF THIS FINDS NOTHING ON A 7150S, THE CHIP IS PROBABLY STILL IN RESET.
 * The SCD holds the FM6000 down from power-on and it does not appear on the
 * bus until released, so "no device" here is the expected state of an
 * un-initialised board rather than evidence of a fault. Releasing it is the
 * platform HAL's job, not this file's.
 */
int fm_open(struct fm6000 *d, const char *slot);

/*
 * Map the chip through the SCD's local-bus window instead. THIS IS THE ONE TO
 * USE FOR BRING-UP.
 *
 * `scd_slot` is the SCD's PCI address, "0000:04:00.0" on a 7150S-52, or NULL to
 * search for the first 3475:0001. It is board data and belongs in board.yml
 * rather than here.
 *
 * This reaches across into the platform's FPGA, which is not normally the
 * datapath's business. It is done deliberately: the local bus IS the SCD's BAR,
 * so there is no way to address the FM6000 cold without touching the SCD, and
 * hiding that behind the platform HAL would buy nothing but indirection.
 *
 * Releasing the FM6000's reset is still the platform HAL's job and is NOT done
 * here. Note that a release alone is not enough -- the chip answers this window
 * only after a reset PULSE (assert bits 1,2,8 in the SCD's resetSet, then clear
 * them in resetClear). Measured: with the resets merely left clear from boot,
 * every register reads 0.
 */
int fm_open_lbus(struct fm6000 *d, const char *scd_slot);

/*
 * Open the chip whichever way it can be reached, and say which.
 *
 * Tries the FM6000's own BAR0 first and falls back to the SCD's local-bus
 * window. That order is not a preference so much as a diagnosis: if the
 * endpoint is there, the chip has already been configured by somebody, and if
 * it is not, this is a cold board and the local bus is the only way in.
 *
 * This is what the daemon uses, because a daemon that can only reach a chip
 * that is already working is no use on the boot where it is not.
 */
int fm_open_auto(struct fm6000 *d, const char *slot, const char *scd_slot);

void fm_close(struct fm6000 *d);

/* Register access by 32-bit WORD address, which is how the chip's own
 * documentation and every address in regs.h are expressed. */
int fm_rd(struct fm6000 *d, uint32_t word, uint32_t *out);
int fm_wr(struct fm6000 *d, uint32_t word, uint32_t val);

/* Access by BYTE offset into BAR0, for the packet DMA block -- the one region
 * that is addressed that way. Kept separate rather than making callers
 * multiply, because getting this wrong reads a different block and the chip
 * answers plausibly. */
int fm_rd_byte(struct fm6000 *d, uint32_t off, uint32_t *out);
int fm_wr_byte(struct fm6000 *d, uint32_t off, uint32_t val);

/*
 * Whether to confirm after every write that the chip is still on the bus.
 *
 * DEFAULTS ON, and during bring-up it should stay on: a write is precisely
 * what kills this chip, a write cannot report failure by itself, and without
 * the check a caller keeps writing into a device that stopped existing several
 * hundred registers ago. The eventual symptom names none of them, which is how
 * the prior work on this chassis lost days.
 *
 * The check is a sysfs read, on the order of tens of microseconds. That is
 * nothing against a bring-up sequence and everything against a bulk memory
 * fill -- the ECC initialisation alone is over a million words, where a check
 * per word turns a second into half a minute.
 *
 * So bulk writers turn it off, and OWE A CHECK AT THE END: write the burst,
 * call fm_check_offbus() once, and treat a positive as "some write in that
 * burst did it" rather than as a mystery. That trade is honest -- it gives up
 * knowing WHICH write, which for a uniform fill is not information anyone
 * wanted -- and it is not a trade any bring-up code should make.
 */
void fm_set_write_check(struct fm6000 *d, int on);

/*
 * Ask the bus, not the BAR, whether the chip is still there.
 *
 * Reads the device's PCI config space through sysfs. A live endpoint answers
 * its vendor ID; one that has gone fatal answers 0xffff to config reads too,
 * which is what separates "this register genuinely contains 0xffffffff" from
 * "there is no longer a chip". Returns 1 if off-bus, 0 if present, negative if
 * the question could not be asked.
 */
int fm_check_offbus(struct fm6000 *d);

/*
 * Is the chip answering? Transport-appropriate, and the only honest check on
 * the local bus.
 *
 * On FM_XPORT_PCIE this is fm_check_offbus(): ask config space.
 *
 * On FM_XPORT_LBUS there is no config space to ask -- the SCD is perfectly
 * healthy whatever the FM6000 is doing, and its BAR keeps answering. So this
 * reads PIN_STRAP, a hardware strap that reads 0x208 on this board whether the
 * chip is cold or forwarding, and reports the chip dead when it reads 0.
 *
 * Returns 1 if alive, 0 if not, negative if the question could not be asked.
 */
int fm_alive(struct fm6000 *d);

/* True once the chip has been seen off-bus. Cheap; does not touch hardware. */
static inline int fm_is_offbus(const struct fm6000 *d) { return d->offbus; }

/*
 * Forget that the chip was ever off-bus.
 *
 * The latch is sticky because on PCIe a dead endpoint stays dead. On the local
 * bus that is not true: a reset pulse brings the chip back, reliably and in
 * under a second, which makes an experiment that kills it cheap to recover
 * from. Call this after pulsing, and only then -- clearing the latch without
 * actually reviving the chip just restores the log flood it exists to prevent.
 */
void fm_clear_offbus(struct fm6000 *d);

/*
 * Declare the ECC bank memories initialised, unlocking access to them.
 *
 * Call this ONLY after they really have been, and never to get past the guard.
 * The guard is the cheapest protection this port has.
 */
void fm_bank_mark_initialised(struct fm6000 *d);

/*
 * Declare the documented cold boot run, unlocking the EPL block.
 *
 * Separate from fm_bank_mark_initialised() because the two facts are separate.
 * Measured on this board 2026-09-25: reading an EPL word after nothing but a
 * reset pulse takes the chip off the local bus, and reading the same word after
 * Table 4-1 steps 5-10 returns 0x00080000 with the chip still answering. The
 * ECC bank memories are NOT made safe by that -- they need the memory
 * initialisation in step 12, which is a different guard.
 *
 * fm_boot_cold() calls this itself on success. Nothing else should.
 */
void fm_boot_mark_done(struct fm6000 *d);

/*
 * Write `val` into every word of a bank memory, so its ECC bits become valid.
 *
 * This is Table 4-1 step 12's "software writes memory manually", and it is the
 * one function entitled to write into a bank the guard is still refusing --
 * refusing reads of an uninitialised bank is the whole point of the guard, and
 * the way out of that state is to write the bank, so the writer cannot be
 * subject to it. It is deliberately the only bypass, and it is here rather than
 * exposed as a flag so there is exactly one of them.
 *
 * ⚠ WRITES, NOT READS. Writing a full 32-bit word stores data and ECC together
 * and is safe on an uninitialised bank; READING one is what raises the
 * uncorrectable error that takes the chip off the bus. Nothing here reads.
 *
 * The per-write bus check is turned off for the burst and restored after --
 * 393,216 words with a sysfs read each would take half a minute for
 * information nobody wants per word -- and one check is done at the end. So a
 * failure says "some write in this bank did it", which for a uniform fill is
 * all there is to know.
 *
 * Returns FM_OK, or FM_EOFFBUS if the chip left the bus during the fill.
 */
int fm_mem_fill(struct fm6000 *d, uint32_t base, uint32_t words, uint32_t val);

/* Whether a word address falls in a bank memory. Exposed so a tool can say
 * why it will not read something. */
int fm_is_bank(uint32_t word);

/*
 * Why this address is refused before the chip is initialised, or NULL if it is
 * not. Covers the bank memories and the registers that are hazardous to READ
 * -- which is not an empty set on this chip, and is the trap that catches
 * someone debugging, because reading more registers is the obvious thing to do
 * when something is wrong.
 */
const char *fm_hazard(const struct fm6000 *d, uint32_t word);

/* Human-readable name for a word address's block, for diagnostics. Returns a
 * static string; never NULL. */
const char *fm_block_name(uint32_t word);

#endif /* NOSAIC_FM6000_PCI_H */
