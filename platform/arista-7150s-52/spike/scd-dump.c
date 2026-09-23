/* SPDX-License-Identifier: Apache-2.0 */
/*
 * scd-dump -- read the Arista SCD's registers, cold or warm.
 *
 * A BRING-UP SPIKE, not part of the OS. It lives here rather than in
 * datapath/ because the SCD is board hardware and not the dataplane, and it is
 * a separate binary from anything that ships because its whole purpose is to
 * be run by hand at an Aboot prompt on a switch that is not working.
 *
 * WHY IT EXISTS. The FM6000 is held in reset by the SCD from power-on, and
 * releasing every reset bit is NOT enough to bring it onto the PCI bus --
 * measured on this board, 2026-09-23. Something else in the vendor's board
 * initialisation powers the chip, and Arista's own GPL SCD driver does not
 * contain it: that driver is a generic FPGA driver, and there is no support
 * for this platform ("raven") in their SONiC tree at all.
 *
 * So the way to find it is a difference. The SCD is reachable in both states:
 *
 *   cold   the Aboot shell, before anything has configured the board
 *   warm   under the vendor OS, with the ASIC up and forwarding
 *
 * Dump the same range in both and subtract. What changed is what board
 * initialisation did, and somewhere in it is whatever this port is missing.
 *
 * It takes a PCI slot rather than hard-coding one, because the same question
 * gets asked of the second BAR and of other devices, and because a tool that
 * can only look at one address is one you have to edit to use.
 *
 * READ-ONLY. It has no write path at all. Turning the board's resets on and
 * off is done deliberately, with devmem, by someone who has decided to -- not
 * as a flag on a tool whose normal use is to look.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(void)
{
	fprintf(stderr,
"usage: scd-dump [-s SLOT] [-b BAR] [-o BYTEOFF] [-n WORDS] [-q]\n"
"\n"
"  -s SLOT    PCI address, default 0000:04:00.0 (the SCD on a 7150S-52)\n"
"  -b BAR     which resourceN to map, default 0\n"
"  -o BYTEOFF byte offset to start at, default 0\n"
"  -n WORDS   how many 32-bit words, default 256\n"
"  -q         only print non-zero words -- a cold SCD is mostly zeros\n"
"\n"
"Output is one word per line, 'offset value', so two runs diff cleanly.\n");
}

int main(int argc, char **argv)
{
	const char *slot = "0000:04:00.0";
	int bar = 0, quiet = 0, fd, i;
	unsigned long off = 0, words = 256;
	char path[256];
	struct stat st;
	volatile uint32_t *map;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-s") && i + 1 < argc) slot = argv[++i];
		else if (!strcmp(argv[i], "-b") && i + 1 < argc) bar = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-o") && i + 1 < argc) off = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "-n") && i + 1 < argc) words = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "-q")) quiet = 1;
		else { usage(); return 2; }
	}

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource%d", slot, bar);
	if ((fd = open(path, O_RDONLY)) < 0) {
		fprintf(stderr, "scd-dump: %s: cannot open\n", path);
		return 1;
	}
	if (fstat(fd, &st) != 0 || st.st_size == 0) {
		fprintf(stderr, "scd-dump: %s: no size\n", path);
		close(fd);
		return 1;
	}
	/* Read-only, so a mistake here cannot change the board. */
	map = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		fprintf(stderr, "scd-dump: mmap of %s failed\n", path);
		close(fd);
		return 1;
	}

	if (off + words * 4 > (unsigned long)st.st_size)
		words = ((unsigned long)st.st_size - off) / 4;

	printf("# %s bar%d, %lld bytes, from 0x%lx, %lu words\n",
	       slot, bar, (long long)st.st_size, off, words);
	for (i = 0; i < (int)words; i++) {
		unsigned long o = off + (unsigned long)i * 4;
		uint32_t v = *(volatile const uint32_t *)((const char *)map + o);

		if (quiet && v == 0)
			continue;
		printf("%06lx %08x\n", o, v);
	}

	munmap((void *)map, st.st_size);
	close(fd);
	return 0;
}
