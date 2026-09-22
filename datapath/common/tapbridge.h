/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_TAPBRIDGE_H
#define NOSAIC_TAPBRIDGE_H

/* The most taps one datapath can have.
 *
 * ⚠ EXPORTED SO CALLERS STOP INVENTING THEIR OWN, SMALLER ONE.
 *
 * nosaic_tap_start() below refuses rather than truncates, on the reasoning
 * that silently dropping taps produces a switch that is short some ports for
 * no stated reason. That protection is worthless if the caller has already
 * truncated to fill a fixed array: both td2 and td2p sized theirs at 8 and
 * stopped scanning properties at 8, so a board declaring all 54 of its ports
 * got 8, silently, and the network service then waited out its full deadline
 * for interfaces that were never going to be made.
 */
#define NOSAIC_MAX_TAPS 64

/* One hardware port presented to Linux under a name. */
struct tap_spec {
	const char *name;
	int         port;
	/* A dedicated VLAN for this routed port, or 0 to leave the port as it is.
	 * Without one the chip tags what it sends and a routed neighbour drops it
	 * -- counted as both received and dropped at the far end. */
	int         vlan;
	/* Interface MTU, or 0 for whatever the kernel gives a new tap. It has to
	 * match the neighbour: OSPF carries the MTU in its database description
	 * packets and refuses an adjacency when the two disagree, leaving it
	 * stuck in ExStart with no message saying why. */
	int         mtu;
};

/* Create the taps and start receiving. Returns how many were created. */
/*
 * The most taps this can build. Exported because the CALLER sizes the array it
 * collects them into, and when that number lived in two places they drifted:
 * tapbridge was raised to 64 and main.c's local array stayed at 8, so a board
 * declaring 52 ports silently got the first 8 and no warning. One definition,
 * and nosaic_tap_start refuses anything above it rather than truncating.
 */
#define NOSAIC_MAX_TAPS 64

/*
 * A board's own test for "this port really has link", for boards where the
 * SDK's link status alone is not one.
 *
 * On a board with external PHYs every copper port can report link with the
 * driver bound, cable or not -- "Link Up with Speed 0M!" -- so a diagnostic
 * gated on link alone fires for every unconnected port. Measured on a
 * 7050TX-64: 42 of 52 ports matched every interval, against one real fault.
 * A detector with that signal-to-noise is worse than none, because the one
 * line that matters is invisible in the other forty-one.
 *
 * The board supplies the discriminator because only it has one, and asking
 * the SDK per port per interval is not an option: a naive speed sweep across
 * 48 external PHYs is the exact call pattern that once killed copper receive
 * on that board. A board that already tracks "link AND a real negotiated
 * speed" for its own purposes can answer for free.
 *
 * Return 1 if the port genuinely has link, 0 if not. A port with no external
 * PHY should return 1 and let link status stand. Unset means link status is
 * trusted, which is right for a board whose ports are direct SerDes.
 */
void nosaic_tap_link_filter(int (*fn)(int port));

int nosaic_tap_start(int unit, const struct tap_spec *specs, int n);

/* How many taps exist, and what each one is.
 *
 * The chip has to be programmed to match the interface: same MAC, same VLAN,
 * same MTU. Reading it back from here rather than re-deriving it from the
 * properties means the two cannot disagree -- and a router interface whose MAC
 * differs from the tap's is a switch that answers ARP and then drops
 * everything sent to the address it answered with.
 */
int nosaic_tap_count(void);
int nosaic_tap_info(int i, const char **name, int *port, int *vlan, int *mtu,
		    unsigned char mac[6]);

/* Print what the chip did with each bridged port: frames in and out, and the
 * discards and errors that separate "never sent" from "sent and rejected". */
void nosaic_tap_stats(void);

/* Pump frames from Linux to the wire. Does not return.
 *
 * tick is called every tick_ms milliseconds, for work that has to happen
 * whether or not there is traffic -- mirroring the routing table, in
 * particular, which must not wait for a packet to arrive. */
void nosaic_tap_pump(void (*tick)(void), int tick_ms);

#endif
