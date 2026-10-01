/* SPDX-License-Identifier: Apache-2.0 */

/*
 * fm6000-memfill -- initialise all ECC-protected SRAM banks.
 *
 * Run after fm6000-probe --lbus --boot, before --ssched:
 *
 *   fm6000-probe  --lbus --boot
 *   fm6000-memfill --lbus
 *   fm6000-probe  --lbus --ssched small
 *
 * The chip must be booted (bank-repair and freelist commands done) before
 * direct memory writes are safe. Use --lbus to reach the chip through the
 * SCD's BAR1 local-bus window; omit it if the chip is PCIe-enumerated.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "pci.h"
#include "boot.h"
#include "memfill.h"

static void nap_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

int main(int argc, char *argv[])
{
	struct fm6000 dev;
	int lbus = 0, rv;
	const char *slot = NULL;
	int i;
	uint32_t probe_base = 0;
	uint32_t probe_words = 0;
	long delay_ms = 0;
	unsigned probe_after_n = 0;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--lbus") == 0) {
			lbus = 1;
			if (i + 1 < argc && argv[i + 1][0] != '-')
				slot = argv[++i];
		} else if (strcmp(argv[i], "--probe-bank") == 0 && i + 2 < argc) {
			probe_base  = (uint32_t)strtoul(argv[++i], NULL, 0);
			probe_words = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "--probe-after-bank") == 0 && i + 3 < argc) {
			probe_after_n = (unsigned)atoi(argv[++i]);
			probe_base    = (uint32_t)strtoul(argv[++i], NULL, 0);
			probe_words   = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "--delay-ms") == 0 && i + 1 < argc) {
			delay_ms = atol(argv[++i]);
		}
	}

	rv = lbus ? fm_open_lbus(&dev, slot) : fm_open(&dev, slot);
	if (rv != FM_OK) {
		fprintf(stderr, "fm6000-memfill: chip not found\n");
		return 1;
	}

	printf("local bus: chip is %s\n",
	       fm_alive(&dev) == 1 ? "answering" : "not answering");

	if (fm_boot_already_done(&dev) != 1) {
		fprintf(stderr, "fm6000-memfill: run --boot first\n");
		return 1;
	}

	if (delay_ms > 0) {
		printf("delaying %ld ms before fill (ring settling)...\n", delay_ms);
		fflush(stdout);
		nap_ms(delay_ms);
		printf("chip after delay: %s\n",
		       fm_alive(&dev) == 1 ? "answering" : "OFF THE BUS");
		fflush(stdout);
	}

	if (probe_after_n > 0) {
		printf("filling first %u banks before probe...\n", probe_after_n);
		fflush(stdout);
		rv = fm_memfill_n_verbose(&dev, probe_after_n);
		if (rv != FM_OK) {
			printf("fill stopped at error (rv=%d), chip: %s\n", rv,
			       fm_alive(&dev) == 1 ? "answering" : "OFF THE BUS");
			return 2;
		}
		printf("fill done, chip: %s -- now probing 0x%06x\n",
		       fm_alive(&dev) == 1 ? "answering" : "OFF THE BUS", probe_base);
		fflush(stdout);
	}

	if (probe_base != 0) {
		/* Word-by-word bisect: find the first word in BASE..BASE+WORDS-1
		 * that takes the chip off the bus. */
		uint32_t word, before = fm_fatal_count(&dev);

		printf("probing 0x%06x for %u words, word by word...\n",
		       probe_base, probe_words);
		fflush(stdout);
		for (word = 0; word < probe_words; word++) {
			rv = fm_mem_fill_paced(&dev, probe_base + word, 1, 0, 0);
			if (!fm_alive(&dev)) {
				uint32_t after = fm_fatal_count(&dev);
				printf("CRASH at word %u (0x%06x), FATAL_COUNT %u -> %u\n",
				       word, probe_base + word, before, after);
				return 2;
			}
			if ((word & 0x3ff) == 0) {
				printf("  word %u ok\n", word);
				fflush(stdout);
			}
		}
		printf("all %u words ok, chip answering\n", probe_words);
		return 0;
	}

	{
		uint32_t before = fm_fatal_count(&dev);

		printf("filling ECC-protected SRAM banks (verbose)...\n");
		rv = fm_memfill_verbose(&dev);

		uint32_t after = fm_fatal_count(&dev);
		printf("result:      %s\n", rv == FM_OK ? "ok" : "CHIP STOPPED ANSWERING");
		printf("FATAL_COUNT: %u -> %u (%s)\n", before, after,
		       after == before ? "clean" : "chip reset during fill");
		printf("chip:        %s\n",
		       fm_alive(&dev) == 1 ? "answering" : "OFF THE BUS");
	}

	return rv == FM_OK ? 0 : 2;
}
