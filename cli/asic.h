/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_CLI_ASIC_H
#define NOSAIC_CLI_ASIC_H

#include "query.h"

/* Print Linux's view and the chip's side by side, and say where they differ.
 * Returns non-zero if any port disagrees in a way that stops traffic. */
int nosaic_asic_ports(void);

/* Print the kernel's routing table against the chip's forwarding table, and
 * say which entries only one of them has. Returns non-zero if the kernel has a
 * route the chip does not. */
int nosaic_asic_routes(void);

#endif
