/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_TD2P_TAPBRIDGE_H
#define NOSAIC_TD2P_TAPBRIDGE_H

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
