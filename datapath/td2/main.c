/*
 * nosd-td2 — the Trident2 datapath.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Implements switch-api over the Broadcom SDK, with the userspace BDE in
 * bde.c underneath it. It speaks the same newline-delimited JSON on the same
 * Unix socket as every other provider, so the CLI, the config model and the
 * HAL above it do not know which chip is answering.
 *
 * STATE: running on hardware. It attaches the chip, brings the ports up,
 * bridges them to Linux, mirrors the routing table and serves the socket.
 *
 * The line above used to say it did none of that and had never run on a
 * switch, which stayed there long after it stopped being true -- and was
 * believed, because a comment saying "this does not work yet" is not something
 * anyone re-checks.
 */
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <dirent.h>

#include "bde.h"
#include "props.h"
#include "portmode.h"
#include "sdk.h"
#include "l3sync.h"
#include "led.h"
#include "phy.h"
#include "phybus.h"
#include "query.h"
#include "tapbridge.h"

/* Runs once a second from the pump, whether or not there is traffic. */
/* Milliseconds between two monotonic samples; a zero `then` reads as due. */
static long elapsed_ms(const struct timespec *then, const struct timespec *now)
{
	if (then->tv_sec == 0 && then->tv_nsec == 0)
		return 1 << 30;
	return (long)(now->tv_sec - then->tv_sec) * 1000 +
	       (now->tv_nsec - then->tv_nsec) / 1000000;
}

/*
 * Periodic work, and it must be periodic in WALL CLOCK rather than per call.
 *
 * The pump calls this every time poll() returns, and poll() returns the instant
 * a packet is waiting -- so on a busy port this runs once per packet, not once
 * per second. nosaic_l3_poll() mirrors the kernel FIB into the chip: it parses
 * /proc/net/route, /proc/net/arp and /proc/net/ipv6_route, makes a netlink
 * RTM_GETNEIGH round trip, and walks every route and neighbour programming the
 * chip. Doing that per packet held the CPU path to about twenty packets a
 * second and built queues nineteen seconds deep.
 *
 * It looked like a rate limit somewhere in the SDK, and it was this. The 1000
 * ms passed to the pump is a poll TIMEOUT -- the longest it will wait when
 * nothing is happening -- and never a guarantee of how often this is called.
 *
 * The guards below fixed the rate. They did not fix the thread: this still ran
 * where packets are moved, so once a second the packet path stopped for as long
 * as a FIB mirror takes. Measured from the AS5610 pinging this box, that showed
 * up as a steady 5-26 ms with a spike to 82-118 ms about once a second, against
 * 1 ms to a neighbour that answers in hardware. So it runs on its own thread
 * now, and the pump does nothing but move packets.
 */
static void datapath_tick(void)
{
	static struct timespec last_l3, last_stats, last_led, last_phy;
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return;

	if (elapsed_ms(&last_l3, &now) >= 1000) {
		last_l3 = now;
		nosaic_l3_poll();
	}
	/* The panel, at a rate a person would notice rather than a machine.
	 * Two seconds is well inside how long anybody takes to walk to a rack,
	 * and 72 register reads at that rate is nothing. */
	if (elapsed_ms(&last_led, &now) >= 2000) {
		last_led = now;
		nosaic_led_poll();
	}
	/* The copper PHYs, at one second. Not for responsiveness -- it is the
	 * bounded MDIO budget inside that decides how fast ports get matched --
	 * but because a port that has just linked and is bridging nothing is
	 * worth fixing before anybody notices it as a dead cable. */
	if (elapsed_ms(&last_phy, &now) >= 1000) {
		last_phy = now;
		nosaic_phy_poll();
	}
	if (elapsed_ms(&last_stats, &now) >= 60000) {
		last_stats = now;
		nosaic_tap_stats();
	}
}


/* Runs datapath_tick off the packet path. The guards inside it still decide
 * how often each piece actually does anything; this only decides how often it
 * is asked. */
static void *periodic(void *arg)
{
	(void)arg;
	for (;;) {
		sleep(1);
		datapath_tick();
	}
	return NULL;
}


/* The Trident2 this daemon is for. Broadcom's own identifier, so a build for
 * the wrong silicon is visible rather than mysterious.
 *
 * The revision is read off the board rather than carried over: the Trident2+
 * this was derived from reports 0x02, and the BCM56855 on the 7050TX-64 reports
 * 0x03. The SDK matches on it, so the wrong one fails to attach a chip that is
 * sitting there working. */
#define TD2_VENDOR   0x14e4
#define TD2_DEVICE   0xb855
#define TD2_REVISION 0x03

/* Where the board's ASIC configuration lives on a running switch.
 *
 * Two directories, in this order, because they hold different kinds of thing:
 *
 *   /etc/nosaic        shipped with the image, the same on every switch of
 *                      this model -- SDK properties, interrupt mode
 *   /mnt/data/config   generated on THIS switch and belonging to it -- the
 *                      port map and the SerDes polarity table, which are read
 *                      from the machine itself and are not in any image
 *
 * The persistent directory is read second so it wins, and it is on the shared
 * data partition rather than in a slot: a port map must survive an upgrade and
 * a rollback, because it describes the board and not the software.
 */
#define SHIPPED_CONF_DIR "/etc/nosaic"
#define PERSIST_CONF_DIR "/mnt/data/config"
#define DEFAULT_ASIC_CONF SHIPPED_CONF_DIR "/asic.conf"

/* The configuration files this daemon owns, loaded from the shipped directory
 * and then the persistent one so the second overrides the first. */
static const char *const datapath_conf[] = {
	"asic.conf",      /* shipped: SDK properties for this board model */
	"portmap.conf",   /* generated: which lane reaches which cage */
	"polarity.conf",  /* generated: which lanes the PCB inverts */
	"serdes.conf",    /* generated: this board's transmit equalisation */
	"portmode.conf",  /* shipped: which QSFP cages run as 4x10G */
};

/* Where the switch chip appears once the board controller releases it.
 *
 * A fallback, not an answer. led_map_scd already makes the argument for the
 * SCD -- "its PCI address is not fixed ... so it is found by vendor id rather
 * than named" -- and the ASIC needs it more, because the address is not the
 * only thing that moves:
 *
 *   7050TX-64      0000:01:00.0   14e4:b855 rev 0x03
 *   Nexus 3172TQ   0000:02:00.0   14e4:b854 rev 0x02
 *
 * Both are Trident II and both attach through the same SDK driver, but the SDK
 * matches on device and revision, so a compiled-in pair is right for exactly
 * one board and silently wrong on the next. */
#define DEFAULT_ASIC_BDF "0000:01:00.0"

/* Trident II covers BCM56850 through BCM5685f. Narrow enough that nothing else
 * Broadcom on a switch answers to it, wide enough to cover the variants. */
#define TD2_DEVICE_FIRST 0xb850
#define TD2_DEVICE_LAST  0xb85f

/* Find the switch chip on the PCI bus.
 *
 * Returns 0 and fills bdf on success, -1 if nothing matched -- which is not
 * fatal on its own: the caller falls back to the compiled-in address so a
 * board whose chip is hidden behind something this cannot see still gets the
 * old behaviour rather than no behaviour.
 */
static int td2_find(char *bdf, size_t bdflen)
{
	DIR *d = opendir("/sys/bus/pci/devices");
	struct dirent *e;

	if (d == NULL)
		return -1;
	while ((e = readdir(d)) != NULL) {
		unsigned vendor = 0, device = 0;
		char path[256];
		FILE *f;

		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor", e->d_name);
		if ((f = fopen(path, "r")) == NULL)
			continue;
		if (fscanf(f, "%x", &vendor) != 1)
			vendor = 0;
		fclose(f);
		if (vendor != TD2_VENDOR)
			continue;

		snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/device", e->d_name);
		if ((f = fopen(path, "r")) == NULL)
			continue;
		if (fscanf(f, "%x", &device) != 1)
			device = 0;
		fclose(f);
		if (device < TD2_DEVICE_FIRST || device > TD2_DEVICE_LAST)
			continue;

		snprintf(bdf, bdflen, "%s", e->d_name);
		closedir(d);
		return 0;
	}
	closedir(d);
	return -1;
}

/* How long --attach holds the device before exiting. Long enough for the SDK's
 * own threads to run and fault if they are going to. */
#define HOLD_SECONDS 60

static void usage(void)
{
	fprintf(stderr,
		"usage: nosd-td2 [--probe <pci-bdf>]\n"
		"\n"
		"  --attach <bdf> [conf]\n"
		"            create the SDK device over the BDE and attach the chip.\n"
		"            One or more property files; defaults to\n"
		"            " DEFAULT_ASIC_CONF ". The board ships generic settings\n"
		"            there and the generated port map goes beside it, so give\n"
		"            both: asic.conf portmap.conf\n"
		"            Built only when the SDK is staged (SDK_DIR).\n"
		"  --soc-init <bdf> [conf]\n"
		"            attach and run soc_init, then stop and hold. The point\n"
		"            between the SOC layer coming up and the BCM layer's\n"
		"            first table write, so the chip can be examined there.\n"
		"  --init <bdf> [conf]\n"
		"            attach, then finish initialisation and survey which\n"
		"            ports have link. Link is the only evidence there is for\n"
		"            which physical lane reaches which front-panel cage.\n"
		"  --probe   map the device and report what is there, then exit.\n"
		"            Requires the DMA region reserved on the kernel command\n"
		"            line and root for /dev/mem. On the 7050SX2 that is\n"
		"            memmap=64M$0xd0000000 iomem=relaxed -- not 0x100000000,\n"
		"            which is past the end of that board's 3844 MB.\n"
		"            NOSAIC_DMA_BASE and NOSAIC_DMA_SIZE override it.\n");
}

static int probe(const char *bdf)
{
	struct nosaic_bde b;

	if (nosaic_bde_open(&b, bdf) != 0)
		return 1;

	printf("device      %s  %04x:%04x rev %#04x\n",
	       b.bdf, b.vendor_id, b.dev_id, b.rev_id);
	printf("BAR0        %zu bytes mapped\n", b.bar_len);
	printf("DMA         %zu bytes at %#llx\n", b.dma_len,
	       (unsigned long long)b.dma_phys);

	nosaic_bde_close(&b);
	return 0;
}

#ifdef NOSAIC_WITH_SDK
/* Hand the chip to the SDK.
 *
 * This is the point of the whole exercise: once the vectors are installed, the
 * SDK owns chip initialisation -- which is deliberately not ours to write,
 * because the Trident2 MMU and LLS sequences are exactly what hand-
 * reproduction repeatedly failed to match on this silicon. */
/*
 * The device the SDK is given.
 *
 * Static, not a local. soc_cm_device_create is handed a pointer to this as its
 * cookie, and every vector call reaches it through dev->cookie for the life of
 * the device -- from threads the SDK starts for itself. A local would go out
 * of scope the moment attach() returned, leaving the SDK dereferencing a dead
 * stack frame.
 */
static struct nosaic_bde attached_dev;

/* Load one or more property files. Later files add to earlier ones, which is
 * what lets the board ship its generic settings and the operator supply the
 * generated port map beside them rather than editing one file. */
/* Like load_confs but tolerant of a file that is not there: the generated
 * ones are absent on a switch nobody has generated them for yet, and that
 * should say so once rather than failing on the first missing name. */
static int load_confs_optional(char **confs, int n)
{
	int i, total = 0;

	for (i = 0; i < n; i++) {
		int c = nosaic_props_load(confs[i]);

		if (c < 0)
			continue;
		printf("config     %d properties from %s\n", c, confs[i]);
		total += c;
	}
	return total;
}

static int load_confs(char **confs, int n)
{
	int i, total = 0;

	for (i = 0; i < n; i++) {
		int c = nosaic_props_load(confs[i]);

		if (c < 0) {
			fprintf(stderr, "nosd-td2: cannot read %s\n", confs[i]);
			return -1;
		}
		printf("config     %d properties from %s\n", c, confs[i]);
		total += c;
	}
	return total;
}

static int attach(const char *bdf, char **confs, int nconf, int full)
{
	struct nosaic_bde *b = &attached_dev;
	int unit, n;

	if (nosaic_bde_open(b, bdf) != 0)
		return 1;

	/* The board's own ASIC configuration. Without it the SDK has no port map,
	 * and port configuration fails during attach with "Port config error !!"
	 * -- which names the symptom and not the cause. */
	n = load_confs(confs, nconf);
	if (n < 0) {
		nosaic_bde_close(b);
		return 1;
	}
	if (nosaic_props_get_unit("portmap_1", 0) == NULL)
		fprintf(stderr, "nosd-td2: warning: no portmap_1 property. The SDK cannot\n"
			"  attach without a port map, and this board's is generated rather\n"
			"  than shipped -- see platform/<board>/tools/mkportmap.sh\n");

	/*
	 * Break out any QSFP cage the board asks for, BEFORE the attach that
	 * reads the map. There is no runtime switch for this on this chip, so
	 * doing it later would do nothing and say it had worked.
	 */
	if (nosaic_portmode_apply(0) < 0) {
		nosaic_bde_close(b);
		return 1;
	}

	unit = nosaic_sdk_attach(b, b->dev_id, b->rev_id);
	if (unit < 0) {
		nosaic_bde_close(b);
		return 1;
	}
	printf("SDK unit   %d\n", unit);
	printf("chip init complete; the SDK has the device.\n");

	/*
	 * The mapping is deliberately NOT torn down here.
	 *
	 * soc_attach starts threads of the SDK's own -- bcmPOLL among them, since
	 * this build runs in polled interrupt mode -- and they keep reading the
	 * BAR through our vectors. Closing the BDE on the way out unmapped it
	 * underneath them, and bcmPOLL segfaulted on its next register read while
	 * this function was busy reporting success. The attach genuinely worked;
	 * the teardown was what broke it, and it did so somewhere nothing was
	 * looking.
	 *
	 * Holding briefly is what makes that visible: a run that returns
	 * immediately cannot tell a healthy poller from one that is about to
	 * die. The daemon will hold it for good once it serves the socket.
	 */
	/*
	 * full is how far to take initialisation:
	 *   0  attach only
	 *   1  attach + soc_init, then stop -- a deliberate halfway point, so
	 *      the chip's state can be examined between the SOC layer coming up
	 *      and the BCM layer's first table write, which is where this
	 *      currently fails
	 *   2  the whole sequence, then survey the ports
	 */
	if (full >= 1) {
		printf("\nsoc_reset_init (resets the chip, then initialises)...\n");
		if (nosaic_sdk_soc_init(unit) != 0)
			return 1;
		printf("the SOC layer is up.\n");
	}
	if (full >= 2) {
		printf("\nbcm_attach and bcm_init...\n");
		if (nosaic_sdk_bcm_init(unit) != 0)
			return 1;
		printf("the chip is initialised and running.\n\n");
		nosaic_props_report_unused();
		nosaic_sdk_ports(unit);
	}

	printf("\nholding the device for %d seconds so the SDK's threads can run\n",
	       HOLD_SECONDS);
	sleep(HOLD_SECONDS);
	printf("still attached after %d seconds\n", HOLD_SECONDS);
	return 0;
}
#endif

#ifdef NOSAIC_WITH_SDK
/* Bring the chip up and stay running.
 *
 * Missing configuration is fatal rather than a warning: a datapath that comes
 * up without its port map initialises a chip that reaches no cage, and
 * reporting that as success is how a switch looks healthy and forwards
 * nothing. Better to fail here, where the service log says why.
 */
static int run_daemon(const char *bdf, char **confs, int nconf)
{
	struct nosaic_bde *b = &attached_dev;
	int unit, n, c, i;

	if (nosaic_bde_open(b, bdf) != 0)
		return 1;

	/*
	 * Named files rather than every *.conf in the directory.
	 *
	 * /etc/nosaic holds more than the datapath's configuration -- the network
	 * addresses are there too -- and reading a file that was never meant as
	 * SDK properties is harmless right up until somebody writes a line with an
	 * equals sign in it. Then a network setting silently becomes a chip
	 * property. Naming the files this daemon owns costs nothing and removes
	 * that entirely.
	 */
	(void)confs; (void)nconf;
	n = 0;
	for (i = 0; i < (int)(sizeof(datapath_conf) / sizeof(datapath_conf[0])); i++) {
		char path[512];

		snprintf(path, sizeof(path), "%s/%s", SHIPPED_CONF_DIR, datapath_conf[i]);
		if ((c = nosaic_props_load(path)) > 0) {
			printf("config     %d properties from %s\n", c, path);
			n += c;
		}
		snprintf(path, sizeof(path), "%s/%s", PERSIST_CONF_DIR, datapath_conf[i]);
		if ((c = nosaic_props_load(path)) > 0) {
			printf("config     %d properties from %s\n", c, path);
			n += c;
		}
	}
	if (n <= 0) {
		fprintf(stderr, "nosd: no configuration in %s or %s\n",
			SHIPPED_CONF_DIR, PERSIST_CONF_DIR);
		return 1;
	}
	if (nosaic_props_get_unit("portmap_1", 0) == NULL) {
		fprintf(stderr,
			"nosd: no port map, so the chip would initialise and reach no\n"
			"      front-panel cage. The map is generated from your own switch\n"
			"      and is not shipped -- it describes this board's wiring:\n"
			"\n"
			"        tools/mkportmap.sh <switch-ip>  > %s/portmap.conf\n"
			"        tools/mkpolarity.sh <switch-ip> > %s/polarity.conf\n"
			"\n"
			"      That directory is on the data partition, so they survive an\n"
			"      upgrade and a rollback.\n",
			PERSIST_CONF_DIR, PERSIST_CONF_DIR);
		return 1;
	}

	/*
	 * Break out any QSFP cage the board asks for, BEFORE the attach that
	 * reads the map. There is no runtime switch for this on this chip, so
	 * doing it later would do nothing and say it had worked.
	 */
	if (nosaic_portmode_apply(0) < 0)
		return 1;

	unit = nosaic_sdk_attach(b, b->dev_id, b->rev_id);
	if (unit < 0)
		return 1;
	if (nosaic_sdk_soc_init(unit) != 0)
		return 1;

	/*
	 * ⚠ THE COPPER PHY BUS GOES IN BEFORE bcm_init, NOT AFTER.
	 *
	 * bcm_init is where the SDK first probes for external PHYs. Installing
	 * the bus afterwards is too late: that probe has already run with no
	 * way to reach the parts, bound every copper port to the chip's
	 * internal SerDes, and cached the result. A later bcm_port_probe then
	 * reports success and changes nothing -- the port's reported ability
	 * stays byte for byte what the internal SerDes said, which is how this
	 * was spotted.
	 */
	nosaic_phybus_install(unit, nosaic_props_get("scd_pci"));

	if (nosaic_sdk_bcm_init(unit) != 0)
		return 1;

	/*
	 * Which properties the SDK actually read, reported here and not only on
	 * the --init diagnostic path.
	 *
	 * It was on the diagnostic path alone, which is the one place it is least
	 * needed: somebody running --init is already watching. The daemon is what
	 * runs the switch, and a property it loaded but the SDK never looked at is
	 * silent -- counted as configuration, reported as configuration, and
	 * having no effect whatever. A misspelt key and a wrong value produce the
	 * same picture from outside, and the difference is the whole question when
	 * a port will not come up.
	 *
	 * After bcm_init, because that is when the SDK has finished asking.
	 */
	nosaic_props_report_unused();

	/*
	 * ⚠ THE EXTERNAL PHYs BEFORE THE PORTS, NOT AFTER.
	 *
	 * Binding a PHY driver re-initialises the port it belongs to: the
	 * enable goes away and the frame size returns to the chip default. So
	 * doing it after the ports are up and the taps are built silently
	 * undoes both -- the ports read `admin down` with a 9412-byte frame
	 * size, having been enabled at 1522 a moment earlier, and nothing
	 * reports an error.
	 *
	 * This is the order the working predecessor uses, and the reason is the
	 * same one: probe, then configure, then enable.
	 */
	nosaic_phy_bind(unit);
	nosaic_phybus_report();

	nosaic_sdk_ports(unit);

	/*
	 * Put the routed ports on the Linux network stack.
	 *
	 * Which ports and under what names is board configuration, not a decision
	 * for this daemon: a switch's front panel is wired to its chip in a way
	 * only the board knows. Stated as
	 *
	 *     tap_et1=1
	 *     tap_et2=2
	 *
	 * so the name is the interface a routing daemon will be configured
	 * against and the value is the logical port behind it.
	 */
	{
		struct tap_spec specs[NOSAIC_MAX_TAPS];
		char names[NOSAIC_MAX_TAPS][32];
		int ntap = 0, i, tap_props = 0;

		for (i = 0; i < nosaic_props_count(); i++) {
			const char *name = nosaic_props_name(i);
			const char *val = nosaic_props_value(i);

			if (name == NULL || strncmp(name, "tap_", 4) != 0)
				continue;
			tap_props++;
			if (ntap >= NOSAIC_MAX_TAPS)
				continue;
			snprintf(names[ntap], sizeof(names[ntap]), "%s", name + 4);
			specs[ntap].name = names[ntap];
			/* "<port>", "<port>:<vlan>" or "<port>:<vlan>:<mtu>" */
			specs[ntap].port = atoi(val);
			specs[ntap].vlan = 0;
			specs[ntap].mtu = 0;
			{
				const char *colon = strchr(val, ':');

				if (colon != NULL) {
					specs[ntap].vlan = atoi(colon + 1);
					colon = strchr(colon + 1, ':');
					if (colon != NULL)
						specs[ntap].mtu = atoi(colon + 1);
				}
			}
			ntap++;
		}

		/* Say so rather than quietly making fewer. The array above is
		 * sized to the bridge's own limit, so this only fires on a board
		 * that really does declare more ports than one datapath can
		 * carry -- but when it fires, the alternative is a switch short
		 * some ports with nothing anywhere saying which or why. */
		if (tap_props > NOSAIC_MAX_TAPS) {
			fprintf(stderr, "nosd: %d tap_ properties but at most %d "
				"taps are supported; refusing rather than "
				"silently bridging %d\n",
				tap_props, NOSAIC_MAX_TAPS, NOSAIC_MAX_TAPS);
			return 1;
		}

		if (ntap == 0) {
			printf("nosd: no tap_<name>=<port> properties, so no port is on "
			       "the Linux stack.\n"
			       "      A routing daemon has nothing to run over until "
			       "there is at least one.\n");
		} else if (nosaic_tap_start(unit, specs, ntap) < 0) {
			fprintf(stderr, "nosd: could not bridge ports to Linux\n");
			return 1;
		}

		/* Give the chip a router interface matching each tap, so the routes
		 * the kernel learns can be programmed into it. Read back from the
		 * bridge rather than from the properties again: a router interface
		 * whose MAC differs from the tap's answers ARP and then drops
		 * everything sent to the address it answered with. */
		for (i = 0; i < nosaic_tap_count(); i++) {
			const char *name;
			unsigned char mac[6];
			int port, vlan, mtu;

			if (nosaic_tap_info(i, &name, &port, &vlan, &mtu, mac) != 0)
				continue;
			if (vlan <= 0) {
				printf("l3: %s has no vlan, so it gets no router "
				       "interface\n", name);
				continue;
			}
			if (nosaic_l3_add_intf(unit, name, port, vlan, mac,
					       mtu) != 0)
				fprintf(stderr, "l3: no router interface for %s; "
					"routes via it cannot be programmed\n", name);
		}

		/* The front panel. Not fatal if it fails: a switch with a dark
		 * panel still forwards, and a board with no SCD has no panel. */
		nosaic_led_start(unit);
		/* After the ports are enabled: a port that is not enabled cannot
		 * report a link, and this has nothing to match until one does.
		 * The PHYs themselves were bound earlier, before the enable. */
		nosaic_phy_start(unit);

		/*
		 * The socket, so the CLI can ask this chip what it holds.
		 *
		 * Same server and same protocol as nosd-tdp: the whole point of
		 * the contract is that `nosaic show ports` does not know which
		 * silicon is answering. Serving it here is what lets the CLI test
		 * run against this board unmodified, which is the gate this board
		 * was chosen to prove.
		 */
		nosaic_query_set_dmapool(&b->pool);
		nosaic_query_start(unit, NOSAIC_QUERY_SOCKET);

		printf("nosd: the datapath is up on unit %d\n", unit);
		fflush(stdout);

		if (ntap > 0) {
			/* Pumping is this thread's job from here; the SDK's own threads
			 * handle the other direction. */
			/* The routing table has to be mirrored whether or not any
			 * packet arrives, so it runs on the pump's timeout. */
			/* The periodic work has a thread of its own; the pump
			 * blocks on packets alone. A NULL tick makes poll()
			 * wait indefinitely rather than waking for work it no
			 * longer has. */
			{
				pthread_t th;
				int trv = pthread_create(&th, NULL,
							 periodic, NULL);

				if (trv != 0) {
					fprintf(stderr, "nosd: no periodic "
						"thread (%s); routes will not "
						"be mirrored\n", strerror(trv));
					return 1;
				}
			}
			nosaic_tap_pump(NULL, 0);
			return 1;   /* pump only returns on failure */
		}
	}

	/* The SDK's threads do the work from here. */
	for (;;)
		pause();
}
#endif

int main(int argc, char **argv)
{
	/*
	 * Unbuffered, because this program can abort inside the SDK.
	 *
	 * Redirected to a file, stdout is fully buffered, so everything printed
	 * before an abort is discarded with the buffer -- and what is left is the
	 * SDK's own writes, which go to stderr, ending at whatever it happened to
	 * say last. That makes the abort look like it happened somewhere it did
	 * not.
	 */
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	if (argc == 3 && strcmp(argv[1], "--probe") == 0)
		return probe(argv[2]);
#ifdef NOSAIC_WITH_SDK
	/* argv[1] is only there to compare against if there IS one. Reaching
	 * strcmp with argc == 1 dereferences NULL, which is how the daemon mode
	 * added below crashed in libc before printing a single line -- a segfault
	 * with no output looks like a broken binary rather than a missing guard. */
	if (argc >= 2) {
		static char *defconf[] = { (char *)DEFAULT_ASIC_CONF };
		int full = -1;

		if (strcmp(argv[1], "--attach") == 0)   full = 0;
		if (strcmp(argv[1], "--soc-init") == 0) full = 1;
		if (strcmp(argv[1], "--init") == 0)     full = 2;
		if (full >= 0 && argc >= 3) {
			if (argc == 3)
				return attach(argv[2], defconf, 1, full);
			return attach(argv[2], &argv[3], argc - 3, full);
		}
	}
#endif
	if (argc == 2 && strcmp(argv[1], "--version") == 0) {
		printf("nosd-td2 (bring-up) for BCM%04x, Broadcom vendor %#06x\n",
		       TD2_DEVICE, TD2_VENDOR);
		return 0;
	}
	/*
	 * No arguments: run as the datapath daemon.
	 *
	 * Bring the chip all the way up from the board's own configuration and
	 * then stay running. Staying is not idleness -- the SDK's own threads live
	 * in this process, linkscan among them, and link state stops being
	 * maintained the moment it exits. A datapath that initialises and returns
	 * leaves ports that come up once and never change again.
	 *
	 * It does not serve the switch-api socket yet, which is the next piece of
	 * work rather than something this pretends to do.
	 */
	if (argc == 1) {
		char *confs[] = {
			(char *)DEFAULT_ASIC_CONF,
			(char *)"/etc/nosaic/portmap.conf",
			(char *)"/etc/nosaic/polarity.conf",
		};
		char found[32];
		const char *bdf = DEFAULT_ASIC_BDF;

		if (td2_find(found, sizeof(found)) == 0)
			bdf = found;
		else
			fprintf(stderr, "nosd: no %04x:%04x-%04x on the PCI bus; "
				"trying %s\n", TD2_VENDOR, TD2_DEVICE_FIRST,
				TD2_DEVICE_LAST, bdf);
		return run_daemon(bdf, confs,
				  (int)(sizeof(confs) / sizeof(confs[0])));
	}

	usage();
	return 2;
}
