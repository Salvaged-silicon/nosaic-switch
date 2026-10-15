/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_FM6000_L2F_H
#define NOSAIC_FM6000_L2F_H

#include "pci.h"

/*
 * Program the L2F (L2 forwarding), LBS (loopback suppression) and related
 * tables that control per-port flood masks.
 *
 * Writes 637 LBUS registers. Table content is the end state from a running
 * 7150S-52 with OSPF active (EdgeNOS feature/arista-7150-fm6000,
 * fm6000_l2finit.c). Cold-boot validated 2026-08-07.
 *
 * Returns FM_OK on success, FM_EOFFBUS if the chip left the bus.
 */
int fm_l2f_init(struct fm6000 *d);

#endif /* NOSAIC_FM6000_L2F_H */
