/* SPDX-License-Identifier: Apache-2.0 */
/*
 * nosd-helix4 — the datapath daemon for BCM56340 (Helix4) boards.
 *
 * One binary with a probe mode, the way nosd-tdp is, and for the same reason:
 * on a board nobody has booted, the questions "can this process reach the
 * chip at all" and "does the chip forward" want separate answers, and a
 * separate tool would drift from the daemon that matters.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bde.h"
#include "sdk.h"
#include "props.h"

#define TAG "nosd-helix4"

/* Read in name order, and last wins, so a switch's own configuration on the
 * data partition layers over the image's. */
static const char *const datapath_conf[] = {
	"/etc/nosaic",
	NOSAIC_CONFIG_DIR,
};

static void usage(void)
{
	fprintf(stderr,
"usage: nosd-helix4 [--probe] [--ports] [--stats N] [--forward] [--uio NAME]\n"
"\n"
"With no argument it attaches the chip, brings it up and serves the datapath.\n"
"\n"
"  --probe        map the chip, identify it, map the DMA pool, and stop.\n"
"                 Touches no chip state: safe on a switch that is running.\n"
"  --ports        bring every port up and report, without serving\n"
"  --stats N      sample every port's counters twice, N seconds apart\n"
"  --forward      with --ports, bridge every port together in VLAN 1.\n"
"                 A LOOP wherever two ports reach the same neighbour.\n"
"  --uio NAME     the UIO device's name, e.g. 48000000.iproc_cmicd.\n"
"                 Default: the first whose name contains \"cmic\".\n");
}

/* What the probe reports, in the order the questions actually get asked. */
static int probe(struct nosaic_helix4_bde *b)
{
	uint32_t raw = 0;
	int ok;

	printf("uio          /dev/%s  (%s)\n", b->uio, b->name);
	printf("registers    %#llx, %zu KiB\n",
	       (unsigned long long)b->bar_phys, b->bar_len / 1024);

	ok = nosaic_helix4_bde_identify(b, &raw);
	printf("chip         id register %#010x", raw);
	if (ok == 0)
		printf(" -> device %#06x rev %#04x\n", b->device_id, b->rev_id);
	else
		printf("  ** not a device **\n");

	if (nosaic_helix4_bde_map_dma(b) == 0) {
		printf("dma pool     %#llx, %zu MiB, via %s\n",
		       (unsigned long long)b->dma_phys, b->dma_len / (1024 * 1024),
		       b->dma_via_uio ? "UIO map1" : "/dev/mem");
	} else {
		printf("dma pool     none  ** the SDK cannot initialise the chip "
		       "without one **\n");
		ok = -1;
	}

	/*
	 * The interrupt, checked because this is the board that has one. A
	 * timeout is not a failure here: an idle chip that has not been
	 * initialised has nothing to raise one about. What is being established
	 * is that the file descriptor works and the re-arm is accepted.
	 */
	if (nosaic_helix4_bde_arm_irq(b) == 0) {
		int n = nosaic_helix4_bde_wait_irq(b, 100);

		printf("interrupt    armed; %s\n", n > 0 ? "one arrived" :
		       n == 0 ? "none in 100 ms, which is expected on an idle chip" :
		       "the wait failed");
	} else {
		printf("interrupt    could not be armed\n");
		ok = -1;
	}
	return ok;
}

int main(int argc, char **argv)
{
	struct nosaic_helix4_bde bde;
	const char *uio = NULL;
	int do_probe = 0, do_ports = 0, do_forward = 0, stats = 0;
	int unit, i, n;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--probe"))
			do_probe = 1;
		else if (!strcmp(argv[i], "--ports"))
			do_ports = 1;
		else if (!strcmp(argv[i], "--forward"))
			do_forward = 1;
		else if (!strcmp(argv[i], "--stats") && i + 1 < argc)
			stats = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--uio") && i + 1 < argc)
			uio = argv[++i];
		else {
			usage();
			return 2;
		}
	}

	if (nosaic_helix4_bde_open(&bde, uio) != 0)
		return 1;

	if (do_probe) {
		int rv = probe(&bde);

		nosaic_helix4_bde_close(&bde);
		return rv == 0 ? 0 : 1;
	}

	if (nosaic_helix4_bde_identify(&bde, NULL) != 0) {
		nosaic_helix4_bde_close(&bde);
		return 1;
	}
	if (nosaic_helix4_bde_map_dma(&bde) != 0) {
		nosaic_helix4_bde_close(&bde);
		return 1;
	}
	printf("chip         device %#06x rev %#04x\n", bde.device_id, bde.rev_id);
	printf("dma pool     %zu MiB at %#llx, via %s\n",
	       bde.dma_len / (1024 * 1024), (unsigned long long)bde.dma_phys,
	       bde.dma_via_uio ? "UIO map1" : "/dev/mem");

	/*
	 * Properties before attach: the SDK asks for them during it, through
	 * config_var_get, and a property loaded afterwards is one the chip was
	 * brought up without.
	 */
	for (i = 0, n = 0; i < (int)(sizeof(datapath_conf) / sizeof(*datapath_conf)); i++) {
		int got = nosaic_props_load_dir(datapath_conf[i]);

		if (got > 0) {
			printf("config       %d propert%s from %s\n",
			       got, got == 1 ? "y" : "ies", datapath_conf[i]);
			n += got;
		}
	}
	if (n == 0)
		printf("config       none found; the chip comes up on its own defaults\n");

	unit = nosaic_helix4_sdk_attach(&bde, bde.device_id, bde.rev_id);
	if (unit < 0) {
		nosaic_helix4_bde_close(&bde);
		return 1;
	}
	if (nosaic_helix4_sdk_soc_init(unit) != 0 ||
	    nosaic_helix4_sdk_bcm_init(unit) != 0) {
		nosaic_helix4_bde_close(&bde);
		return 1;
	}

	/* Anything the SDK asked for and did not get. Reported here rather than
	 * at the moment of the access, so it is one block at the end of
	 * bring-up instead of scattered through the SDK's own log. */
	{
		uint32_t miss[8];
		int total = nosaic_helix4_sdk_iproc_misses(miss, 8);

		if (total > 0) {
			printf("iproc        %d unmapped register%s the SDK wanted:",
			       total, total == 1 ? "" : "s");
			for (i = 0; i < total && i < 8; i++)
				printf(" %#010x", miss[i]);
			printf("%s\n", total > 8 ? " ..." : "");
			printf("             map exactly these by adding a reg range to the "
			       "CMIC node\n");
		}
	}

	nosaic_props_report_unused();

	if (stats > 0) {
		int rv = nosaic_helix4_sdk_stats(unit, stats);

		nosaic_helix4_bde_close(&bde);
		return rv == 0 ? 0 : 1;
	}

	if (nosaic_helix4_sdk_ports_up(unit, do_forward) != 0) {
		nosaic_helix4_bde_close(&bde);
		return 1;
	}
	if (do_ports) {
		nosaic_helix4_bde_close(&bde);
		return 0;
	}

	/* Does not return unless something fails. */
	nosaic_helix4_sdk_run(unit);
	nosaic_helix4_bde_close(&bde);
	return 1;
}
