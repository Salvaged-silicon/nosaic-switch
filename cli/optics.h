/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_CLI_OPTICS_H
#define NOSAIC_CLI_OPTICS_H

/* Transceiver diagnostics on boards the Go CLI cannot target.
 *
 * Same command, same output shape and the same two traps as
 * internal/platformhal/sff, because a module's own memory does not change
 * meaning with the CPU reading it.
 *
 * `cage` is the front-panel port number: 1-48 SFP+, 49-52 QSFP+.
 * Returns a process exit status. */
int nosaic_optics_list(void);
int nosaic_optics_show(int cage);
int nosaic_optics_dump(int cage);

#endif
