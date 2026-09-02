/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_TDP_SDK_H
#define NOSAIC_TDP_SDK_H

#include <stdint.h>
#include "bde.h"

/*
 * Bring the chip up as far as soc_attach and return its unit number, or -1.
 *
 * Sets big-endian programmed I/O first: this SDK is built with SYS_BE_PIO=1,
 * which is a claim about the chip that something has to make true before the
 * first register is read.
 */
int nosaic_tdp_sdk_attach(struct nosaic_tdp_bde *b, uint16_t dev_id, uint8_t rev_id);

/* Reset the chip and bring the SOC layer up. Separate from the BCM layer
 * because a failure in one says something different from a failure in the
 * other, and because the reset has to happen exactly once. */
int nosaic_tdp_sdk_soc_init(int unit);

/* The rest: misc, MMU, and the BCM layer above them. */
int nosaic_tdp_sdk_bcm_init(int unit);

/* Enable every port and put it in spanning-tree forwarding. bcm_init leaves
 * both off, so nothing forwards until this runs. */
int nosaic_tdp_sdk_ports_up(int unit);

#endif
