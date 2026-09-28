/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_FM6000_CMWM_H
#define NOSAIC_FM6000_CMWM_H

struct fm6000;

/*
 * What actually took, and what cannot be asked.
 *
 * ⚠ A COUNT OF WORDS WRITTEN IS NOT EVIDENCE THAT ANYTHING LANDED, so the
 * tables that can be read back are read back.
 *
 * ⚠ AND TWO OF THEM CANNOT BE. The transmit-side tables read zero however
 * they are written -- on a chip that is forwarding traffic as much as on
 * ours, which is what settles it. They are write-only, and an earlier
 * version of this file read their zero as "the writes were discarded" and
 * reported a fault on a correctly configured switch. Not being able to check
 * something is a different answer from having checked it and found it wrong,
 * and reporting the second when you mean the first sends people hunting.
 */
struct fm_cmwm_report {
	unsigned written;	/* words the chip accepted without complaint */
	unsigned tables;	/* tables attempted */
	unsigned readable;	/* of those, how many can be read back at all */
	unsigned verified;	/* of the readable ones, how many matched */
	const char *first_bad;	/* NULL if every readable table matched */
};

/*
 * Congestion-management watermarks: when the chip drops, and when it asks
 * the far end to stop sending.
 */
int fm_cmwm_init(struct fm6000 *d, struct fm_cmwm_report *rep);

#endif /* NOSAIC_FM6000_CMWM_H */
