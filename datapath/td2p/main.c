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
#include "sdk.h"

/* The Trident2+ this daemon is for. Broadcom's own identifier, so a build for
 * the wrong silicon is visible rather than mysterious. */
#define TD2P_VENDOR 0x14e4
#define TD2P_DEVICE 0xb860

static void usage(void)
{
	fprintf(stderr,
		"usage: nosd-td2p [--probe <pci-bdf>]\n"
		"\n"
		"  --attach  create the SDK device over the BDE and report the unit.\n"
		"            Built only when the SDK is staged (SDK_DIR).\n"
		"  --probe   map the device and report what is there, then exit.\n"
		"            Requires the DMA region reserved on the kernel command\n"
		"            line and root for /dev/mem. On the 7050SX2 that is\n"
		"            memmap=64M$0xd0000000 iomem=relaxed -- not 0x100000000,\n"
		"            which is past the end of that board's 3844 MB.\n"
		"            NOSAIC_DMA_BASE and NOSAIC_DMA_SIZE override it.\n");
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

#ifdef NOSAIC_WITH_SDK
/* Hand the chip to the SDK.
 *
 * This is the point of the whole exercise: once the vectors are installed, the
 * SDK owns chip initialisation -- which is deliberately not ours to write,
 * because the Trident2 MMU and LLS sequences are exactly what hand-
 * reproduction repeatedly failed to match on this silicon. */
static int attach(const char *bdf)
{
	struct nosaic_bde b;
	int unit;

	if (nosaic_bde_open(&b, bdf) != 0)
		return 1;

	unit = nosaic_sdk_attach(&b, TD2P_DEVICE, 0x02);
	if (unit < 0) {
		nosaic_bde_close(&b);
		return 1;
	}
	printf("SDK unit   %d\n", unit);
	printf("the SDK has the device; chip init is next.\n");

	nosaic_bde_close(&b);
	return 0;
}
#endif

int main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--probe") == 0)
		return probe(argv[2]);
#ifdef NOSAIC_WITH_SDK
	if (argc == 3 && strcmp(argv[1], "--attach") == 0)
		return attach(argv[2]);
#endif
	if (argc == 2 && strcmp(argv[1], "--version") == 0) {
		printf("nosd-td2p (bring-up) for BCM%04x, Broadcom vendor %#06x\n",
		       TD2P_DEVICE, TD2P_VENDOR);
		return 0;
	}
	usage();
	return 2;
}
