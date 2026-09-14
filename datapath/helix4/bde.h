/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_HELIX4_BDE_H
#define NOSAIC_HELIX4_BDE_H

#include <stddef.h>
#include <stdint.h>

#include "dmapool.h"

/*
 * The BDE for Helix4 (BCM56340), and the first in this tree with no PCI bus
 * under it.
 *
 * Every other NOSaic BDE starts the same way: find a PCI device, read its
 * configuration space, map BAR0. On the AS4610 the CMIC is part of the same
 * die as the CPU and appears as a platform device at a fixed physical address,
 * so there is no device to find, no configuration space to read and no BAR.
 * What there is instead is a UIO character device: a mapping of the register
 * window, and a file descriptor that becomes readable when the chip raises its
 * interrupt.
 *
 * WHAT THAT REMOVES.
 *
 * The AS5610's hardest bug cannot happen here. There, every DMA the chip
 * issued was correct and was discarded one hop upstream at a PCIe root port
 * whose Bus Master Enable was clear, because Linux binds no driver to either
 * end and so calls pci_set_master() on neither. There is no hop. There is no
 * bridge, no bus master bit, and no configuration space in which to find one.
 *
 * WHAT THAT ADDS.
 *
 * The SDK still expects PCI configuration space -- soc_cm_device_vectors_t has
 * pci_conf_read and pci_conf_write, and the CMICd code paths read a handful of
 * fields out of them. Those have to be answered by something. See sdk.c.
 */
struct nosaic_helix4_bde {
	/* Which UIO device, e.g. "uio0", and the platform device name it
	 * carries, e.g. "48000000.iproc_cmicd". Kept for messages: half the
	 * failures here are "bound to the wrong thing". */
	char           uio[16];
	char           name[64];

	int            uio_fd;
	volatile void *bar;      /* map0: the CMIC register window */
	size_t         bar_len;
	uint64_t       bar_phys;

	uint16_t       device_id;   /* read out of the chip, not off a bus */
	uint8_t        rev_id;

	/*
	 * The DMA pool. Two ways in, and the BDE takes whichever it finds:
	 *
	 *   map1 of the same UIO device, when the CMIC node in the device tree
	 *   carries a second reg range covering the reserved region. No
	 *   /dev/mem, and nothing for CONFIG_STRICT_DEVMEM to object to.
	 *
	 *   /proc/device-tree/reserved-memory plus /dev/mem, which is what the
	 *   AS5610 does and the only one of the two proven on hardware.
	 *
	 * dma_via_uio records which, because when the pool misbehaves that is
	 * the first thing worth knowing.
	 */
	int            mem_fd;
	int            dma_via_uio;
	void          *dma;
	uint64_t       dma_phys;
	size_t         dma_len;
	struct nosaic_dmapool pool;
};

/*
 * Find and map the CMIC.
 *
 * name may be NULL, in which case the first UIO device whose name contains
 * "cmic" is taken. Naming it matters on a board with more than one UIO device
 * and costs nothing on a board with one.
 *
 * Returns 0 on success. On failure it says what it looked at, because the
 * usual cause is not a broken mapping but a kernel that bound nothing: mainline
 * uio_pdrv_genirq matches only what its `of_id` parameter names, so without
 *
 *     uio_pdrv_genirq.of_id=brcm,iproc-cmicd
 *
 * on the kernel command line there is no /dev/uio* at all and the device tree
 * looks correct while nothing has happened.
 */
int  nosaic_helix4_bde_open(struct nosaic_helix4_bde *b, const char *name);
void nosaic_helix4_bde_close(struct nosaic_helix4_bde *b);

/*
 * Map the DMA pool, preferring UIO map1 and falling back to reserved-memory.
 *
 * Separate from open() so a probe can report the register window working and
 * the pool not, which are different problems with different fixes.
 *
 * Returns 0 on success, -1 having explained which paths were tried.
 */
int nosaic_helix4_bde_map_dma(struct nosaic_helix4_bde *b);

/*
 * Read the chip's identity back through the mapping.
 *
 * On a PCI board this is a cross-check: the bus already said what the device
 * is, and disagreeing with it means the mapping is wrong. Here it is the only
 * source -- nothing else on this machine knows what the switch chip is -- so
 * it is also how dev_id and rev_id get filled in before the SDK is told.
 *
 * Returns 0 if the register read plausibly, -1 if it came back as all-ones or
 * all-zeroes, which is what an unmapped or unpowered window looks like.
 * *raw is filled in either way, for a caller that wants to print it.
 */
int nosaic_helix4_bde_identify(struct nosaic_helix4_bde *b, uint32_t *raw);

/*
 * Wait for the chip's interrupt, up to timeout_ms. Returns the number of
 * interrupts since the last call, 0 on timeout, -1 on error.
 *
 * This is the thing the AS5610 does not have. That board polls the interrupt
 * status register because nothing delivers its chip's interrupt to userspace,
 * which costs a core and puts a floor under punt latency. Here the interrupt
 * is a read on a file descriptor.
 */
int nosaic_helix4_bde_wait_irq(struct nosaic_helix4_bde *b, int timeout_ms);

/* Re-arm the interrupt. uio_pdrv_genirq masks at the controller on each one
 * and will not deliver another until this is written. Forgetting it gives
 * exactly one interrupt and then silence. */
int nosaic_helix4_bde_arm_irq(struct nosaic_helix4_bde *b);

uint32_t nosaic_helix4_bde_rd(struct nosaic_helix4_bde *b, uint32_t off);
void     nosaic_helix4_bde_wr(struct nosaic_helix4_bde *b, uint32_t off, uint32_t v);

#endif
