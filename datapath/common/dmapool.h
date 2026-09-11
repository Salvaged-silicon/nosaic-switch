/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_DMAPOOL_H
#define NOSAIC_DMAPOOL_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

/*
 * The DMA pool the SDK allocates from, shared by every Broadcom datapath.
 *
 * Both boards reserve a region of physical memory, map it once, and hand
 * pieces of it to the chip. Neither the Trident+ nor the Trident2+ BDE has
 * any business having its own opinion about how that region is divided, and
 * until this file existed both had the same one: bump a pointer, and make
 * free a no-op.
 *
 * That was deliberate and it was wrong. The reasoning recorded in both
 * datapaths was that the SDK takes what it needs during initialisation and
 * keeps it for the life of the process, so a real allocator would be
 * complexity for a case that does not arise. The case arises. `bcm_tx`
 * allocates a DMA vector per transmitted packet and frees it again, and with
 * a free that reclaims nothing every packet the control plane sends costs
 * the pool a buffer for ever. On the AS5610 that consumed 64 MiB and then
 * failed every subsequent allocation, roughly two hundred times a second,
 * for as long as the box stayed up.
 *
 * So this is a real allocator: first fit over a list of blocks in address
 * order, splitting on allocation and coalescing on free. It is not clever.
 * It does not need to be -- the SDK's own free list means the steady-state
 * churn is a handful of blocks -- it only needs to give memory back.
 *
 * WHAT THE CHIP SEES. Every payload is 64-byte aligned, because these
 * addresses are handed to the chip and an unaligned descriptor shows up as
 * corrupt traffic rather than as an error. Each block's bookkeeping lives in
 * the 64 bytes immediately before its payload, inside the pool but never
 * given out, so the physical address of a payload is still its offset from
 * the base of the region and l2p/p2l stay subtraction.
 *
 * WHY ALLOCATIONS CARRY A NAME. The SDK names every allocation it makes.
 * Keeping a per-name total of what is outstanding is what turns the next
 * occurrence of this bug from an inference into a measurement: the daemon
 * can be asked which caller is holding the pool rather than having it
 * deduced from the vendor's source.
 */

/* Bookkeeping bytes before each payload. Also the alignment every payload
 * gets, which is what the chip requires. */
#define NOSAIC_DMA_HDR    64u

/* Distinct allocation names tracked. Slot 0 is a catch-all for the rest, so
 * the totals stay correct however many names the SDK invents. */
#define NOSAIC_DMA_NAMES  64
#define NOSAIC_DMA_NAMELEN 32

struct nosaic_dma_stat {
	char     name[NOSAIC_DMA_NAMELEN];
	size_t   outstanding;   /* payload bytes allocated and not yet freed */
	size_t   peak;          /* the high-water mark of the above */
	uint64_t allocs, frees, fails;
};

struct nosaic_dmapool {
	void            *base;
	size_t           len;
	const char      *tag;      /* "nosd-tdp" or "nosd-td2p", for messages */
	pthread_mutex_t  lock;
	struct nosaic_dma_blk *head;
	size_t           used;     /* payload plus headers, currently allocated */
	size_t           peak;
	uint64_t         fails;
	int              nstats;
	struct nosaic_dma_stat stat[NOSAIC_DMA_NAMES];
};

/*
 * Take ownership of a mapped region. `base` must be at least 64-byte aligned,
 * which an mmap of a reserved region always is.
 *
 * Returns 0, or -1 if the region is too small to hold a single block.
 */
int nosaic_dmapool_init(struct nosaic_dmapool *p, void *base, size_t len,
			const char *tag);

/* Allocate `size` bytes for `name`, zeroed. NULL when the pool cannot satisfy
 * it, having said so on stderr with what was asked for and by whom. */
void *nosaic_dmapool_alloc(struct nosaic_dmapool *p, size_t size, const char *name);

/* Give a payload back. NULL is accepted and ignored, which is what the SDK
 * expects of free. A pointer that did not come from this pool, or one that is
 * already free, is reported rather than acted on: both are bugs above us, and
 * corrupting the block list in response would turn a caller's mistake into
 * silently wrong traffic. */
void nosaic_dmapool_free(struct nosaic_dmapool *p, void *ptr);

/* Bytes currently allocated, including per-block bookkeeping. */
size_t nosaic_dmapool_used(struct nosaic_dmapool *p);

/* The largest single allocation that would succeed right now. Smaller than
 * the free total when the pool is fragmented, which is the one failure mode
 * a bump allocator could not have and this one can. */
size_t nosaic_dmapool_largest(struct nosaic_dmapool *p);

/* Copy the per-name table out, most outstanding first. Returns how many were
 * written, at most `max`. */
int nosaic_dmapool_stats(struct nosaic_dmapool *p, struct nosaic_dma_stat *out, int max);

#endif
