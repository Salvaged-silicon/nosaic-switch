/* SPDX-License-Identifier: Apache-2.0 */
/*
 * thorn-read -- read the board's power-sequencer CPLD over the host SMBus.
 *
 * A BRING-UP SPIKE. It exists to answer one question, and the question is the
 * M1 blocker on this board:
 *
 *   Every reset bit in the SCD can be cleared and the FM6000 still does not
 *   appear on the PCI bus. Something else in the vendor's board initialisation
 *   powers the ASIC, and the best candidate is `thorn` -- an Altera EPM240, a
 *   240-element MAX II CPLD, which is the part a board uses for power
 *   sequencing rather than for datapath logic.
 *
 * thorn is NOT on the SCD. The vendor reads it as `smbus read8 /sb/1/0x23 1`:
 * the HOST southbridge's SMBus, address 0x23, register 1. That is an ordinary
 * i2c bus behind the SB700, which the kernel drives with i2c-piix4 and exposes
 * through /dev/i2c-* -- so this needs nothing vendor-specific, and nothing of
 * the SCD.
 *
 * WHAT TO DO WITH IT. Read thorn on a cold board and on a warm one and compare.
 * A register that differs between "ASIC unpowered" and "ASIC running" is the
 * handle this port is looking for. On this chassis the vendor's own boot reads
 * version 34 from register 1.
 *
 * ⚠ Reading it from under a running EOS returns "Smbus transaction failed" --
 * the vendor's platform agent holds the bus. Run this from NOSaic or from the
 * Aboot prompt, not underneath the vendor OS.
 *
 * IT CAN NOW WRITE, AND THAT WAS A DECISION RATHER THAN A FEATURE.
 *
 * Reading thorn cold and warm found exactly one difference in its whole
 * 256-register file: register 5 is 0x01 with the ASIC unpowered and 0xa1 with
 * it running. Bits 7 and 5. Whether those are CONTROL bits that enable the
 * rails or STATUS bits that report them good cannot be told by reading, and
 * the only experiment that separates them is writing one on a cold board.
 *
 * So -w exists, and it is awkward on purpose:
 *
 *   - it refuses unless -b names one bus, so a write can never fan out across
 *     every adapter the way a read does;
 *   - it says what it is about to do, and to whom, before doing it;
 *   - it reads the value back, because a status bit will not take one and that
 *     is itself the answer;
 *   - there is no write-all, no register range, and no default value.
 *
 * Writing to a power sequencer on a board with no schematic and no spare in
 * the rack is a thing to do deliberately, once, watching the console.
 */
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define THORN_ADDR 0x23
#define THORN_VERSION_REG 1

static int smbus_read_byte_data(int fd, uint8_t reg, uint8_t *out)
{
	union i2c_smbus_data data;
	struct i2c_smbus_ioctl_data args;

	memset(&data, 0, sizeof(data));
	args.read_write = I2C_SMBUS_READ;
	args.command = reg;
	args.size = I2C_SMBUS_BYTE_DATA;
	args.data = &data;
	if (ioctl(fd, I2C_SMBUS, &args) < 0)
		return -1;
	*out = data.byte;
	return 0;
}

/* The adapter's own name, which is how you tell the SB700's SMBus from
 * anything else the kernel has registered. */
static int smbus_write_byte_data(int fd, uint8_t reg, uint8_t val)
{
	union i2c_smbus_data data;
	struct i2c_smbus_ioctl_data args;

	memset(&data, 0, sizeof(data));
	data.byte = val;
	args.read_write = I2C_SMBUS_WRITE;
	args.command = reg;
	args.size = I2C_SMBUS_BYTE_DATA;
	args.data = &data;
	if (ioctl(fd, I2C_SMBUS, &args) < 0)
		return -1;
	return 0;
}

static void adapter_name(int n, char *buf, size_t len)
{
	char path[128];
	FILE *f;

	snprintf(path, sizeof(path), "/sys/class/i2c-dev/i2c-%d/name", n);
	buf[0] = '\0';
	if ((f = fopen(path, "r")) == NULL)
		return;
	if (fgets(buf, (int)len, f) != NULL) {
		char *nl = strchr(buf, '\n');
		if (nl)
			*nl = '\0';
	}
	fclose(f);
}

int main(int argc, char **argv)
{
	int addr = THORN_ADDR, reg = THORN_VERSION_REG, count = 1;
	int i, found = 0, read_ok = 0;
	int bus = -1, wval = -1;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-a") && i + 1 < argc)
			addr = (int)strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "-r") && i + 1 < argc)
			reg = (int)strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "-n") && i + 1 < argc)
			count = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-b") && i + 1 < argc)
			bus = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-w") && i + 1 < argc)
			wval = (int)strtol(argv[++i], NULL, 0);
		else {
			fprintf(stderr,
"usage: thorn-read [-a ADDR] [-r REG] [-n COUNT]\n"
"       thorn-read -b BUS -r REG -w VALUE   (writes -- see the file header)\n"
"  defaults: addr 0x23, reg 1, one byte -- thorn's version on a 7150S-52\n"
"  every /dev/i2c-* is tried, because which one is the SB700's SMBus is a\n"
"  property of the running kernel and not worth hard-coding.\n");
			return 2;
		}
	}

	if (wval >= 0) {
		char path[64];
		uint8_t back = 0;
		int fd, rc;

		if (bus < 0) {
			fprintf(stderr,
"thorn-read: -w requires -b to name one bus.\n"
"thorn-read: a write that fanned out across every adapter the way a read\n"
"thorn-read: does would poke unknown devices on unknown buses.\n");
			return 2;
		}
		snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
		printf("WRITING to %s, device 0x%02x, register %d, value 0x%02x\n",
		       path, addr, reg, wval);
		if ((fd = open(path, O_RDWR)) < 0) {
			fprintf(stderr, "thorn-read: %s: cannot open\n", path);
			return 1;
		}
		if (ioctl(fd, I2C_SLAVE_FORCE, addr) < 0) {
			fprintf(stderr, "thorn-read: cannot address 0x%02x\n", addr);
			close(fd);
			return 1;
		}
		rc = smbus_write_byte_data(fd, (uint8_t)reg, (uint8_t)wval);
		printf("  write %s\n", rc == 0 ? "accepted" : "FAILED");
		if (smbus_read_byte_data(fd, (uint8_t)reg, &back) == 0)
			printf("  reads back 0x%02x (%u)%s\n", back, back,
			       back == (uint8_t)wval ? "" : "   <- did NOT take the value");
		else
			printf("  read-back failed\n");
		close(fd);
		return rc == 0 ? 0 : 1;
	}

	/*
	 * Try every adapter rather than naming one. Which /dev/i2c-N is the
	 * southbridge's SMBus depends on probe order, and a tool that hard-codes
	 * it reports "not found" on a board where it is simply somewhere else.
	 */
	for (i = 0; i < 32; i++) {
		char path[64], name[128];
		int fd, j;

		snprintf(path, sizeof(path), "/dev/i2c-%d", i);
		if ((fd = open(path, O_RDWR)) < 0)
			continue;
		found++;
		adapter_name(i, name, sizeof(name));
		printf("%s  %s\n", path, name[0] ? name : "(unnamed)");

		if (ioctl(fd, I2C_SLAVE_FORCE, addr) < 0) {
			printf("    0x%02x: cannot address\n", addr);
			close(fd);
			continue;
		}
		for (j = 0; j < count; j++) {
			uint8_t v = 0;

			if (smbus_read_byte_data(fd, (uint8_t)(reg + j), &v) == 0) {
				printf("    0x%02x reg %2d = 0x%02x (%u)\n",
				       addr, reg + j, v, v);
				read_ok++;
			} else {
				printf("    0x%02x reg %2d : no response\n",
				       addr, reg + j);
			}
		}
		close(fd);
	}

	if (found == 0) {
		fprintf(stderr,
"thorn-read: no /dev/i2c-* at all.\n"
"thorn-read: the kernel needs I2C_PIIX4 for the SB700's SMBus and\n"
"thorn-read: I2C_CHARDEV to expose it. Both are in NOSaic's x86_64 config.\n");
		return 1;
	}
	if (read_ok == 0) {
		fprintf(stderr,
"thorn-read: no adapter answered at 0x%02x.\n"
"thorn-read: under a running EOS that is expected -- its platform agent holds\n"
"thorn-read: the bus. Try from NOSaic or from the Aboot prompt.\n", addr);
		return 1;
	}
	return 0;
}
