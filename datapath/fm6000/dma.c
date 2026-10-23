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

static void dma_kmsg(const char *msg)
{
	int fd = open("/dev/kmsg", O_WRONLY | O_NOCTTY);
	if (fd >= 0) {
		write(fd, msg, strlen(msg));
		write(fd, "\n", 1);
		close(fd);
	}
}

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
 * DMA registers live in the FM6000 PCIe BAR0 space (offsets 0x5000+).
 * They are NOT reachable via the SCD local-bus (d->regs); that space maps
 * FM6000 switch/management registers (PLLs, EPL, parser, etc.) at entirely
 * different addresses.  Always use d->pcie_regs for DMA register access.
 *
 * Written with posted Memory Write TLPs, dma_wr is safe as long as BAR0
 * is mapped.  The 30-second pre-DMA sleep in start_forwarding ensures the
 * PCIe link is in L0 before dma_settle reads BAR0.
 *
 * install85 root cause: an earlier version probed d->regs[DMA_STATUS/4] and
 * found it returned 0x00000000 (some random LBUS register), mistook that for
 * "DMA registers reachable via LBUS", then wrote all ring addresses to wrong
 * LBUS offsets.  BAR0 was never programmed; the DMA engine never started.
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

uint32_t fm_dma_rd_reg(const struct fm_dma *dma, uint32_t byte_off)
{
	__sync_synchronize();
	return dma->d->pcie_regs[byte_off / 4];
}

/*
 * Brief settle after a COMMAND write.
 *
 * FM6000 PCIe BAR0 MMIO reads can stall indefinitely even after the 30-second
 * pre-DMA sleep: the PCIe endpoint responds to posted writes (fire-and-forget)
 * but completion TLPs for reads may not arrive until the link wakes from a
 * power state or finishes a training sequence the root complex cannot see.
 *
 * DMA command writes are posted writes; no completion is needed and they never
 * stall.  All we need here is a brief delay for the chip to process the command
 * before the next register write.  Skip the STATUS read entirely.
 */
static void dma_settle(struct fm6000 *d)
{
	(void)d;
	usleep(2000);	/* 2ms: time for FM6000 to process a DMA command */
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
	fprintf(stderr, "nosd-fm6000: DMA phys: tx_base=0x%lx tx_end=0x%lx rx_base=0x%lx rx_end=0x%lx\n",
		(unsigned long)tx_base, (unsigned long)tx_end,
		(unsigned long)rx_base, (unsigned long)rx_end);
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

	/*
	 * DMA_IM_RUN = 0x3: matches the vendor fpdma.ko sequence (recovered
	 * from disasm in EdgeNOS FPDMA.md).  This must be written AFTER both
	 * TX and RX engines are started, not before.  Setting it before start
	 * prevents the engines from running -- the chip needs this "unmask"
	 * step to begin scanning descriptor rings.
	 *
	 * PCIe-level INTx is suppressed by the Interrupt Disable bit (bit 10)
	 * in the FM6000 endpoint command register (set by fm_pcie_enable_command
	 * during boot step 9), so no interrupt storm can occur even if DMA_IM
	 * also controls interrupt generation.
	 */
	dma_wr(d, FM6000_DMA_IM,   FM6000_DMA_IM_RUN);
}

/* ---- public API ---------------------------------------------------------- */

int fm_dma_init(struct fm_dma *dma, struct fm6000 *d)
{
	memset(dma, 0, sizeof(*dma));
	dma->d = d;

	dma_kmsg("nosd-fm6000: DMA TX ring_alloc start");
	if (ring_alloc(&dma->tx, FM_DMA_TX_RING, 0) < 0) {
		fprintf(stderr, "dma: TX ring alloc failed\n");
		return -1;
	}
	dma_kmsg("nosd-fm6000: DMA TX ring_alloc done; RX start");
	if (ring_alloc(&dma->rx, FM_DMA_RX_RING, FM6000_RX_MAX_LEN) < 0) {
		fprintf(stderr, "dma: RX ring alloc failed\n");
		ring_free(&dma->tx);
		return -1;
	}
	dma_kmsg("nosd-fm6000: DMA RX ring_alloc done");
	/* Bus Master was restored by start_forwarding() via fm_pcie_enable_portio()
	 * after the FATAL_COUNT stability wait.  No Config Write needed here. */
	dma_kmsg("nosd-fm6000: DMA program_rings start");
	program_rings(dma);
	dma_kmsg("nosd-fm6000: DMA program_rings done");
	fprintf(stderr, "nosd-fm6000: DMA rings up (tx=%u rx=%u)\n",
		FM_DMA_TX_RING, FM_DMA_RX_RING);
	return 0;
}

void fm_dma_reprogram(struct fm_dma *dma)
{
	/* Caller (fwd_thread) has already called fm_pcie_enable_portio() to
	 * restore Bus Master after the FATAL_COUNT stability wait.  We only
	 * need to reprogram the DMA ring addresses. */
	program_rings(dma);
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

	/* Ring full check */
	if (((r->head + 1) & r->mask) == (r->tail & r->mask))
		return -1;
	/* Frame (without F64 tag) must fit in one page */
	if (len > 4096 - 4)
		return -1;

	/* Allocate TX buffer lazily; reuse slot once allocated. */
	if (!r->buf_va[slot]) {
		r->buf_va[slot] = alloc_pinned_page(&r->buf_phys[slot]);
		if (!r->buf_va[slot])
			return -1;
	}

	buf = r->buf_va[slot];
	/*
	 * Frame buffer: verbatim copy WITHOUT any F64 tag.
	 * The DMA engine reads the F64 tag from the BD's field at offset 0x0C
	 * and inserts it into the frame at offset 12 on the way to the fabric
	 * (datasheet §7.11.1.4).  The frame buffer must be tag-free.
	 */
	memcpy(buf, frame, len);

	d = desc_at(r, slot);
	/* Write F64 into the BD field; zero unused bytes first. */
	memset((void *)(d + FM6000_DESC_F64), 0, FM6000_DESC_STRIDE - FM6000_DESC_F64);
	{
		volatile uint8_t *f = d + FM6000_DESC_F64;
		f[0] = 0x00; f[1] = 0x00;              /* FTYPE = 0 (normal)    */
		f[2] = 0x00; f[3] = 0x00;              /* VLAN = 0              */
		f[4] = (uint8_t)(sglort >> 8);
		f[5] = (uint8_t)(sglort & 0xff);       /* SGLORT big-endian     */
		f[6] = (uint8_t)(dglort >> 8);
		f[7] = (uint8_t)(dglort & 0xff);       /* DGLORT big-endian     */
	}
	desc_write(d, FM6000_DESC_HANDOFF, len, r->buf_phys[slot]);
	/* Log first TX descriptor physical address once for diagnostics. */
	if (r->head == 0) {
		static int logged;
		if (!logged) {
			logged = 1;
			fprintf(stderr, "nosd-fm6000: TX slot0 buf_phys=0x%lx len=%u\n",
				(unsigned long)r->buf_phys[slot], len);
		}
	}
	r->head++;

	/* TX_POST notifies the engine that a new descriptor is ready.
	 * EdgeNOS fpdma.ko uses TX_POST per-frame (FPDMA.md confirmed). */
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
	/* DMA_IP is NOT cleared here.  program_rings clears it once at startup
	 * and sets DMA_IM=0 (all interrupts masked).  With no interrupt line
	 * asserted there is nothing to ack; writing DMA_IP while the DMA engine
	 * is actively writing received frames via PCIe DMA causes the FM6000
	 * PCIe bridge to deadlock (the write never completes). */

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
