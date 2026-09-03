/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_CLI_HAL_H
#define NOSAIC_CLI_HAL_H

/*
 * The board hardware a switch has to talk to that is not the switch chip:
 * fans, power supplies, LEDs, temperatures.
 *
 * This mirrors internal/platformhal in the Go CLI deliberately, because the two
 * present the same commands to an operator and the fastest way for them to
 * drift is for the shapes underneath to differ.
 *
 * It exists at all because the Go CLI cannot run everywhere. The gc toolchain
 * has ppc64 and ppc64le and has never had 32-bit big-endian PowerPC, so on the
 * AS5610 there is no `nosaic` unless something else provides it. That is the
 * whole reason for a second implementation, and the reason it is kept small:
 * the build host is x86_64 by design, so nothing that only runs there -- the
 * image builder, the package builder -- needs to be here.
 */

#include <stddef.h>
#include <stdint.h>

struct nosaic_temp {
	char name[32];
	int  milli_c;
	int  crit_milli_c;   /* 0 when the sensor does not publish one */
};

struct nosaic_psu {
	int  index;          /* 1-based, as the bays are labelled */
	int  fitted;
	int  powered;
	char raw[32];        /* what the decode was made from */
};

/*
 * A board driver. Every entry may be NULL, and the caller reports that as
 * "this board does not offer it" rather than failing -- a switch with no
 * watchdog is a real switch.
 */
struct nosaic_hal {
	const char *name;

	/* Probe returns 0 when this driver's hardware is present. It is how the
	 * right board is chosen without asking a configuration file to be
	 * correct: the controller either answers or it does not. */
	int (*probe)(void);

	int (*identify)(char *buf, size_t len);

	/* Temperatures, into an array the caller sizes. Returns how many were
	 * written, or -1. */
	int (*temps)(struct nosaic_temp *out, int max);

	int (*psus)(struct nosaic_psu *out, int max);

	/* Fan duty as a percentage, and the floor below which this board will
	 * not go. A controller that can be told to stop the fans is one that
	 * will eventually be told to stop them by a bug. */
	int (*fan_get)(void);
	int (*fan_set)(int percent);
	int (*fan_floor)(void);

	int (*leds)(char *buf, size_t len);
};

const struct nosaic_hal *nosaic_hal_find(void);

/* The AS5610-52X's CPLD. */
extern const struct nosaic_hal nosaic_hal_as5610;

#endif
