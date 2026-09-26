/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_FM6000_SAF_H
#define NOSAIC_FM6000_SAF_H

#include "pci.h"

/*
 * Write the store-and-forward matrix's end state.
 *
 * 168 writes, replacing the 34,668 the vendor's boot spends accumulating the
 * same thing one port at a time. `written` takes the count if non-NULL.
 */
int fm_saf_init(struct fm6000 *d, unsigned *written);

#endif /* NOSAIC_FM6000_SAF_H */
