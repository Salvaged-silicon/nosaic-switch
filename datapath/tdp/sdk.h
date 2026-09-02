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

#endif
