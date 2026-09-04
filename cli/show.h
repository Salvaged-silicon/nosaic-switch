/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_CLI_SHOW_H
#define NOSAIC_CLI_SHOW_H

/* The northbound contract, asked of whatever datapath is running.
 *
 * These are the same commands the Go CLI serves on boards the Go toolchain can
 * target, answering from the same ops on the same socket and printing the same
 * columns. A switch is not supposed to be a different machine to operate
 * because of what its CPU is.
 */
int nosaic_show_caps(void);
int nosaic_show_ports(void);
int nosaic_show_routes(void);

#endif
