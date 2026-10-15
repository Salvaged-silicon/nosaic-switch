/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_FM6000_GLORT_H
#define NOSAIC_FM6000_GLORT_H

#include "pci.h"

/*
 * Program the GLORT CAM+RAM tables.
 *
 * Writes 151 LBUS registers in range 0xe000-0xf000. Table content is the
 * end state recovered from a running 7150S with OSPF active (EdgeNOS
 * feature/arista-7150-fm6000, fm6000_glortinit.c), cold-boot validated
 * 2026-08-07.
 *
 * CAM[0] is the catch-all (mask=0x007fffff = all don't-care), mapped to
 * L2F entry 1 = {HOST port, et1, et2}. All traffic punts to CPU via the
 * DMA engine until MAC learning fills the forwarding tables.
 *
 * Returns FM_OK on success, FM_EOFFBUS if the chip left the bus.
 */
int fm_glort_init(struct fm6000 *d);

#endif /* NOSAIC_FM6000_GLORT_H */
