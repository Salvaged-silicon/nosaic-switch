/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_CLI_ASIC_H
#define NOSAIC_CLI_ASIC_H

/* Where the datapath serves its state. Matches datapath/common/query.h and
 * internal/nosd/proto.SocketPath. */
#define NOSAIC_QUERY_SOCKET "/run/nosd.sock"

/* Print Linux's view and the chip's side by side, and say where they differ.
 * Returns non-zero if any port disagrees in a way that stops traffic. */
int nosaic_asic_ports(void);

#endif
