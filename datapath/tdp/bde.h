/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_TDP_BDE_H
#define NOSAIC_TDP_BDE_H

#include <stddef.h>
#include <stdint.h>

#include "dmapool.h"

/*
 * The BDE for Trident+ (BCM56840 family, BCM56846 on the AS5610).
 *
 * Same shape as the Trident2+ one and still not the same file: what the two
 * genuinely share is PCI plumbing, MMIO ordering and the DMA pool, and the
 * last two are already in datapath/common/. The pool moved there the way this
 * comment said such things should be decided -- with two working boards
 * rather than one, after both were found carrying the same bug in their own
 * copy of it. PCI plumbing has not earned the same treatment: the two boards
 * find their device and their memory in genuinely different places, a device
 * tree on one and the kernel command line on the other.
 */
struct nosaic_tdp_bde {
	char           bdf[32];
	uint16_t       device_id;      /* as PCI reports it */
	int            bar_fd, cfg_fd, mem_fd;
	volatile void *bar;
	size_t         bar_len;

	/* The DMA pool, from the device tree's reserved-memory. dma/dma_phys/
	 * dma_len describe the region; pool is what divides it up. */
	void          *dma;
	uint64_t       dma_phys;
	size_t         dma_len;
	struct nosaic_dmapool pool;
};

/*
 * Map the DMA pool the device tree reserved for us.
 *
 * Found rather than configured: the board's device tree carries a
 * reserved-memory node, and reading its address out of /proc/device-tree
 * means the daemon and the device tree cannot disagree about where the pool
 * is. The 7050SX2 takes a base address from the kernel command line and an
 * environment variable, which is two places to get it wrong.
 *
 * Returns 0 on success, -1 if there is no such node or it cannot be mapped.
 * Not fatal on its own -- the probe wants to report it rather than die.
 */
int nosaic_tdp_bde_map_dma(struct nosaic_tdp_bde *b);

/* bdf may be NULL, in which case the first Broadcom switch on the bus is
 * taken. Returns 0 on success. */
int  nosaic_tdp_bde_open(struct nosaic_tdp_bde *b, const char *bdf);
void nosaic_tdp_bde_close(struct nosaic_tdp_bde *b);

/*
 * Put programmed I/O into the host's byte order.
 *
 * A no-op on a little-endian host, where the chip's default already matches.
 * On a big-endian one it writes CMIC_ENDIAN_SELECT, which is safe to do before
 * the current byte order is known because the value carries the same bit in
 * bytes 0 and 3.
 *
 * Must happen before any register value is believed.
 */
void nosaic_tdp_bde_set_endian(struct nosaic_tdp_bde *b);

/*
 * Read the chip's own identity back through the mapping and compare it with
 * what PCI said.
 *
 * The point is to fail at startup rather than later: if this does not agree,
 * every register the SDK subsequently reads is wrong in the same way, and the
 * symptom will be chip initialisation failing somewhere far from the cause.
 *
 * Returns 0 if they agree. Fills *raw with the register as read, for a caller
 * that wants to report it.
 */
int nosaic_tdp_bde_selftest(struct nosaic_tdp_bde *b, uint32_t *raw);

/* PCI configuration space, read and written as a file. */
uint32_t nosaic_tdp_bde_cfg_read(struct nosaic_tdp_bde *b, uint32_t addr);
void     nosaic_tdp_bde_cfg_write(struct nosaic_tdp_bde *b, uint32_t addr, uint32_t data);

uint32_t nosaic_tdp_bde_rd(struct nosaic_tdp_bde *b, uint32_t off);
void     nosaic_tdp_bde_wr(struct nosaic_tdp_bde *b, uint32_t off, uint32_t v);

#endif
