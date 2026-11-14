/* SPDX-License-Identifier: Apache-2.0 */
/*
 * SPICO firmware loader -- reads the firmware from the EOS SWI at runtime.
 *
 * The FM6000's SPICO is an Avago micro-controller embedded in the SBus ring.
 * The SSCHED ring engine relies on SPICO to generate its clock ticks; without
 * firmware loaded and the processor running, INIT_COMPLETE fires into a dead
 * clock domain and the chip resets itself (~112 times per attempt, measured
 * 2026-10-02).
 *
 * The firmware (6000 × uint16_t, little-endian) lives in Arista's proprietary
 * libFocalpointSDK.so at file offset 0x37e500.  NOSaic cannot redistribute it,
 * so this file reads it at runtime from the EOS SWI that is always present on
 * the 7150S-52's flash.
 *
 * EXTRACTION PATH
 *   1. /run/spico_fw.bin  -- pre-extracted cache from a prior boot
 *   2. /tmp/eos_root/usr/lib/libFocalpointSDK.so  -- EOS squashfs mounted here
 *      The EOS SWI (/mnt/flash/EOS-*.swi) is a ZIP whose 'EOS' member is a
 *      squashfs. try_swi() extracts it to /tmp/eos_sqsh and loop-mounts it at
 *      /tmp/eos_root so the direct path works. (~2 s on first call; cached.)
 *      The initramfs bind-moves the vfat flash to /mnt/flash after pivot_root.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "pci.h"
#include "sbus.h"
#include "spico.h"

static void spico_kmsg(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	int fd, n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	fd = open("/dev/kmsg", O_WRONLY | O_NOCTTY);
	if (fd >= 0) {
		write(fd, buf, n);
		write(fd, "\n", 1);
		close(fd);
	}
	fprintf(stderr, "%s\n", buf);
}

/* Byte offset of the firmware blob inside libFocalpointSDK.so. [RE] */
#define FM_SPICO_LIB_OFF  0x37e500ul
/* Pre-extracted firmware cache -- survives reboots on tmpfs. */
#define FM_SPICO_CACHE    "/run/spico_fw.bin"
/*
 * Direct path to the library in the EOS squashfs.  try_swi() mounts the EOS
 * squashfs at /tmp/eos_root; once it is there this path is a direct seek+read
 * with no further shell involvement.
 */
#define FM_SPICO_LIB_DIRECT "/tmp/eos_root/usr/lib/libFocalpointSDK.so"
/* Member name inside the EOS SWI (ZIP) for the fallback path. */
#define FM_SPICO_SWI_MBR  "usr/lib/libFocalpointSDK.so"

static int try_file(uint16_t *buf, const char *path, unsigned long off)
{
	FILE *f = fopen(path, "rb");
	size_t n;

	if (!f)
		return -1;
	if (fseek(f, (long)off, SEEK_SET) != 0) {
		fclose(f);
		return -1;
	}
	n = fread(buf, sizeof(uint16_t), FM_SPICO_NWORDS, f);
	fclose(f);
	return (n == FM_SPICO_NWORDS) ? 0 : -1;
}

static int try_swi(uint16_t *buf)
{
	FILE *f;
	char swi[256];
	char cmd[1024];
	size_t len;
	int rv;

	/*
	 * Already mounted? The mount persists for the process lifetime, so a
	 * second call is just a fast file read.
	 */
	if (try_file(buf, FM_SPICO_LIB_DIRECT, FM_SPICO_LIB_OFF) == 0)
		return 0;

	/*
	 * Locate the EOS SWI. On the 7150S-52 the vendor SWI is stored as a
	 * hidden file (.boot-image.swi) not as the conventional EOS-<ver>.swi.
	 * Try the hidden name first, then fall back to the conventional glob.
	 * The initramfs bind-moves the vfat flash to /mnt/flash before
	 * pivot_root, so that is the correct path in the running system.
	 */
	f = popen(
		"ls /mnt/flash/.boot-image.swi /mnt/flash/EOS-*.swi 2>/dev/null"
		" | grep -v NOSaic | head -1",
		"r");
	if (!f)
		return -1;
	swi[0] = '\0';
	if (fgets(swi, sizeof(swi), f) == NULL)
		swi[0] = '\0';
	pclose(f);
	len = strlen(swi);
	if (len > 0 && swi[len - 1] == '\n')
		swi[len - 1] = '\0';
	if (swi[0] == '\0') {
		spico_kmsg("nosd-fm6000: spico: no EOS SWI found in /mnt/flash");
		return -1;
	}
	spico_kmsg("nosd-fm6000: spico: SWI=%s", swi);

	/*
	 * The EOS SWI is a ZIP archive.  On 4.16.x the root filesystem
	 * squashfs is the member named 'rootfs-i386.sqsh'; on other releases
	 * it may simply be 'EOS'.  Find it dynamically by looking for the
	 * largest member whose name ends in '.sqsh', falling back to 'EOS'.
	 *
	 * Extract it to /tmp/eos_sqsh and loop-mount it at /tmp/eos_root.
	 * This runs once; subsequent calls return from the early try_file().
	 * The mount is not unmounted -- it persists for the process lifetime.
	 *
	 * The squashfs is ~400 MB; extracting into RAM takes ~2 s. Acceptable
	 * for a one-time init on a switch with 3 GB of usable RAM.
	 */
	snprintf(cmd, sizeof(cmd),
		"swi='%s'; "
		"sqsh=$(unzip -l \"$swi\" 2>/dev/null"
		" | awk 'NF==4 && $NF~/\\.sqsh$/{print $1,$NF}'"
		" | sort -rn | awk '{print $2}' | head -1); "
		"[ -z \"$sqsh\" ] && sqsh=EOS; "
		"mkdir -p /tmp/eos_root && "
		"unzip -p \"$swi\" \"$sqsh\" > /tmp/eos_sqsh 2>/dev/null && "
		"mount -t squashfs -o loop,ro /tmp/eos_sqsh /tmp/eos_root 2>/dev/null",
		swi);
	rv = system(cmd);
	spico_kmsg("nosd-fm6000: spico: swi_mount rv=%d", rv);
	if (rv != 0)
		return -1;

	return try_file(buf, FM_SPICO_LIB_DIRECT, FM_SPICO_LIB_OFF);
}

static void write_cache(const uint16_t *buf)
{
	FILE *f = fopen(FM_SPICO_CACHE, "wb");

	if (!f)
		return;
	fwrite(buf, sizeof(uint16_t), FM_SPICO_NWORDS, f);
	fclose(f);
}

int fm_spico_load(struct fm6000 *d)
{
	uint16_t *fw;
	int rv;

	fw = malloc(FM_SPICO_NWORDS * sizeof(uint16_t));
	if (!fw)
		return FM_ERR;

	/* 1. Fastest: pre-extracted cache on tmpfs. */
	if (try_file(fw, FM_SPICO_CACHE, 0) == 0)
		goto have_fw;

	/* 2. Direct read from the mounted EOS squashfs. */
	if (try_file(fw, FM_SPICO_LIB_DIRECT, FM_SPICO_LIB_OFF) == 0) {
		write_cache(fw);
		goto have_fw;
	}

	/* 3. Fall back to SWI extraction (not yet implemented). */
	if (try_swi(fw) == 0) {
		write_cache(fw);
		goto have_fw;
	}

	free(fw);
	return FM_ERR;

have_fw:
	rv = fm_spico_download(d, fw, FM_SPICO_NWORDS);
	free(fw);
	if (rv != FM_OK)
		return rv;

	/*
	 * Restore the SBus controller before issuing the run command.
	 *
	 * The download loop makes ~30,000 SBus writes.  During that window the
	 * chip self-resets ~39 times (normal for this chip at this stage).  Each
	 * chip reset returns SBUS_CFG to its non-zero default, holding the SBus
	 * master in reset.  Any SBus write after that reset -- including the run
	 * command that would end fm_spico_download -- silently goes nowhere:
	 * CMD_BUSY never asserts (master is in reset), the poll loop exits with
	 * stale RC=0, and SPICO never starts.
	 *
	 * fm_sbus_start() clears SBUS_CFG and takes SPICO out of host-side reset
	 * unconditionally, so the 0x0C=0x08 (run) write below is guaranteed to
	 * reach the SBus device regardless of how many resets happened during
	 * the download. [RE 2026-10-05]
	 */
	if ((rv = fm_sbus_start(d)) != FM_OK)
		return rv;
	if ((rv = fm_sbus_write(d, FM_SBUS_DEV_SPICO, 0x0C, 0x08)) != FM_OK)
		return rv;

	return fm_spico_enable(d);
}
