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
	printf("nosd-fm6000: Intel FM6000 at %s, BAR0 %zu MB\n",
	       d->slot, d->bar_bytes >> 20);
	printf("nosd-fm6000: this datapath does not forward yet -- M1. "
	       "`nosaic show caps` reports what it can actually do.\n");
	fflush(stdout);
}

static void usage(void)
{
	fprintf(stderr,
"usage: nosd-fm6000 [--slot ADDR] [--socket PATH] [--boot]\n"
"\n"
"  --wait N  seconds to wait for the chip before failing (default %d).\n"
"            0 fails immediately. The wait exists because the SCD holds the\n"
"            ASIC in reset until the platform HAL releases it, so \"not there\n"
"            yet\" is a normal transient rather than a fault.\n"
"  --boot    run the documented cold-boot sequence at start-up.\n"
"            OFF BY DEFAULT: it writes to the chip, it is incomplete, and a\n"
"            supervisor that restarts this daemon would run it again on a\n"
"            chip in an unknown state. Use fm6000-probe --boot by hand.\n",
		FM_DEFAULT_WAIT_SECS);
}

int main(int argc, char **argv)
{
	struct fm6000 dev;
	const char *slot = NULL, *sockpath = NULL;
	int do_boot = 0, i, reported_offbus = 0, waited = 0;
	int wait_secs = FM_DEFAULT_WAIT_SECS;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--slot") == 0 && i + 1 < argc)
			slot = argv[++i];
		else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc)
			sockpath = argv[++i];
		else if (strcmp(argv[i], "--boot") == 0)
			do_boot = 1;
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
	while (fm_open(&dev, slot) != FM_OK) {
		if (waited >= wait_secs) {
			fprintf(stderr,
"nosd-fm6000: no 8086:155b on the PCI bus after %ds.\n"
"nosd-fm6000: on a 7150S the SCD holds the FM6000 in reset from power-on,\n"
"nosd-fm6000: so this is what an un-released chip looks like. The platform\n"
"nosd-fm6000: HAL releases it; check the SCD (3475:0001) is present.\n",
				waited);
			return 1;
		}
		nap_ms(1000);
		waited++;
	}
	if (waited > 0)
		fprintf(stderr, "nosd-fm6000: chip appeared after %ds\n", waited);
	banner(&dev);

	if (do_boot) {
		struct fm_boot_report rep;
		int rv = fm_boot_cold(&dev, &rep);

		fm_boot_report_print(&rep);
		if (rv == FM_EOFFBUS) {
			fprintf(stderr, "nosd-fm6000: the chip left the bus "
					"during boot. Not continuing.\n");
			fm_close(&dev);
			return 1;
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
		if (!reported_offbus && fm_check_offbus(&dev) == 1) {
			fprintf(stderr,
"nosd-fm6000: ⚠ THE CHIP HAS LEFT THE PCIe BUS.\n"
"nosd-fm6000: config space reads all ones and the link is still up, which\n"
"nosd-fm6000: is what an uncorrectable ECC error on an uninitialised bank\n"
"nosd-fm6000: memory looks like. It will not come back without a reset.\n"
"nosd-fm6000: %llu reads, %llu writes, %llu refused before this.\n",
				dev.reads, dev.writes, dev.refused);
			reported_offbus = 1;
		}
		nap_ms(1000);
	}

	fm_close(&dev);
	return 0;
}
