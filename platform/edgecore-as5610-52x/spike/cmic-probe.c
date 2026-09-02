/*
 * cmic-probe — can a userspace BDE reach this chip on PowerPC?
 *
 * NOSaic's BDE is userspace over an mmap'd BAR, chosen on the 7050SX2 so that
 * no kernel module has to be owned. EdgeNOS tried that on this board and
 * reverted to ioctl through the SDK's kernel BDE, recording in
 * asic/edged/bde_interface.c that "plain userspace stores miss the PPC MMIO
 * barriers + endianness that the kernel's ioread32/iowrite32 applies. Tried
 * direct mmap once -- broke S-Channel within seconds."
 *
 * That conflates two problems and only one of them is ours.
 *
 * Endianness is a chip setting, not a software byte-swap. CMIC_ENDIAN_SELECT
 * (0x174, include/soc/cmic.h:453) takes ES_BIG_ENDIAN_PIO 0x01000001 --
 * note the same bit in byte 0 and byte 3, so the write lands correctly
 * whichever way the word is currently swapped, which is how a register that
 * selects endianness can be written before you know the endianness. Once set,
 * PIO matches a big-endian host, and SYS_BE_PIO=1 is the SDK being told so.
 *
 * Ordering is ours. PowerPC needs an explicit eieio between MMIO accesses and
 * a plain volatile store does not emit one. S-Channel is a multi-write
 * sequence that depends on order, which is what breaking "within seconds"
 * looks like.
 *
 * So this reads CMIC_REVID_DEVID (0x178, cmic.h:468) and checks the device id
 * against what PCI already told us. If it matches, endianness and basic access
 * are right and the remaining risk is ordering under traffic. If it does not,
 * the answer is a small kernel shim and we have spent an afternoon.
 *
 * RESULT, 2026-09-02, against the live board with edged running:
 *
 *   BAR0 physical            0xa0000000 (0001:01:00.0, device 0xb846)
 *   CMIC_ENDIAN_SELECT       0x04040404
 *   CMIC_REVID_DEVID         0x46B80200   byteswapped 0x0002B846
 *
 * The second line is the whole answer. 0x0002B846 is device 0xb846 revision 2
 * -- so a plain volatile read of an mmap'd BAR from userspace reaches this
 * chip and returns the correct register, byte-swapped. Read with `devmem`,
 * which is the same naive mmap this program does.
 *
 * Two things follow. Userspace access is not the problem: EdgeNOS's revert to
 * ioctl was not because the BAR is unreachable. And the chip is currently in
 * little-endian PIO, which is why the kernel's ioread32 (le32_to_cpu on a
 * big-endian host) gives edged the right answer while a raw load does not.
 *
 * So endianness is a choice, not an obstacle: either set ES_BIG_ENDIAN_PIO and
 * let the chip present the host's order -- which is what SYS_BE_PIO=1 tells
 * the SDK has been done -- or leave it little-endian and swap in the accessor.
 *
 * What remains unproven is ordering. S-Channel is a multi-write sequence and
 * "broke within seconds" is what a missing eieio looks like, not what an
 * unreadable BAR looks like. That is the risk the BDE has to handle, and it is
 * a barrier in the accessor rather than a change of approach.
 *
 * READ-ONLY BY DEFAULT. --set-endian writes CMIC_ENDIAN_SELECT, which changes
 * how every subsequent access is interpreted: do not do that on a switch whose
 * datapath is running, or forwarding stops immediately.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define BCM_VENDOR          0x14e4
#define CMIC_ENDIAN_SELECT  0x00000174
#define CMIC_REVID_DEVID    0x00000178
#define ES_BIG_ENDIAN_PIO       0x01000001
#define ES_BIG_ENDIAN_DMA_PACKET 0x02000002
#define ES_BIG_ENDIAN_DMA_OTHER  0x04000004

/* eieio orders MMIO against MMIO, which is what a register sequence needs;
 * sync is the heavier barrier and is used after a write that must land before
 * anything else is attempted. Compiled for any arch so the file still builds
 * on a development host, where it will not find the device anyway. */
#if defined(__powerpc__) || defined(__PPC__)
#define mmio_barrier()  __asm__ volatile("eieio" ::: "memory")
#define mmio_sync()     __asm__ volatile("sync"  ::: "memory")
#else
#define mmio_barrier()  __asm__ volatile("" ::: "memory")
#define mmio_sync()     __asm__ volatile("" ::: "memory")
#endif

static volatile uint32_t *bar;

static uint32_t rd32(uint32_t off)
{
	uint32_t v = bar[off / 4];
	mmio_barrier();
	return v;
}

static void wr32(uint32_t off, uint32_t v)
{
	bar[off / 4] = v;
	mmio_sync();
}

static uint32_t swap32(uint32_t v)
{
	return ((v & 0xffu) << 24) | ((v & 0xff00u) << 8) |
	       ((v >> 8) & 0xff00u) | ((v >> 24) & 0xffu);
}

/* Read a small hex value out of a sysfs attribute, e.g. "0xb846\n". */
static long sysfs_hex(const char *dir, const char *name)
{
	char path[512], buf[64];
	int fd, n;

	snprintf(path, sizeof path, "%s/%s", dir, name);
	if ((fd = open(path, O_RDONLY)) < 0)
		return -1;
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	return strtol(buf, NULL, 16);
}

int main(int argc, char **argv)
{
	int set_endian = (argc > 1 && !strcmp(argv[1], "--set-endian"));
	DIR *d = opendir("/sys/bus/pci/devices");
	struct dirent *e;
	char dir[512] = "", res[512];
	long devid = -1;
	int fd;

	if (!d) {
		fprintf(stderr, "no /sys/bus/pci/devices: is PCI enabled?\n");
		return 1;
	}
	while ((e = readdir(d))) {
		char cand[512];
		if (e->d_name[0] == '.')
			continue;
		snprintf(cand, sizeof cand, "/sys/bus/pci/devices/%s", e->d_name);
		if (sysfs_hex(cand, "vendor") != BCM_VENDOR)
			continue;
		devid = sysfs_hex(cand, "device");
		snprintf(dir, sizeof dir, "%s", cand);
		break;
	}
	closedir(d);

	if (!dir[0]) {
		fprintf(stderr, "no Broadcom device (vendor 0x%04x) on the PCI bus\n",
			BCM_VENDOR);
		return 1;
	}
	printf("pci device   %s\n", dir);
	printf("pci says     device id 0x%04lx\n", devid);

	snprintf(res, sizeof res, "%s/resource0", dir);
	if ((fd = open(res, O_RDWR | O_SYNC)) < 0) {
		perror("open resource0");
		return 1;
	}
	bar = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (bar == MAP_FAILED) {
		perror("mmap BAR0");
		close(fd);
		return 1;
	}
	printf("bar0         mapped\n\n");

	uint32_t before = rd32(CMIC_REVID_DEVID);
	printf("CMIC_REVID_DEVID (0x178) before: 0x%08x   byteswapped 0x%08x\n",
	       before, swap32(before));

	if (set_endian) {
		uint32_t es = ES_BIG_ENDIAN_PIO | ES_BIG_ENDIAN_DMA_PACKET |
			      ES_BIG_ENDIAN_DMA_OTHER;
		printf("\nwriting CMIC_ENDIAN_SELECT (0x174) = 0x%08x\n", es);
		wr32(CMIC_ENDIAN_SELECT, es);
		uint32_t after = rd32(CMIC_REVID_DEVID);
		printf("CMIC_REVID_DEVID (0x178) after:  0x%08x   byteswapped 0x%08x\n",
		       after, swap32(after));
		before = after;
	}

	/* The device id is somewhere in that word; rather than assume which half,
	 * say which interpretation contains what PCI already told us. */
	printf("\n");
	uint32_t cands[4] = { before >> 16, before & 0xffff,
			      swap32(before) >> 16, swap32(before) & 0xffff };
	const char *what[4] = { "as-read, high half", "as-read, low half",
				"swapped, high half", "swapped, low half" };
	int hit = 0;
	for (int i = 0; i < 4; i++)
		if (cands[i] == (uint32_t)devid) {
			printf("MATCH  %s == 0x%04lx\n", what[i], devid);
			hit = 1;
		}
	if (!hit) {
		printf("NO MATCH for 0x%04lx in any interpretation.\n", devid);
		printf("Either the BAR is not readable this way, or the chip is\n");
		printf("held in reset, or this register is not where it is thought\n");
		printf("to be. A kernel shim over ioread32 is the fallback.\n");
	}
	munmap((void *)bar, 0x10000);
	close(fd);
	return hit ? 0 : 2;
}
