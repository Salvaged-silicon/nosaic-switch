/* SPDX-License-Identifier: Apache-2.0 */
/*
 * nosd-fm6000 -- the datapath daemon for the Intel FM6000 "Alta".
 *
 * SPDX note aside, the first thing to say about this file is what it does NOT
 * do: it does not forward packets, it does not bring ports up, and it does not
 * program a forwarding table. The 7150S-52 port is at M1. Saying so here, in
 * the daemon's own banner and in the capabilities it reports, is deliberate --
 * a datapath that claims more than it does produces a switch that accepts
 * configuration and silently drops it on the floor.
 *
 * What it does today:
 *   - finds the chip, maps BAR0, and says whether it is really there
 *   - serves the switch-api socket, so the CLI works against this board and
 *     reports the truth about it
 *   - watches for the chip leaving the PCIe bus, and says so once, loudly
 *
 * That last one is not a placeholder for real work. On this silicon an
 * illegal access takes the endpoint off the bus while the link stays up, and
 * the host then sees stalls and RCU warnings somewhere unrelated, minutes
 * later, naming nothing. Something that watches for it and reports it at the
 * moment it happens is worth more than the next three features.
 *
 * There is no SDK under this. Every register this daemon will ever touch goes
 * through pci.c, and every address it uses is in regs.h with a note saying
 * where the address came from.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "boot.h"
#include "cmrest.h"
#include "cmwm.h"
#include "memfill.h"
#include "parser.h"
#include "pci.h"
#include "regs.h"
#include "saf.h"
#include "sock.h"

/*
 * How long to wait for the chip before failing.
 *
 * Long enough that the platform HAL releasing the ASIC from SCD reset is never
 * a race, short enough that a genuinely broken image fails its A/B trial while
 * somebody is still watching.
 */
#define FM_DEFAULT_WAIT_SECS 30

static volatile int running = 1;

/*
 * Pipeline init: the blocks that boot_cold() leaves zeroed and that the
 * forwarding path needs before frames can move.
 *
 * These are idempotent -- the same end state on every call -- so running them
 * on a chip that has already been through this once (e.g. nosd restarted) is
 * harmless. A nosd restart that skipped this and left the pipeline unconfigured
 * would be worse than one that re-ran 400 writes.
 */
static void pipeline_init(struct fm6000 *d)
{
	struct fm_cmwm_report cmwm;
	uint32_t fc_before, fc_after;
	unsigned n;
	int rv;

	fc_before = fm_fatal_count(d);
	fprintf(stderr, "nosd-fm6000: pipeline_init entry: alive=%d fc=%u\n",
		fm_alive(d), fc_before);

	/*
	 * Probe SAF word 0 before writing anything.
	 *
	 * If BIST covered SAF, this read succeeds and returns a non-zero
	 * march residue or 0x00000000 (the march zeros on completion). If
	 * BIST did NOT cover SAF, the read itself triggers an ECC RMW on
	 * uninitialized SRAM, which causes a soft self-reset -- and this
	 * log line never appears, nosd restarts, and we know what to do next.
	 *
	 * The CRM engine times out for SAF (measured), meaning SAF is not on
	 * the CRM's internal memory bus. Attempting CRM pre-init before BIST
	 * coverage is confirmed costs two seconds and may itself cause a soft
	 * reset if the CRM address walk hits uninitialized ECC on the way.
	 * Skip it: if BIST covered SAF, ECC is already valid and the write
	 * loop works directly; if BIST did not, we need a different approach
	 * anyway and the CRM can't provide it.
	 */
	{
		uint32_t saf_probe = 0xdeadbeef;
		int probe_rv = fm_rd(d, 0x0a0000u, &saf_probe);
		fc_after = fm_fatal_count(d);
		fprintf(stderr, "nosd-fm6000: SAF probe: rv=%d val=0x%08x "
				"resets=%u\n", probe_rv, saf_probe,
				fc_after > fc_before ? fc_after - fc_before : 0);
		fc_before = fc_after;
	}

	rv = fm_saf_init(d, &n);
	fc_after = fm_fatal_count(d);
	if (rv != FM_OK)
		fprintf(stderr, "nosd-fm6000: SAF init failed (%d); resets during: %u\n",
			rv, fc_after > fc_before ? fc_after - fc_before : 0);
	else
		fprintf(stderr, "nosd-fm6000: SAF: %u writes ok; resets during: %u\n",
			n, fc_after > fc_before ? fc_after - fc_before : 0);
	fc_before = fc_after;

	/*
	 * Pre-initialise parser SRAM ECC with a zero-fill burst before
	 * writing the actual per-port seeds. The step-10 fill writes these
	 * banks early in the sequence; the ~235 resets from later banks clear
	 * all SRAM globally (soft self-resets preserve config registers, not
	 * SRAM); BIST does not cover these parser banks. After step 10+11 the
	 * ECC at 0x108200 is therefore uninitialised.
	 *
	 * A read from uninitialised ECC causes a HARD chip fault (needs SCD
	 * reset pulse -- measured 2026-10-13). A write from the management
	 * path also faults because the ECC RMW first reads the cluster and
	 * hits the bad ECC. The burst path (fm_mem_fill, check_writes off)
	 * matches how the step-10 fill initialised these banks and is the
	 * safe way to establish valid ECC before per-word writes follow.
	 *
	 * TICK_CFG=2 must be active before any write to parser SRAM; without
	 * it writes cause a hard chip fault (FATAL_COUNT 4→0) rather than the
	 * recoverable ECC soft-reset. It is written in boot.c before step 10's
	 * fill and must survive through BIST and esched init. Re-assert it
	 * here as a safeguard, and read it back first so the log can show
	 * whether the boot left it set (if 0x2 appears it was fine; if 0x0
	 * appears the fault was TICK_CFG dropping and this write fixes it).
	 */
	{
		uint32_t tick_cfg_val = 0;
		int pre_rv;

		(void)fm_rd(d, FM6000_SSCHED_TICK_CFG, &tick_cfg_val);
		/*
		 * TICK_CFG is 0x0 here: esched_init clears it (measured 2026-10-13)
		 * and writing 0x00f010 after the ring is running + ESCHED configured
		 * kills the chip (fatal write when ring is live, measured).
		 *
		 * Do NOT write TICK_CFG here. The ring clock domain is already running
		 * (kept by the FM6000 hardware once started); TICK_CFG=0 at this point
		 * is the hardware clearing the one-shot trigger after use. The parser
		 * SRAM pre-fill below goes through the burst path (fm_mem_fill,
		 * check_writes=0), which does not trigger ECC read-modify-write, so it
		 * initialises ECC without needing TICK_CFG=2 as an arming condition.
		 */
		fprintf(stderr, "nosd-fm6000: TICK_CFG at pipeline_init: 0x%x alive=%d\n",
			tick_cfg_val, fm_alive(d));

		pre_rv = fm_mem_fill(d, 0x100200u, 14336u, 0u);
		fprintf(stderr, "nosd-fm6000: parser pre-fill 0x100200: %s\n",
			pre_rv == FM_OK ? "ok" : "failed");
		fc_after = fm_fatal_count(d);
		fprintf(stderr, "nosd-fm6000: parser pre-fill resets after: %u alive=%d\n",
			fc_after > fc_before ? fc_after - fc_before : 0,
			fm_alive(d));
		fm_clear_offbus(d);
		fc_before = fc_after;

		pre_rv = fm_mem_fill(d, 0x108000u, 152u, 0u);
		fprintf(stderr, "nosd-fm6000: parser pre-fill 0x108000: %s\n",
			pre_rv == FM_OK ? "ok" : "failed");

		pre_rv = fm_mem_fill(d, 0x108200u, 304u, 0u);
		fprintf(stderr, "nosd-fm6000: parser pre-fill 0x108200: %s alive=%d\n",
			pre_rv == FM_OK ? "ok" : "failed", fm_alive(d));
		fc_after = fm_fatal_count(d);
		fprintf(stderr, "nosd-fm6000: parser pre-fill resets: %u\n",
			fc_after > fc_before ? fc_after - fc_before : 0);
		fc_before = fc_after;
		fm_clear_offbus(d);
	}

	rv = fm_parser_fields_init(d, &n);
	fc_after = fm_fatal_count(d);
	if (rv == FM_EOFFBUS) {
		fprintf(stderr,
			"nosd-fm6000: parser fields init: ECC pass (resets: %u); retrying\n",
			fc_after > fc_before ? fc_after - fc_before : 0);
		fc_before = fc_after;
		fm_clear_offbus(d);
		rv = fm_parser_fields_init(d, &n);
		fc_after = fm_fatal_count(d);
	}
	if (rv != FM_OK)
		fprintf(stderr, "nosd-fm6000: parser fields init failed (%d); resets during: %u\n",
			rv, fc_after > fc_before ? fc_after - fc_before : 0);
	else
		fprintf(stderr, "nosd-fm6000: parser fields: %u writes ok; resets during: %u\n",
			n, fc_after > fc_before ? fc_after - fc_before : 0);
	fc_before = fc_after;

	rv = fm_cmwm_init(d, &cmwm);
	if (rv != FM_OK)
		fprintf(stderr, "nosd-fm6000: CM watermarks init failed (%d)\n", rv);
	else
		fprintf(stderr, "nosd-fm6000: CM watermarks: %u writes, "
				"%u/%u tables verified%s\n",
				cmwm.written, cmwm.verified, cmwm.readable,
				cmwm.first_bad ? " (MISMATCH)" : "");

	rv = fm_cmrest_init(d, &n);
	if (rv != FM_OK)
		fprintf(stderr, "nosd-fm6000: CM rest init failed (%d)\n", rv);
	else
		fprintf(stderr, "nosd-fm6000: CM rest: %u writes ok\n", n);
}

static void nap_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static void banner(const struct fm6000 *d)
{
	if (d->xport == FM_XPORT_LBUS)
		printf("nosd-fm6000: Intel FM6000 via SCD %s BAR1, %zu MB window\n",
		       d->slot, d->bar_bytes >> 20);
	else
		printf("nosd-fm6000: Intel FM6000 at %s, BAR0 %zu MB\n",
		       d->slot, d->bar_bytes >> 20);
	printf("nosd-fm6000: this datapath does not forward yet. "
	       "`nosaic show caps` reports what it can actually do.\n");
	fflush(stdout);
}

static void usage(void)
{
	fprintf(stderr,
"usage: nosd-fm6000 [--slot ADDR] [--scd ADDR] [--socket PATH] [--no-boot]\n"
"\n"
"  --slot A  the FM6000's PCI address. Default: search for 8086:155b.\n"
"  --scd A   the SCD's PCI address, for the local-bus window. Default:\n"
"            search for 3475:0001. The chip is reached over the SCD when it\n"
"            is not on the PCI bus, which on a cold 7150S is always.\n"
"  --wait N  seconds to wait for the chip before failing (default %d).\n"
"            0 fails immediately. The wait exists because the SCD holds the\n"
"            ASIC in reset until the platform HAL releases it, so \"not there\n"
"            yet\" is a normal transient rather than a fault.\n"
"  --no-boot do NOT run the documented cold-boot sequence at start-up.\n"
"            The sequence runs by default, because on a cold board nothing\n"
"            else runs it and an image that boots to a prompt with a dead\n"
"            ASIC is not a shippable image. It is skipped automatically on a\n"
"            chip that has already been booted, so restarting this daemon\n"
"            does not disturb a running dataplane. Use this to hold a chip\n"
"            in its cold state for debugging.\n",
		FM_DEFAULT_WAIT_SECS);
}

int main(int argc, char **argv)
{
	struct fm6000 dev;
	const char *slot = NULL, *sockpath = NULL;
	int no_boot = 0, i, reported_offbus = 0, waited = 0;
	const char *scd_slot = NULL;
	int wait_secs = FM_DEFAULT_WAIT_SECS;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--slot") == 0 && i + 1 < argc)
			slot = argv[++i];
		else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc)
			sockpath = argv[++i];
		else if (strcmp(argv[i], "--no-boot") == 0)
			no_boot = 1;
		else if (strcmp(argv[i], "--scd") == 0 && i + 1 < argc)
			scd_slot = argv[++i];
		else if (strcmp(argv[i], "--wait") == 0 && i + 1 < argc)
			wait_secs = atoi(argv[++i]);
		else {
			usage();
			return 2;
		}
	}

	/*
	 * Wait for the chip, up to a bound, and then give up.
	 *
	 * Both halves of that matter and they pull in opposite directions.
	 *
	 * GIVING UP IS REQUIRED. A datapath that cannot find its chip is exactly
	 * the condition A/B trial-confirm exists to catch: the supervisor sees
	 * nosd fail, the trial boot does not confirm itself healthy, and the
	 * switch rolls back to the slot that worked. A daemon that stayed up
	 * reporting "no chip" would defeat that by looking alive, and a bad
	 * image would be committed.
	 *
	 * WAITING IS ALSO REQUIRED, and this board is why. The FM6000 is held in
	 * reset by the SCD from power-on and does not appear on the PCI bus until
	 * the platform HAL releases it -- so "no chip yet" is a normal transient
	 * at start-up, not a fault, and a daemon that exits on the first look is
	 * racing something that is doing its job.
	 *
	 * Exiting instantly did both wrong at once. Measured on the switch: with
	 * the chip held in reset, `restart: always` respawned this several times
	 * a second, which printed a line each time and made the console unusable
	 * for the very debugging the restarts were evidence for. The exit was
	 * right and the cadence was not.
	 *
	 * So the wait is what bounds the restart rate, rather than a sleep bolted
	 * on before exit: while the chip is legitimately on its way this is not a
	 * failure at all, and once the deadline passes it is one.
	 */
	if (wait_secs > 0)
		fprintf(stderr, "nosd-fm6000: looking for the chip, up to %ds\n",
			wait_secs);
	while (fm_open_auto(&dev, slot, scd_slot) != FM_OK) {
		if (waited >= wait_secs) {
			fprintf(stderr,
"nosd-fm6000: could not reach the chip after %ds, either way.\n"
"nosd-fm6000: no 8086:155b on the PCI bus -- which on a 7150S is normal,\n"
"nosd-fm6000: the FM6000 does not enumerate until it has been configured --\n"
"nosd-fm6000: and no SCD (3475:0001) whose BAR1 would give us the local bus.\n"
"nosd-fm6000: Without one of those two there is no way to address the chip.\n",
				waited);
			return 1;
		}
		nap_ms(1000);
		waited++;
	}
	if (waited > 0)
		fprintf(stderr, "nosd-fm6000: chip appeared after %ds\n", waited);
	if (dev.xport == FM_XPORT_LBUS)
		fprintf(stderr, "nosd-fm6000: reaching the chip over the SCD's "
				"local bus at %s\n", dev.slot);
	banner(&dev);

	/*
	 * Bring the chip up, unless it is already up or we were told not to.
	 *
	 * ⚠ THIS IS THE DEFAULT, AND IT HAS TO BE. On a cold board nothing else
	 * runs Table 4-1: the platform HAL releases the reset and stops there,
	 * because what to do with a released chip is the datapath's business. A
	 * daemon that waited to be asked would ship an image that boots to a
	 * prompt with a dead ASIC and no indication why.
	 *
	 * ⚠ AND IT HAS TO BE CONDITIONAL. `restart: always` means this runs
	 * again every time nosd is restarted, and putting a forwarding chip back
	 * through the sequence would drop the dataplane on what should be a
	 * no-op. fm_boot_already_done() tests the end state rather than
	 * remembering anything, so it is still right after a crash, and right
	 * for a chip somebody else booted.
	 */
	if (!no_boot) {
		int done = fm_boot_already_done(&dev);

		if (done < 0) {
			fprintf(stderr, "nosd-fm6000: cannot read the chip to find "
					"out whether it has been booted; not writing to it\n");
			fm_close(&dev);
			return 1;
		}
		if (done == 1) {
			fprintf(stderr, "nosd-fm6000: the chip is already booted; "
					"leaving it alone\n");
			fm_boot_mark_done(&dev);
		} else {
			struct fm_boot_report rep;
			int rv;

			fprintf(stderr, "nosd-fm6000: cold chip -- running the "
					"documented boot sequence\n");
			rv = fm_boot_cold(&dev, &rep);
			fm_boot_report_print(&rep);
			if (rv == FM_EOFFBUS) {
				fprintf(stderr, "nosd-fm6000: the chip left the bus "
						"during boot. Not continuing.\n");
				fm_close(&dev);
				return 1;
			}
			if (rv != FM_OK) {
				fprintf(stderr, "nosd-fm6000: the boot sequence did not "
						"complete. Not continuing.\n");
				fm_close(&dev);
				return 1;
			}
		}
		pipeline_init(&dev);
	}

	if (fm_sock_start(&dev, sockpath) != 0)
		fprintf(stderr, "nosd-fm6000: could not serve %s -- carrying on "
				"without it\n",
			sockpath ? sockpath : NOSAIC_QUERY_SOCKET);

	/*
	 * The watch loop. One second is frequent enough that an operator
	 * watching the log sees the failure while they still remember what they
	 * did, and slow enough to cost nothing.
	 *
	 * It reports the transition ONCE. A chip that has gone off the bus does
	 * not come back without a reset, so repeating the message every second
	 * would bury the one line that says when it happened -- which is the
	 * only piece of information the message carries.
	 */
	while (running) {
		if (!reported_offbus && fm_alive(&dev) == 0) {
			fprintf(stderr,
"nosd-fm6000: ⚠ THE CHIP HAS STOPPED ANSWERING.\n"
"nosd-fm6000: this is what an uncorrectable ECC error on an uninitialised\n"
"nosd-fm6000: bank memory looks like. Over PCIe the endpoint reads all ones;\n"
"nosd-fm6000: over the local bus it reads all zeros. Either way it will not\n"
"nosd-fm6000: come back without a reset PULSE from the SCD.\n"
"nosd-fm6000: %llu reads, %llu writes, %llu refused before this.\n",
				dev.reads, dev.writes, dev.refused);
			reported_offbus = 1;
		}
		nap_ms(1000);
	}

	fm_close(&dev);
	return 0;
}
