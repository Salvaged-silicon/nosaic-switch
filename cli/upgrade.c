/* SPDX-License-Identifier: Apache-2.0 */
/*
 * A/B slots, from the switch itself.
 *
 * The boot pointer is three small files, deliberately: the state a switch will
 * fall back on should be readable and repairable with the tools present in an
 * initramfs. This reads and writes exactly the files internal/upgrade and the
 * initramfs do, so the Go CLI, this one and the boot script cannot disagree
 * about what is committed.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "query.h"
#include "upgrade.h"

/* Where the pointer lives, resolved the way the initramfs resolves it: a board
 * with a boot partition keeps it there, and one whose bootloader owns the whole
 * disk has none, so it goes on the data filesystem instead. */
static const char *state_dir(void)
{
	static const char *candidates[] = { "/mnt/boot/boot", "/mnt/data/boot" };
	struct stat st;
	size_t i;

	for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++)
		if (stat(candidates[i], &st) == 0 && S_ISDIR(st.st_mode))
			return candidates[i];
	return NULL;
}

static int read_state(const char *dir, const char *name, char *out, size_t len)
{
	char path[256];
	FILE *f;
	size_t n;

	*out = '\0';
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	if ((f = fopen(path, "r")) == NULL)
		return -1;
	if (fgets(out, (int)len, f) == NULL) {
		fclose(f);
		return -1;
	}
	fclose(f);
	n = strlen(out);
	while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
		out[--n] = '\0';
	return 0;
}

static int write_state(const char *dir, const char *name, const char *value)
{
	char path[256];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	if (value == NULL) {
		if (unlink(path) != 0 && errno != ENOENT)
			return -1;
		return 0;
	}
	if ((f = fopen(path, "w")) == NULL)
		return -1;
	fprintf(f, "%s\n", value);
	if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
		fclose(f);
		return -1;
	}
	return fclose(f) == 0 ? 0 : -1;
}

static void active_trial(const char *dir, char *active, size_t alen,
			 char *trial, size_t tlen, int *tries)
{
	char n[32];

	if (read_state(dir, "active", active, alen) != 0 || active[0] == '\0')
		snprintf(active, alen, "a");
	if (read_state(dir, "trial", trial, tlen) != 0)
		*trial = '\0';
	*tries = 0;
	if (read_state(dir, "tries", n, sizeof(n)) == 0)
		*tries = atoi(n);
}

static int cmd_status(const char *dir)
{
	char active[16], trial[16];
	int tries;

	active_trial(dir, active, sizeof(active), trial, sizeof(trial), &tries);
	printf("active  %s\n", active);
	if (trial[0] != '\0') {
		printf("trial   %s (attempt %d)\n", trial, tries);
		printf("        not yet committed: it rolls back unless it confirms itself healthy\n");
	} else {
		printf("trial   none\n");
	}
	return 0;
}

static int commit(const char *dir, char *slot, size_t len)
{
	char active[16], trial[16];
	int tries;

	active_trial(dir, active, sizeof(active), trial, sizeof(trial), &tries);
	if (trial[0] == '\0') {
		fprintf(stderr, "nosaic: no trial is in progress: slot %s is already "
			"the committed one\n", active);
		return 1;
	}
	snprintf(slot, len, "%s", trial);
	/* active first, then clear the trial. In the other order a power cut
	 * between the two writes leaves neither, and the switch quietly boots
	 * the old slot. */
	if (write_state(dir, "active", trial) != 0) {
		fprintf(stderr, "nosaic: cannot write the boot pointer: %s\n", strerror(errno));
		return 1;
	}
	write_state(dir, "trial", NULL);
	write_state(dir, "tries", NULL);
	return 0;
}

/* Is this image good enough to keep?
 *
 * The same question internal/health asks, and the same answers: the datapath
 * must respond, know about ports, and if anything is configured up something
 * must actually be up. "It booted" is deliberately not the bar -- an image that
 * reaches userspace and does not forward is the case rollback exists for.
 */
static int healthy(char *why, size_t len)
{
	int fd = nosaic_query_open(NOSAIC_QUERY_SOCKET);
	char names[512][40];
	char *resp;
	const char *p;
	int n = 0, i, admin = 0, oper = 0;

	if (fd < 0) {
		snprintf(why, len, "the datapath never came up: nothing is listening on %s",
			 NOSAIC_QUERY_SOCKET);
		return 0;
	}
	if ((resp = nosaic_query_ask(fd, "{\"op\":\"ports\"}")) == NULL) {
		snprintf(why, len, "the datapath did not answer");
		nosaic_query_close(fd);
		return 0;
	}
	p = strstr(resp, "\"result\":[");
	p = p ? p + strlen("\"result\":[") : resp;
	while ((p = strchr(p, '{')) != NULL && n < 512) {
		nosaic_jstr(p, "Name", names[n], sizeof(names[n]));
		if (names[n][0] != '\0')
			n++;
		p++;
	}
	free(resp);
	if (n == 0) {
		snprintf(why, len, "the datapath is running but knows about no ports");
		nosaic_query_close(fd);
		return 0;
	}
	for (i = 0; i < n; i++) {
		char req[128];

		snprintf(req, sizeof(req), "{\"op\":\"port.status\",\"name\":\"%s\"}", names[i]);
		if ((resp = nosaic_query_ask(fd, req)) == NULL)
			continue;
		if (nosaic_jbool(resp, "AdminUp", 0))
			admin++;
		if (nosaic_jbool(resp, "OperUp", 0))
			oper++;
		free(resp);
	}
	nosaic_query_close(fd);
	printf("datapath answers: %d ports, %d configured up, %d actually up\n",
	       n, admin, oper);
	/* Links are only required where something asked for a port to be up: a
	 * switch whose ports are all administratively down is not failed for
	 * having no traffic. */
	if (admin > 0 && oper == 0) {
		snprintf(why, len, "%d port(s) are configured up and none of them is up: "
			 "this image is not forwarding", admin);
		return 0;
	}
	return 1;
}

static int cmd_confirm(const char *dir)
{
	char active[16], trial[16], why[256], slot[16];
	int tries;

	active_trial(dir, active, sizeof(active), trial, sizeof(trial), &tries);
	if (trial[0] == '\0')
		return 0; /* the ordinary boot: nothing on trial, nothing to say */

	printf("NOSAIC-TRIAL slot %s is on trial (attempt %d); checking whether it works\n",
	       trial, tries);
	if (!healthy(why, sizeof(why))) {
		printf("NOSAIC-TRIAL DECLINED slot %s: %s\n", trial, why);
		printf("NOSAIC-TRIAL it rolls back once the attempts are used up\n");
		return 0; /* declining is a normal outcome, not an error */
	}
	if (commit(dir, slot, sizeof(slot)) != 0)
		return 0;
	printf("NOSAIC-TRIAL COMMIT slot %s is healthy and is now the slot this switch boots\n",
	       slot);
	return 0;
}

int nosaic_upgrade(int argc, char **argv)
{
	const char *dir = state_dir();
	char slot[16];

	if (dir == NULL) {
		fprintf(stderr, "nosaic: this system has no mounted boot state: "
			"neither /mnt/boot/boot nor /mnt/data/boot is there\n");
		return 1;
	}
	if (argc < 3 || strcmp(argv[2], "status") == 0)
		return cmd_status(dir);
	if (strcmp(argv[2], "commit") == 0) {
		if (commit(dir, slot, sizeof(slot)) != 0)
			return 1;
		printf("committed slot %s: it is now the slot this switch boots\n", slot);
		return 0;
	}
	if (strcmp(argv[2], "confirm") == 0)
		return cmd_confirm(dir);

	/* Writing a slot is not offered here on purpose.
	 *
	 * On this board a slot is a partition, and putting a raw-device write
	 * behind a one-word subcommand on the switch itself is how somebody
	 * overwrites the running image. The Go CLI on the build host does it,
	 * with the partition table in front of it and the active-slot refusal in
	 * the same code path. */
	fprintf(stderr,
		"usage: nosaic upgrade <status|commit|confirm>\n"
		"\n"
		"Installing an image into a slot is done from the build host with\n"
		"`nosaic upgrade install <disk> <image> --slot <a|b>`, which has the\n"
		"partition table in front of it.\n");
	return 2;
}
