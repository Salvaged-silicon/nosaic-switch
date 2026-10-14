/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fm6000-pcie-init: configure FM6000 PCIe SerDes and enable the endpoint.
 *
 * Run AFTER nosd-fm6000 has fully booted the chip (ring circulating). The 42
 * self-resets from fm_boot_freelists() wipe any SerDes config written before
 * the ring starts; this tool must not run before that.
 *
 * Usage:
 *   fm6000-pcie-init [--scd ADDR]
 *
 * After this tool exits, wait ~4 s for hotplug enumeration, then (re)start
 * nosd-fm6000. The daemon finds 8086:155b on the bus, calls fm_attach_pcie(),
 * and writes ESCHED via PCIe resource0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "boot.h"
#include "pci.h"
#include "pcie_init.h"

int main(int argc, char **argv)
{
	struct fm6000 dev;
	const char *scd_slot = NULL;
	int i, rv;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--scd") == 0 && i + 1 < argc)
			scd_slot = argv[++i];
		else {
			fprintf(stderr,
				"usage: fm6000-pcie-init [--scd ADDR]\n"
				"\n"
				"  Configures the FM6000 PCIe SerDes and enables the endpoint.\n"
				"  Run after nosd-fm6000 has fully booted the chip.\n"
				"  After this exits, wait ~4 s then (re)start nosd-fm6000.\n");
			return 2;
		}
	}

	memset(&dev, 0, sizeof(dev));
	dev.pcie_bar_fd = -1;
	dev.bar_fd      = -1;

	if (fm_open_lbus(&dev, scd_slot) != FM_OK) {
		fprintf(stderr, "fm6000-pcie-init: cannot open LBUS "
				"(no SCD found or BAR1 mapping failed)\n");
		return 1;
	}

	/* Verify the chip is alive and fully booted before touching SerDes. */
	if (fm_alive(&dev) != 1) {
		fprintf(stderr, "fm6000-pcie-init: chip not answering on LBUS -- "
				"run nosd-fm6000 to boot the chip first\n");
		fm_close(&dev);
		return 1;
	}

	rv = fm_boot_already_done(&dev);
	if (rv < 0) {
		fprintf(stderr, "fm6000-pcie-init: cannot read chip state\n");
		fm_close(&dev);
		return 1;
	}
	if (rv == 0) {
		fprintf(stderr, "fm6000-pcie-init: chip is not booted -- "
				"run nosd-fm6000 first (ring must be circulating "
				"before SerDes config survives freelists init)\n");
		fm_close(&dev);
		return 1;
	}

	fprintf(stderr, "fm6000-pcie-init: chip is booted; configuring PCIe SerDes\n");

	rv = fm_pcie_serdes_init(&dev);
	if (rv != FM_OK) {
		fprintf(stderr, "fm6000-pcie-init: SerDes init failed (rv=%d) -- "
				"chip may have reset itself; try again\n", rv);
		fm_close(&dev);
		return 1;
	}

	fprintf(stderr, "fm6000-pcie-init: PCI_CORE_CTRL_1 written; exiting now\n"
			"fm6000-pcie-init: wait ~4 s for enumeration, then restart nosd-fm6000\n");

	/*
	 * Exit immediately without calling fm_close(). Writing PCI_CORE_CTRL_1
	 * starts PCIe link training which disrupts the SCD's slot on the host
	 * bus. Any access to dev.regs (the SCD BAR1 mapping) after this hangs
	 * the kernel. Process exit reclaims the mmap before that can happen.
	 */
	_exit(0);
}
