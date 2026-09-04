/* SPDX-License-Identifier: Apache-2.0 */
/*
 * nosaic, for boards the Go CLI cannot reach.
 *
 * The gc toolchain has ppc64 and ppc64le and has never had 32-bit big-endian
 * PowerPC, so on a board like the AS5610-52X there is no `nosaic` at all unless
 * something else provides one. This provides the part that has to run on a
 * switch; the part that only runs on a build host -- building images, building
 * packages, scaffolding boards -- stays in Go, where it belongs and where the
 * host architecture is x86_64 by design.
 *
 * The commands, their arguments and their output are meant to match the Go CLI
 * exactly. Two implementations of one interface drift, and the defence is that
 * the surface here is small and shaped like output, so a golden-output test can
 * hold them together.
 *
 * Commands this board cannot answer are refused by name rather than being
 * absent. "this board has no watchdog" and "this build has no such command" are
 * different facts and an operator needs to be able to tell them apart.
 */

#include "hal.h"
#include "config.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char usage[] =
"usage: nosaic <command>\n"
"\n"
"  config show [pattern]   every setting, and whether it came from the image\n"
"                          or from this switch\n"
"  config get <name>       one setting's effective value\n"
"  config set <name> <val> write it into this switch's own configuration\n"
"  config unset <name>     remove it; the image's default applies again\n"
"  config files            which files are layered, in order\n"
"\n"
"usage: nosaic platform <command>\n"
"\n"
"  status               what the board reports about itself\n"
"  ledwalk [seconds]    light the status-LED bits one at a time so the panel\n"
"                       can be mapped; restores the registers when done\n"
"  thermal [--once] [--interval N]\n"
"                       run the cooling loop: fans track the hottest sensor,\n"
"                       fail to full cooling, and are left at full on exit\n"
"\n"
"Commands this board does not support are refused by name.\n";

static const struct nosaic_hal *hal;

/* ------------------------------------------------------------------ status */

static int cmd_status(void)
{
	struct nosaic_temp temps[32];
	struct nosaic_psu psus[4];
	char buf[128];
	int n, i, pct;

	if (hal->identify != NULL && hal->identify(buf, sizeof(buf)) == 0)
		printf("board      %s\n", buf);

	pct = hal->fan_get != NULL ? hal->fan_get() : -1;
	if (pct >= 0)
		printf("fans       %d%% (floor %d%%)\n", pct,
		       hal->fan_floor != NULL ? hal->fan_floor() : 0);

	n = hal->psus != NULL ? hal->psus(psus, 4) : 0;
	for (i = 0; i < n; i++)
		printf("psu%d       %s, %s (%s)\n", psus[i].index,
		       psus[i].fitted ? "fitted" : "absent",
		       psus[i].powered ? "ok" : "no-power", psus[i].raw);

	if (hal->leds != NULL && hal->leds(buf, sizeof(buf)) == 0)
		printf("leds       %s\n", buf);

	n = hal->temps != NULL ? hal->temps(temps, 32) : 0;
	for (i = 0; i < n; i++) {
		printf("temp       %-8s %dC", temps[i].name,
		       temps[i].milli_c / 1000);
		if (temps[i].crit_milli_c > 0)
			printf(" (crit %dC)", temps[i].crit_milli_c / 1000);
		printf("\n");
	}
	return 0;
}

/* ----------------------------------------------------------------- thermal */

/*
 * The curve, from the board's own description.
 *
 * board.yml is YAML and this does not parse YAML: it looks for the two keys it
 * needs. That is enough for a file this project generates and validates
 * elsewhere, and the defaults below are the ones the AS5610 ships, so a board
 * that says nothing still gets a sane curve rather than an error.
 */
static int min_c = 40, max_c = 60;

static void read_curve(void)
{
	FILE *f = fopen("/etc/nosaic/board.yml", "r");
	char line[256];

	if (f == NULL)
		return;
	while (fgets(line, sizeof(line), f) != NULL) {
		int v;

		if (sscanf(line, " min_c: %d", &v) == 1)
			min_c = v;
		else if (sscanf(line, " max_c: %d", &v) == 1)
			max_c = v;
	}
	fclose(f);
	if (max_c <= min_c)
		max_c = min_c + 1;
}

static volatile sig_atomic_t stopping;

static void on_signal(int sig)
{
	(void)sig;
	stopping = 1;
}

static int hottest_c(void)
{
	struct nosaic_temp t[32];
	int n = hal->temps != NULL ? hal->temps(t, 32) : 0;
	int i, hot = -1;

	for (i = 0; i < n; i++) {
		if (t[i].milli_c / 1000 > hot)
			hot = t[i].milli_c / 1000;
	}
	return hot;
}

static int cmd_thermal(int argc, char **argv)
{
	int once = 0, interval = 5, i, cur = -1;

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--once") == 0)
			once = 1;
		else if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc)
			interval = atoi(argv[++i]);
		else {
			fprintf(stderr, "nosaic: thermal: unknown argument %s\n",
				argv[i]);
			return 2;
		}
	}
	if (interval < 1)
		interval = 1;

	if (hal->fan_set == NULL) {
		fprintf(stderr, "nosaic: this board cannot control its fans\n");
		return 1;
	}
	read_curve();

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	printf("thermal    curve %d-%dC, every %ds, floor %d%%\n",
	       min_c, max_c, interval, hal->fan_floor());
	fflush(stdout);

	while (!stopping) {
		int hot = hottest_c(), want;

		if (hot < 0) {
			/* Not knowing the temperature has one safe answer, and
			 * it is loud enough that somebody notices. */
			fprintf(stderr, "nosaic: no temperature available; "
				"going to full cooling\n");
			hal->fan_set(100);
			cur = 100;
		} else {
			if (hot <= min_c)
				want = hal->fan_floor();
			else if (hot >= max_c)
				want = 100;
			else
				want = hal->fan_floor() +
				       (hot - min_c) * (100 - hal->fan_floor()) /
				       (max_c - min_c);

			/* Rising is immediate; falling is limited, because
			 * reacting instantly downward makes the fans hunt around
			 * a threshold while reacting slowly upward is a thermal
			 * risk. They are not symmetric problems. */
			if (cur >= 0 && want < cur)
				want = cur - 3 < want ? cur - 3 : want;

			if (want != cur) {
				if (hal->fan_set(want) != 0)
					fprintf(stderr, "nosaic: fan duty %d%% "
						"did not land\n", want);
				else
					printf("thermal    %dC -> %d%%\n", hot, want);
				fflush(stdout);
				cur = want;
			}
		}
		if (once)
			break;
		sleep((unsigned)interval);
	}

	/*
	 * Leave the fans at full on the way out.
	 *
	 * Whatever stopped this loop, nothing is regulating the box after it, so
	 * the last thing it does is set the safe extreme. Leaving them wherever
	 * the curve happened to be is a switch cooling itself by luck.
	 */
	if (!once) {
		hal->fan_set(100);
		printf("thermal    stopping; fans left at full\n");
	}
	return 0;
}

/* ---------------------------------------------------------------- dispatch */

static int unsupported(const char *what)
{
	fprintf(stderr, "nosaic: %s is not supported on this board\n", what);
	return 1;
}


/* ------------------------------------------------------------------ config */

/*
 * The configuration, and where each setting came from.
 *
 * Saying which file won is the point. A switch behaving unlike its neighbour
 * is nearly always one overridden setting, and "which of these two layers is
 * this coming from" is the question that answers it -- without it an operator
 * has to know the layering rules and read both directories by hand.
 */
static int cmd_config(int argc, char **argv)
{
	char err[256];
	int i, n = 0;

	nosaic_config_load();

	if (argc < 3 || strcmp(argv[2], "show") == 0) {
		const char *pat = argc > 3 ? argv[3] : NULL;

		for (i = 0; i < nosaic_config_count(); i++) {
			const char *name = nosaic_config_name(i);

			if (pat != NULL && strstr(name, pat) == NULL)
				continue;
			printf("%-34s %-24s %s\n", name, nosaic_config_value(i),
			       nosaic_config_is_site(i) ? "switch" : "image");
			n++;
		}
		if (n == 0)
			printf("no settings%s\n", pat ? " match" : "");
		else
			printf("\n%d setting(s). "
			       "\"switch\" comes from %s and survives an upgrade; "
			       "\"image\" is the default this image shipped.\n",
			       n, NOSAIC_CONFIG_SITE_DIR);
		return 0;
	}
	if (strcmp(argv[2], "files") == 0) {
		printf("%s        the image's defaults, replaced by every upgrade\n",
		       NOSAIC_CONFIG_IMAGE_DIR);
		printf("%s   this switch's own, and it wins\n", NOSAIC_CONFIG_SITE_DIR);
		printf("\n`config set` writes %s/%s only.\n",
		       NOSAIC_CONFIG_SITE_DIR, NOSAIC_CONFIG_SITE_FILE);
		return 0;
	}
	if (strcmp(argv[2], "get") == 0) {
		const char *v;

		if (argc < 4) {
			fprintf(stderr, "usage: nosaic config get <name>\n");
			return 2;
		}
		v = nosaic_config_get(argv[3]);
		if (v == NULL) {
			fprintf(stderr, "nosaic: %s is not set\n", argv[3]);
			return 1;
		}
		printf("%s\n", v);
		return 0;
	}
	if (strcmp(argv[2], "set") == 0) {
		if (argc < 5) {
			fprintf(stderr, "usage: nosaic config set <name> <value>\n");
			return 2;
		}
		if (nosaic_config_set(argv[3], argv[4], err, sizeof(err)) != 0) {
			fprintf(stderr, "nosaic: %s\n", err);
			return 1;
		}
		printf("%s=%s written to %s/%s\n", argv[3], argv[4],
		       NOSAIC_CONFIG_SITE_DIR, NOSAIC_CONFIG_SITE_FILE);
		printf("It takes effect when the thing that reads it restarts.\n");
		return 0;
	}
	if (strcmp(argv[2], "unset") == 0) {
		if (argc < 4) {
			fprintf(stderr, "usage: nosaic config unset <name>\n");
			return 2;
		}
		if (nosaic_config_set(argv[3], NULL, err, sizeof(err)) != 0) {
			fprintf(stderr, "nosaic: %s\n", err);
			return 1;
		}
		printf("%s removed; the image's default applies again\n", argv[3]);
		return 0;
	}
	fprintf(stderr, "nosaic: unknown config command %s\n", argv[2]);
	return 2;
}

int main(int argc, char **argv)
{
	if (argc < 2 || strcmp(argv[1], "--help") == 0) {
		fputs(usage, stdout);
		return argc < 2 ? 2 : 0;
	}
	if (strcmp(argv[1], "version") == 0) {
		printf("nosaic (C, for architectures without a Go target)\n");
		return 0;
	}
	if (strcmp(argv[1], "config") == 0)
		return cmd_config(argc, argv);
	if (strcmp(argv[1], "platform") != 0) {
		fprintf(stderr, "nosaic: this build provides `platform` only; "
			"the rest of the CLI runs on the build host\n");
		return 2;
	}
	if (argc < 3) {
		fputs(usage, stdout);
		return 2;
	}

	hal = nosaic_hal_find();
	if (hal == NULL) {
		fprintf(stderr, "nosaic: no board controller answered, so there "
			"is nothing to report\n");
		return 1;
	}

	if (strcmp(argv[2], "status") == 0)
		return cmd_status();
	if (strcmp(argv[2], "ledwalk") == 0) {
		int hold = argc > 3 ? atoi(argv[3]) : 3;

		if (hal->led_walk == NULL)
			return unsupported("this board has no status-LED walk");
		return hal->led_walk(hold) == 0 ? 0 : 1;
	}
	if (strcmp(argv[2], "thermal") == 0)
		return cmd_thermal(argc - 3, argv + 3);

	/* Named refusals for the rest of the Go CLI's platform surface, so an
	 * operator can tell "this board cannot" from "this build has not". */
	if (strcmp(argv[2], "release-asic") == 0)
		return unsupported("release-asic: this board's switch chip is on "
				   "PCI and out of reset before Linux starts");
	if (strcmp(argv[2], "watchdog") == 0)
		return unsupported("watchdog");
	if (strcmp(argv[2], "transceivers") == 0 || strcmp(argv[2], "tx") == 0)
		return unsupported("transceiver control: this board's cages are "
				   "driven by the front-panel init script");
	if (strcmp(argv[2], "asic") == 0 || strcmp(argv[2], "schan") == 0)
		return unsupported("chip access: use tdp-probe on this board");

	fprintf(stderr, "nosaic: unknown platform command %s\n", argv[2]);
	return 2;
}
