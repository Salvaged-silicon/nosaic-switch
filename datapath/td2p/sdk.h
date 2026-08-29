/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_TD2P_SDK_H
#define NOSAIC_TD2P_SDK_H

#include <stdint.h>
struct nosaic_bde;

/* Create the SDK's device and install the access vectors over the BDE.
 * Returns the SDK unit number, or -1. */
int nosaic_sdk_attach(struct nosaic_bde *b, uint16_t dev_id, uint16_t rev_id);

#endif
