/* SPDX-License-Identifier: Apache-2.0 */
/*
 * FM6000 packet-DMA engine (CPU punt/inject), userspace implementation.
 *
 * Adapted from EdgeNOS fpdma.c (feature/arista-7150-fm6000). The major
 * difference from the EdgeNOS kernel module is the backing store: instead of
 * dma_alloc_coherent, we use posix_memalign + mlock + /proc/self/pagemap to
 * get physically-pinned buffers.  This works on the 7150S because the AMD
 * RS780 has no IOMMU -- physical addresses are usable directly.
 *
 * ⚠ This is deliberately NOT an IOMMU-safe implementation. It would need to
 * be replaced on any platform with an IOMMU.
 */
#ifndef NOSAIC_FM6000_DMA_H
#define NOSAIC_FM6000_DMA_H

#include <stdint.h>
#include <stddef.h>

#include "pci.h"

#define FM_DMA_TX_RING	16	/* TX ring size (power of two) */
#define FM_DMA_RX_RING	16	/* RX ring size (power of two) */

struct fm_dma_ring {
	uint8_t  *desc;		/* descriptor ring (virtual, page-aligned) */
	uintptr_t desc_phys;	/* descriptor ring (physical) */
	uint32_t  size;		/* number of descriptors */
	uint32_t  mask;		/* size - 1 */
	uint32_t  head;		/* producer index */
	uint32_t  tail;		/* consumer index */
	uint8_t **buf_va;	/* per-slot buffer virtual addresses */
	uintptr_t *buf_phys;	/* per-slot buffer physical addresses */
	size_t    buf_len;	/* per-slot buffer size (RX only; 0 = TX) */
};

struct fm_dma {
	struct fm6000    *d;
	struct fm_dma_ring tx;
	struct fm_dma_ring rx;
};

/*
 * Initialise the DMA engine and both rings.
 *
 * Must be called AFTER fm_attach_pcie() has succeeded (d->pcie_regs != NULL).
 * Allocates per-slot buffers; the caller owns fm_dma and must call
 * fm_dma_shutdown() when done.
 *
 * Returns 0 on success, -1 on failure.
 */
int fm_dma_init(struct fm_dma *dma, struct fm6000 *d);

void fm_dma_shutdown(struct fm_dma *dma);

/*
 * Inject a frame from the CPU to the fabric.
 *
 * `frame`  = raw Ethernet frame WITHOUT the F64 tag: DMAC(6) SMAC(6) EType...
 *             The tag is inserted inline between SMAC and EType.
 * `len`    = frame length (without the 8-byte F64 tag that will be inserted)
 * `sglort` = source GLORT (FM6000_GLORT_CPU for CPU-generated frames)
 * `dglort` = destination GLORT (target port's GLORT; e.g. 1 for et1)
 *
 * Returns 0 on success, -1 if TX ring full or frame too long.
 */
int fm_dma_tx(struct fm_dma *dma, const uint8_t *frame, uint16_t len,
	      uint16_t sglort, uint16_t dglort);

/*
 * Reclaim completed TX descriptors.  Returns number reclaimed.
 */
int fm_dma_tx_reclaim(struct fm_dma *dma);

/*
 * Poll the RX ring for at most `budget` frames.
 *
 * For each received frame, `cb` is called with the raw frame (including the
 * inline F64 tag at offset 12) and its length.  The SGLORT at tag bytes 4-5
 * identifies the ingress port.
 *
 * Returns number of frames delivered.
 */
int fm_dma_rx_poll(struct fm_dma *dma, int budget,
		   void (*cb)(void *ctx, const uint8_t *frame, uint16_t len),
		   void *ctx);

/*
 * Extract the SGLORT from an F64 tag already inline in a received frame.
 * `frame` points to byte 0 (DMAC).  Returns 0 if frame is too short.
 */
static inline uint16_t fm_f64_sglort(const uint8_t *frame, uint16_t len)
{
	if (len < 14)
		return 0;
	/* tag bytes 4-5 (big-endian) = SGLORT */
	return ((uint16_t)frame[16] << 8) | frame[17];
}

#endif /* NOSAIC_FM6000_DMA_H */
