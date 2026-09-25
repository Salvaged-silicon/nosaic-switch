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
#include "pci.h"
#include "regs.h"
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
