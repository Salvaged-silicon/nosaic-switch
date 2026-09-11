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

/*
 * Let the socket answer questions about the DMA pool.
 *
 * Optional, and separate from starting the server, because the pool belongs to
 * the BDE and the BDE is per datapath. A daemon that does not call this still
 * serves everything else; `asic.dma` then reports that it has no pool to look
 * at rather than inventing numbers.
 *
 * This exists because the pool ran out once and naming the caller that had
 * consumed it meant reading the vendor's source and inferring. Per-name
 * totals turn the next occurrence into a question the switch can answer
 * about itself.
 */
struct nosaic_dmapool;
void nosaic_query_set_dmapool(struct nosaic_dmapool *p);

/* What this provider calls itself in `show caps`. Set per datapath so an
 * operator can tell which silicon answered without knowing the board. */
#ifndef NOSAIC_QUERY_DRIVER
#define NOSAIC_QUERY_DRIVER "bcm"
#endif

#endif
