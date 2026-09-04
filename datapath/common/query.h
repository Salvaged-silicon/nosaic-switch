/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_QUERY_H
#define NOSAIC_QUERY_H

/* Where the CLI looks. Under /run because it is runtime state: it belongs on a
 * tmpfs that is empty again after a reboot, not in the image and not on the
 * data partition. Matches internal/nosd/proto.SocketPath. */
#define NOSAIC_QUERY_SOCKET "/run/nosd.sock"

/*
 * Serve the chip's own state on a Unix socket, read-only.
 *
 * Runs on a thread of its own so a caller cannot stall the packet path, and
 * answers one request per connection. Returns 0 if it is listening; a failure
 * is reported and is not fatal, because a switch that forwards without a
 * diagnostic socket is better than one that refuses to start without it.
 */
int nosaic_query_start(int unit, const char *path);

#endif
