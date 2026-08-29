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
#include "props.h"
#include "sdk.h"

/* The Trident2+ this daemon is for. Broadcom's own identifier, so a build for
 * the wrong silicon is visible rather than mysterious. */
#define TD2P_VENDOR 0x14e4
#define TD2P_DEVICE 0xb860

/* Where the board's ASIC configuration lives on a running switch. The image
 * places it there from platform/<board>/config/asic.conf. */
#define DEFAULT_ASIC_CONF "/etc/nosaic/asic.conf"

/* How long --attach holds the device before exiting. Long enough for the SDK's
 * own threads to run and fault if they are going to. */
#define HOLD_SECONDS 10

static void usage(void)
{
	fprintf(stderr,
		"usage: nosd-td2p [--probe <pci-bdf>]\n"
		"\n"
		"  --attach <bdf> [conf]\n"
		"            create the SDK device over the BDE and attach the chip.\n"
		"            conf defaults to " DEFAULT_ASIC_CONF ", which carries the\n"
		"            board's port map; without one the SDK cannot attach.\n"
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
/*
 * The device the SDK is given.
 *
 * Static, not a local. soc_cm_device_create is handed a pointer to this as its
 * cookie, and every vector call reaches it through dev->cookie for the life of
 * the device -- from threads the SDK starts for itself. A local would go out
 * of scope the moment attach() returned, leaving the SDK dereferencing a dead
 * stack frame.
 */
static struct nosaic_bde attached_dev;

static int attach(const char *bdf, const char *conf)
{
	struct nosaic_bde *b = &attached_dev;
	int unit, n;

	if (nosaic_bde_open(b, bdf) != 0)
		return 1;

	/* The board's own ASIC configuration. Without it the SDK has no port map,
	 * and port configuration fails during attach with "Port config error !!"
	 * -- which names the symptom and not the cause. */
	n = nosaic_props_load(conf);
	if (n < 0) {
		fprintf(stderr, "nosd-td2p: no ASIC configuration at %s\n"
			"  the SDK needs a port map to attach; this board's is in\n"
			"  platform/<board>/config/asic.conf\n", conf);
		nosaic_bde_close(b);
		return 1;
	}
	printf("config     %d properties from %s\n", n, conf);

	unit = nosaic_sdk_attach(b, TD2P_DEVICE, 0x02);
	if (unit < 0) {
		nosaic_bde_close(b);
		return 1;
	}
	printf("SDK unit   %d\n", unit);
	printf("chip init complete; the SDK has the device.\n");

	/*
	 * The mapping is deliberately NOT torn down here.
	 *
	 * soc_attach starts threads of the SDK's own -- bcmPOLL among them, since
	 * this build runs in polled interrupt mode -- and they keep reading the
	 * BAR through our vectors. Closing the BDE on the way out unmapped it
	 * underneath them, and bcmPOLL segfaulted on its next register read while
	 * this function was busy reporting success. The attach genuinely worked;
	 * the teardown was what broke it, and it did so somewhere nothing was
	 * looking.
	 *
	 * Holding briefly is what makes that visible: a run that returns
	 * immediately cannot tell a healthy poller from one that is about to
	 * die. The daemon will hold it for good once it serves the socket.
	 */
	printf("holding the device for %d seconds so the SDK's threads can run\n",
	       HOLD_SECONDS);
	sleep(HOLD_SECONDS);
	printf("still attached after %d seconds\n", HOLD_SECONDS);
	return 0;
}
#endif

int main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--probe") == 0)
		return probe(argv[2]);
#ifdef NOSAIC_WITH_SDK
	if (argc == 3 && strcmp(argv[1], "--attach") == 0)
		return attach(argv[2], DEFAULT_ASIC_CONF);
	if (argc == 4 && strcmp(argv[1], "--attach") == 0)
		return attach(argv[2], argv[3]);
#endif
	if (argc == 2 && strcmp(argv[1], "--version") == 0) {
		printf("nosd-td2p (bring-up) for BCM%04x, Broadcom vendor %#06x\n",
		       TD2P_DEVICE, TD2P_VENDOR);
		return 0;
	}
	usage();
	return 2;
}
