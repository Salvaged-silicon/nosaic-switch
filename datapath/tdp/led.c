/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Front-panel port LEDs.
 *
 * A switch whose ports work and whose panel is dark reads, to anyone standing
 * in front of the rack, as a switch that is not working. On this board nothing
 * lights the panel unless we do: the chip comes out of reset with both LED
 * processors halted and their program memory full of whatever the RAM powered
 * up holding.
 *
 * The BCM56846 drives the front-panel LED serial chain from two LED
 * microprocessors, LEDUP0 and LEDUP1. There are two ways to render link state,
 * and EdgeNOS tried both on this exact board:
 *
 *  1. Fully autonomous. Load the factory microcode and let the chip scan its
 *     own per-port MAC status into PORTSTATUS. This is what the vendor OS does,
 *     and it needs two things we do not have: the board's PORT_ORDER_REMAP,
 *     which is only obtainable from a live capture of that OS, and chip link
 *     state that is exactly right at all times. EdgeNOS got a panel with more
 *     ports lit than had carrier, in the wrong places.
 *
 *  2. Software driven, which is this. A tiny "passthrough" microcode shifts 64
 *     chain bits straight out of data RAM, and the poll below writes those bits
 *     from link state the SDK is asked for directly. It cannot be fooled by a
 *     remap nobody has, because there is no remap in it -- the mapping from
 *     panel port to chain bit is data in this file, and it was verified by
 *     lighting ports and having somebody look at the panel.
 *
 * The second is slower to react by up to a poll interval. That is the correct
 * trade for a lamp: an LED that is right one second late beats one that is
 * wrong indefinitely.
 *
 * Register map, from bcm56840_a0_defs.h:
 *
 *   LEDUP0   ctrl 0x1000   data RAM 0x1400   program RAM 0x1800
 *   LEDUP1   ctrl 0x2000   data RAM 0x2400   program RAM 0x2800
 *
 * Both RAMs are byte-wide and addressed one byte per 32-bit word, so an index
 * steps by four. CTRL bit 0 runs the processor. All of these are far below the
 * 256 KB BAR0 this chip presents, so they are reached through the ordinary
 * mapping -- no window register is involved, which is worth stating because the
 * kernel BDE this was first written against routed anything above 0x1000
 * through a PAXB sub-window and that has no equivalent here.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>

#include <bcm/error.h>
#include <bcm/stat.h>
#include <bcm/port.h>
#include <bcm/stg.h>
#include <bcm/vlan.h>

#include "led.h"
#include "sdk.h"

/* One LED microprocessor's register block. */
struct ledup {
	const char *name;
	uint32_t    ctrl;
	uint32_t    data_ram;
	uint32_t    prog_ram;
};

static const struct ledup ledups[2] = {
	{ "LEDUP0", 0x1000, 0x1400, 0x1800 },
	{ "LEDUP1", 0x2000, 0x2400, 0x2800 },
};

#define LED_CTRL_EN      0x1u
#define LED_PROG_SIZE    0x100      /* 256 instruction bytes */
#define LED_CHAIN_BASE   0xA0       /* data RAM offset the microcode reads from */
#define LED_CHAIN_BYTES  8          /* 64 chain bits per processor */
#define LED_MAX_PORT     52

/*
 * The passthrough microcode.
 *
 * Shifts 64 bits from data RAM 0xA0..0xA7 out to the LED chain, so that byte
 * 0xA0+N bit i becomes chain bit N*8+i. It does nothing else: it reads no port
 * status and makes no decisions, which is the entire point -- every decision
 * belongs to led_poll below, where it can be read and changed.
 *
 * Assembled from utils/leddance/passthrough.asm in the newnos tree with
 * Cumulus's ledasm. 52 meaningful bytes, zero-padded to the program RAM size.
 * Kept as bytes rather than reassembled at build time because that would put an
 * assembler nobody has on the critical path of building a switch.
 */
static const uint8_t led_passthrough[LED_PROG_SIZE] = {
	0x02, 0xA0, 0x60, 0xFE, 0x06, 0xFE, 0x10, 0x15,
	0x1A, 0x00, 0x27, 0x87, 0x1A, 0x01, 0x27, 0x87,
	0x1A, 0x02, 0x27, 0x87, 0x1A, 0x03, 0x27, 0x87,
	0x1A, 0x04, 0x27, 0x87, 0x1A, 0x05, 0x27, 0x87,
	0x1A, 0x06, 0x27, 0x87, 0x1A, 0x07, 0x27, 0x87,
	0x06, 0xFE, 0x80, 0x60, 0xFE, 0xD2, 0xA8, 0x74,
	0x04, 0x3A, 0x40, 0x00,
	/* remainder zero */
};

/*
 * Panel port -> (processor, amber chain bit). Green is the next bit up.
 *
 * This is the part that cannot be derived. The order is neither the SDK's port
 * numbering nor anything monotonic -- port 1 is on LEDUP1 at bit 34 and port 9
 * is on LEDUP0 at bit 2 -- because it follows how the board wires the serial
 * chain through the cages. It came from Cumulus's accton.py by way of EdgeNOS
 * and was confirmed on this board by lighting one bit at a time and reading
 * back which physical port lit.
 *
 * On this board the SDK's logical port number and the front-panel number are
 * the same, which is why this table is indexed directly by the port the SDK
 * hands us. config/taps.conf says the same thing from the other end
 * (tap_swp49=49), and the uplinks work, so the identity is load bearing
 * elsewhere and not an assumption made here.
 */
struct panel_led { uint8_t proc; uint8_t amber_bit; };

static const struct panel_led panel_ports[LED_MAX_PORT + 1] = {
	[1]  = {1, 34}, [2]  = {1, 32}, [3]  = {1, 38}, [4]  = {1, 36},
	[5]  = {1, 62}, [6]  = {1, 60}, [7]  = {1, 58}, [8]  = {1, 56},
	[9]  = {0,  2}, [10] = {0,  0}, [11] = {0,  6}, [12] = {0,  4},
	[13] = {0, 50}, [14] = {0, 48}, [15] = {0, 54}, [16] = {0, 52},
	[17] = {0, 46}, [18] = {0, 44}, [19] = {0, 42}, [20] = {0, 40},
	[21] = {0, 62}, [22] = {0, 60}, [23] = {0, 58}, [24] = {0, 56},
	[25] = {0, 38}, [26] = {0, 36}, [27] = {0, 34}, [28] = {0, 32},
	[29] = {0, 30}, [30] = {0, 28}, [31] = {0, 26}, [32] = {0, 24},
	[33] = {0, 14}, [34] = {0,  8}, [35] = {0, 10}, [36] = {0, 12},
	[37] = {0, 22}, [38] = {0, 20}, [39] = {0, 18}, [40] = {0, 16},
	[41] = {1, 44}, [42] = {1, 42}, [43] = {1, 40}, [44] = {1, 46},
	[45] = {1, 52}, [46] = {1, 50}, [47] = {1, 48}, [48] = {1, 54},
	[49] = {1, 26}, [50] = {1, 24}, [51] = {1, 30}, [52] = {1, 28},
};

static struct nosaic_tdp_bde *led_bde;
static int led_unit = -1;
static int led_on;

/* Per-port state carried between polls, so a failed query can be aged out
 * rather than acted on immediately. */
#define LED_UNKNOWN_GRACE 3
static uint8_t last_state[LED_MAX_PORT + 1];
static uint8_t unknown_run[LED_MAX_PORT + 1];
static unsigned long led_polls, led_unknown, led_unknown_said;

/*
 * Blink, and why the render rate and the counter rate are not the same number.
 *
 * A blinking port lamp means traffic, and that is not decoration: solid-green
 * on a port carrying nothing looks identical to solid-green on a port carrying
 * line rate, and the difference is usually the thing being diagnosed. EdgeNOS
 * blinked on this board and an operator who has used it will expect it.
 *
 * Knowing whether traffic moved means reading the chip's own per-port counters,
 * because hardware-forwarded frames never reach the CPU -- the tap counters
 * this daemon already keeps would leave a port at line rate looking idle, which
 * is worse than not blinking at all.
 *
 * So the two rates are split. Rendering runs at LED_TICK_US, fast enough for a
 * blink to read as activity rather than as a slow flash; the counters are
 * sampled once every LED_SAMPLE_TICKS of those, and the flag they set drives
 * the blink until the next sample. Only ports that HAVE link are sampled, which
 * on any real switch is a small fraction of the panel.
 */
#define LED_TICK_US        100000   /* render at 10 Hz */
#define LED_SAMPLE_TICKS   10       /* counters once a second */
#define LED_BLINK_TICKS    2        /* 200 ms half period, about 2.5 Hz */

#define LED_ACTIVE_HOLD    2        /* samples to keep blinking after the last frame */

static uint64_t last_pkts[LED_MAX_PORT + 1];
static uint8_t  active[LED_MAX_PORT + 1];
static uint8_t  active_hold[LED_MAX_PORT + 1];

static uint32_t reg_rd(uint32_t off)
{
	return nosaic_tdp_bde_rd(led_bde, off);
}

static void reg_wr(uint32_t off, uint32_t v)
{
	nosaic_tdp_bde_wr(led_bde, off, v);
}

static void ledup_run(const struct ledup *lp, int on)
{
	uint32_t ctrl = reg_rd(lp->ctrl);

	if (on)
		ctrl |= LED_CTRL_EN;
	else
		ctrl &= ~LED_CTRL_EN;
	reg_wr(lp->ctrl, ctrl);
}

/* Load the passthrough microcode into one processor and start it. */
static void ledup_load(const struct ledup *lp)
{
	int i;

	/* Halted first. The program RAM is being rewritten underneath a
	 * processor that would otherwise be executing it, and half of the old
	 * microcode with half of the new one is not a program. */
	ledup_run(lp, 0);
	for (i = 0; i < LED_PROG_SIZE; i++)
		reg_wr(lp->prog_ram + (uint32_t)i * 4, led_passthrough[i]);
	/* Start dark rather than with whatever the RAM held. */
	for (i = 0; i < LED_CHAIN_BYTES; i++)
		reg_wr(lp->data_ram + (uint32_t)(LED_CHAIN_BASE + i) * 4, 0);
	ledup_run(lp, 1);
}

/*
 * What colour a port should be showing.
 *
 * Green needs link AND forwarding, so a cable that is fine on a port the switch
 * is not using looks different from one that is working. That case is otherwise
 * invisible from the front of the rack, and it is the one worth having a lamp
 * for.
 *
 * Reading the forwarding state has a trap on this board specifically.
 * bcm_port_stp_get answers for the DEFAULT spanning-tree group, and every front
 * panel port here lives in a per-port service VLAN with an STG of its own --
 * which is exactly why bcm_port_stp_set was not enough to make this switch
 * forward in the first place. Asking the wrong STG would return a state that
 * has nothing to do with the port, and the failure would be a panel of the
 * wrong colour rather than an error.
 *
 * A failed STP query falls back to green. That is the opposite of the rule the
 * thermal loop uses, where an unreadable sensor is a safety matter; here
 * treating unknown as "not forwarding" would repaint the panel amber over a
 * transient and turn a lamp that means something into one that cries wolf.
 *
 * A failed LINK query is different again, and is reported as UNKNOWN for the
 * caller to age out -- see the note in the poll.
 */
enum led_state { LED_DARK = 0, LED_GREEN, LED_AMBER, LED_UNKNOWN };

static enum led_state port_colour(int port)
{
	int link = 0, stp = BCM_STG_STP_FORWARD;
	bcm_stg_t stg;

	if (bcm_port_link_status_get(led_unit, port, &link) != BCM_E_NONE)
		return LED_UNKNOWN;
	if (!link)
		return LED_DARK;

	if (bcm_vlan_stg_get(led_unit, NOSAIC_TDP_SERVICE_VLAN_BASE + port,
			     &stg) == BCM_E_NONE) {
		if (bcm_stg_stp_get(led_unit, stg, port, &stp) != BCM_E_NONE)
			stp = BCM_STG_STP_FORWARD;
	}
	return (stp == BCM_STG_STP_FORWARD) ? LED_GREEN : LED_AMBER;
}

static pthread_t led_thread;

static void *led_loop(void *arg)
{
	(void)arg;
	for (;;) {
		nosaic_led_poll();
		usleep(LED_TICK_US);
	}
	return NULL;
}

/*
 * Has this port moved a frame since the last sample?
 *
 * Four counters rather than one: unicast alone misses a port whose only traffic
 * is broadcast or multicast, which is exactly the state of a link that has come
 * up and is doing nothing but ARP and routing hellos -- the case where somebody
 * is most likely to be staring at the panel asking whether the cable works.
 *
 * A counter that cannot be read contributes nothing rather than being treated
 * as zero. Zero would make the total go backwards and register as activity on
 * the next sample, so an unreadable counter would blink a quiet port for ever.
 */
static int port_active(int port)
{
	static bcm_stat_val_t want[] = {
		snmpIfInUcastPkts, snmpIfInNUcastPkts,
		snmpIfOutUcastPkts, snmpIfOutNUcastPkts,
	};
	const int n = (int)(sizeof(want) / sizeof(want[0]));
	uint64 v[4];
	uint64_t total = 0;
	int i, moved;

	/*
	 * sync_multi_get, not bcm_stat_get.
	 *
	 * bcm_stat_get returns the SDK's accumulated value, which is refreshed
	 * on the SDK's own schedule rather than on ours -- so a sample can land
	 * between refreshes, see no change, and report a busy port as idle. That
	 * is not theoretical: under a continuous 20 pps ping this read "stop" in
	 * the middle of the stream, which on the panel is a lamp that gives up
	 * blinking while traffic is still flowing.
	 *
	 * The sync variant forces the update, and the multi form does it once
	 * for all four counters instead of four times. It is called only for
	 * ports that have link, once a second.
	 */
	memset(v, 0, sizeof(v));
	if (bcm_stat_sync_multi_get(led_unit, port, n, want, v) != BCM_E_NONE)
		return active[port];    /* unreadable: leave the lamp as it is */

	for (i = 0; i < n; i++)
		total += (uint64_t)v[i];

	/* First sample establishes the baseline rather than counting every frame
	 * since the port came up as one burst of activity. */
	moved = (last_pkts[port] != 0 && total != last_pkts[port]);
	last_pkts[port] = total;

	/*
	 * Hold the blink briefly after the last frame seen.
	 *
	 * An activity lamp is meant to say "this port is carrying traffic", not
	 * to resolve individual samples. Without the hold, anything bursty --
	 * which is most real traffic -- gives a lamp that stutters on and off
	 * once a second, and that reads as a fault rather than as use.
	 */
	if (moved)
		active_hold[port] = LED_ACTIVE_HOLD;
	else if (active_hold[port] > 0)
		active_hold[port]--;
	return active_hold[port] > 0;
}

int nosaic_led_start(int unit, struct nosaic_tdp_bde *b)
{
	uint32_t c0, c1;
	int i;

	if (b == NULL)
		return -1;
	led_bde = b;
	led_unit = unit;
	memset(last_state, 0, sizeof(last_state));
	memset(unknown_run, 0, sizeof(unknown_run));

	for (i = 0; i < 2; i++)
		ledup_load(&ledups[i]);

	c0 = reg_rd(ledups[0].ctrl);
	c1 = reg_rd(ledups[1].ctrl);
	if (!(c0 & LED_CTRL_EN) || !(c1 & LED_CTRL_EN)) {
		fprintf(stderr, "led: LED processors will not run "
			"(LEDUP0 ctrl=%#x LEDUP1 ctrl=%#x); panel stays dark\n",
			c0, c1);
		led_bde = NULL;
		return -1;
	}

	led_on = 1;
	printf("led: driving the front panel from link state "
	       "(LEDUP0 ctrl=%#x LEDUP1 ctrl=%#x)\n", c0, c1);
	fflush(stdout);

	/* Announced before the first render, so the per-port transitions it
	 * logs appear under that line rather than above it. */
	nosaic_led_poll();

	/*
	 * The panel gets its own thread.
	 *
	 * It used to hang off the one-second periodic work, which is the right
	 * rate for mirroring the FIB and far too slow for a lamp: a blink at
	 * that rate reads as a slow flash, not as traffic. It must not go on the
	 * packet thread either -- that is the mistake this daemon already made
	 * once and measured, in punt latency.
	 *
	 * What it does per tick is a handful of cached link-status reads and
	 * sixteen register writes. The counters, which are the only part that
	 * touches the chip properly, are sampled a tenth as often and only for
	 * ports that have link.
	 */
	if (pthread_create(&led_thread, NULL, led_loop, NULL) != 0) {
		/* Not fatal, but say which panel you are now looking at: one
		 * frozen on this first render rather than one following link. */
		fprintf(stderr, "led: no render thread; the panel will not "
			"follow link state\n");
		return -1;
	}
	return 0;
}

/*
 * Render link state to the chain.
 *
 * The whole picture is rewritten every pass rather than diffed, because the one
 * thing that cannot be trusted here is a read of the chain RAM -- see the note
 * on the write loop below.
 *
 * The processors' run bits ARE checked, and those reads are reliable: CTRL is
 * an ordinary register rather than RAM the microcode is walking, and it reads
 * 0xb3/0xf3 identically across as many samples as anyone cares to take. A
 * halted LEDUP is a dark panel that no amount of correct chain data will fix.
 *
 * Blink is an activity indication over the top of the colour -- see the note on
 * the sampling rates above. It costs one forced counter read per LINKED port
 * per second, which is a small fraction of a panel on any real switch, and it
 * is worth that: EdgeNOS blinked on this board, and a solid lamp cannot tell
 * a port carrying line rate from one carrying nothing.
 */
void nosaic_led_poll(void)
{
	uint8_t bits[2][LED_CHAIN_BYTES];
	int port, pr, i, lit = 0;
	int sample, blink_off;

	if (!led_on || led_bde == NULL)
		return;

	sample    = (led_polls % LED_SAMPLE_TICKS) == 0;
	blink_off = ((led_polls / LED_BLINK_TICKS) & 1);

	memset(bits, 0, sizeof(bits));

	for (port = 1; port <= LED_MAX_PORT; port++) {
		const struct panel_led *pl = &panel_ports[port];
		enum led_state st = port_colour(port);
		int bit;

		/*
		 * A query that failed is NOT darkness.
		 *
		 * This is what the first version of this file got wrong, and the
		 * panel is what showed it: ports 1, 2 and 4 blinked once a second
		 * while 6, 7 and 8 sat steady, and the once-a-minute counter dump
		 * -- which makes the same call and ignores its return value --
		 * reported all of them unchanged the whole time. Two readers of one
		 * SDK call disagreeing means the disagreement is in the readers.
		 *
		 * So a failure holds the last colour for a few polls and only then
		 * goes dark. Holding it forever would be the other bug, a lamp lit
		 * for a port that is not there; a few seconds of grace covers a
		 * transient without ever leaving a stale light for long.
		 */
		if (st == LED_UNKNOWN) {
			led_unknown++;
			if (unknown_run[port] < LED_UNKNOWN_GRACE) {
				unknown_run[port]++;
				st = last_state[port];
			} else {
				st = LED_DARK;
			}
		} else {
			unknown_run[port] = 0;
		}
		if ((uint8_t)st != last_state[port]) {
			static const char *n[] = { "dark", "green", "amber", "?" };
			printf("led: swp%d %s -> %s\n", port,
			       n[last_state[port] & 3], n[st & 3]);
			fflush(stdout);
		}
		last_state[port] = (uint8_t)st;

		if (st != LED_GREEN && st != LED_AMBER) {
			/* A dark port has no activity worth remembering, and its
			 * counters stop moving. Clearing this means a port that
			 * comes back up does not blink on the accumulated
			 * difference across the time it was down. */
			active[port] = 0;
			active_hold[port] = 0;
			last_pkts[port] = 0;
			continue;
		}

		if (sample)
			active[port] = (uint8_t)port_active(port);

		/* Blink is an ACTIVITY indication laid over the colour, not a
		 * colour of its own: amber that blinks is still amber, still
		 * saying the port is not forwarding. */
		if (active[port] && blink_off)
			continue;

		bit = (st == LED_AMBER) ? pl->amber_bit : pl->amber_bit + 1;
		bits[pl->proc][bit / 8] |= (uint8_t)(1u << (bit & 7));
		lit++;
	}

	/* Say how often the chip could not be asked. A panel that is quietly
	 * guessing should not look the same as one that is being told. */
	if (++led_polls % (60 * LED_SAMPLE_TICKS) == 0 &&
	    led_unknown != led_unknown_said) {
		printf("led: %lu link query failure(s) since start\n", led_unknown);
		fflush(stdout);
		led_unknown_said = led_unknown;
	}

	for (pr = 0; pr < 2; pr++) {
		const struct ledup *lp = &ledups[pr];

		if (!(reg_rd(lp->ctrl) & LED_CTRL_EN))
			ledup_load(lp);         /* halted: reload and restart */

		/*
		 * Written every pass, never compared against a read.
		 *
		 * The obvious cheaper loop -- read the byte, write only if it
		 * differs -- is what nosd-td2p does on the other board, and it is
		 * wrong here. THE LED PROCESSOR IS READING THIS RAM CONTINUOUSLY,
		 * and a host read of it while that is happening does not reliably
		 * return what was last written: the panel appeared to flap once a
		 * second on ports 1, 2 and 4, and the instrumented build showed the
		 * chip holding 0x18 in a byte this code had written 0x02 to and had
		 * not touched since, with the SDK reporting no link change at all.
		 * The comparison was reacting to a value nothing had written.
		 *
		 * That read-back-and-repair discipline is right on the 7050SX2,
		 * where the lamps are plain registers other things can scribble on.
		 * Here the RAM belongs to this driver alone, so there is nothing to
		 * repair and nothing worth reading. Sixteen writes a second is not
		 * a cost worth measuring against getting the panel wrong.
		 */
		for (i = 0; i < LED_CHAIN_BYTES; i++)
			reg_wr(lp->data_ram +
			       (uint32_t)(LED_CHAIN_BASE + i) * 4, bits[pr][i]);
	}
	(void)lit;
}

/* Dark rather than frozen on a stale picture. */
void nosaic_led_stop(void)
{
	int pr, i;

	if (!led_on || led_bde == NULL)
		return;
	for (pr = 0; pr < 2; pr++)
		for (i = 0; i < LED_CHAIN_BYTES; i++)
			reg_wr(ledups[pr].data_ram +
			       (uint32_t)(LED_CHAIN_BASE + i) * 4, 0);
	led_on = 0;
}
