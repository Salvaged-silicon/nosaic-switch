/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_FM6000_SSCHED_H
#define NOSAIC_FM6000_SSCHED_H

#include <stdint.h>

struct fm6000;

/*
 * Bring up the scheduler ring.
 *
 * On success the scheduler engine is circulating, which is the precondition
 * for the egress scheduler block being reachable at all -- see ssched.c.
 *
 * `circulating` is set to 1 if the running engine was observed to walk the
 * ring and find a token, 0 if it did not. A ring that initialises without
 * circulating is a failure that reports success everywhere else, so the
 * caller is given it separately rather than folded into the return value.
 */
/*
 * Set the Sync bit on the management port's token.
 *
 * ⚠ OUR TWO PRIOR GENERATIONS DISAGREE ABOUT THIS BIT. The earlier one sets
 * it and says the golden ring captured from running EOS has it set; the later
 * one clears it and says the SDK's own ring builder never sets Sync on any
 * token. They cannot both be right, and this has cost a day before, so the
 * bit is a parameter and the answer is whichever one circulates.
 */
#define FM_SSCHED_SYNC_MGMT	(1u << 0)

int fm_ssched_ring_init(struct fm6000 *d, unsigned flags, int *circulating);

#endif /* NOSAIC_FM6000_SSCHED_H */
