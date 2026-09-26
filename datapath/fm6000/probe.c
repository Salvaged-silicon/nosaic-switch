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
#include "sbus.h"
#include "serdes.h"
#include "bist.h"
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
"  --port-up N           run the SerDes lane enable for front-panel port N\n"
"                        (1-8 only; the rest have no established placement)\n"
"  --bist [config]       configure the memory controllers and run the BIST\n"
"                        march. 'config' stops after the controllers. WRITES,\n"
"                        and unpaced writes here hang the HOST -- see bist.h.\n"
"  --spico N             is the SPICO running? post an interrupt and see\n"
"  --dfe N               run the RX equaliser adaptation for port N\n"
"  --sbus                bring the SerDes bus up and read one lane per EPL\n"
"  --fill WORD COUNT     write zeros into COUNT words from WORD. The bulk\n"
"                        writer on its own, for finding where a bank really\n"
"                        begins and ends. WRITES.\n"
"  --meminit             Table 4-1 step 12: fill the bank memories so their\n"
"                        ECC is valid, then prove they can be read. WRITES.\n"
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
	case FM_EOFFBUS: return "CHIP STOPPED ANSWERING";
	case FM_EUNSAFE: return "refused as unsafe";
	case FM_ENOADDR: return "not attempted";
	case FM_ETIMEOUT: return "TIMED OUT";
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
	  "0x00090003 is 10GBASE-R. ⚠ READING THIS BEFORE INIT KILLS THE CHIP" },
};

static int cmd_survey(struct fm6000 *d)
{
	size_t i;
	int bad = 0;

	if (d->xport == FM_XPORT_LBUS) {
		/* The slot here is the SCD's, not the chip's -- the FM6000 has
		 * no PCI address of its own until it has been configured, and
		 * printing one would be a lie that sends someone looking for a
		 * device that is not supposed to be there yet. */
		printf("reached   via SCD %s BAR1 (local bus)\n", d->slot);
		printf("window    %zu bytes mapped (%zu MB), words 0..0x%x\n",
		       d->bar_bytes, d->bar_bytes >> 20, FM6000_WORD_MAX);
		printf("answering %s\n\n",
		       fm_alive(d) == 1 ? "yes" : "NO -- PIN_STRAP reads 0");
	} else {
		printf("chip     %s  (8086:155b)\n", d->slot);
		printf("BAR0     %zu bytes mapped (%zu MB), words 0..0x%x\n",
		       d->bar_bytes, d->bar_bytes >> 20, FM6000_WORD_MAX);
		printf("on bus   %s\n\n", fm_check_offbus(d) == 1 ? "NO" : "yes");
	}

	if (fm_is_offbus(d)) {
		printf("The chip is not answering. Nothing below would mean\n"
		       "anything, so it is not attempted.\n");
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

/*
 * Table 4-1 step 12, done carefully enough to watch.
 *
 * The order matters and is the whole design: fill each bank FIRST and only
 * then read one back. A read of an uninitialised bank is what takes the chip
 * off the bus, so a "check before we start" would be the very thing we are
 * trying to survive. Writes are safe -- a full 32-bit write stores data and
 * ECC together.
 *
 * Liveness is checked after every bank rather than at the end, so a bank that
 * kills the chip is named instead of leaving three suspects.
 */
static int cmd_meminit(struct fm6000 *d, uint32_t pattern)
{
	uint32_t v = 0;
	int rv, i;

	if (fm_boot_already_done(d) != 1) {
		printf("The chip has not been through the cold boot. Step 12 comes\n"
		       "after the boot controller's commands, not before: run\n"
		       "  fm6000-probe --lbus --boot\n");
		return 1;
	}

	/*
	 * One bank memory, not three. The two addresses this used to fill --
	 * 0x240000 and 0x260000 -- are register blocks, and filling them takes
	 * the chip off the bus 54 and 20 words in respectively. See regs.h.
	 */
	printf("Filling STATS, 0x%06x for %u words with 0x%08x.\n\n",
	       FM6000_BANK_STATS_BASE, FM6000_BANK_STATS_SPAN, pattern);
	printf("  writing ... ");
	fflush(stdout);
	/* A recognisable pattern, not zero: zero is what half the chip reads
	 * anyway, so filling with it makes "our write landed" and "nothing is
	 * there" look identical. */
	rv = fm_mem_fill(d, FM6000_BANK_STATS_BASE, FM6000_BANK_STATS_SPAN,
			 pattern);
	if (rv != FM_OK) {
		printf("%s\n", rvstr(rv));
		return 2;
	}
	printf("filled, chip still answering\n");

	/* Only now is the claim true, and only this path may make it. */
	fm_bank_mark_initialised(d);
	printf("\nDeclared initialised. Reading it back -- this is the read that\n"
	       "would have taken the chip off the bus before the fill.\n\n");

	rv = fm_rd(d, FM6000_BANK_STATS_BASE, &v);
	printf("  0x%06x = ", FM6000_BANK_STATS_BASE);
	if (rv != FM_OK) {
		printf("%s\n", rvstr(rv));
		return 2;
	}
	printf("0x%08x%s\n", v,
	       v == pattern ? "   <-- the pattern we wrote" : "   <-- NOT the pattern");
	if (fm_alive(d) != 1) {
		printf("\nThe chip stopped answering on that read.\n");
		return 2;
	}

	/*
	 * Characterise it here rather than from a second invocation, because a
	 * fresh process has banks_ready clear and the guard -- correctly --
	 * refuses it. Reads of a filled bank belong in the process that filled
	 * it; the alternative is a flag that turns the guard off, which is the
	 * one thing pci.h says not to build.
	 */
	printf("\n  eight words from the base:\n");
	for (i = 0; i < 8; i++) {
		uint32_t w = 0;

		if (fm_rd(d, FM6000_BANK_STATS_BASE + i, &w) != FM_OK)
			break;
		printf("    0x%06x  0x%08x\n", FM6000_BANK_STATS_BASE + i, w);
	}
	printf("\n  the same word four times (does it move on its own?):\n    ");
	for (i = 0; i < 4; i++) {
		uint32_t w = 0;

		if (fm_rd(d, FM6000_BANK_STATS_BASE, &w) != FM_OK)
			break;
		printf("0x%08x ", w);
	}
	printf("\n");

	printf("\nStep 12 done for STATS: the fill made it readable, which is what\n"
	       "the step is for. The other memories are not mapped -- 0x240000 and\n"
	       "0x260000 are register blocks, not banks.\n");
	return 0;
}

/*
 * Prove the SerDes bus works, or say exactly how it does not.
 *
 * The controller's own reserved id answers first because it is the one device
 * §9.4 guarantees exists -- if that does not respond, nothing about a lane's
 * silence means anything.
 */
static int cmd_sbus(struct fm6000 *d)
{
	unsigned epl, rc = 0;
	uint32_t v = 0, cfg = 0;
	int rv, answered = 0;

	if (fm_boot_already_done(d) != 1) {
		printf("The chip has not been booted; the JSS block that holds the\n"
		       "SBus controller is still in soft reset. Run --boot first.\n");
		return 1;
	}

	(void)fm_rd(d, FM6000_SBUS_CFG, &cfg);
	printf("SBUS_CFG was 0x%08x", cfg);
	rv = fm_sbus_start(d);
	(void)fm_rd(d, FM6000_SBUS_CFG, &cfg);
	printf(", now 0x%08x%s\n\n", cfg, rv == FM_OK ? "" : "  (write failed)");

	printf("controller 0xfe reg 0: ");
	rv = fm_sbus_txn(d, FM_SBUS_OP_READ, FM_SBUS_DEV_CONTROLLER, 0, 0, &v, &rc);
	if (rv != FM_OK) {
		printf("%s\n", rvstr(rv));
		return 2;
	}
	printf("rc=%u data=0x%08x\n", rc, v);

	/* One lane per EPL, which is enough to tell "the ring works" from "one
	 * device is quiet" without 96 transactions over a 6 us bus. */
	/* Register 2, not 0. Register 0 reads zero on every device on this
	 * ring, so asking it makes a working bus look dead. */
	printf("\nEPL  sbus  reg2        rc\n");
	for (epl = FM_SBUS_EPL_MIN; epl <= FM_SBUS_EPL_MAX; epl++) {
		uint8_t dev = fm_sbus_epl_base(epl);

		v = 0;
		rv = fm_sbus_txn(d, FM_SBUS_OP_READ, dev, 2, 0, &v, &rc);
		if (rv != FM_OK) {
			printf(" %2u   %3u   %s\n", epl, dev, rvstr(rv));
			continue;
		}
		printf(" %2u   %3u   0x%08x  %u%s\n", epl, dev, v, rc,
		       rc == FM_SBUS_RC_NO_DEVICE ? "  no device" : "");
		if (rc == FM_SBUS_RC_OK)
			answered++;
	}
	printf("\n%d of %d EPL lane-0 SerDes answered (rc=%d)\n",
	       answered, FM_SBUS_EPL_MAX, FM_SBUS_RC_OK);

	/* A negative control, printed every time. A bus that answers everything
	 * is not answering anything, and the only way to know which this is is
	 * to ask for something that cannot be there. */
	rv = fm_sbus_txn(d, FM_SBUS_OP_READ, 200, 2, 0, &v, &rc);
	printf("control: device 200 does not exist -> rc=%u%s\n", rc,
	       rc == FM_SBUS_RC_NO_DEVICE ? "  (as it should)" :
	       "  ⚠ SAME AS A REAL DEVICE -- this bus is not discriminating");
	return fm_alive(d) == 1 ? 0 : 2;
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

	/*
	 * Find out whether somebody has already booted this chip, and if so say
	 * so to the guard.
	 *
	 * Nothing carries over between processes: a fresh fm6000-probe against a
	 * chip that nosd booted minutes ago starts with boot_done clear and
	 * refuses the EPL block as though the chip were cold. That is safe and
	 * wrong -- it reports a hazard that is not there and hides the register
	 * the operator asked for. The chip's own state is the authority.
	 */
	if (fm_boot_already_done(&dev) == 1)
		fm_boot_mark_done(&dev);

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
	} else if (strcmp(argv[i], "--fill") == 0 && i + 2 < argc) {
		uint32_t base = (uint32_t)strtoul(argv[i + 1], NULL, 0);
		uint32_t n = (uint32_t)strtoul(argv[i + 2], NULL, 0);

		printf("filling 0x%06x for %u words... ", base, n);
		fflush(stdout);
		rv = fm_mem_fill(&dev, base, n, 0);
		printf("%s\n", rv == FM_OK ? "chip still answering" : rvstr(rv));
		rc = rv == FM_OK ? 0 : 2;
	} else if (strcmp(argv[i], "--port-up") == 0 && i + 1 < argc) {
		const struct fm_port *p = fm_port_lookup(atoi(argv[i + 1]));
		struct fm_lane_report lrep;
		uint32_t before = 0;

		if (p == NULL) {
			printf("port %s: no established SerDes placement. Only\n"
			       "ports 1-8 are known -- the EPL-to-SBus permutation\n"
			       "for the other EPLs has not been worked out.\n",
			       argv[i + 1]);
			rc = 3;
		} else if (fm_boot_already_done(&dev) != 1) {
			printf("the chip has not been booted; run --boot first\n");
			rc = 1;
		} else if ((rv = fm_sbus_start(&dev)) != FM_OK) {
			printf("could not start the SBus: %s\n", rvstr(rv));
			rc = 2;
		} else {
			(void)fm_lane_status(&dev, p, &before);
			printf("port %d: EPL %d lane %d, SBus device %#02x\n",
			       p->port, p->epl, p->lane, p->dev);
			printf("PORT_STATUS before 0x%08x\n\n", before);
			rv = fm_lane_enable(&dev, p, &lrep);
			fm_lane_report_print(&lrep);
			rc = (lrep.port_status & (1u << 11)) ? 0 : 2;
		}
	} else if (strcmp(argv[i], "--dfe") == 0 && i + 1 < argc) {
		const struct fm_port *p = fm_port_lookup(atoi(argv[i + 1]));
		uint32_t v = 0, before = 0, after = 0;

		if (p == NULL) {
			printf("port %s: no established SerDes placement\n", argv[i + 1]);
			rc = 3;
		} else {
			(void)fm_lane_status(&dev, p, &before);
			(void)fm_sbus_start(&dev);
			rv = fm_lane_dfe(&dev, p, &v);
			(void)fm_lane_status(&dev, p, &after);
			printf("port %d dfe: %s, 0x2b = 0x%02x\n",
			       p->port, rv == FM_OK ? "settled" : rvstr(rv), v);
			printf("PORT_STATUS 0x%08x -> 0x%08x  RxLinkUp(6)=%d\n",
			       before, after, (after >> 6) & 1);
			rc = (after & (1u << 6)) ? 0 : 2;
		}
	} else if (strcmp(argv[i], "--spico") == 0 && i + 1 < argc) {
		const struct fm_port *p = fm_port_lookup(atoi(argv[i + 1]));
		uint32_t v = 0;
		unsigned rc2 = 0, k;

		if (p == NULL) {
			printf("port %s: no established SerDes placement\n", argv[i + 1]);
			rc = 3;
		} else {
			(void)fm_sbus_start(&dev);

			/* The SBus block's own registers. §9.4.1 says SBUS_SPICO
			 * carries the controller's Reset and Enable, and where it
			 * sits is not established -- so print the block and look. */
			printf("SBus block registers:\n");
			for (k = 0; k < 8; k++) {
				uint32_t w = 0;

				if (fm_rd(&dev, FM6000_SBUS_CFG + k, &w) == FM_OK)
					printf("  0x%05x = 0x%08x\n", FM6000_SBUS_CFG + k, w);
			}

			/* Does the SPICO device answer at all? */
			v = 0;
			rv = fm_sbus_txn(&dev, FM_SBUS_OP_READ, FM_SBUS_DEV_SPICO,
					 2, 0, &v, &rc2);
			printf("\nSPICO device 0x%02x reg 2: rc=%u data=0x%08x%s\n",
			       FM_SBUS_DEV_SPICO, rc2, v,
			       rc2 == FM_SBUS_RC_OK ? "" : "   <-- not answering");

			/*
			 * A SPICO interrupt: post a command in the SerDes' own
			 * register 3 and read the answer from register 4. If no
			 * firmware is running, nothing answers and register 4
			 * stays where it was -- which is the difference between
			 * "rx termination was set" and "that step silently did
			 * nothing", and it is the question this command exists
			 * to settle.
			 */
			(void)fm_sbus_read(&dev, (uint8_t)p->dev, 4, &v);
			printf("\nbefore interrupt: SerDes reg 4 = 0x%08x\n", v);
			rv = fm_sbus_write(&dev, (uint8_t)p->dev, 3,
					   (0x2bu << 16) | 1u);   /* rx termination */
			printf("posted rx-termination interrupt: %s\n", rvstr(rv));
			for (k = 0; k < 200; k++) {
				uint32_t w = 0;

				if (fm_sbus_read(&dev, (uint8_t)p->dev, 4, &w) != FM_OK)
					break;
				if (w != v) {
					printf("reg 4 answered 0x%08x after %u polls "
					       "-- SPICO IS RUNNING\n", w, k);
					break;
				}
			}
			if (k >= 200)
				printf("reg 4 never moved in 200 polls -- nothing is "
				       "answering, so every spico_int step in the\n"
				       "vendor sequence is a no-op on this chip\n");
			rc = 0;
		}
	} else if (strcmp(argv[i], "--bist") == 0) {
		struct fm_bist_report brep;
		int cfg_only = (i + 1 < argc && strcmp(argv[i + 1], "config") == 0);

		if (fm_boot_already_done(&dev) != 1) {
			printf("the chip has not been booted; run --boot first\n");
			rc = 1;
		} else {
			printf("⚠ the memory-controller writes are paced because "
			       "unpaced ones hang the HOST, not the chip.\n\n");
			rv = cfg_only ? fm_bist_configure_only(&dev, 0, &brep)
				      : fm_bist_memory_init(&dev, 0, &brep);
			printf("  controllers configured  %s\n",
			       brep.configured ? "yes" : "NO");
			if (!cfg_only) {
				printf("  march completed         %s", brep.marched ? "yes" : "NO");
				if (brep.marched)
					printf(" after %u ms", brep.march_ms);
				printf("\n  BM_ENGINE_STATUS        0x%08x\n", brep.status);
				printf("  result registers set    %u (want 0)\n", brep.defects);
			}
			printf("  chip                    %s\n",
			       fm_alive(&dev) == 1 ? "answering" : "OFF THE BUS");
			printf("\n%s\n", rv == FM_OK ? "ok" : rvstr(rv));
			rc = rv == FM_OK ? 0 : 2;
		}
	} else if (strcmp(argv[i], "--sbus") == 0) {
		rc = cmd_sbus(&dev);
	} else if (strcmp(argv[i], "--meminit") == 0) {
		uint32_t pat = 0xa5a5a5a5;

		if (i + 1 < argc)
			pat = (uint32_t)strtoul(argv[i + 1], NULL, 0);
		rc = cmd_meminit(&dev, pat);
	} else if (strcmp(argv[i], "--boot-nomem") == 0) {
		struct fm_boot_report rep;

		rv = fm_boot_cold_opt(&dev, &rep, 0);
		fm_boot_report_print(&rep);
		rc = (rv == FM_OK) ? 0 : 2;
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
