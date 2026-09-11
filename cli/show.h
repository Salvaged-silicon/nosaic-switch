/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_CLI_SHOW_H
#define NOSAIC_CLI_SHOW_H

/* Matches NOSAIC_DMA_NAMELEN in datapath/common/dmapool.h. The CLI cannot
 * include that header -- it builds without the datapath tree -- so the two are
 * kept in step by name rather than by inclusion. */
#define NOSAIC_DMA_NAME_MAX 32

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

/* What the datapath's DMA pool holds, and which allocation names hold it.
 *
 * Reported rather than compared, so it is a `show`. A datapath with no pool --
 * the virtual platform has none -- answers with a refusal rather than zeroes,
 * which is the capability model rather than a gap in it. */
int nosaic_show_dma(void);

#endif
