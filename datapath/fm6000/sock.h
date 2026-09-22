/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_FM6000_SOCK_H
#define NOSAIC_FM6000_SOCK_H

#include "pci.h"

/* Matches internal/nosd/proto.SocketPath. */
#define NOSAIC_QUERY_SOCKET "/run/nosd.sock"

/* Serve the switch-api socket on a thread of its own. Returns 0 if listening.
 * A failure is reported and is not fatal: a switch that runs without a
 * diagnostic socket is better than one that refuses to start without it. */
int fm_sock_start(struct fm6000 *d, const char *path);

#endif
