/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Bringing one SerDes lane up.
 *
 * PROVENANCE. This is a port of EdgeNOS's fm6000_serdes_enable.c, which is
 * our own prior work on this same chassis -- every commit to it is authored
 * by this project. It was GPL-2.0 there and is Apache-2.0 here by the
 * copyright holder's decision, not by any reinterpretation of that licence.
 * It was in turn recovered by disassembling the vendor's fm6000EnableSerDes:
 * facts about the silicon, written out in our own words, with nothing copied
 * from the SDK.
 *
 * ⚠ WHY A CAPTURE CANNOT SUBSTITUTE FOR THIS. The vendor's own 389,809-line
 * boot trace contains SerDes bring-up for exactly two devices -- the two
 * ports that had modules in them. A third port gets nothing at all, and its
 * EPL and MAC registers still end up byte-identical to a working lane, which
 * is why diffing them finds nothing. Beyond that, two of the steps are WAITS,
 * and a recording has no notion of waiting; and most of the writes are
 * read-modify-writes, so replaying one lane's resulting values into another
 * writes numbers derived from the wrong lane.
 *
 * ⚠ IT IS INCOMPLETE AND SAYS SO. Six of the eighteen steps are not
 * implemented: steps 3-6 write values computed by arithmetic in the SDK that
 * was never decoded, and steps 2, 14, 17 and 18 are separate SDK functions
 * (KrTraining, SetTxConfig, DFE tuning, RxDataGate). What is here is every
 * step whose operation is known exactly. If a lane does not come up, the
 * missing steps are where to look first.
 *
 * ⚠ AND READBACK PROVES NOTHING ON THIS BUS. A SerDes register has separate
 * read and write views at the same number, so what a write puts there is not
 * what a read returns. The acceptance test is behavioural -- PORT_STATUS bit
 * 11, SerXmit, in the lane's EPL slot -- and never a read of what was
 * written.
 */
#ifndef NOSAIC_FM6000_SERDES_H
#define NOSAIC_FM6000_SERDES_H

#include "pci.h"

/* One front-panel port's place in the silicon. */
struct fm_port {
	int port;	/* front panel, 1-based */
	int epl;	/* EPL id as the board's FDL counts them */
	int lane;	/* 0..3 within that EPL */
	int dev;	/* SBus device id */

	/* Board routing, from the FDL. Which pairs are swapped and how hard to
	 * drive are properties of the traces, so they differ per port and
	 * cannot be computed. */
	int rxpol, txpol;
	int drive, pre, post;
};

/*
 * The ports whose SerDes placement is established.
 *
 * ⚠ ONLY THE FIRST EIGHT. The EPL-to-SBus permutation is known for EPL 14 and
 * EPL 16 and for no others, so a table covering all 52 would be 44 guesses
 * with 8 facts hidden among them. Returns NULL for a port that is not known.
 */
const struct fm_port *fm_port_lookup(int port);

/* How far a lane enable got. Each step is reported whether it ran or not. */
enum fm_lane_step {
	FM_LANE_RESET = 1,	/* SBus device reset -- required before writes */
	FM_LANE_POLARITY,	/* regs 11, 7 */
	FM_LANE_QUIESCE,	/* reg 34: hold RX down */
	FM_LANE_RATE,		/* regs 0, 29, 54, 59, 23: 10G rate select */
	FM_LANE_RELEASE,	/* reg 34: release RX */
	FM_LANE_PLL,		/* wait reg 15 bit 3 */
	FM_LANE_TXRX,		/* regs 6, 3 */
	FM_LANE_SIGDET,		/* regs 31, 38 */
	FM_LANE_EPL,		/* EPL_CFG_B PcsSel, EPL_CFG_A Active */
	FM_LANE_TXEQ,		/* regs 61, 62, 65 */
	FM_LANE_SIGNAL,		/* wait reg 0x14 bit 6 */
	FM_LANE__COUNT
};

struct fm_lane_report {
	struct { int rv; const char *what; const char *note; } step[FM_LANE__COUNT];
	int reached;
	uint32_t port_status;	/* the lane's PORT_STATUS after the attempt */
};

/*
 * Run the lane enable for one port.
 *
 * The chip must have been through fm_boot_cold() and the SBus must be started
 * (fm_sbus_start). Returns FM_OK if every implemented step succeeded, which is
 * NOT the same as the lane coming up -- read the report and PORT_STATUS.
 */
int fm_lane_enable(struct fm6000 *d, const struct fm_port *p,
		   struct fm_lane_report *rep);

void fm_lane_report_print(const struct fm_lane_report *rep);

/* PORT_STATUS for a lane: the first word of its EPL slot. 0x8c0 is up. */
int fm_lane_status(struct fm6000 *d, const struct fm_port *p, uint32_t *out);

#endif /* NOSAIC_FM6000_SERDES_H */
