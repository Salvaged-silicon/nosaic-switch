/* SPDX-License-Identifier: Apache-2.0 */
/*
 * First-fit allocator over the reserved DMA region. See dmapool.h for why
 * this exists rather than the bump allocator both datapaths used to carry.
 *
 * The block list tiles the pool: every byte of the region belongs to exactly
 * one block, each block is a 64-byte header followed by its payload, and the
 * list is kept in address order so that coalescing on free is a look at the
 * two neighbours rather than a search.
 */
#include <stdio.h>
#include <string.h>

#include "dmapool.h"

#define BLK_MAGIC 0x4e4f5344u   /* "NOSD" -- a free that lands elsewhere says so */
#define BLK_FREE  1u

struct nosaic_dma_blk {
	uint32_t magic;
	uint32_t flags;
	size_t   size;        /* payload bytes in this block */
	struct nosaic_dma_blk *next, *prev;
	int      name_idx;    /* which stat slot owns it while allocated */
};

/* The header must fit in the alignment gap it occupies, or payloads stop
 * being 64-byte aligned and the chip is handed addresses it cannot use.
 * Checked here rather than discovered as corrupt traffic. */
typedef char nosaic_dma_hdr_fits[(sizeof(struct nosaic_dma_blk) <= NOSAIC_DMA_HDR) ? 1 : -1];

static void *payload_of(struct nosaic_dma_blk *b)
{
	return (char *)b + NOSAIC_DMA_HDR;
}

static size_t round64(size_t n)
{
	return (n + 63u) & ~(size_t)63u;
}

static int stat_index(struct nosaic_dmapool *p, const char *name)
{
	int i;

	if (name == NULL || *name == '\0')
		name = "?";
	for (i = 0; i < p->nstats; i++)
		if (strncmp(p->stat[i].name, name, NOSAIC_DMA_NAMELEN - 1) == 0)
			return i;
	if (p->nstats < NOSAIC_DMA_NAMES) {
		i = p->nstats++;
		strncpy(p->stat[i].name, name, NOSAIC_DMA_NAMELEN - 1);
		p->stat[i].name[NOSAIC_DMA_NAMELEN - 1] = '\0';
		return i;
	}
	return 0;   /* the catch-all, so the totals stay right */
}

int nosaic_dmapool_init(struct nosaic_dmapool *p, void *base, size_t len,
			const char *tag)
{
	struct nosaic_dma_blk *b;

	memset(p, 0, sizeof(*p));
	p->tag = tag ? tag : "nosd";

	if (base == NULL || len < NOSAIC_DMA_HDR + 64u)
		return -1;

	pthread_mutex_init(&p->lock, NULL);
	p->base = base;
	p->len  = len;

	b = (struct nosaic_dma_blk *)base;
	b->magic    = BLK_MAGIC;
	b->flags    = BLK_FREE;
	b->size     = len - NOSAIC_DMA_HDR;
	b->next     = NULL;
	b->prev     = NULL;
	b->name_idx = 0;
	p->head     = b;

	p->nstats = 1;
	strncpy(p->stat[0].name, "(other)", NOSAIC_DMA_NAMELEN - 1);
	return 0;
}

/* Free bytes and the largest single block, walked under the caller's lock. */
static void free_survey(struct nosaic_dmapool *p, size_t *total, size_t *largest)
{
	struct nosaic_dma_blk *b;

	*total = *largest = 0;
	for (b = p->head; b != NULL; b = b->next) {
		if (!(b->flags & BLK_FREE))
			continue;
		*total += b->size;
		if (b->size > *largest)
			*largest = b->size;
	}
}

void *nosaic_dmapool_alloc(struct nosaic_dmapool *p, size_t size, const char *name)
{
	struct nosaic_dma_blk *b, *n;
	size_t want, freetot, largest;
	int idx;
	void *ret;

	if (p->base == NULL) {
		fprintf(stderr, "%s: salloc(%zu, %s) with no DMA pool mapped\n",
			p->tag, size, name ? name : "?");
		return NULL;
	}
	/* Zero-sized allocations are not a thing the SDK should ask for, and
	 * handing back a block with no payload would make free ambiguous. */
	want = round64(size == 0 ? 1 : size);

	pthread_mutex_lock(&p->lock);
	idx = stat_index(p, name);

	for (b = p->head; b != NULL; b = b->next)
		if ((b->flags & BLK_FREE) && b->size >= want)
			break;

	if (b == NULL) {
		p->fails++;
		p->stat[idx].fails++;
		free_survey(p, &freetot, &largest);
		pthread_mutex_unlock(&p->lock);
		/* Both numbers, because with a real allocator "the pool is full"
		 * and "the pool is fragmented" are different faults with
		 * different fixes, and the totals alone cannot tell them apart. */
		fprintf(stderr,
			"%s: DMA pool exhausted: %s wanted %zu, %zu of %zu bytes used, "
			"%zu free in blocks of at most %zu\n"
			"  ask the daemon which caller holds it before enlarging the pool\n",
			p->tag, name ? name : "?", size, p->used, p->len,
			freetot, largest);
		return NULL;
	}

	/* Split only when the remainder can carry its own header and still be
	 * worth handing out; otherwise the tail goes to this allocation and is
	 * returned with it. */
	if (b->size >= want + NOSAIC_DMA_HDR + 64u) {
		n = (struct nosaic_dma_blk *)((char *)b + NOSAIC_DMA_HDR + want);
		n->magic    = BLK_MAGIC;
		n->flags    = BLK_FREE;
		n->size     = b->size - want - NOSAIC_DMA_HDR;
		n->name_idx = 0;
		n->next     = b->next;
		n->prev     = b;
		if (n->next != NULL)
			n->next->prev = n;
		b->next = n;
		b->size = want;
	}

	b->flags   &= ~BLK_FREE;
	b->name_idx = idx;

	p->used += NOSAIC_DMA_HDR + b->size;
	if (p->used > p->peak)
		p->peak = p->used;
	p->stat[idx].allocs++;
	p->stat[idx].outstanding += b->size;
	if (p->stat[idx].outstanding > p->stat[idx].peak)
		p->stat[idx].peak = p->stat[idx].outstanding;

	ret = payload_of(b);
	size = b->size;
	pthread_mutex_unlock(&p->lock);

	/* Zeroed outside the lock: it is the caller's block now, and on a 64 MiB
	 * pool this is the only part of an allocation with a cost worth keeping
	 * off a mutex every other thread is waiting on. */
	memset(ret, 0, size);
	return ret;
}

void nosaic_dmapool_free(struct nosaic_dmapool *p, void *ptr)
{
	struct nosaic_dma_blk *b, *n;

	if (ptr == NULL)
		return;

	if (p->base == NULL ||
	    (char *)ptr < (char *)p->base + NOSAIC_DMA_HDR ||
	    (char *)ptr >= (char *)p->base + p->len) {
		fprintf(stderr, "%s: DMA free of %p, which is not in the pool "
			"(%p..%p)\n", p->tag, ptr, p->base,
			(char *)p->base + p->len);
		return;
	}

	b = (struct nosaic_dma_blk *)((char *)ptr - NOSAIC_DMA_HDR);

	pthread_mutex_lock(&p->lock);

	if (b->magic != BLK_MAGIC) {
		pthread_mutex_unlock(&p->lock);
		fprintf(stderr, "%s: DMA free of %p, which is inside the pool but "
			"is not the start of a block\n", p->tag, ptr);
		return;
	}
	if (b->flags & BLK_FREE) {
		pthread_mutex_unlock(&p->lock);
		fprintf(stderr, "%s: DMA double free of %p\n", p->tag, ptr);
		return;
	}

	p->used -= NOSAIC_DMA_HDR + b->size;
	p->stat[b->name_idx].frees++;
	p->stat[b->name_idx].outstanding -= b->size;

	b->flags |= BLK_FREE;
	b->name_idx = 0;

	/* Coalesce forwards, then backwards. Without this the pool fragments
	 * into the shape of whatever the SDK happened to ask for first, and
	 * runs out with megabytes free. */
	n = b->next;
	if (n != NULL && (n->flags & BLK_FREE)) {
		b->size += NOSAIC_DMA_HDR + n->size;
		b->next = n->next;
		if (n->next != NULL)
			n->next->prev = b;
		n->magic = 0;
	}
	n = b->prev;
	if (n != NULL && (n->flags & BLK_FREE)) {
		n->size += NOSAIC_DMA_HDR + b->size;
		n->next = b->next;
		if (b->next != NULL)
			b->next->prev = n;
		b->magic = 0;
	}

	pthread_mutex_unlock(&p->lock);
}

size_t nosaic_dmapool_used(struct nosaic_dmapool *p)
{
	size_t v;

	pthread_mutex_lock(&p->lock);
	v = p->used;
	pthread_mutex_unlock(&p->lock);
	return v;
}

size_t nosaic_dmapool_largest(struct nosaic_dmapool *p)
{
	size_t total, largest;

	pthread_mutex_lock(&p->lock);
	free_survey(p, &total, &largest);
	pthread_mutex_unlock(&p->lock);
	return largest;
}

int nosaic_dmapool_stats(struct nosaic_dmapool *p, struct nosaic_dma_stat *out, int max)
{
	int i, j, n = 0;

	if (out == NULL || max <= 0)
		return 0;

	pthread_mutex_lock(&p->lock);
	for (i = 0; i < p->nstats && n < max; i++) {
		if (p->stat[i].allocs == 0 && p->stat[i].fails == 0)
			continue;
		out[n++] = p->stat[i];
	}
	pthread_mutex_unlock(&p->lock);

	/* Most outstanding first: the caller is asking this question because
	 * something is holding the pool, so put that at the top. Insertion sort
	 * over at most NOSAIC_DMA_NAMES entries. */
	for (i = 1; i < n; i++) {
		struct nosaic_dma_stat t = out[i];
		for (j = i; j > 0 && out[j - 1].outstanding < t.outstanding; j--)
			out[j] = out[j - 1];
		out[j] = t;
	}
	return n;
}
