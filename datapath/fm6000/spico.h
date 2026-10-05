/* SPDX-License-Identifier: Apache-2.0 */
/*
 * SPICO firmware loader.
 *
 * fm_spico_load() finds, reads and downloads the SPICO firmware into the
 * FM6000, then starts the processor.  The firmware is Arista's proprietary
 * code from libFocalpointSDK.so and cannot be shipped with NOSaic.  It is
 * read at runtime from the EOS SWI on the switch's flash.
 *
 * Firmware location:
 *   /mnt/flash/EOS-*.swi  (ZIP archive)
 *     → usr/lib/libFocalpointSDK.so
 *       file offset 0x37e500, 12000 bytes (6000 × uint16_t little-endian)
 * [RE from libFocalpointSDK.so disassembly 2026-10-02]
 *
 * Cache: once extracted the raw 12000-byte blob is written to
 * /run/spico_fw.bin so subsequent boots skip the SWI extraction.
 */
#ifndef NOSAIC_FM6000_SPICO_H
#define NOSAIC_FM6000_SPICO_H

#include "pci.h"

/*
 * Load SPICO firmware and start the processor.
 *
 * Calls fm_spico_download() then fm_spico_enable().  Returns FM_OK on
 * success, FM_ERR if the firmware could not be found or the download
 * failed, or any fm_sbus_write / fm_rd / fm_wr error code.
 *
 * Pre-condition: fm_sbus_start() was called.
 */
int fm_spico_load(struct fm6000 *d);

#endif /* NOSAIC_FM6000_SPICO_H */
