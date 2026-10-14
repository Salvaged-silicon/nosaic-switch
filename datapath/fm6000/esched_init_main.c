/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fm6000-esched-init: write the FM6000 egress scheduler configuration.
 *
 * Run AFTER fm6000-pcie-init has enumerated the chip as 8086:155b AND
 * after the ~4 s hotplug wait. This tool:
 *
 *   1. Opens PCIe resource0 directly (NO SCD/LBUS).
 *   2. Calls fm_esched_init() to write 390 ESCHED registers.
 *   3. Calls _exit(0) immediately.
 *
 * The immediate exit matters. If any ESCHED write triggers an asynchronous
 * chip fault the kernel could hang trying to service the PCIe bus error
 * while the region is still mapped. Exiting lets the kernel reclaim the
 * mmap cleanly before any fault propagates.
 *
 * nosd-fm6000 does NOT write ESCHED. The daemon must be DOWN when this
 * runs; concurrent LBUS (daemon) + PCIe (this tool) access to the ESCHED
 * block has been observed to hang the kernel.
 *
 * Sequence:
 *   1. Start nosd-fm6000 and let it reach the watch loop.
 *   2. Stop nosd-fm6000.
 *   3. Run fm6000-pcie-init; wait ~4 s for hotplug.
 *   4. Run fm6000-esched-init  (this tool).
 *   5. (Re)start nosd-fm6000.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esched.h"
#include "pci.h"

int main(void)
{
	struct fm6000 dev;
	int rv;
	unsigned written = 0;
	uint32_t culprit = 0;

	memset(&dev, 0, sizeof(dev));
	dev.bar_fd      = -1;
	dev.pcie_bar_fd = -1;

	/*
	 * Attach via PCIe resource0 only -- no LBUS. The daemon holds LBUS;
	 * concurrent LBUS+PCIe writes to ESCHED hang the kernel. Call this
	 * tool with the daemon stopped.
	 */
	if (fm_attach_pcie(&dev) != FM_OK) {
		fprintf(stderr, "fm6000-esched-init: cannot open PCIe resource0 "
				"(8086:155b not enumerated? run fm6000-pcie-init "
				"first, then wait ~4 s)\n");
		return 1;
	}

	fprintf(stderr, "fm6000-esched-init: PCIe attached; writing ESCHED\n");

	rv = fm_esched_init(&dev, &written, &culprit);

	if (rv != FM_OK)
		fprintf(stderr, "fm6000-esched-init: FAILED at 0x%06x after %u writes\n",
				culprit, written);
	else
		fprintf(stderr, "fm6000-esched-init: %u writes ok\n", written);

	/*
	 * Do not munmap(). If an esched write triggered a delayed chip reset
	 * the PCIe device may have disappeared; manually unmapping a gone
	 * device can stall. Let process exit handle reclamation.
	 */
	_exit(rv == FM_OK ? 0 : 1);
}
