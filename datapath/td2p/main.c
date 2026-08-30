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
#define HOLD_SECONDS 60

static void usage(void)
{
	fprintf(stderr,
		"usage: nosd-td2p [--probe <pci-bdf>]\n"
		"\n"
		"  --attach <bdf> [conf]\n"
		"            create the SDK device over the BDE and attach the chip.\n"
		"            One or more property files; defaults to\n"
		"            " DEFAULT_ASIC_CONF ". The board ships generic settings\n"
		"            there and the generated port map goes beside it, so give\n"
		"            both: asic.conf portmap.conf\n"
		"            Built only when the SDK is staged (SDK_DIR).\n"
		"  --soc-init <bdf> [conf]\n"
		"            attach and run soc_init, then stop and hold. The point\n"
		"            between the SOC layer coming up and the BCM layer's\n"
		"            first table write, so the chip can be examined there.\n"
		"  --init <bdf> [conf]\n"
		"            attach, then finish initialisation and survey which\n"
		"            ports have link. Link is the only evidence there is for\n"
		"            which physical lane reaches which front-panel cage.\n"
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

/* Load one or more property files. Later files add to earlier ones, which is
 * what lets the board ship its generic settings and the operator supply the
 * generated port map beside them rather than editing one file. */
static int load_confs(char **confs, int n)
{
	int i, total = 0;

	for (i = 0; i < n; i++) {
		int c = nosaic_props_load(confs[i]);

		if (c < 0) {
			fprintf(stderr, "nosd-td2p: cannot read %s\n", confs[i]);
			return -1;
		}
		printf("config     %d properties from %s\n", c, confs[i]);
		total += c;
	}
	return total;
}

static int attach(const char *bdf, char **confs, int nconf, int full)
{
	struct nosaic_bde *b = &attached_dev;
	int unit, n;

	if (nosaic_bde_open(b, bdf) != 0)
		return 1;

	/* The board's own ASIC configuration. Without it the SDK has no port map,
	 * and port configuration fails during attach with "Port config error !!"
	 * -- which names the symptom and not the cause. */
	n = load_confs(confs, nconf);
	if (n < 0) {
		nosaic_bde_close(b);
		return 1;
	}
	if (nosaic_props_get("portmap_1") == NULL)
		fprintf(stderr, "nosd-td2p: warning: no portmap_1 property. The SDK cannot\n"
			"  attach without a port map, and this board's is generated rather\n"
			"  than shipped -- see platform/<board>/tools/mkportmap.sh\n");

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
	/*
	 * full is how far to take initialisation:
	 *   0  attach only
	 *   1  attach + soc_init, then stop -- a deliberate halfway point, so
	 *      the chip's state can be examined between the SOC layer coming up
	 *      and the BCM layer's first table write, which is where this
	 *      currently fails
	 *   2  the whole sequence, then survey the ports
	 */
	if (full >= 1) {
		printf("\nsoc_reset_init (resets the chip, then initialises)...\n");
		if (nosaic_sdk_soc_init(unit) != 0)
			return 1;
		printf("the SOC layer is up.\n");
	}
	if (full >= 2) {
		printf("\nbcm_attach and bcm_init...\n");
		if (nosaic_sdk_bcm_init(unit) != 0)
			return 1;
		printf("the chip is initialised and running.\n\n");
		nosaic_props_report_unused();
		nosaic_sdk_ports(unit);
	}

	printf("\nholding the device for %d seconds so the SDK's threads can run\n",
	       HOLD_SECONDS);
	sleep(HOLD_SECONDS);
	printf("still attached after %d seconds\n", HOLD_SECONDS);
	return 0;
}
#endif

int main(int argc, char **argv)
{
	/*
	 * Unbuffered, because this program can abort inside the SDK.
	 *
	 * Redirected to a file, stdout is fully buffered, so everything printed
	 * before an abort is discarded with the buffer -- and what is left is the
	 * SDK's own writes, which go to stderr, ending at whatever it happened to
	 * say last. That makes the abort look like it happened somewhere it did
	 * not.
	 */
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	if (argc == 3 && strcmp(argv[1], "--probe") == 0)
		return probe(argv[2]);
#ifdef NOSAIC_WITH_SDK
	{
		static char *defconf[] = { (char *)DEFAULT_ASIC_CONF };
		int full = -1;

		if (strcmp(argv[1], "--attach") == 0)   full = 0;
		if (strcmp(argv[1], "--soc-init") == 0) full = 1;
		if (strcmp(argv[1], "--init") == 0)     full = 2;
		if (full >= 0 && argc >= 3) {
			if (argc == 3)
				return attach(argv[2], defconf, 1, full);
			return attach(argv[2], &argv[3], argc - 3, full);
		}
	}
#endif
	if (argc == 2 && strcmp(argv[1], "--version") == 0) {
		printf("nosd-td2p (bring-up) for BCM%04x, Broadcom vendor %#06x\n",
		       TD2P_DEVICE, TD2P_VENDOR);
		return 0;
	}
	usage();
	return 2;
}
