/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_FM6000_TAP_H
#define NOSAIC_FM6000_TAP_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/*
 * Create a TAP interface (Layer 2) with the given name.
 *
 * Returns an open file descriptor for the TAP device on success, or -1 on
 * failure (errno set).  The interface is created with IFF_NO_PI so the fd
 * carries raw Ethernet frames with no prepended packet-info header.
 *
 * The returned fd is suitable for select()/poll() and non-blocking read/write.
 */
int fm_tap_create(const char *name);

/*
 * Write a raw Ethernet frame to a TAP interface (kernel → fabric direction).
 * `len` must be <= 4096.  Returns 0 on success, -1 on error.
 */
int fm_tap_write(int fd, const uint8_t *frame, size_t len);

/*
 * Read a raw Ethernet frame from a TAP interface (fabric → kernel direction).
 * Returns the frame length on success, 0 if no frame is ready, -1 on error.
 */
ssize_t fm_tap_read(int fd, uint8_t *buf, size_t bufsz);

#endif /* NOSAIC_FM6000_TAP_H */
