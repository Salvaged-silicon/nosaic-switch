/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_FM6000_ESCHED_H
#define NOSAIC_FM6000_ESCHED_H

#include "pci.h"

/*
 * Configure the egress scheduler.
 *
 * Writes CFG_1 and CFG_2 for front-panel ports and the CPU port.
 * DRR registers are not written here; they are only accessible after
 * the ring circulates.  See esched.c for the full write order.
 *
 * No-op when d->pcie_regs is NULL (LBUS path cannot write this block).
 */
int fm_esched_init(struct fm6000 *d, unsigned *written, uint32_t *culprit);

/*
 * Post-ring ESCHED completion.  Call after the SSCHED token ring is
 * circulating (probe FOUND) and PCIe Memory Space is restored.
 * Writes DRR_Q, CFG_2 (front-panel + CPU), CFG_3, and DRR_CFG.
 * No-op when d->pcie_regs is NULL.
 */
int fm_esched_set_cfg2(struct fm6000 *d);

#endif /* NOSAIC_FM6000_ESCHED_H */
