/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fm6000-probe -- look at the chip without breaking it.
 *
 * The first thing to run on a 7150S, and deliberately a separate binary from
 * the daemon: at the point where somebody needs this, the daemon is very
 * likely the thing that is wrong.
 *
 * Everything here is read-only unless a flag says otherwise, every read goes
 * through the guard in pci.c, and the guard refuses the addresses that are
 * known to kill a cold chip rather than trusting the operator to remember
 * which ones those are at two in the morning.
 *
 * Exit status is meaningful, because this gets run from scripts:
 *   0  chip found and answering
 *   1  chip not found -- most likely still held in reset by the SCD
 *   2  chip found and OFF THE BUS
 *   3  bad usage
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot.h"
#include "pci.h"
#include "regs.h"

static void usage(void)
{
	fprintf(stderr,
"usage: fm6000-probe [--lbus [SCD_ADDR]] [--slot ADDR] [command]\n"
"\n"
"  --lbus [SCD_ADDR]     reach the chip through the SCD's local-bus window\n"
"                        instead of its own BAR0. THE ONLY PATH THAT WORKS\n"
"                        ON A COLD BOARD, because the FM6000 does not\n"
"                        enumerate on PCIe until it has been configured.\n"
"                        SCD_ADDR defaults to searching for 3475:0001.\n"
"\n"
"  (no command)          identify the chip and read what is safe to read\n"
"  --dump WORD COUNT     dump COUNT words from word address WORD\n"
"  --read WORD           read one word\n"
"  --boot                run the documented cold-boot sequence and report\n"
"                        (this WRITES -- see Table 4-1 in the board's\n"
"                         docs/hardware.md before using it)\n"
"\n"
"Word addresses, as in the datasheet and regs.h. Byte offset = word * 4.\n");
}

static const char *rvstr(int rv)
{
	switch (rv) {
	case FM_OK:      return "ok";
	case FM_EOFFBUS: return "CHIP IS OFF THE PCIe BUS";
	case FM_EUNSAFE: return "refused as unsafe";
	case FM_ENOADDR: return "address not established";
	default:         return "failed";
	}
}

/* Registers worth reading on sight, and safe to. Nothing from a bank memory
 * and nothing from the ESCHED hazard: the guard would refuse them anyway, and
 * a default command that trips its own safety net teaches the wrong lesson. */
static const struct {
	uint32_t    word;
	const char *name;
	const char *note;
} survey[] = {
	{ FM6000_BOOT_CTRL,          "BOOT_CTRL",
	  "warm chips have read 0x313 here, cold ones 0x320" },
	{ FM6000_SCAN_CONFIG_DATA_IN, "SCAN_CONFIG_DATA_IN", NULL },
	{ FM6000_SCAN_CHAIN_DATA_IN,  "SCAN_CHAIN_DATA_IN",
	  "Table 4-1 step 5 writes 0xffffffff here" },
	{ FM6000_SWEEPER,            "SWEEPER",
	  "warm 0x0008bb2c, cold 0" },
	{ FM6000_EPL_CFG_B,          "EPL_CFG_B",
	  "0x00090003 is 10GBASE-R; a cold chip reads 0x00080000" },
};

static int cmd_survey(struct fm6000 *d)
{
	size_t i;
	int bad = 0;

	printf("chip     %s  (8086:155b)\n", d->slot);
	printf("BAR0     %zu bytes mapped (%zu MB), words 0..0x%x\n",
	       d->bar_bytes, d->bar_bytes >> 20, FM6000_WORD_MAX);
	printf("on bus   %s\n\n", fm_check_offbus(d) == 1 ? "NO" : "yes");

	if (fm_is_offbus(d)) {
		printf("The chip is not answering config space. Nothing below\n"
		       "would mean anything, so it is not attempted.\n");
		return 2;
	}

	for (i = 0; i < sizeof(survey) / sizeof(survey[0]); i++) {
		uint32_t v = 0;
		int rv = fm_rd(d, survey[i].word, &v);

		if (rv == FM_OK)
			printf("  %-20s 0x%06x = 0x%08x\n",
			       survey[i].name, survey[i].word, v);
		else
			printf("  %-20s 0x%06x : %s\n",
			       survey[i].name, survey[i].word, rvstr(rv));
		if (survey[i].note != NULL)
			printf("  %-20s   %s\n", "", survey[i].note);
		if (rv == FM_EOFFBUS) {
			bad = 2;
			printf("\nSTOPPING: that read took the chip off the bus.\n");
			break;
		}
	}

	printf("\nrefused %llu, read %llu, wrote %llu\n",
	       d->refused, d->reads, d->writes);
	return bad;
}

static int cmd_dump(struct fm6000 *d, uint32_t base, uint32_t count)
{
	uint32_t i;

	printf("%s, from word 0x%06x\n", fm_block_name(base), base);
	for (i = 0; i < count; i++) {
		uint32_t w = base + i, v = 0;
		const char *why = fm_hazard(d, w);
		int rv;

		if (why != NULL) {
			printf("  0x%06x  --------  refused: %s\n", w, why);
			continue;
		}
		rv = fm_rd(d, w, &v);
		if (rv == FM_OK) {
			printf("  0x%06x  %08x\n", w, v);
			continue;
		}
		printf("  0x%06x  --------  %s\n", w, rvstr(rv));
		if (rv == FM_EOFFBUS) {
			printf("\nSTOPPING at word 0x%06x: the chip left the bus.\n"
			       "That word is the one that did it -- write it down.\n", w);
			return 2;
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct fm6000 dev;
	const char *slot = NULL;
	int i, rv, rc, lbus = 0;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--slot") == 0 && i + 1 < argc) {
			slot = argv[++i];
			continue;
		}
		if (strcmp(argv[i], "--lbus") == 0) {
			lbus = 1;
			/* The optional address is the SCD's, not the chip's. */
			if (i + 1 < argc && argv[i + 1][0] != '-')
				slot = argv[++i];
			continue;
		}
		break;
	}

	rv = lbus ? fm_open_lbus(&dev, slot) : fm_open(&dev, slot);
	if (rv != FM_OK) {
		if (lbus)
			fprintf(stderr,
"fm6000-probe: no SCD (3475:0001) found, or its BAR1 would not map.\n"
"\n"
"The local-bus window IS the SCD's BAR1, so without the SCD there is no way\n"
"to reach the FM6000 at all on a cold board.\n");
		else
			fprintf(stderr,
"fm6000-probe: no 8086:155b found on the PCI bus.\n"
"\n"
"On a 7150S that is the EXPECTED state of a board nobody has initialised:\n"
"the FM6000 does not enumerate on PCIe until it has been configured, so it\n"
"is absent here even after its reset has been released. This is not a fault\n"
"and waiting for it will not help.\n"
"\n"
"USE --lbus INSTEAD. The chip's registers are reachable cold through the\n"
"SCD's BAR1, which is how the vendor's own OS brings it up.\n"
"\n"
"    lspci -nn | grep 3475        # the SCD should be here\n"
"    fm6000-probe --lbus\n");
		return 1;
	}

	if (lbus) {
		int alive = fm_alive(&dev);

		printf("local bus: SCD %s BAR1, %zu bytes -- chip is %s\n",
		       dev.slot, dev.bar_bytes,
		       alive == 1 ? "answering" :
		       alive == 0 ? "SILENT (PIN_STRAP reads 0)" : "unreadable");
		if (alive == 0)
			printf("\n"
"A silent chip needs a reset PULSE, not a release: assert bits 1,2,8 in the\n"
"SCD's resetSet (BAR0+0x4000) and then clear them in resetClear (+0x4010).\n"
"Leaving the resets clear from boot is NOT enough -- measured.\n\n");
	}

	if (i >= argc) {
		rc = cmd_survey(&dev);
	} else if (strcmp(argv[i], "--read") == 0 && i + 1 < argc) {
		rc = cmd_dump(&dev, (uint32_t)strtoul(argv[i + 1], NULL, 0), 1);
	} else if (strcmp(argv[i], "--dump") == 0 && i + 2 < argc) {
		rc = cmd_dump(&dev, (uint32_t)strtoul(argv[i + 1], NULL, 0),
			      (uint32_t)strtoul(argv[i + 2], NULL, 0));
	} else if (strcmp(argv[i], "--boot") == 0) {
		struct fm_boot_report rep;

		printf("Running Intel 331496-002 Table 4-1, in order.\n"
		       "Steps whose register address is not established refuse\n"
		       "rather than guess -- a wrong address does not fail, it\n"
		       "writes somewhere else.\n\n");
		rv = fm_boot_cold(&dev, &rep);
		fm_boot_report_print(&rep);
		rc = (rv == FM_OK) ? 0 : (rv == FM_EOFFBUS ? 2 : 0);
	} else {
		usage();
		rc = 3;
	}

	fm_close(&dev);
	return rc;
}
