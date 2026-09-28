/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_FM6000_LBS_H
#define NOSAIC_FM6000_LBS_H

struct fm6000;

/*
 * Loopback suppression: stop a frame being sent back out of the port it
 * arrived on.
 */
int fm_lbs_init(struct fm6000 *d, unsigned *written);

#endif /* NOSAIC_FM6000_LBS_H */
