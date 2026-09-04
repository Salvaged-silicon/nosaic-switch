/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_CLI_UPGRADE_H
#define NOSAIC_CLI_UPGRADE_H

/* `nosaic upgrade` for architectures the Go toolchain cannot target.
 *
 * Same subcommands, same refusals and the same boot-pointer files as
 * internal/upgrade, because a switch's slots do not change meaning with its
 * CPU. Returns a process exit status. */
int nosaic_upgrade(int argc, char **argv);

#endif
