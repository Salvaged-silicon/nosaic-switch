/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NOSAIC_SCDMDIO_H
#define NOSAIC_SCDMDIO_H

#include <stdint.h>

/*
 * THIS FILE AND ITS .c ARE GPL-2.0. THE REST OF THE DATAPATH IS APACHE-2.0.
 *
 * The transfer protocol here is ported from Arista's GPL-2.0 scd-mdio driver.
 * Transcribing a driver's register offsets and transaction sequence produces a
 * derivative work, so the licence follows it -- the same arrangement, and the
 * same reasoning, as internal/platformhal/scdsmbus on the Go side.
 *
 * It lives in its own directory so the licence boundary is a directory rather
 * than a comment somebody has to notice, and it knows nothing about NOSaic:
 * the mapping is injected, so this code can be lifted out whole.
 */

/* One MDIO accelerator, at a base offset within a mapped SCD BAR. */
struct nosaic_mdio {
	volatile uint32_t *bar;  /* the mapped BAR, 32-bit words */
	unsigned long      base; /* byte offset of this accelerator */
	unsigned           speed;/* MHz code for the control/status word */
	unsigned           req;  /* rolling request id */
};

/* Take the accelerator out of reset. Call once, before any transfer: without
 * it every read returns 0xffff and no error. */
void nosaic_mdio_init(struct nosaic_mdio *m);

/* Clause-45 read and write. Returns 0 on success, negative on failure;
 * a read stores the 16-bit value in *out. */
int nosaic_mdio_read(struct nosaic_mdio *m, int bus, int prtad, int devad,
		     int reg, uint16_t *out);
int nosaic_mdio_write(struct nosaic_mdio *m, int bus, int prtad, int devad,
		      int reg, uint16_t val);

#endif
