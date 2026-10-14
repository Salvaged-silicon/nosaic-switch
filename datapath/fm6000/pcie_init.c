/* SPDX-License-Identifier: Apache-2.0 */
/*
 * FM6000 PCIe SerDes bringup. See pcie_init.h for the contract.
 *
 * Translated from EdgeNOS fm6000_i2c_bringup.c pcie() (GPL-2.0, reference
 * only). The SOFT_RESET write uses RMW because NOSaic's boot step 5 leaves
 * SOFT_RESET=0x00 (all released), unlike EdgeNOS which starts from 0x16.
 */
#include <time.h>

#include "pci.h"
#include "pcie_init.h"
#include "regs.h"
#include "sbus.h"

static void nap_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

int fm_pcie_serdes_init(struct fm6000 *d)
{
	uint32_t sr0 = 0;
	int rv;

	/* Re-apply scan chain to guarantee the PCIe SerDes sees the right
	 * mode bits on the settled chip (the first copy can be reset out from
	 * under itself by fm_boot_freelists()'s self-resets). */
	rv = fm_wr(d, FM6000_SCAN_CONFIG_DATA_IN, 0x88800000u);
	if (rv == FM_OK) { nap_ms(1); rv = fm_wr(d, FM6000_SCAN_CONFIG_DATA_IN, 0x88008000u); }
	if (rv == FM_OK) { nap_ms(1); rv = fm_wr(d, FM6000_SCAN_CONFIG_DATA_IN, 0x80000040u); }
	if (rv == FM_OK) rv = fm_wr(d, FM6000_SCAN_CHAIN_DATA_IN, 0xffffffffu);
	if (rv != FM_OK)
		return rv;

	rv = fm_wr(d, FM6000_DLL_CTRL, FM6000_DLL_CTRL_ENABLE);
	if (rv != FM_OK)
		return rv;
	nap_ms(1);

	/* Hold PCIe in reset while configuring the SerDes, then release. RMW
	 * preserves all other module-reset bits step 5 left cleared. */
	rv = fm_rd(d, FM6000_SOFT_RESET, &sr0);
	if (rv != FM_OK)
		return rv;
	rv = fm_wr(d, FM6000_SOFT_RESET, sr0 | FM6000_SOFT_RESET_PCIE);
	if (rv != FM_OK)
		return rv;

	rv = fm_wr(d, FM6000_SBUS_CFG, 0u);
	if (rv != FM_OK)
		return rv;
	nap_ms(1);

	rv = fm_wr(d, FM6000_LCI_IM, 1u);
	if (rv != FM_OK)
		return rv;

	/* Release PCIe from reset (SerDes starts locking to reference). */
	rv = fm_wr(d, FM6000_SOFT_RESET, sr0 & ~(uint32_t)FM6000_SOFT_RESET_PCIE);
	if (rv != FM_OK)
		return rv;

	/* SBus WRITE to device 0xFE (SBus controller), reg 0x0A, data 0x4.
	 * [RE from EdgeNOS: 0x0121FE0A = EXECUTE | OP_WRITE<<16 | DEV_CTRL<<8 | 0x0A] */
	rv = fm_sbus_write(d, FM_SBUS_DEV_CONTROLLER, 0x0au, 0x4u);
	if (rv != FM_OK)
		return rv;

	/* PCIe block: endian, config, DMA, interrupt masks, SerDes lanes,
	 * management clock gate. Everything before the core enable. */
	rv = fm_wr(d, FM6000_PCI_ENDIANISM,     0u);
	if (rv == FM_OK) rv = fm_wr(d, FM6000_PCI_CFG_1,         0x02000000u);
	if (rv == FM_OK) rv = fm_wr(d, FM6000_PCI_DMA_CFG,       0x35u);
	if (rv == FM_OK) rv = fm_wr(d, FM6000_PCI_MASK_A,        0xffffffffu);
	if (rv == FM_OK) rv = fm_wr(d, FM6000_PCI_MASK_B,        0xffffffffu);
	if (rv == FM_OK) rv = fm_wr(d, FM6000_PCI_SERDES_CTRL_1, 0x0f121f34u);
	if (rv == FM_OK) rv = fm_wr(d, FM6000_MGMT_PCIE_CLK,     0x3fffu);
	if (rv != FM_OK)
		return rv;

	/*
	 * Enable the PCIe endpoint. This starts link training on the host bus.
	 *
	 * ⚠ The caller MUST exit immediately after this returns without
	 * accessing d->regs again. Link training disrupts the SCD's PCIe slot;
	 * any LBUS read or write after this hangs the kernel. See pcie_init.h.
	 */
	return fm_wr(d, FM6000_PCI_CORE_CTRL_1, 0x1c01fu);
}
