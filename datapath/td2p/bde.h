/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_TD2P_BDE_H
#define NOSAIC_TD2P_BDE_H

#include <stddef.h>
#include <stdint.h>

struct nosaic_bde {
	char      bdf[32];
	int       bar_fd, cfg_fd, mem_fd;
	volatile void *bar;
	size_t    bar_len;
	void     *dma;
	uint64_t  dma_phys;
	size_t    dma_len, dma_used;
};

int  nosaic_bde_open(struct nosaic_bde *b, const char *bdf);
void nosaic_bde_close(struct nosaic_bde *b);
size_t nosaic_bde_bar_size(const char *bdf, int bar);

/* The SDK's SAL DMA hooks carry no context, so the device they draw from is
 * set once, here, before the SDK is initialised. */
void nosaic_bde_set_sal_device(struct nosaic_bde *b);

#endif
