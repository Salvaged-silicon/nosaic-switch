/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_TDP_BDE_H
#define NOSAIC_TDP_BDE_H

#include <stddef.h>
#include <stdint.h>

/*
 * The BDE for Trident+ (BCM56840 family, BCM56846 on the AS5610).
 *
 * Same shape as the Trident2+ one and deliberately not the same file yet: what
 * the two genuinely share is PCI plumbing and MMIO ordering, and the second is
 * already in datapath/common/mmio.h. Whether the first should follow is a
 * question worth answering with two working boards rather than one, which is
 * the test the project plan says this board exists to run.
 */
struct nosaic_tdp_bde {
	char           bdf[32];
	uint16_t       device_id;      /* as PCI reports it */
	int            bar_fd, cfg_fd;
	volatile void *bar;
	size_t         bar_len;
};

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

uint32_t nosaic_tdp_bde_rd(struct nosaic_tdp_bde *b, uint32_t off);
void     nosaic_tdp_bde_wr(struct nosaic_tdp_bde *b, uint32_t off, uint32_t v);

#endif
