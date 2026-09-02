/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_MMIO_H
#define NOSAIC_MMIO_H

#include <stdint.h>

/*
 * Register access to a memory-mapped switch chip, with the ordering the host
 * architecture requires.
 *
 * On x86 a plain volatile load is already correct: the architecture orders
 * MMIO against MMIO, and the chip's default little-endian programmed I/O
 * matches the CPU. That is what nosd-td2p did, and it works.
 *
 * On PowerPC neither of those holds. Stores may be reordered or merged unless
 * an explicit barrier says otherwise, and a load may still be in flight when
 * the next instruction runs -- so a register sequence that must happen in
 * order does not. S-Channel is exactly such a sequence: address, then data,
 * then a command that acts on both. EdgeNOS tried a plain userspace mmap on
 * this board and recorded that it "broke S-Channel within seconds", then
 * reverted to ioctl so the kernel's ioread32 would supply the barriers. This
 * header supplies them instead, which is what lets the BDE stay in userspace.
 *
 * The forms below are the ones Linux uses in arch/powerpc/include/asm/io.h:
 *
 *   store   sync, then the store            -- prior work is visible first
 *   load    sync, the load, then twi/isync  -- the trap-never depends on the
 *                                              loaded value, so the load must
 *                                              have completed before anything
 *                                              after it may issue
 *
 * Byte order is deliberately NOT handled here. Both accessors move a word in
 * the host's order, and the chip is configured to match the host at open time
 * -- little-endian by default for x86, big-endian via CMIC_ENDIAN_SELECT for
 * PowerPC. That is the same statement the SDK's own PowerPC platform makes
 * with SYS_BE_PIO=1, so software byte-swapping here would swap it back.
 */

#if defined(__powerpc__) || defined(__PPC__)

static inline uint32_t nosaic_mmio_rd32(volatile const void *addr)
{
	uint32_t v;
	__asm__ __volatile__("sync; lwz %0,0(%1); twi 0,%0,0; isync"
			     : "=r"(v) : "r"(addr) : "memory");
	return v;
}

static inline void nosaic_mmio_wr32(volatile void *addr, uint32_t v)
{
	__asm__ __volatile__("sync; stw %1,0(%0)"
			     : : "r"(addr), "r"(v) : "memory");
}

/* Ordering between two MMIO accesses that need not be individually complete,
 * for a caller writing a burst and then one register that acts on it. */
static inline void nosaic_mmio_barrier(void)
{
	__asm__ __volatile__("eieio" ::: "memory");
}

#else

static inline uint32_t nosaic_mmio_rd32(volatile const void *addr)
{
	return *(volatile const uint32_t *)addr;
}

static inline void nosaic_mmio_wr32(volatile void *addr, uint32_t v)
{
	*(volatile uint32_t *)addr = v;
}

static inline void nosaic_mmio_barrier(void)
{
	__asm__ __volatile__("" ::: "memory");
}

#endif

/*
 * CMIC_ENDIAN_SELECT, include/soc/cmic.h:453 in the OpenBCM SDK.
 *
 * The values carry the same bit in byte 0 and byte 3, which is how a register
 * that selects byte order can be written before the byte order is known: the
 * write lands correctly whichever way the word is currently interpreted.
 */
#define NOSAIC_CMIC_ENDIAN_SELECT   0x00000174
#define NOSAIC_CMIC_REVID_DEVID     0x00000178
#define NOSAIC_ES_BIG_ENDIAN_PIO        0x01000001
#define NOSAIC_ES_BIG_ENDIAN_DMA_PACKET 0x02000002
#define NOSAIC_ES_BIG_ENDIAN_DMA_OTHER  0x04000004

/*
 * What a big-endian host should select, and it is not all three.
 *
 * Read off the vendor OS running on this hardware with working DMA: its
 * CMIC_ENDIAN_SELECT is 0x04040404, which is DMA_OTHER alone. PIO is left
 * little-endian there because that stack reaches the chip through the kernel's
 * ioread32, which byte-swaps for it; NOSaic reads registers directly, so PIO
 * has to be big-endian here.
 *
 * Packet DMA stays little-endian either way, matching the machine that works.
 * Setting all three was a guess that the chip should simply match the host,
 * and it produced a SLAM DMA that never completed:
 *
 *   SlamDmaTimeOut:_soc_xgs3_mem_slam, Abort Failed
 *   soc_mem_write_range: write CPU_COS_MAP.ipipe0[0-127] failed
 *
 * The vectors handed to the SDK must agree with whatever is selected here, or
 * it byte-swaps descriptors the chip does not, and nothing reports it.
 */
#define NOSAIC_ES_BE_HOST (NOSAIC_ES_BIG_ENDIAN_PIO | NOSAIC_ES_BIG_ENDIAN_DMA_OTHER)

#endif /* NOSAIC_MMIO_H */
