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
 * ⚠ The caller MUST exit immediately after this returns. Writing
 * PCI_CORE_CTRL_1 starts PCIe link training; link training disrupts the SCD's
 * PCIe slot on the x86 host. If the SCD BAR1 mapping (d->regs) is still live
 * when that happens, any subsequent access to it hangs the kernel.
 *
 * The intended call site is fm6000-pcie-init's main(), which calls _exit(0)
 * right after this returns so the kernel reclaims the mapping before the bus
 * retrains.
 */
int fm_pcie_serdes_init(struct fm6000 *d);

#endif /* FM6000_PCIE_INIT_H */
