/*
 * nosd-td2p — the Trident2+ datapath.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Implements switch-api over the Broadcom SDK, with the userspace BDE in
 * bde.c underneath it. It speaks the same newline-delimited JSON on the same
 * Unix socket as every other provider, so the CLI, the config model and the
 * HAL above it do not know which chip is answering.
 *
 * STATE: bring-up. This links the SDK and reports what it found; it does not
 * yet attach the device or serve the socket. Nothing here has run on hardware.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bde.h"

/* The Trident2+ this daemon is for. Broadcom's own identifier, so a build for
 * the wrong silicon is visible rather than mysterious. */
#define TD2P_VENDOR 0x14e4
#define TD2P_DEVICE 0xb860

static void usage(void)
{
	fprintf(stderr,
		"usage: nosd-td2p [--probe <pci-bdf>]\n"
		"\n"
		"  --probe   map the device and report what is there, then exit.\n"
		"            Requires the DMA region reserved on the kernel command\n"
		"            line (memmap=64M$0x100000000) and root for /dev/mem.\n");
}

static int probe(const char *bdf)
{
	struct nosaic_bde b;

	if (nosaic_bde_open(&b, bdf) != 0)
		return 1;

	printf("device      %s\n", b.bdf);
	printf("BAR0        %zu bytes mapped\n", b.bar_len);
	printf("DMA         %zu bytes at %#llx\n", b.dma_len,
	       (unsigned long long)b.dma_phys);

	nosaic_bde_close(&b);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--probe") == 0)
		return probe(argv[2]);
	if (argc == 2 && strcmp(argv[1], "--version") == 0) {
		printf("nosd-td2p (bring-up) for BCM%04x, Broadcom vendor %#06x\n",
		       TD2P_DEVICE, TD2P_VENDOR);
		return 0;
	}
	usage();
	return 2;
}
