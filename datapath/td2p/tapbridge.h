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
};

/* Create the taps and start receiving. Returns how many were created. */
int nosaic_tap_start(int unit, const struct tap_spec *specs, int n);

/* Pump frames from Linux to the wire. Does not return. */
void nosaic_tap_pump(void);

#endif
