/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_FM6000_CMREST_H
#define NOSAIC_FM6000_CMREST_H

struct fm6000;

/*
 * The congestion-management state that is not the six per-port watermark
 * tables: the PAUSE configuration, the class and partition maps, and the
 * shared-partition thresholds.
 */
int fm_cmrest_init(struct fm6000 *d, unsigned *written);

#endif /* NOSAIC_FM6000_CMREST_H */
