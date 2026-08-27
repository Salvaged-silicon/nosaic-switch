/*
 * The smoke test's init: proof that a NOSaic kernel boots and runs NOSaic
 * userspace.
 *
 * It reports what it observes rather than printing "hello", for the same
 * reason the toolchain test does. A kernel that boots proves the kernel
 * boots; it does not prove that the filesystems the config asked for are
 * present. Those are dropped silently when their dependencies are unmet, and
 * the symptom appears much later as an image that cannot mount its own root.
 *
 * So this mounts /proc, reads the filesystem list out of the running kernel,
 * and says plainly which of the ones NOSaic depends on are there.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/reboot.h>
#include <linux/reboot.h>

static int have_fs(const char *want)
{
	FILE *f = fopen("/proc/filesystems", "r");
	char line[256];
	int found = 0;

	if (!f)
		return -1;
	while (fgets(line, sizeof line, f)) {
		char *p = strrchr(line, '\t');
		p = p ? p + 1 : line;
		p[strcspn(p, "\n")] = '\0';
		if (!strcmp(p, want)) {
			found = 1;
			break;
		}
	}
	fclose(f);
	return found;
}

int main(void)
{
	static const char *required[] = { "squashfs", "overlay", "ext4", "tmpfs", "proc" };
	int ok = 1;

	mkdir("/proc", 0555);
	if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
		printf("NOSAIC-FAIL cannot mount /proc\n");
		ok = 0;
	}

	printf("NOSAIC-INIT userspace is running\n");

	for (size_t i = 0; i < sizeof required / sizeof *required; i++) {
		int f = have_fs(required[i]);
		printf("NOSAIC-FS %-9s %s\n", required[i], f == 1 ? "present" : "MISSING");
		if (f != 1)
			ok = 0;
	}

	printf(ok ? "NOSAIC-OK all required filesystems present\n"
		  : "NOSAIC-FAIL a required filesystem is missing\n");
	fflush(stdout);

	/* Power off rather than panic, so the harness gets a clean exit. */
	sync();
	reboot(LINUX_REBOOT_CMD_POWER_OFF);
	return ok ? 0 : 1;
}
