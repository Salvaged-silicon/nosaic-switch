/* SPDX-License-Identifier: Apache-2.0 */
/*
 * FM6000 packet-DMA engine, userspace implementation.
 *
 * Ring mechanics adapted from EdgeNOS fpdma.c (GPL-2.0, our code).
 * Backing store uses mlock + /proc/self/pagemap instead of dma_alloc_coherent.
 * Only valid on the 7150S (AMD RS780, no IOMMU).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>

#include "dma.h"
#include "pci.h"
#include "regs.h"

/* x86: sfence orders descriptor-body stores before the ownership handoff. */
#define DMA_WMB() __asm__ __volatile__("sfence" ::: "memory")

/* ---- physical address lookup via /proc/self/pagemap --------------------- */

static uintptr_t virt_to_phys(const void *ptr)
{
	uintptr_t va = (uintptr_t)ptr;
	uint64_t entry = 0;
	int fd;
	off_t off;

	fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd < 0)
		return 0;
	off = (off_t)((va >> 12) * 8);
	if (pread(fd, &entry, 8, off) != 8) {
		close(fd);
		return 0;
	}
	close(fd);
	if (!(entry & (1ULL << 63)))
		return 0;		/* page not present */
	return (uintptr_t)((entry & 0x7fffffffffffffULL) << 12) | (va & 0xfff);
}

/* Alloc one page, pin it, return physical address. */
static uint8_t *alloc_pinned_page(uintptr_t *phys_out)
{
	void *p = NULL;

	if (posix_memalign(&p, 4096, 4096) != 0)
		return NULL;
	memset(p, 0, 4096);
	if (mlock(p, 4096) != 0) {
		free(p);
		return NULL;
	}
	*phys_out = virt_to_phys(p);
	if (*phys_out == 0) {
		munlock(p, 4096);
		free(p);
		return NULL;
	}
	return p;
}

/* Alloc N contiguous pages (for the descriptor ring). */
static uint8_t *alloc_pinned_pages(size_t bytes, uintptr_t *phys_out)
{
	size_t pages;
	uint8_t *p = NULL;
	uintptr_t ph0, ph1;

	bytes = (bytes + 4095) & ~(size_t)4095;
	pages = bytes / 4096;

	if (posix_memalign((void **)&p, FM6000_DMA_RING_ALIGN, bytes) != 0)
		return NULL;
	memset(p, 0, bytes);
	if (mlock(p, bytes) != 0) {
		free(p);
		return NULL;
	}
	/* Verify physical contiguity for multi-page allocations. */
	ph0 = virt_to_phys(p);
	if (ph0 == 0) {
		munlock(p, bytes);
		free(p);
		return NULL;
	}
	if (pages > 1) {
		ph1 = virt_to_phys(p + 4096);
		if (ph1 != ph0 + 4096) {
			fprintf(stderr, "dma: ring pages not physically contiguous\n");
			munlock(p, bytes);
			free(p);
			return NULL;
		}
	}
	*phys_out = ph0;
	return p;
}

/* ---- descriptor helpers ------------------------------------------------- */

static inline volatile uint8_t *desc_at(struct fm_dma_ring *r, uint32_t i)
{
	return (volatile uint8_t *)(r->desc + (size_t)(i & r->mask) * FM6000_DESC_STRIDE);
}

static void desc_write(volatile uint8_t *d, uint8_t status,
		       uint16_t len, uintptr_t addr)
{
	*(volatile uint16_t *)(d + FM6000_DESC_LEN)     = len;
	*(volatile uint32_t *)(d + FM6000_DESC_ADDR_LO) = (uint32_t)(addr & 0xFFFFFFFFu);
	*(volatile uint32_t *)(d + FM6000_DESC_ADDR_HI) = (uint32_t)((uint64_t)addr >> 32);
	DMA_WMB();
	*(volatile uint8_t *)(d + FM6000_DESC_STATUS)   = status;
}

/* ---- DMA register I/O --------------------------------------------------- */

/*
 * DMA registers live in PCIe BAR0 (word address 0x1400-0x141f, byte 0x5000-
 * 0x507f).  They are not accessible via the SCD LocalBus (LBUS / BAR1) --
 * the DMA engine is a PCIe-side block.
 *
 * Write via d->pcie_regs directly, exactly as fm_wr_pcie() does for ESCHED.
 * Do NOT use fm_wr_byte()/fm_rd_byte() here: those functions call
 * fm_check_offbus() (a sysfs config-space read) after every access, and that
 * sysfs path can deadlock the kernel's PCIe AER handler on the 7150S-52 --
 * the same problem fm_reopen_lbus_devmem() and fm_attach_pcie() were fixed
 * for.  __sync_synchronize() (MFENCE) orders MMIO writes to the ring
 * registers before the DMA_COMMAND doorbell, matching fm_wr_pcie's barrier.
 */
static inline void dma_wr(struct fm6000 *d, uint32_t off, uint32_t val)
{
	d->pcie_regs[off / 4] = val;
	__sync_synchronize();
}

static inline uint32_t dma_rd(struct fm6000 *d, uint32_t off)
{
	uint32_t v;
	__sync_synchronize();
	v = d->pcie_regs[off / 4];
	return v;
}

/* Brief settle after a COMMAND write (matches EdgeNOS fpdma.c dma_settle). */
static void dma_settle(struct fm6000 *d)
{
	int i;
	for (i = 0; i < 50; i++) {
		uint32_t st = dma_rd(d, FM6000_DMA_STATUS);
		if (st == 0xFFFFFFFFu || (st & FM6000_DMA_STATUS_READY))
			break;
		usleep(10);
	}
}

/* ---- ring alloc / free --------------------------------------------------- */

static int ring_alloc(struct fm_dma_ring *r, uint32_t size, size_t buf_len)
{
	uint32_t i;
	size_t ring_bytes;

	memset(r, 0, sizeof(*r));
	r->size    = size;
	r->mask    = size - 1;
	r->buf_len = buf_len;

	ring_bytes = (size_t)size * FM6000_DESC_STRIDE;
	r->desc = alloc_pinned_pages(ring_bytes, &r->desc_phys);
	if (!r->desc)
		return -1;
	if (r->desc_phys & (FM6000_DMA_RING_ALIGN - 1)) {
		fprintf(stderr, "dma: ring base not %u-byte aligned\n",
			FM6000_DMA_RING_ALIGN);
		munlock(r->desc, ring_bytes);
		free(r->desc);
		r->desc = NULL;
		return -1;
	}

	r->buf_va   = calloc(size, sizeof(*r->buf_va));
	r->buf_phys = calloc(size, sizeof(*r->buf_phys));
	if (!r->buf_va || !r->buf_phys)
		return -1;

	if (buf_len) {
		/* Pre-fill RX ring: give every descriptor a buffer. */
		for (i = 0; i < size; i++) {
			r->buf_va[i] = alloc_pinned_page(&r->buf_phys[i]);
			if (!r->buf_va[i])
				return -1;
			desc_write(desc_at(r, i), FM6000_DESC_RX_READY,
				   (uint16_t)buf_len, r->buf_phys[i]);
		}
		r->head = size;
	}
	return 0;
}

static void ring_free(struct fm_dma_ring *r)
{
	uint32_t i;

	if (r->buf_va && r->buf_len) {
		for (i = 0; i < r->size; i++) {
			if (r->buf_va[i]) {
				munlock(r->buf_va[i], 4096);
				free(r->buf_va[i]);
			}
		}
	}
	if (r->desc) {
		size_t ring_bytes = (size_t)r->size * FM6000_DESC_STRIDE;
		munlock(r->desc, ring_bytes);
		free(r->desc);
	}
	free(r->buf_va);
	free(r->buf_phys);
	memset(r, 0, sizeof(*r));
}

/* ---- engine programming -------------------------------------------------- */

static void program_rings(struct fm_dma *dma)
{
	struct fm6000 *d = dma->d;
	uintptr_t rx_base = dma->rx.desc_phys;
	uintptr_t rx_end  = rx_base + (uintptr_t)dma->rx.size * FM6000_DESC_STRIDE;
	uintptr_t tx_base = dma->tx.desc_phys;
	uintptr_t tx_end  = tx_base + (uintptr_t)dma->tx.size * FM6000_DESC_STRIDE;

	/*
	 * FM6000 DMA is 32-bit: ring buffers must be below 4 GB.  On this
	 * board the Normal zone (>4 GB) is excluded with mem=3G in kernel_params;
	 * without it posix_memalign returns pages from the Normal zone and the
	 * chip silently cannot reach them.
	 */
	if (tx_base >> 32 || rx_base >> 32) {
		fprintf(stderr,
			"nosd-fm6000: FATAL: DMA rings above 4 GB "
			"(tx=0x%lx rx=0x%lx) -- add mem=3G to kernel_params\n",
			(unsigned long)tx_base, (unsigned long)rx_base);
		_exit(1);
	}

	/* Stop both engines before reprogramming ring addresses so the chip
	 * restarts from BASE (slot 0) rather than wherever the old ring left
	 * the current pointer. */
	dma_wr(d, FM6000_DMA_COMMAND, FM6000_DMA_CMD_TX_STOP);
	dma_settle(d);
	dma_wr(d, FM6000_DMA_COMMAND, FM6000_DMA_CMD_RX_STOP);
	dma_settle(d);

	dma_wr(d, FM6000_DMA_IP,    0xFFFFFFFFu);	/* clear pending  */
	dma_wr(d, FM6000_DMA_UNK68, 0);
	dma_wr(d, FM6000_DMA_CFG,   0x37u);		/* enable         */
	dma_wr(d, FM6000_DMA_CFG2,  FM6000_DMA_CFG2_INIT);

	dma_wr(d, FM6000_DMA_TX_BD_BASE_LO, (uint32_t)tx_base);
	dma_wr(d, FM6000_DMA_TX_BD_BASE_HI, (uint32_t)((uint64_t)tx_base >> 32));
	dma_wr(d, FM6000_DMA_TX_BD_END_LO,  (uint32_t)tx_end);
	dma_wr(d, FM6000_DMA_TX_BD_END_HI,  (uint32_t)((uint64_t)tx_end >> 32));
	dma_wr(d, FM6000_DMA_COMMAND, FM6000_DMA_CMD_TX);
	dma_settle(d);

	dma_wr(d, FM6000_DMA_RX_BD_BASE_LO, (uint32_t)rx_base);
	dma_wr(d, FM6000_DMA_RX_BD_BASE_HI, (uint32_t)((uint64_t)rx_base >> 32));
	dma_wr(d, FM6000_DMA_RX_BD_END_LO,  (uint32_t)rx_end);
	dma_wr(d, FM6000_DMA_RX_BD_END_HI,  (uint32_t)((uint64_t)rx_end >> 32));
	dma_wr(d, FM6000_DMA_COMMAND, FM6000_DMA_CMD_RX);
	dma_settle(d);

	dma_wr(d, FM6000_DMA_IM, FM6000_DMA_IM_RUN);
}

/* ---- public API ---------------------------------------------------------- */

int fm_dma_init(struct fm_dma *dma, struct fm6000 *d)
{
	memset(dma, 0, sizeof(*dma));
	dma->d = d;

	if (ring_alloc(&dma->tx, FM_DMA_TX_RING, 0) < 0) {
		fprintf(stderr, "dma: TX ring alloc failed\n");
		return -1;
	}
	if (ring_alloc(&dma->rx, FM_DMA_RX_RING, FM6000_RX_MAX_LEN) < 0) {
		fprintf(stderr, "dma: RX ring alloc failed\n");
		ring_free(&dma->tx);
		return -1;
	}
	/* Enable Memory Space + Bus Master so the chip can complete DMA
	 * transactions.  Done here rather than in boot.c so Bus Master is not
	 * active before the ring base addresses are written into the chip. */
	(void)fm_pcie_enable_command(d);
	program_rings(dma);
	fprintf(stderr, "nosd-fm6000: DMA rings up (tx=%u rx=%u)\n",
		FM_DMA_TX_RING, FM_DMA_RX_RING);
	return 0;
}

void fm_dma_shutdown(struct fm_dma *dma)
{
	if (dma->d) {
		dma_wr(dma->d, FM6000_DMA_COMMAND, 0);
		dma_wr(dma->d, FM6000_DMA_IM, 0xFFFFFFFFu);
	}
	ring_free(&dma->tx);
	ring_free(&dma->rx);
}

int fm_dma_tx(struct fm_dma *dma, const uint8_t *frame, uint16_t len,
	      uint16_t sglort, uint16_t dglort)
{
	struct fm_dma_ring *r = &dma->tx;
	uint32_t slot = r->head & r->mask;
	volatile uint8_t *d;
	uint8_t *buf;
	uint16_t total;

	/* Ring full check */
	if (((r->head + 1) & r->mask) == (r->tail & r->mask))
		return -1;
	/* Max frame with tag must fit in 4096 */
	if (len + FM6000_F64_LEN > 4096 - 4)
		return -1;

	/* Allocate TX buffer lazily; reuse slot once allocated. */
	if (!r->buf_va[slot]) {
		r->buf_va[slot] = alloc_pinned_page(&r->buf_phys[slot]);
		if (!r->buf_va[slot])
			return -1;
	}

	buf   = r->buf_va[slot];
	total = len + FM6000_F64_LEN;

	/*
	 * Build the frame with the F64 tag inline at offset 12.
	 * Frame layout: DMAC(6) SMAC(6) F64(8) EType(2) Payload...
	 * Input `frame`: DMAC(6) SMAC(6) EType(2) Payload...
	 */
	memcpy(buf, frame, 12);                  /* DMAC + SMAC           */
	buf[12] = 0x00; buf[13] = 0x00;          /* FTYPE = 0 (normal)    */
	buf[14] = 0x00; buf[15] = 0x00;          /* VLAN = 0              */
	buf[16] = (uint8_t)(sglort >> 8);
	buf[17] = (uint8_t)(sglort & 0xff);      /* SGLORT big-endian     */
	buf[18] = (uint8_t)(dglort >> 8);
	buf[19] = (uint8_t)(dglort & 0xff);      /* DGLORT big-endian     */
	memcpy(buf + 20, frame + 12, len - 12);  /* EType + payload       */

	d = desc_at(r, slot);
	/* Clear BD F64 field (must be zero when using inline tag). */
	memset((void *)(d + 0x0C), 0, FM6000_DESC_STRIDE - 0x0C);
	desc_write(d, FM6000_DESC_HANDOFF, total, r->buf_phys[slot]);
	r->head++;

	dma_wr(dma->d, FM6000_DMA_COMMAND, FM6000_DMA_CMD_TX_POST);
	return 0;
}

int fm_dma_tx_reclaim(struct fm_dma *dma)
{
	struct fm_dma_ring *r = &dma->tx;
	int n = 0;

	while (r->tail != r->head) {
		volatile uint8_t *d = desc_at(r, r->tail);
		if (!(*(volatile uint8_t *)(d + FM6000_DESC_STATUS) & FM6000_DESC_DONE))
			break;
		r->tail++;
		n++;
	}
	return n;
}

int fm_dma_rx_poll(struct fm_dma *dma, int budget,
		   void (*cb)(void *ctx, const uint8_t *frame, uint16_t len),
		   void *ctx)
{
	struct fm_dma_ring *r = &dma->rx;
	int n = 0;

	/* Ack pending interrupts (W1C) */
	dma_wr(dma->d, FM6000_DMA_IP,
	       dma_rd(dma->d, FM6000_DMA_IP));

	while (n < budget) {
		volatile uint8_t *d = desc_at(r, r->tail);
		uint8_t  status = *(volatile uint8_t *)(d + FM6000_DESC_STATUS);
		uint16_t len;

		if (!(status & FM6000_DESC_DONE))
			break;

		len = *(volatile uint16_t *)(d + FM6000_DESC_LEN);
		if (len > FM6000_RX_MAX_LEN)
			len = FM6000_RX_MAX_LEN;

		if (cb)
			cb(ctx, r->buf_va[r->tail & r->mask], len);

		/* Recycle: hand descriptor back to HW */
		desc_write(d, FM6000_DESC_RX_READY, (uint16_t)r->buf_len,
			   r->buf_phys[r->tail & r->mask]);
		r->tail++;
		n++;
	}
	if (n)
		dma_wr(dma->d, FM6000_DMA_COMMAND, FM6000_DMA_CMD_RX_POST);
	return n;
}
