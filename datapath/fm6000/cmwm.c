/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Congestion-management watermarks.
 *
 * Six tables, one word per (port, traffic class), indexed port * 16 + class:
 *
 *     RXMP_PRIVATE    0x112800   76 ports x 12 classes
 *     RXMP_HOG        0x113000   76 x 16
 *     TXMP_PRIVATE    0x113800   80 x 16
 *     TXMP_HOG        0x114000   80 x 16
 *     RXMP_PAUSE_ON   0x115000   76 x 12
 *     RXMP_PAUSE_OFF  0x115800   76 x 12
 *
 * ⚠ THESE DECIDE WHEN THE CHIP DROPS AND WHEN IT ASSERTS PAUSE, AND A WRONG
 * VALUE DOES NOT FAIL LOUDLY. It appears as loss or head-of-line blocking
 * under load, which a ping and an iperf across an idle lab will not catch.
 * Treat a change here as a change to forwarding behaviour, not to a constant.
 *
 * ⚠ TWO OF THESE TABLES NEVER READ BACK. TXMP_PRIVATE and TXMP_HOG read
 * zero whatever is written to them -- on a chip that is forwarding traffic
 * as much as on one that is not, which is what establishes it rather than
 * assuming it. They are write-only. This file used to read that zero as the
 * writes having been discarded and reported a fault on a correctly
 * configured switch.
 *
 * ⚠ The host port here is physical 0, not the physical 1 that saf.c calls
 * the CPU port. That is not a slip -- see portmap.h, which records that the
 * chip's own tables disagree and why each one gets the port it asked for.
 *
 * The shape is the same in every table: a port is either carrying traffic or
 * it is not. A port that is carrying traffic gets a real limit; a port that
 * is not gets a limit it cannot reach, so that an unconfigured port can never
 * be the thing that makes the chip drop. Which ports those are comes from
 * this board's own map -- the 52 front-panel ports, the CPU port and the
 * internal port -- rather than from a list of port numbers, so a board with a
 * different map gets the right answer without anybody editing a table.
 *
 * PROVENANCE. Ported from EdgeNOS's fm6000_cmwm.c -- our own prior work on
 * this chassis -- and relicensed. There the port groups are materialised
 * ranges; here they are predicates over portmap.h, which is the same content
 * and one fewer thing to keep in step.
 */
#include "cmwm.h"
#include "pci.h"
#include "portmap.h"
#include "regs.h"

#define CMWM_RXMP_PRIVATE	0x112800u
#define CMWM_RXMP_HOG		0x113000u
#define CMWM_TXMP_PRIVATE	0x113800u
#define CMWM_TXMP_HOG		0x114000u
#define CMWM_RXMP_PAUSE_ON	0x115000u
#define CMWM_RXMP_PAUSE_OFF	0x115800u

/* One word per class, sixteen classes' worth of space per port. */
#define CMWM_PORT_STRIDE	16u
#define CMWM_TC			12u

/* The RX tables cover the 76 switch ports; the TX tables cover four more. */
#define CMWM_RX_PORTS		76u
#define CMWM_TX_PORTS		80u

/* A limit no queue reaches: the port is not carrying traffic and must never
 * be the reason the chip drops or pauses. */
#define CMWM_NO_LIMIT		0xffffffffu
#define CMWM_TX_NO_LIMIT	0x00003fffu

/*
 * Is this physical port one we actually forward on?
 *
 * The CPU port and the internal port count: they carry punted and recirculated
 * frames and need real watermarks like any other.
 */
static int cmwm_active(unsigned port)
{
	unsigned i;

	if (port == FM6000_ALTA_HOST || port == FM6000_ALTA_INTERNAL)
		return 1;
	for (i = 1; i <= FM6000_FRONT_PORTS; i++)
		if (fm6000_alta_of[i] == port)
			return 1;
	return 0;
}

/* Write one port's classes, and count them. */
static int cmwm_port(struct fm6000 *d, uint32_t base, unsigned port,
		     const uint32_t *vals, unsigned nclass, unsigned *n)
{
	unsigned c;
	int rv;

	for (c = 0; c < nclass; c++) {
		rv = fm_wr(d, base + port * CMWM_PORT_STRIDE + c, vals[c]);
		if (rv != FM_OK)
			return rv;
		(*n)++;
	}
	return FM_OK;
}

/* Fill a vector with one repeated value, which most of these are. */
static void cmwm_flat(uint32_t *v, unsigned n, uint32_t val)
{
	unsigned i;

	for (i = 0; i < n; i++)
		v[i] = val;
}

/*
 * One word per table whose value we know, kept so the end of the run can ask
 * the chip what it actually holds.
 *
 * Port 0 class 0 in every table: it is written in all six, and in five of
 * them the value is distinctive. RXMP_HOG is the exception -- every word in
 * it is the same no-limit value the table already held -- so verifying it
 * proves the address is readable rather than that the write landed. That is
 * worth having and is not worth more than it is.
 */
struct cmwm_witness {
	const char *name;
	uint32_t word, want;
	int readable;		/* 0 for a table that never reads back */
};

static void witness(struct cmwm_witness *w, const char *name,
		    uint32_t base, uint32_t want, int readable)
{
	w->name = name;
	w->word = base;		/* port 0, class 0 */
	w->want = want;
	w->readable = readable;
}

int fm_cmwm_init(struct fm6000 *d, struct fm_cmwm_report *rep)
{
	uint32_t active[CMWM_PORT_STRIDE], idle[CMWM_PORT_STRIDE];
	struct cmwm_witness wit[6];
	unsigned port, n = 0, i;
	int rv;

	if (rep != NULL) {
		rep->written = 0;
		rep->tables = 6;
		rep->readable = 0;
		rep->verified = 0;
		rep->first_bad = NULL;
	}

	/*
	 * RXMP_PRIVATE -- the per-port receive allowance.
	 *
	 * Only the first two classes have a real allowance; the rest are zero
	 * because nothing on this board maps traffic into them yet, and a
	 * class with no mapping and a nonzero allowance is buffer set aside
	 * for traffic that cannot arrive.
	 */
	cmwm_flat(active, CMWM_TC, 0x00000000u);
	active[0] = 0x0013003au;
	active[1] = 0x00060014u;
	cmwm_flat(idle, CMWM_TC, CMWM_NO_LIMIT);
	witness(&wit[0], "RXMP_PRIVATE", CMWM_RXMP_PRIVATE, active[0], 1);
	for (port = 0; port < CMWM_RX_PORTS; port++)
		if ((rv = cmwm_port(d, CMWM_RXMP_PRIVATE, port,
				    cmwm_active(port) ? active : idle,
				    CMWM_TC, &n)) != FM_OK)
			return rv;

	/* RXMP_HOG -- the shared-pool ceiling. No port is limited here; the
	 * private allowance above is what does the limiting. */
	cmwm_flat(active, CMWM_PORT_STRIDE, CMWM_NO_LIMIT);
	witness(&wit[1], "RXMP_HOG", CMWM_RXMP_HOG, CMWM_NO_LIMIT, 1);
	for (port = 0; port < CMWM_RX_PORTS; port++)
		if ((rv = cmwm_port(d, CMWM_RXMP_HOG, port, active,
				    CMWM_PORT_STRIDE, &n)) != FM_OK)
			return rv;

	/*
	 * TXMP_PRIVATE -- the per-port transmit allowance.
	 *
	 * The CPU port is its own case: it drains to host memory rather than
	 * to a wire, so it is given more of the eleven mapped classes than a
	 * front-panel port is.
	 */
	cmwm_flat(idle, CMWM_PORT_STRIDE, CMWM_TX_NO_LIMIT);
	/* Write-only: zero on a forwarding chip too. See cmwm.h. */
	witness(&wit[2], "TXMP_PRIVATE", CMWM_TXMP_PRIVATE, 0x00008014u, 0);
	witness(&wit[3], "TXMP_HOG", CMWM_TXMP_HOG, 0x000000d6u, 0);
	for (port = 0; port < CMWM_TX_PORTS; port++) {
		const uint32_t *v;
		unsigned c;

		if (port >= CMWM_RX_PORTS) {
			/* The four beyond the switch ports are not scheduled. */
			cmwm_flat(active, CMWM_PORT_STRIDE, 0x00000000u);
			v = active;
		} else if (port == FM6000_ALTA_HOST) {
			for (c = 0; c < CMWM_PORT_STRIDE; c++)
				active[c] = c < 11 ? 0x00008014u :
					    c < 15 ? 0x00008000u : 0x00000007u;
			v = active;
		} else if (cmwm_active(port)) {
			for (c = 0; c < CMWM_PORT_STRIDE; c++)
				active[c] = c < 9 ? 0x00008004u :
					    c < 15 ? 0x00008000u : 0x00000007u;
			v = active;
		} else {
			v = idle;
		}
		if ((rv = cmwm_port(d, CMWM_TXMP_PRIVATE, port, v,
				    CMWM_PORT_STRIDE, &n)) != FM_OK)
			return rv;
	}

	/* TXMP_HOG -- the transmit shared-pool ceiling, same port grouping. */
	for (port = 0; port < CMWM_TX_PORTS; port++) {
		const uint32_t *v;
		unsigned c;

		if (port >= CMWM_RX_PORTS) {
			cmwm_flat(active, CMWM_PORT_STRIDE, 0x00000111u);
			v = active;
		} else if (port == FM6000_ALTA_HOST) {
			for (c = 0; c < CMWM_PORT_STRIDE; c++)
				active[c] = c < 11 ? 0x000000d6u :
					    c < 15 ? 0x00000000u : 0x000000cdu;
			v = active;
		} else if (cmwm_active(port)) {
			for (c = 0; c < CMWM_PORT_STRIDE; c++)
				active[c] = c < 11 ? 0x000015f5u :
					    c < 15 ? 0x00000000u : 0x000000cdu;
			v = active;
		} else {
			v = idle;
		}
		if ((rv = cmwm_port(d, CMWM_TXMP_HOG, port, v,
				    CMWM_PORT_STRIDE, &n)) != FM_OK)
			return rv;
	}

	/*
	 * The two PAUSE thresholds -- the occupancy at which the chip starts
	 * asking the far end to stop, and the one at which it stops asking.
	 *
	 * ⚠ THE INTERNAL PORT IS NOT INCLUDED HERE, THOUGH IT IS IN
	 * RXMP_PRIVATE. Pausing has to reach a link partner and the internal
	 * port has none, so a threshold on it would arm a mechanism with
	 * nothing at the other end. This asymmetry is deliberate and matches
	 * the configuration captured from the running switch.
	 */
	cmwm_flat(active, CMWM_TC, 0x4000c000u);
	cmwm_flat(idle, CMWM_TC, CMWM_NO_LIMIT);
	witness(&wit[4], "RXMP_PAUSE_ON", CMWM_RXMP_PAUSE_ON, 0x4000c000u, 1);
	witness(&wit[5], "RXMP_PAUSE_OFF", CMWM_RXMP_PAUSE_OFF, 0x4000c000u, 1);
	for (port = 0; port < CMWM_RX_PORTS; port++) {
		int paused = cmwm_active(port) && port != FM6000_ALTA_INTERNAL;

		if ((rv = cmwm_port(d, CMWM_RXMP_PAUSE_ON, port,
				    paused ? active : idle, CMWM_TC, &n)) != FM_OK)
			return rv;
		if ((rv = cmwm_port(d, CMWM_RXMP_PAUSE_OFF, port,
				    paused ? active : idle, CMWM_TC, &n)) != FM_OK)
			return rv;
	}

	if (!fm_alive(d))
		return FM_EOFFBUS;

	/*
	 * Ask the chip what it kept.
	 *
	 * This is the whole point of the function reporting anything: the
	 * failure mode here is not an error code, it is a table that took
	 * 1280 words and held none of them, and which will show up months
	 * later as loss under load.
	 */
	for (i = 0; i < 6; i++) {
		uint32_t got = 0;

		if (!wit[i].readable)
			continue;
		if (rep != NULL)
			rep->readable++;
		if ((rv = fm_rd(d, wit[i].word, &got)) != FM_OK)
			return rv;
		if (got == wit[i].want) {
			if (rep != NULL)
				rep->verified++;
		} else if (rep != NULL && rep->first_bad == NULL) {
			rep->first_bad = wit[i].name;
		}
	}

	if (rep != NULL)
		rep->written = n;
	return FM_OK;
}
