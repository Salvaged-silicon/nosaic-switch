/* SPDX-License-Identifier: Apache-2.0 */
/*
 * How much light is arriving, from the switch itself.
 *
 * WHERE A MODULE LIVES.
 *
 * Every port sits behind two layers of i2c mux, and every port channel
 * presents the same three addresses -- 0x50 for the module, 0x51 for an SFP's
 * diagnostic page, 0x27 for the retimer. Talking to the wrong one reads a
 * different port with no error at all.
 *
 * That danger is the kernel's problem rather than ours, because the muxes are
 * in the device tree with i2c-mux-idle-disconnect: Linux gives each channel
 * its own /dev/i2c-N, arbitrates between them, and parks the mux when nobody
 * holds it. So the whole job here is working out which /dev/i2c-N is which
 * cage.
 *
 * That is NOT done by bus number. Bus numbers come from probe order, and a
 * table of them is a file that is right until the day a driver is added. It is
 * done by asking each bus which device-tree node it came from and reading the
 * mux channels out of the path, which is the same source the buses themselves
 * were created from:
 *
 *     mux@75/i2c@A/mux@74/i2c@B   ->  cage      A*8 + B + 1     (1-32)
 *     mux@76/i2c@A/mux@74/i2c@B   ->  cage 33 + A*8 + B         (33-48)
 *     mux@77/i2c@A                ->  cage 49 + A               (49-52)
 *
 * Checked against the hardware: i2c-16 resolves to mux@75/i2c@0/mux@74/i2c@5,
 * which is cage 6, and swp6 is up.
 *
 * WHY NOT THE at24 DRIVER. The device tree declares eeprom@50 on each port
 * channel, so the devices exist -- but nothing binds them, and the sysfs
 * `eeprom` file a bound at24 would give is absent. Reading the bus directly
 * also avoids pretending a transceiver is a 24c04, which matters: at24 would
 * read a 24c04's second half from 0x51, and on an SFP that address is a
 * different page with different meaning.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#include "optics.h"

/* Bytes per i2c transaction. See read_bytes: longer reads return zeros here
 * rather than failing, which is a fault that decodes. */
#define NOSAIC_I2C_CHUNK 16

#define I2C_ADDR_MODULE 0x50   /* identity and, on QSFP, the diagnostics too */
#define I2C_ADDR_SFPDOM 0x51   /* SFF-8472 page A2h: an SFP's diagnostics */

#define CAGE_MIN 1
#define CAGE_MAX 52
#define QSFP_FIRST 49

/* SFF-8636 (QSFP+), lower page 00h -- always mapped, so light levels need no
 * page select. Offsets match internal/platformhal/sff/qsfp.go. */
#define QSFP_ID        0
#define QSFP_STATUS2   2    /* bit 0: data not ready */
#define QSFP_TEMP      22
#define QSFP_VCC       26
#define QSFP_RXPOWER   34   /* 4 lanes, 2 bytes each */
#define QSFP_TXBIAS    42
#define QSFP_TXPOWER   50   /* optional */
#define QSFP_LOWER_LEN 86
#define QSFP_VENDOR    148  /* upper page 00h, directly addressable */
#define QSFP_PN        168
#define QSFP_SN        196

/* SFF-8472 (SFP+): identity at 0x50, diagnostics on a separate address. */
#define SFP_ID         0
#define SFP_VENDOR     20
#define SFP_PN         40
#define SFP_SN         68
#define SFP_DOM_TEMP   96
#define SFP_DOM_VCC    98
#define SFP_DOM_BIAS   100
#define SFP_DOM_TXPWR  102
#define SFP_DOM_RXPWR  104

static int be16(const unsigned char *b, int off)
{
	return (int)b[off] << 8 | (int)b[off + 1];
}

static int sbe16(const unsigned char *b, int off)
{
	int v = be16(b, off);
	return v >= 0x8000 ? v - 0x10000 : v;
}

/* Power in 0.1 uW units, as both SFF specs report it.
 *
 * Zero is not 0 dBm, it is no light at all -- log10(0) is not a number, and
 * printing one would be a reading. "no signal" is the honest answer and is
 * what the Go CLI prints for the same case. */
static void fmt_dbm(char *out, size_t len, int tenths_uw)
{
	if (tenths_uw <= 0) {
		snprintf(out, len, "no signal");
		return;
	}
	snprintf(out, len, "%.2f dBm", 10.0 * log10((double)tenths_uw / 10000.0));
}

/* Which cage a bus belongs to, from its device-tree path. 0 if it is not a
 * port channel at all -- most buses on this board are not. */
static int cage_of_path(const char *p)
{
	const char *m;
	int a, b;

	if ((m = strstr(p, "/mux@77/i2c@")) != NULL) {
		if (sscanf(m, "/mux@77/i2c@%d", &a) == 1 && a >= 0 && a <= 3)
			return QSFP_FIRST + a;
		return 0;
	}
	if ((m = strstr(p, "/mux@75/i2c@")) != NULL) {
		if (sscanf(m, "/mux@75/i2c@%d/mux@74/i2c@%d", &a, &b) == 2 &&
		    a >= 0 && a <= 3 && b >= 0 && b <= 7)
			return a * 8 + b + 1;
		return 0;
	}
	if ((m = strstr(p, "/mux@76/i2c@")) != NULL) {
		/* Channels 2 and 3 of this mux are GPIO control, not ports. */
		if (sscanf(m, "/mux@76/i2c@%d/mux@74/i2c@%d", &a, &b) == 2 &&
		    a >= 0 && a <= 1 && b >= 0 && b <= 7)
			return 33 + a * 8 + b;
		return 0;
	}
	return 0;
}

/* bus_for[cage] filled once by scanning sysfs; -1 means "no such channel". */
static int bus_for[CAGE_MAX + 1];
static int scanned;

static void scan_buses(void)
{
	char link[512], path[512];
	struct dirent *e;
	DIR *d;
	int i, n, bus, cage;

	for (i = 0; i <= CAGE_MAX; i++)
		bus_for[i] = -1;
	scanned = 1;

	if ((d = opendir("/sys/bus/i2c/devices")) == NULL)
		return;
	while ((e = readdir(d)) != NULL) {
		if (sscanf(e->d_name, "i2c-%d", &bus) != 1)
			continue;
		snprintf(path, sizeof(path), "/sys/bus/i2c/devices/%s/of_node", e->d_name);
		if ((n = (int)readlink(path, link, sizeof(link) - 1)) <= 0)
			continue;
		link[n] = '\0';
		if ((cage = cage_of_path(link)) != 0)
			bus_for[cage] = bus;
	}
	closedir(d);
}

static int bus_of_cage(int cage)
{
	if (cage < CAGE_MIN || cage > CAGE_MAX)
		return -1;
	if (!scanned)
		scan_buses();
	return bus_for[cage];
}

/*
 * Read from a module.
 *
 * I2C_SLAVE first and I2C_SLAVE_FORCE only if that is refused. The device tree
 * registers an eeprom at 0x50 on every port channel, so if a driver ever does
 * bind one, I2C_SLAVE returns EBUSY -- and forcing past a bound driver is how
 * two things drive one device and neither one's reads mean anything. Forcing
 * when nothing is bound is safe and is what the front-panel script does.
 */
static int read_bytes(int bus, int addr, int off, unsigned char *buf, int len)
{
	char dev[32];
	unsigned char reg;
	int fd, rc = -1;

	snprintf(dev, sizeof(dev), "/dev/i2c-%d", bus);
	if ((fd = open(dev, O_RDWR)) < 0)
		return -1;
	/* No I2C_SLAVE needed: I2C_RDWR carries the address in each message,
	 * and it is not refused by a bound driver the way I2C_SLAVE is. */

	/*
	 * ONE combined transaction per chunk: write the byte address, repeated
	 * START, read. Never a write() followed by a separate read().
	 *
	 * The muxes on this board are i2c-mux-idle-disconnect, so the kernel
	 * parks the channel when it releases the parent adapter -- which it does
	 * between two separate transactions. The address write and the read then
	 * reach the module either side of a deselect, and what comes back is
	 * ZEROES rather than an error. That is the worst shape of failure,
	 * because it decodes: a real Avago SR4 reported no temperature, no
	 * voltage and no light, and was briefly written up here as passive
	 * copper. The vendor strings in upper page 00h read correctly through
	 * the same bug, which made it look like the module rather than the read.
	 *
	 * i2cget gets this right without anyone noticing because an SMBus
	 * read-byte IS a combined transaction.
	 */
	{
		int done = 0;

		while (done < len) {
			struct i2c_msg msg[2];
			struct i2c_rdwr_ioctl_data io;
			int n = len - done;

			if (n > NOSAIC_I2C_CHUNK)
				n = NOSAIC_I2C_CHUNK;
			reg = (unsigned char)(off + done);

			msg[0].addr  = (unsigned short)addr;
			msg[0].flags = 0;
			msg[0].len   = 1;
			msg[0].buf   = &reg;
			msg[1].addr  = (unsigned short)addr;
			msg[1].flags = I2C_M_RD;
			msg[1].len   = (unsigned short)n;
			msg[1].buf   = buf + done;

			io.msgs  = msg;
			io.nmsgs = 2;
			if (ioctl(fd, I2C_RDWR, &io) < 0)
				goto out;
			done += n;
		}
	}
	rc = 0;
out:
	close(fd);
	return rc;
}

/* A printable field, trimmed. SFF pads with spaces and vendors pad with
 * whatever they like, so anything unprintable ends the string rather than
 * reaching a terminal. */
static void sff_string(char *out, size_t len, const unsigned char *b, int n)
{
	int i, last = -1;

	if (n > (int)len - 1)
		n = (int)len - 1;
	for (i = 0; i < n; i++) {
		if (b[i] < 0x20 || b[i] > 0x7e)
			break;
		out[i] = (char)b[i];
		if (b[i] != ' ')
			last = i;
	}
	out[last + 1] = '\0';
}

static int present(int cage, unsigned char *id)
{
	int bus = bus_of_cage(cage);

	if (bus < 0)
		return 0;
	return read_bytes(bus, I2C_ADDR_MODULE, 0, id, 1) == 0;
}

int nosaic_optics_list(void)
{
	unsigned char id;
	int cage, n = 0;

	printf("%-6s %-6s %s\n", "CAGE", "BUS", "MODULE");
	for (cage = CAGE_MIN; cage <= CAGE_MAX; cage++) {
		int bus = bus_of_cage(cage);

		if (bus < 0)
			continue;
		if (!present(cage, &id)) {
			printf("%-6d %-6d %s\n", cage, bus, "empty");
			continue;
		}
		n++;
		printf("%-6d %-6d %s (identifier %#04x)\n", cage, bus,
		       cage >= QSFP_FIRST ? "QSFP+" : "SFP+", id);
	}
	if (n == 0)
		printf("\nno modules are seated\n");
	return 0;
}

static void print_lane(int lane, int rx_tenths, int tx_tenths, int tx_ok, int bias_2ua)
{
	char rx[32], tx[32];

	fmt_dbm(rx, sizeof(rx), rx_tenths);
	if (tx_ok)
		fmt_dbm(tx, sizeof(tx), tx_tenths);
	else
		snprintf(tx, sizeof(tx), "not measured");
	printf("%-5d %-14s %-14s %.1f mA\n", lane, rx, tx, (double)bias_2ua * 2.0 / 1000.0);
}

static int show_qsfp(int cage, int bus)
{
	unsigned char lo[QSFP_LOWER_LEN], up[64];
	char s[64];
	int i, tx_ok = 0;

	if (read_bytes(bus, I2C_ADDR_MODULE, 0, lo, sizeof(lo)) != 0) {
		fprintf(stderr, "nosaic: cage %d: reading the module: %s\n",
			cage, strerror(errno));
		return 1;
	}

	printf("cage         %d\n", cage);
	printf("type         QSFP+ (identifier %#04x)\n", lo[QSFP_ID]);
	printf("bus          /dev/i2c-%d\n", bus);

	if (read_bytes(bus, I2C_ADDR_MODULE, QSFP_VENDOR, up, 16) == 0) {
		sff_string(s, sizeof(s), up, 16);
		if (*s != '\0')
			printf("vendor       %s\n", s);
	}
	if (read_bytes(bus, I2C_ADDR_MODULE, QSFP_PN, up, 16) == 0) {
		sff_string(s, sizeof(s), up, 16);
		if (*s != '\0')
			printf("part         %s\n", s);
	}
	if (read_bytes(bus, I2C_ADDR_MODULE, QSFP_SN, up, 16) == 0) {
		sff_string(s, sizeof(s), up, 16);
		if (*s != '\0')
			printf("serial       %s\n", s);
	}

	/* The module has not finished its first measurement cycle. Every
	 * diagnostic reads zero until it has, which is indistinguishable from a
	 * dark link unless this bit is consulted. */
	if (lo[QSFP_STATUS2] & 0x01) {
		printf("\nthis module has not finished measuring yet "
		       "(data not ready); try again in a moment\n");
		return 0;
	}

	printf("temperature  %.1f C\n", (double)sbe16(lo, QSFP_TEMP) / 256.0);
	printf("supply       %.2f V\n", (double)be16(lo, QSFP_VCC) / 10000.0);

	/* Transmit power is optional on QSFP. A module that does not implement
	 * it reports zero, and so does a dead laser -- so they are separated by
	 * asking whether ANY lane is non-zero rather than by a capability bit,
	 * which vendors set inconsistently. */
	for (i = 0; i < 4; i++)
		if (be16(lo, QSFP_TXPOWER + 2 * i) != 0)
			tx_ok = 1;

	for (i = 0; i < 4; i++)
		if (be16(lo, QSFP_RXPOWER + 2 * i) != 0 ||
		    be16(lo, QSFP_TXBIAS + 2 * i) != 0)
			break;
	if (i == 4 && !tx_ok) {
		/*
		 * Every monitor reads zero.
		 *
		 * This says what was observed and does not say why, because the
		 * honest answer is that several causes look identical here and
		 * this code cannot tell them apart: a passive copper cable has
		 * no monitors, an optical module can decline to implement them,
		 * and a module that is not powered reports the same zeros. An
		 * earlier version of this line asserted passive copper, and on
		 * the first module it met -- an Avago SR4 -- it was wrong.
		 *
		 * The identity fields above are the useful signal for the
		 * reader: if the vendor and part number came back, the bus and
		 * the address are fine and only the monitors are empty.
		 */
		printf("\nno diagnostics: temperature, supply and all four lanes "
		       "read zero\n");
		if (lo[QSFP_TEMP] == 0 && lo[QSFP_TEMP + 1] == 0)
			printf("the identity above read correctly, so the bus is "
			       "good and the monitors themselves are empty\n");
		return 0;
	}

	printf("\n%-5s %-14s %-14s %s\n", "LANE", "RX", "TX", "BIAS");
	for (i = 0; i < 4; i++)
		print_lane(i + 1, be16(lo, QSFP_RXPOWER + 2 * i),
			   be16(lo, QSFP_TXPOWER + 2 * i), tx_ok,
			   be16(lo, QSFP_TXBIAS + 2 * i));
	return 0;
}

static int show_sfp(int cage, int bus)
{
	unsigned char b[128], dom[112];
	char s[64];

	if (read_bytes(bus, I2C_ADDR_MODULE, 0, b, sizeof(b)) != 0) {
		fprintf(stderr, "nosaic: cage %d: reading the module: %s\n",
			cage, strerror(errno));
		return 1;
	}
	printf("cage         %d\n", cage);
	printf("type         SFP+ (identifier %#04x)\n", b[SFP_ID]);
	printf("bus          /dev/i2c-%d\n", bus);
	sff_string(s, sizeof(s), b + SFP_VENDOR, 16);
	if (*s != '\0')
		printf("vendor       %s\n", s);
	sff_string(s, sizeof(s), b + SFP_PN, 16);
	if (*s != '\0')
		printf("part         %s\n", s);
	sff_string(s, sizeof(s), b + SFP_SN, 16);
	if (*s != '\0')
		printf("serial       %s\n", s);

	/* SFF-8472 keeps diagnostics at a different i2c address, not a
	 * different page. A module without them does not answer there at all,
	 * which is not an error. */
	if (read_bytes(bus, I2C_ADDR_SFPDOM, 0, dom, sizeof(dom)) != 0) {
		printf("\nthis module reports no diagnostics "
		       "(nothing answers at %#04x)\n", I2C_ADDR_SFPDOM);
		return 0;
	}
	printf("temperature  %.1f C\n", (double)sbe16(dom, SFP_DOM_TEMP) / 256.0);
	printf("supply       %.2f V\n", (double)be16(dom, SFP_DOM_VCC) / 10000.0);

	printf("\n%-5s %-14s %-14s %s\n", "LANE", "RX", "TX", "BIAS");
	print_lane(1, be16(dom, SFP_DOM_RXPWR), be16(dom, SFP_DOM_TXPWR), 1,
		   be16(dom, SFP_DOM_BIAS));
	return 0;
}

int nosaic_optics_show(int cage)
{
	unsigned char id;
	int bus = bus_of_cage(cage);

	if (bus < 0) {
		fprintf(stderr, "nosaic: cage %d is not a port on this board "
			"(1-48 are SFP+, 49-52 are QSFP+)\n", cage);
		return 1;
	}
	if (!present(cage, &id)) {
		printf("cage %d is empty, or its module is not answering on "
		       "/dev/i2c-%d\n", cage, bus);
		return 1;
	}
	return cage >= QSFP_FIRST ? show_qsfp(cage, bus) : show_sfp(cage, bus);
}

/*
 * The module's memory, as bytes.
 *
 * What you want the moment a decode disagrees with reality: decoded zeroes and
 * a bus that is not answering look identical through any amount of formatting.
 */
int nosaic_optics_dump(int cage)
{
	unsigned char b[256];
	int bus = bus_of_cage(cage), i, len = 256;

	if (bus < 0) {
		fprintf(stderr, "nosaic: cage %d is not a port on this board\n", cage);
		return 1;
	}
	if (read_bytes(bus, I2C_ADDR_MODULE, 0, b, len) != 0) {
		/* Some modules will not return 256 bytes in one go. */
		len = 128;
		if (read_bytes(bus, I2C_ADDR_MODULE, 0, b, len) != 0) {
			fprintf(stderr, "nosaic: cage %d: reading the module: %s\n",
				cage, strerror(errno));
			return 1;
		}
	}
	printf("cage %d, i2c-%d, address %#04x\n", cage, bus, I2C_ADDR_MODULE);
	for (i = 0; i < len; i += 16) {
		int j;

		printf("%03d:", i);
		for (j = 0; j < 16 && i + j < len; j++)
			printf(" %02x", b[i + j]);
		printf("  ");
		for (j = 0; j < 16 && i + j < len; j++)
			putchar(b[i + j] >= 0x20 && b[i + j] <= 0x7e ? b[i + j] : '.');
		putchar('\n');
	}
	return 0;
}
