/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_FM6000_ESCHED_H
#define NOSAIC_FM6000_ESCHED_H

#include "pci.h"

/*
 * Configure the egress scheduler: strict priority on every traffic class for
 * the front-panel ports, and the CPU port's own pair. 212 writes.
 */
int fm_esched_init(struct fm6000 *d, unsigned *written, uint32_t *culprit);

#endif /* NOSAIC_FM6000_ESCHED_H */
