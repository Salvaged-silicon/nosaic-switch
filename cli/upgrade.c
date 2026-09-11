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

/* How long a trial waits for its datapath. See healthy(). */
#define NOSAIC_CONFIRM_WAIT_SECS 900

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

/*
 * ---- installing an image into a slot -------------------------------------
 *
 * This used to refuse, and say so:
 *
 *   "Installing an image into a slot is done from the build host with
 *    `nosaic upgrade install <disk> <image> --slot <a|b>`"
 *
 * The reasoning was that putting a raw-device write behind a one-word
 * subcommand on the switch itself is how somebody overwrites the running
 * image. The reasoning was sound and the conclusion was wrong, because the Go
 * CLI it pointed at cannot run on this board -- 32-bit big-endian PowerPC is a
 * target the Go toolchain has never had, which is why this CLI exists at all.
 * So the board had no upgrade path, and the way an image actually got into
 * slot B on 2026-09-11 was a hand-typed dd at a serial console, with no
 * partition table in front of it, no active-slot refusal, no squashfs check
 * and no overlay clear. Refusing to offer the guarded operation did not
 * prevent the dangerous one; it guaranteed it.
 *
 * So the guards move here, where they can actually run:
 *
 *   - the target is the INACTIVE slot, chosen automatically, and naming the
 *     active one is refused;
 *   - the resolved device must not be the one carrying the running root, which
 *     catches a slot table that disagrees with reality rather than trusting it;
 *   - the image must start with the squashfs magic, because a truncated
 *     download that gets written and pointed at is a switch that boots to
 *     nothing;
 *   - it must fit the partition;
 *   - the slot's overlay is cleared, or the new image comes up wearing the old
 *     one's changes;
 *   - and the slot is marked as a TRIAL, never as active. Nothing becomes the
 *     committed choice until it has booted and said it is healthy.
 *
 * That is internal/upgrade.Install, in the same order, for the boards Go
 * cannot reach.
 */

/* Where a slot's image lives. Resolved the way the initramfs resolves it, and
 * for the same reason: if this and the boot script disagree about which device
 * is slot B, an upgrade installs somewhere the switch will never boot from.
 *
 * By label first, because that is what the installer writes and the only
 * answer that is not a convention. The numeric fallback is the convention --
 * slot a is partition 2, slot b is partition 3 -- for a disk whose labels the
 * running kernel cannot read. */
static int slot_device(const char *slot, char *out, size_t len)
{
	static const char *disks[] = { "/dev/vda", "/dev/sda", "/dev/mmcblk0p" };
	struct stat st;
	size_t i;
	int n;

	snprintf(out, len, "/dev/disk/by-label/nosaic-slot-%s", slot);
	if (stat(out, &st) == 0 && S_ISBLK(st.st_mode))
		return 0;

	n = (strcmp(slot, "a") == 0) ? 2 : 3;
	for (i = 0; i < sizeof(disks) / sizeof(disks[0]); i++) {
		snprintf(out, len, "%s%d", disks[i], n);
		if (stat(out, &st) == 0 && S_ISBLK(st.st_mode))
			return 0;
	}
	*out = '\0';
	return -1;
}

/* Refuse to write the device the running system is mounted from.
 *
 * The slot tables say which partition is which; this asks the kernel what is
 * actually underneath "/". If those two ever disagree, believing the table
 * overwrites the running switch, and the first symptom is at the next reboot. */
static int is_root_device(const char *dev)
{
	struct stat root, d;

	if (stat("/", &root) != 0 || stat(dev, &d) != 0)
		return 0;
	return d.st_rdev == root.st_dev;
}

/* rm -rf, without a shell. */
static int rm_rf(const char *path)
{
	char child[512];
	struct dirent *e;
	struct stat st;
	DIR *d;

	if (lstat(path, &st) != 0)
		return (errno == ENOENT) ? 0 : -1;
	if (!S_ISDIR(st.st_mode))
		return unlink(path);

	if ((d = opendir(path)) == NULL)
		return -1;
	while ((e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
		if (rm_rf(child) != 0) {
			closedir(d);
			return -1;
		}
	}
	closedir(d);
	return rmdir(path);
}

/* The new image must not inherit the old one's writable layer.
 *
 * internal/upgrade records two silently-wrong installs from skipping this: the
 * version reported and the files present came from different images, which is
 * the hardest kind of wrong to see because everything works. */
static int clear_overlay(const char *slot)
{
	char dir[256], sub[320];
	struct stat st;
	size_t i;
	static const char *layers[] = { "upper", "work" };

	snprintf(dir, sizeof(dir), "/mnt/data/slot-%s", slot);
	if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode))
		return 0;   /* never booted; nothing to clear */

	for (i = 0; i < sizeof(layers) / sizeof(layers[0]); i++) {
		snprintf(sub, sizeof(sub), "%s/%s", dir, layers[i]);
		if (stat(sub, &st) != 0)
			continue;
		if (rm_rf(sub) != 0 || mkdir(sub, 0755) != 0) {
			fprintf(stderr, "nosaic: clearing slot %s's %s layer: %s\n",
				slot, layers[i], strerror(errno));
			return -1;
		}
		printf("cleared slot %s's %s layer\n", slot, layers[i]);
	}
	return 0;
}

static int cmd_install(const char *dir, const char *image, const char *want)
{
	char active[16], trial[16], slot[16], dev[256];
	char buf[1 << 20];
	unsigned char magic[4];
	off_t isize, dsize;
	int tries, src, dst, rc = 1;
	ssize_t n;

	active_trial(dir, active, sizeof(active), trial, sizeof(trial), &tries);

	/* Default to the slot that is not running. Choosing it here rather than
	 * making the operator name it is most of the safety: the overwhelmingly
	 * common mistake is naming the one you are booted from. */
	if (want != NULL)
		snprintf(slot, sizeof(slot), "%s", want);
	else
		snprintf(slot, sizeof(slot), "%s",
			 strcmp(active, "a") == 0 ? "b" : "a");

	if (strcmp(slot, "a") != 0 && strcmp(slot, "b") != 0) {
		fprintf(stderr, "nosaic: slot must be a or b, not %s\n", slot);
		return 1;
	}
	if (strcmp(slot, active) == 0) {
		fprintf(stderr,
			"nosaic: slot %s is the active slot; installing into it would "
			"overwrite the running image and leave nothing to roll back to\n",
			slot);
		return 1;
	}

	if ((src = open(image, O_RDONLY)) < 0) {
		fprintf(stderr, "nosaic: %s: %s\n", image, strerror(errno));
		return 1;
	}
	if (read(src, magic, sizeof(magic)) != (ssize_t)sizeof(magic) ||
	    memcmp(magic, "hsqs", 4) != 0) {
		fprintf(stderr,
			"nosaic: %s does not start with the squashfs magic.\n"
			"A truncated download written into a slot and booted is a switch\n"
			"that comes up to nothing, so this refuses rather than finding out.\n",
			image);
		close(src);
		return 1;
	}
	isize = lseek(src, 0, SEEK_END);
	if (isize < 0 || lseek(src, 0, SEEK_SET) != 0) {
		fprintf(stderr, "nosaic: %s: %s\n", image, strerror(errno));
		close(src);
		return 1;
	}

	if (slot_device(slot, dev, sizeof(dev)) != 0) {
		fprintf(stderr, "nosaic: cannot find the device for slot %s\n", slot);
		close(src);
		return 1;
	}
	if (is_root_device(dev)) {
		fprintf(stderr,
			"nosaic: %s is the device this system is running from, and the\n"
			"slot table says it is slot %s. Those disagree, and writing would\n"
			"overwrite the running switch. Nothing was written.\n", dev, slot);
		close(src);
		return 1;
	}

	if ((dst = open(dev, O_WRONLY)) < 0) {
		fprintf(stderr, "nosaic: %s: %s\n", dev, strerror(errno));
		close(src);
		return 1;
	}
	dsize = lseek(dst, 0, SEEK_END);
	if (dsize > 0 && isize > dsize) {
		fprintf(stderr, "nosaic: the image is %.1f MiB and slot %s is %.1f MiB\n",
			(double)isize / (1 << 20), slot, (double)dsize / (1 << 20));
		goto out;
	}
	if (lseek(dst, 0, SEEK_SET) != 0) {
		fprintf(stderr, "nosaic: %s: %s\n", dev, strerror(errno));
		goto out;
	}

	printf("installing %s (%.1f MiB) into slot %s on %s\n",
	       image, (double)isize / (1 << 20), slot, dev);
	while ((n = read(src, buf, sizeof(buf))) > 0) {
		ssize_t off = 0;
		while (off < n) {
			ssize_t w = write(dst, buf + off, (size_t)(n - off));
			if (w <= 0) {
				fprintf(stderr, "nosaic: writing %s: %s\n", dev, strerror(errno));
				goto out;
			}
			off += w;
		}
	}
	if (n < 0) {
		fprintf(stderr, "nosaic: reading %s: %s\n", image, strerror(errno));
		goto out;
	}
	/* On disk before the pointer moves. A trial that is pointed at but not
	 * yet written is a rollback nobody asked for. */
	if (fsync(dst) != 0) {
		fprintf(stderr, "nosaic: flushing %s: %s\n", dev, strerror(errno));
		goto out;
	}

	if (clear_overlay(slot) != 0)
		goto out;

	if (write_state(dir, "trial", slot) != 0 ||
	    write_state(dir, "tries", "0") != 0) {
		fprintf(stderr, "nosaic: cannot write the boot pointer: %s\n",
			strerror(errno));
		goto out;
	}
	printf("slot %s is installed and on trial; reboot to try it\n", slot);
	printf("it commits itself if the datapath comes up, and rolls back to %s if not\n",
	       active);
	rc = 0;
out:
	close(dst);
	close(src);
	return rc;
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
	char names[512][40];
	char *resp;
	const char *p;
	int fd = -1, waited, n = 0, i, admin = 0, oper = 0;

	/* Wait for the datapath rather than asking once.
	 *
	 * A daemon carrying a vendor SDK does not serve anything for minutes after
	 * the boot that started it -- about eighty seconds on a Trident2+ and
	 * several times that on this board at its usual log verbosity. Asking once
	 * and giving up declines a perfectly good image, which is what happened:
	 * the switch rolled back an image whose datapath came up a minute after it
	 * had been written off.
	 *
	 * The cost of waiting too long is a slow decline. The cost of waiting too
	 * little is an upgrade that can never succeed on this board at all.
	 */
	for (waited = 0; waited < NOSAIC_CONFIRM_WAIT_SECS; waited += 2) {
		if ((fd = nosaic_query_open(NOSAIC_QUERY_SOCKET)) >= 0)
			break;
		sleep(2);
	}
	if (fd < 0) {
		snprintf(why, len, "the datapath never came up: nothing was listening on "
			 "%s after %d seconds", NOSAIC_QUERY_SOCKET, NOSAIC_CONFIRM_WAIT_SECS);
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

/* The same text the Go CLI prints, because they are the same command.
 *
 * Printed before the boot state is resolved, so that asking what the
 * subcommands are works on a machine that is not a switch. It used to need
 * mounted boot state to tell you what it could do. */
static int usage(void)
{
	fprintf(stderr,
		"usage: nosaic upgrade <status|install|commit|confirm>\n"
		"\n"
		"  status   which slot is active, and whether one is on trial\n"
		"  install  write an image into the inactive slot and mark it a trial\n"
		"  commit   accept the slot on trial as the one this switch boots\n"
		"  confirm  commit only if the datapath is actually up\n");
	return 2;
}

int nosaic_upgrade(int argc, char **argv)
{
	const char *dir;
	char slot[16];

	/* Bare `nosaic upgrade` lists the subcommands rather than running one.
	 * It used to print status, which the Go CLI does not, and two CLIs that
	 * answer the same command differently is the divergence the single-CLI
	 * commitment exists to prevent. */
	if (argc < 3 || strcmp(argv[2], "help") == 0 ||
	    strcmp(argv[2], "-h") == 0 || strcmp(argv[2], "--help") == 0)
		return usage();

	dir = state_dir();
	if (dir == NULL) {
		fprintf(stderr, "nosaic: this system has no mounted boot state: "
			"neither /mnt/boot/boot nor /mnt/data/boot is there\n");
		return 1;
	}
	if (strcmp(argv[2], "status") == 0)
		return cmd_status(dir);
	if (strcmp(argv[2], "commit") == 0) {
		if (commit(dir, slot, sizeof(slot)) != 0)
			return 1;
		printf("committed slot %s: it is now the slot this switch boots\n", slot);
		return 0;
	}
	if (strcmp(argv[2], "confirm") == 0)
		return cmd_confirm(dir);

	if (strcmp(argv[2], "install") == 0) {
		const char *image = NULL, *slot = NULL;
		int i;

		for (i = 3; i < argc; i++) {
			if (strcmp(argv[i], "--slot") == 0 && i + 1 < argc)
				slot = argv[++i];
			else if (strncmp(argv[i], "--slot=", 7) == 0)
				slot = argv[i] + 7;
			else if (image == NULL)
				image = argv[i];
			else {
				fprintf(stderr, "nosaic: unexpected argument %s\n", argv[i]);
				return 2;
			}
		}
		if (image == NULL) {
			fprintf(stderr,
				"usage: nosaic upgrade install <image.sqsh> [--slot <a|b>]\n"
				"\n"
				"Without --slot the inactive slot is chosen, which is almost\n"
				"always what is meant. The image is the rootfs squashfs from a\n"
				"build, not the installer .bin -- that one replaces the whole\n"
				"disk and is for a first install from ONIE.\n");
			return 2;
		}
		return cmd_install(dir, image, slot);
	}

	fprintf(stderr, "nosaic: %s is not an upgrade subcommand\n", argv[2]);
	return usage();
}
