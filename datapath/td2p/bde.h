/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_TD2P_BDE_H
#define NOSAIC_TD2P_BDE_H

#include <stddef.h>
#include <stdint.h>

struct nosaic_bde {
	char      bdf[32];
	int       bar_fd, cfg_fd, mem_fd;
	int       uio_fd;      /* -1 when the device is not bound to uio_pci_generic */
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

/* Interrupt delivery through uio_pci_generic.
 *
 * Without these the SDK has no way to be told the chip wants attention and
 * falls back to a polling thread, which holds a core permanently and still
 * delivers about twenty packets a second to the CPU.
 *
 * nosaic_bde_irq_open returns 0 if the device is bound to uio_pci_generic and
 * -1 otherwise, which is not an error: a board may legitimately run polled.
 */
int  nosaic_bde_irq_open(struct nosaic_bde *b);
int  nosaic_bde_irq_wait(struct nosaic_bde *b);   /* blocks until the chip interrupts */
void nosaic_bde_irq_arm(struct nosaic_bde *b);    /* unmask INTx again afterwards */

uint32_t nosaic_bde_cfg_read(struct nosaic_bde *b, uint32_t addr);
void     nosaic_bde_cfg_write(struct nosaic_bde *b, uint32_t addr, uint32_t data);

/* The SAL DMA hooks the SDK calls. Declared here so the vector layer can pass
 * them through rather than duplicating the allocator. */
void *sal_dma_alloc(unsigned int size, char *name);
void  sal_dma_free(void *ptr);

#endif
