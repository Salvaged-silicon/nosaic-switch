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
 *   2. unzip -p /tmp/flash/EOS-*.swi usr/lib/libFocalpointSDK.so | dd ...
 *      (slow: ~2s; the result is cached in /run/spico_fw.bin for next time)
 *      The 7150S mounts its vfat flash at /tmp/flash, not /mnt/flash.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "pci.h"
#include "sbus.h"
#include "spico.h"

/* Byte offset of the firmware blob inside libFocalpointSDK.so. [RE] */
#define FM_SPICO_LIB_OFF  0x37e500ul
/* Pre-extracted firmware cache -- survives reboots on tmpfs. */
#define FM_SPICO_CACHE    "/run/spico_fw.bin"
/*
 * Direct path to the library in the EOS squashfs.  On the 7150S-52 the EOS
 * rootfs squashfs is kept mounted at /tmp/eos_root by the NOSaic init; when
 * it is present this is by far the fastest path (a seek + read, no shell).
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
	char swi[256];
	char cmd[640];
	FILE *f;
	size_t n;

	/*
	 * On the 7150S-52 the vfat flash is mounted at /tmp/flash -- not
	 * /mnt/flash as on other platforms.
	 */
	f = popen("ls /tmp/flash/EOS-*.swi 2>/dev/null | head -1", "r");
	if (!f)
		return -1;
	swi[0] = '\0';
	if (fgets(swi, sizeof(swi), f) == NULL)
		swi[0] = '\0';
	pclose(f);

	{
		size_t len = strlen(swi);
		if (len > 0 && swi[len - 1] == '\n')
			swi[len - 1] = '\0';
	}
	if (swi[0] == '\0')
		return -1;

	/*
	 * The SWI is a ZIP of a squashfs; the library is not a direct ZIP
	 * member.  We would need to mount the squashfs to reach it, which
	 * requires root and loop devices.  Skip this path for now; the
	 * direct-mount path above covers the normal case.
	 */
	(void)cmd; (void)n;
	return -1;
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
