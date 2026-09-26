/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_FM6000_CMWM_H
#define NOSAIC_FM6000_CMWM_H

struct fm6000;

/*
 * What actually took.
 *
 * ⚠ ON THIS CHIP SOME OF THESE TABLES ACCEPT A WRITE AND KEEP NOTHING. The
 * transmit-side tables discard writes outright while the receive-side tables
 * next to them store them, with no error either way -- see cmwm.c. A count of
 * words written is therefore not evidence of anything, so the caller is given
 * a verified count instead and the name of the first table that did not take.
 */
struct fm_cmwm_report {
	unsigned written;	/* words the chip accepted without complaint */
	unsigned tables;	/* tables attempted */
	unsigned verified;	/* tables that read back what we wrote */
	const char *first_bad;	/* NULL if every table verified */
};

/*
 * Congestion-management watermarks: when the chip drops, and when it asks
 * the far end to stop sending.
 */
int fm_cmwm_init(struct fm6000 *d, struct fm_cmwm_report *rep);

#endif /* NOSAIC_FM6000_CMWM_H */
