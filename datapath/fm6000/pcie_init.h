/* SPDX-License-Identifier: Apache-2.0 */
#ifndef FM6000_PCIE_INIT_H
#define FM6000_PCIE_INIT_H

#include "pci.h"

/*
 * Configure the FM6000 PCIe SerDes and enable the endpoint (PCI_CORE_CTRL_1).
 *
 * MUST be called after the chip has been fully booted (ring circulating).
 * The 42 self-resets from fm_boot_freelists() wipe any SerDes config written
 * earlier; there is no point running this before ring_init settles.
 *
 * ⚠ The caller MUST NOT access d->regs after this returns. Writing
 * PCI_CORE_CTRL_1 starts PCIe link training; link training disrupts the SCD's
 * PCIe slot on the x86 host. Any access to d->regs while training is in
 * progress risks a kernel fault.
 *
 * Two safe patterns for the caller:
 *
 *   1. The short-lived tool (fm6000-pcie-init): call _exit(0) immediately
 *      after this returns. The kernel reclaims the mapping before the bus
 *      retrains.
 *
 *   2. The daemon (boot.c): munmap d->regs and close d->bar_fd immediately
 *      after this returns (before any further access), then sleep until
 *      enumeration completes (~5 s), then call fm_open_lbus() and
 *      fm_attach_pcie() to reopen both transports.
 */
int fm_pcie_serdes_init(struct fm6000 *d);

#endif /* FM6000_PCIE_INIT_H */
