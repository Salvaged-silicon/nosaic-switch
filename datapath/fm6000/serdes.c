/* SPDX-License-Identifier: Apache-2.0 */
/* Bringing one SerDes lane up. See serdes.h for provenance and caveats. */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "regs.h"
#include "sbus.h"
#include "serdes.h"

/*
 * Front-panel port to EPL, lane and SBus device.
 *
 * The EPL and lane come from the board's FDL. The SBus device ids were
 * confirmed on hardware: EPL 14 lane 0 is 0x49 and lane 1 is 0x4a, EPL 16
 * lane 0 is 0x45 -- consecutive within an EPL, which is what Table 9-4's
 * four-addresses-per-EPL says and an independent check on both.
 *
 * ⚠ EIGHT PORTS, NOT FIFTY-TWO. The EPL-to-SBus permutation is established
 * for EPL 14 and EPL 16 only. Extending this table without establishing the
 * rest would bury eight facts in forty-four guesses, and a lane enable sent
 * to the wrong SerDes looks exactly like a port that will not come up.
 */
static void nap_ms(long ms)
{
	struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&t, NULL);
}

static const struct fm_port ports[] = {
	/* port epl lane  dev   rxpol txpol  drive pre post   -- from the FDL */
	{  1,  14,  0,  0x49,   0,    1,      4,   0,  5 },
	{  2,  16,  0,  0x45,   0,    0,      4,   1,  5 },
	{  3,  14,  1,  0x4a,   0,    1,      4,   1,  5 },
	{  4,  16,  1,  0x46,   1,    0,      4,   1,  5 },
	{  5,  14,  2,  0x4b,   0,    1,      4,   0,  5 },
	{  6,  16,  2,  0x47,   1,    0,      4,   0,  5 },
	{  7,  14,  3,  0x4c,   0,    1,      4,   0,  5 },
	{  8,  16,  3,  0x48,   1,    0,      4,   0,  5 },
};

const struct fm_port *fm_port_lookup(int port)
{
	size_t i;

	for (i = 0; i < sizeof(ports) / sizeof(ports[0]); i++)
		if (ports[i].port == port)
			return &ports[i];
	return NULL;
}

int fm_lane_status(struct fm6000 *d, const struct fm_port *p, uint32_t *out)
{
	return fm_rd(d, FM6000_EPL_LANE(p->epl, p->lane), out);
}

static void set(struct fm_lane_report *rep, int step, int rv,
		const char *what, const char *note)
{
	rep->step[step].rv = rv;
	rep->step[step].what = what;
	rep->step[step].note = note;
	if (step > rep->reached)
		rep->reached = step;
}

/*
 * Read, modify, write one SerDes register.
 *
 * ⚠ The read and the write are not the same register. A SerDes register has
 * separate read and write views at one number, so this reads the observable
 * state, computes from it, and writes the control -- which is what the
 * vendor's own routine does. It is not verifiable by reading back, and
 * nothing here tries.
 */
static int rmw(struct fm6000 *d, uint8_t dev, uint8_t reg,
	       uint32_t and_m, uint32_t or_m, uint32_t xor_m)
{
	uint32_t v = 0;
	int rv;

	rv = fm_sbus_read(d, dev, reg, &v);
	if (rv != FM_OK)
		return rv;
	return fm_sbus_write(d, dev, reg, ((v & and_m) | or_m) ^ xor_m);
}

/*
 * Poll one SerDes register until every bit in `bits` is set.
 *
 * ⚠ THE BUDGET IS THE POINT. The vendor waits up to 4999 milliseconds here,
 * and the earlier replay-based attempt on this chassis issued about seven
 * reads and moved on -- three orders of magnitude short, which presents as a
 * lane that never comes up rather than as a timeout.
 */
#define LANE_WAIT_MS 5000

static int wait_bits(struct fm6000 *d, uint8_t dev, uint8_t reg, uint32_t bits)
{
	unsigned i;

	for (i = 0; i < LANE_WAIT_MS; i++) {
		uint32_t v = 0;
		int rv = fm_sbus_read(d, dev, reg, &v);

		if (rv != FM_OK)
			return rv;
		if ((v & bits) == bits)
			return FM_OK;
		nap_ms(1);
	}
	return FM_ETIMEOUT;
}

int fm_lane_enable(struct fm6000 *d, const struct fm_port *p,
		   struct fm_lane_report *rep)
{
	uint8_t dev = (uint8_t)p->dev;
	uint32_t eb = FM6000_EPL_LANE(p->epl, 0);
	int rv;

	memset(rep, 0, sizeof(*rep));

	/*
	 * ⚠ THE DEVICE RESET IS NOT OPTIONAL. SBus writes to a SerDes do not
	 * take without it, and the vendor's own boot capture issues exactly
	 * two in 389,809 lines -- to the two SerDes belonging to the two ports
	 * that worked. A capture of a boot where fifty cages are empty does
	 * not show you the step the other two needed.
	 */
	rv = fm_sbus_device_reset(d, dev);
	set(rep, FM_LANE_RESET, rv, "SBus device reset", NULL);
	if (rv != FM_OK)
		return rv;

	/* Polarity before enable, as the vendor does. Which lanes are inverted
	 * is board routing and comes from the port table. */
	if ((rv = rmw(d, dev, 11, ~0x4u, p->txpol ? 0x4u : 0, 0)) == FM_OK)
		rv = rmw(d, dev, 7, ~0x10u, p->rxpol ? 0x10u : 0, 0);
	set(rep, FM_LANE_POLARITY, rv, "regs 11, 7: tx and rx polarity", NULL);
	if (rv != FM_OK)
		return rv;

	/* Hold the RX datapath down while the rate is selected. */
	rv = rmw(d, dev, 34, ~0x3u, 0, 0);
	set(rep, FM_LANE_QUIESCE, rv, "reg 34: hold RX datapath in reset", NULL);
	if (rv != FM_OK)
		return rv;

	/*
	 * Rate select and lane enable. This is the step the first attempt
	 * skipped, and skipping it is why the PLL had nothing to lock to:
	 * reg 0 carries the rate code and the enable bit, and 54 and 59 the
	 * 10G divider.
	 */
	if ((rv = rmw(d, dev, 0, ~0x7fu, (0x1bu << 1) | 1u, 0)) == FM_OK &&
	    (rv = fm_sbus_write(d, dev, 29, 0)) == FM_OK &&
	    (rv = rmw(d, dev, 54, ~0x7fu, 0x40u, 0)) == FM_OK &&
	    (rv = rmw(d, dev, 59, ~0x7fu, 0x40u, 0)) == FM_OK)
		rv = rmw(d, dev, 23, ~0x1fu, 0x10u, 0);
	set(rep, FM_LANE_RATE, rv,
	    "regs 0, 29, 54, 59, 23: 10G rate select and lane enable", NULL);
	if (rv != FM_OK)
		return rv;
	nap_ms(7);

	rv = rmw(d, dev, 34, ~0u, 0x3u, 0);
	set(rep, FM_LANE_RELEASE, rv, "reg 34: release RX datapath", NULL);
	if (rv != FM_OK)
		return rv;

	/*
	 * ⚠ PLL lock is reg 15 BIT 3, and bit 3 alone. An earlier version of
	 * this waited on bits 0 and 3 together, from a superseded reading, and
	 * timed out for five seconds on a chip whose PLL was already locked --
	 * bit 0 does not set on this part.
	 */
	rv = wait_bits(d, dev, 15, 0x8u);
	set(rep, FM_LANE_PLL, rv, "wait reg 15 bit 3: PLL lock",
	    rv == FM_ETIMEOUT ? "the rate select above it is where to look" : NULL);
	if (rv != FM_OK)
		return rv;

	if ((rv = rmw(d, dev, 6, ~0u, 0x8u, 0)) == FM_OK)
		rv = rmw(d, dev, 3, ~0u, 0x1u, 0);
	set(rep, FM_LANE_TXRX, rv, "regs 6, 3: RX and TX enable", NULL);
	if (rv != FM_OK)
		return rv;

	if ((rv = rmw(d, dev, 31, ~0x7eu, (8u & 0x3f) << 1, 0)) == FM_OK)
		rv = rmw(d, dev, 38, ~0u, 0x1u, 0);
	set(rep, FM_LANE_SIGDET, rv,
	    "regs 31, 38: signal-detect threshold and commit", NULL);
	if (rv != FM_OK)
		return rv;

	/*
	 * ⚠ THE EPL LANE MUST BE ACTIVE BEFORE THE TX EQUALISER WILL LATCH.
	 * The vendor's SBus write path checks the EPL's per-lane status before
	 * letting a write through, so the order here is load-bearing: enable
	 * the lane in the EPL, THEN write the equaliser.
	 *
	 * The Active bit is 1 << (19 + lane) in EPL_CFG_A, and the PCS type
	 * goes in EPL_CFG_B as four bits per lane.
	 */
	{
		uint32_t a = 0;

		uint32_t b = 0;

		/*
		 * ⚠ READ-MODIFY-WRITE, NOT A PLAIN WRITE. Four lanes share this
		 * register, so writing the selector on its own takes the other
		 * three down with it -- and it also clears the upper bits the
		 * boot sequence left there. Writing 3 absolutely here left
		 * CFG_B at 0x00000003 against a forwarding chip's 0x00090033,
		 * which is the difference between configuring one lane and
		 * wiping the EPL.
		 *
		 * Bit 16 is set because a forwarding chip has it and a booted
		 * one does not; what it selects is not established. [UNKNOWN]
		 */
		rv = fm_rd(d, eb + FM6000_EPL_CFG_SLOT + FM6000_EPL_CFG_B_OFF, &b);
		if (rv == FM_OK) {
			b &= ~(0xfu << (4 * p->lane));
			b |= (uint32_t)FM6000_EPL_PCS_10GBASE_R << (4 * p->lane);
			b |= 1u << 16;
			rv = fm_wr(d, eb + FM6000_EPL_CFG_SLOT + FM6000_EPL_CFG_B_OFF, b);
		}
		if (rv == FM_OK)
			rv = fm_rd(d, eb + FM6000_EPL_CFG_SLOT + FM6000_EPL_CFG_A_OFF, &a);
		if (rv == FM_OK)
			rv = fm_wr(d, eb + FM6000_EPL_CFG_SLOT + FM6000_EPL_CFG_A_OFF,
				   a | (1u << (19 + p->lane)));
	}
	set(rep, FM_LANE_EPL, rv, "EPL_CFG_B PcsSel and EPL_CFG_A Active", NULL);
	if (rv != FM_OK)
		return rv;

	/* The transmit equaliser, from the board's own per-port routing data. */
	{
		uint32_t r61 = 0, r62 = 0, r65 = 0;

		if ((rv = fm_sbus_read(d, dev, 61, &r61)) == FM_OK &&
		    (rv = fm_sbus_read(d, dev, 62, &r62)) == FM_OK &&
		    (rv = fm_sbus_read(d, dev, 65, &r65)) == FM_OK) {
			r61 = (r61 & ~0x3cu) | ((uint32_t)(p->drive & 0xf) << 2);
			r65 = (r65 & ~0x0cu) | ((uint32_t)((p->pre >> 2) & 1) << 2);
			r62 = (r62 & ~0xffu) | ((uint32_t)(p->post & 0xf) << 4) |
			      ((uint32_t)(p->pre & 0x3) << 2) | 0x3u;
			if ((rv = fm_sbus_write(d, dev, 61, r61)) == FM_OK &&
			    (rv = fm_sbus_write(d, dev, 62, r62)) == FM_OK)
				rv = fm_sbus_write(d, dev, 65, r65);
		}
	}
	set(rep, FM_LANE_TXEQ, rv, "regs 61, 62, 65: transmit equaliser", NULL);
	if (rv != FM_OK)
		return rv;

	/*
	 * ⚠ THE DATAPATH ENABLE, and dropping it is why the receiver saw
	 * nothing. Everything above configures the lane; this is what puts the
	 * RX and TX datapaths into service. Without it the transmitter runs --
	 * SerXmit sets -- and signal detect never asserts no matter what light
	 * is arriving, which is a confusing failure because the optic reports
	 * healthy receive power the whole time.
	 */
	rv = rmw(d, dev, 13, ~0u, 0x11u, 0);
	set(rep, FM_LANE_DATAPATH, rv, "reg 13: datapath enable", NULL);
	if (rv != FM_OK)
		return rv;

	/*
	 * ⚠ THE ACCEPTANCE TEST IS LANE_STATUS, NOT SIGNAL DETECT.
	 *
	 * This used to wait on reg 20 bit 6 and report a timeout. That wait was
	 * wrong: a port that links and forwards reads reg 20 = 0x14 with bit 6
	 * CLEAR, exactly as a dark one does -- measured on this chassis on both
	 * a working and a non-working port, which is why sweeping the threshold
	 * across its whole range moved nothing. There was nothing to move.
	 *
	 * What separates the two is the lane's own status word at +0x38:
	 * 0x940 on a lane that has locked, 0x00000000 on one that has not. So
	 * that is what is waited on, and PORT_STATUS 0x8c0 is its confirmation
	 * at the port level.
	 */
	{
		uint32_t ls = 0;
		unsigned k;

		rv = FM_ETIMEOUT;
		for (k = 0; k < LANE_WAIT_MS; k++) {
			if (fm_rd(d, FM6000_EPL_LANE(p->epl, p->lane) + 0x38, &ls) != FM_OK)
				break;
			if (ls != 0) {
				rv = FM_OK;
				break;
			}
			nap_ms(1);
		}
		rep->lane_status = ls;
	}
	set(rep, FM_LANE_LOCK, rv, "wait lane +0x38: receiver lock",
	    rv == FM_ETIMEOUT ? "stays 0x00000000 -- the receiver is not "
				"recovering the signal" : NULL);

	/* One-shot DFE kick. Harmless if signal detect never came. */
	{
		uint32_t v = 0;

		if (fm_sbus_read(d, dev, 42, &v) == FM_OK)
			(void)fm_sbus_write(d, dev, 42, (v & ~0x6u) | 0x2u);
	}

	(void)fm_lane_status(d, p, &rep->port_status);
	return rv;
}

/*
 * How many times to toggle the mailbox before giving up.
 *
 * The prior investigation reports the value settling within a few dozen
 * toggles when it settles at all. A hundred is generous and still finishes in
 * well under a second over the local bus -- which is the point of doing this
 * in C rather than from a shell, where each toggle was several process spawns
 * and a hundred of them did not finish in five minutes. [OURS]
 */
#define DFE_TOGGLES 100

int fm_lane_dfe(struct fm6000 *d, const struct fm_port *p, uint32_t *out)
{
	uint8_t dev = (uint8_t)p->dev;
	uint32_t v = 0;
	unsigned i;
	int rv;

	if (out != NULL)
		*out = 0;

	/* Clear the low five bits of 0x17 first, as the vendor's routine does
	 * before posting anything. */
	if ((rv = fm_sbus_read(d, dev, 0x17, &v)) != FM_OK)
		return rv;
	if ((rv = fm_sbus_write(d, dev, 0x17, v & ~0x1fu)) != FM_OK)
		return rv;

	if ((rv = fm_sbus_write(d, dev, 0x2a, 0x0e)) != FM_OK)
		return rv;
	if ((rv = fm_sbus_write(d, dev, 0x2b, 0x02)) != FM_OK)
		return rv;

	for (i = 0; i < DFE_TOGGLES; i++) {
		if ((rv = fm_sbus_write(d, dev, 0x2a, 0x16)) != FM_OK)
			return rv;
		if ((rv = fm_sbus_write(d, dev, 0x2a, 0x0e)) != FM_OK)
			return rv;
		if ((rv = fm_sbus_read(d, dev, 0x2b, &v)) != FM_OK)
			return rv;
		if (out != NULL)
			*out = v;
		/* 0x03 is what a working fibre lane holds; 0x04 is the value
		 * the vendor's own poll waits for. Either is settled. */
		if (v == 0x03 || v == 0x04)
			return FM_OK;
		nap_ms(1);
	}
	return FM_ETIMEOUT;
}

void fm_lane_report_print(const struct fm_lane_report *rep)
{
	int i;

	for (i = 1; i < FM_LANE__COUNT; i++) {
		const char *mark;

		switch (rep->step[i].rv) {
		case FM_OK:       mark = " ok "; break;
		case FM_ENOADDR:  mark = "skip"; break;
		case FM_ETIMEOUT: mark = "TIME"; break;
		default:          mark = "FAIL"; break;
		}
		if (rep->step[i].what == NULL)
			continue;
		printf("  %2d  %s  %s\n", i, mark, rep->step[i].what);
		if (rep->step[i].note != NULL)
			printf("            %s\n", rep->step[i].note);
	}
	printf("\n  LANE_STATUS 0x%08x   (0x940 on a locked lane, 0 on a dark one)\n",
	       rep->lane_status);
	printf("  PORT_STATUS 0x%08x   SerXmit(11)=%d RxLinkUp(6)=%d HeartbeatOk(7)=%d\n",
	       rep->port_status,
	       (rep->port_status >> 11) & 1,
	       (rep->port_status >> 6) & 1,
	       (rep->port_status >> 7) & 1);
	printf("  a forwarding lane reads 0x8c0: all three set\n");
	fflush(stdout);
}
