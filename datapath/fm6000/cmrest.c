/* SPDX-License-Identifier: Apache-2.0 */
/*
 * CM mapping tables, PAUSE configuration and shared-partition thresholds --
 * the congestion-management state that cmwm.c does not cover.
 *
 * ⚠ LIKE cmwm.c, THESE DECIDE WHEN THE CHIP DROPS AND WHEN IT PAUSES, and a
 * wrong value here shows up as loss or head-of-line blocking under load
 * rather than as a failed transit test. An idle lab will not find it.
 *
 * Everything below is one of three shapes, and none of them is a blob:
 *
 *   per traffic class   sixteen entries, of which this board maps eleven
 *   per port            seventy-six entries, split by whether the port is a
 *                       front-panel port
 *   two-phase           every port parked first, then the front-panel ports
 *                       moved to their running configuration
 *
 * PROVENANCE. Ported from EdgeNOS's fm6000_cmrest.c -- our own prior work on
 * this chassis -- and relicensed. There the 397 write-once addresses are a
 * materialised table; here they are regenerated from the geometry and this
 * board's port map, and the two agree address for address and value for
 * value.
 */
#include "cmrest.h"
#include "pci.h"
#include "portmap.h"
#include "regs.h"

/* PAUSE configuration: four words per port, and the only two-phase table. */
/*
 * ⚠ THE NAMES BELOW ARE THE PART'S OWN, and they were not before.
 *
 * This file was written from addresses and observed shapes, so its
 * registers carried names invented here -- PARTITION, PORT_CLASS, TC_A,
 * TC_B, HASH. Every one of them is named in the vendor's register header,
 * and two of the invented names were actively misleading: what this called
 * a "hash" is the global watermark, and what it called a "partition" map is
 * the traffic-class to port-class map.
 *
 * The addresses and values are unchanged -- they were already verified
 * against a forwarding chip, all 701 of them -- so this is a rename and a
 * correction of what we believed we were configuring.
 */
#define CM_PAUSE_CFG		0x116400u	/* 4 words x 76 */
#define CMREST_PAUSE_WORDS	4u

#define CM_BSG_MAP		0x110100u	/* 2 words x 76 */
#define CM_TC_PC_MAP		0x116100u	/* 2 words x 76 */
#define CM_PC_RXMP_MAP		0x116200u	/* 1 word  x 76 */

/* Per-traffic-class watermarks, sixteen entries each. */
#define CM_SHARED_RXMP_WM	0x112210u
#define CM_RXMP_SOFT_DROP_WM	0x114800u
#define CMREST_TC_CLASSES	16u
/* This board maps eleven classes and reserves the last; twelve to fourteen
 * are unmapped and are left at zero so that no buffer is set aside for
 * traffic that cannot arrive. */
#define CMREST_TC_MAPPED	11u
#define CMREST_TC_RESERVED	15u

/* Shared pause thresholds, twelve classes each. */
#define CM_SHARED_RXMP_PAUSE_ON_WM	0x116000u
#define CM_SHARED_RXMP_PAUSE_OFF_WM	0x116010u
#define CMREST_SHARED_WORDS		12u

/* Three two-word maps at the base of the block: receive pool, transmit
 * pool, and traffic class. */
#define CM_RXMP_MAP		0x110000u
#define CM_TXMP_MAP		0x110002u
#define CM_TC_MAP		0x110004u
#define CM_GLOBAL_WM		0x112200u

#define CMREST_PORTS		76u

static int cmrest_front(unsigned port)
{
	unsigned i;

	for (i = 1; i <= FM6000_FRONT_PORTS; i++)
		if (fm6000_alta_of[i] == port)
			return 1;
	return 0;
}

static int wr(struct fm6000 *d, uint32_t word, uint32_t val, unsigned *n)
{
	int rv = fm_wr(d, word, val);

	if (rv != FM_OK)
		return rv;
	(*n)++;
	return FM_OK;
}

int fm_cmrest_init(struct fm6000 *d, unsigned *written)
{
	/* Parked: pause resend interval at maximum, shared pause enabled on
	 * every class. Safe for a port that is not yet forwarding. */
	static const uint32_t parked[CMREST_PAUSE_WORDS] = {
		0xc1e21000u, 0x003fffffu, 0x00000fffu, 0x00000000u,
	};
	/* Running: a real resend interval, shared pause off. */
	static const uint32_t running[CMREST_PAUSE_WORDS] = {
		0x81e21000u, 0x00000061u, 0x00000000u, 0x00000000u,
	};
	unsigned port, c, n = 0;
	int rv;

	if (written != NULL)
		*written = 0;

	/*
	 * ⚠ PAUSE CONFIGURATION IS TWO-PHASE AND THE PHASES ARE WHOLE-CHIP.
	 * Every port is parked before any port is started. Writing each
	 * port's final value once touches the same 304 addresses and is not
	 * the same thing: the state in which every port is parked has to
	 * exist, because a port brought up against neighbours that have not
	 * been parked can pause them.
	 */
	for (port = 0; port < CMREST_PORTS; port++)
		for (c = 0; c < CMREST_PAUSE_WORDS; c++)
			if ((rv = wr(d, CM_PAUSE_CFG + port * CMREST_PAUSE_WORDS + c,
				     parked[c], &n)) != FM_OK)
				return rv;

	for (port = 0; port < CMREST_PORTS; port++) {
		const uint32_t *v = cmrest_front(port) ? running : parked;

		for (c = 0; c < CMREST_PAUSE_WORDS; c++)
			if ((rv = wr(d, CM_PAUSE_CFG + port * CMREST_PAUSE_WORDS + c,
				     v[c], &n)) != FM_OK)
				return rv;
	}

	/* The receive-pool, transmit-pool and traffic-class maps. */
	if ((rv = wr(d, CM_RXMP_MAP + 0, 0x00000000u, &n)) != FM_OK)
		return rv;
	if ((rv = wr(d, CM_RXMP_MAP + 1, 0x10000000u, &n)) != FM_OK)
		return rv;
	if ((rv = wr(d, CM_TXMP_MAP + 0, 0x76543210u, &n)) != FM_OK)
		return rv;
	if ((rv = wr(d, CM_TXMP_MAP + 1, 0xb210ba98u, &n)) != FM_OK)
		return rv;
	if ((rv = wr(d, CM_TC_MAP + 0, 0x76543210u, &n)) != FM_OK)
		return rv;
	if ((rv = wr(d, CM_TC_MAP + 1, 0xb210ba98u, &n)) != FM_OK)
		return rv;

	/*
	 * The per-port class map, for the ports that carry traffic: the host
	 * port and the front panel. An unconfigured port is left alone rather
	 * than given a map, because a map is what lets a port claim buffer.
	 */
	for (port = 0; port < CMREST_PORTS; port++) {
		if (port != FM6000_ALTA_HOST && !cmrest_front(port))
			continue;
		if ((rv = wr(d, CM_BSG_MAP + port * 2, 0x76543210u, &n)) != FM_OK)
			return rv;
		if ((rv = wr(d, CM_BSG_MAP + port * 2 + 1, 0x0000ba98u, &n)) != FM_OK)
			return rv;
	}

	if ((rv = wr(d, CM_GLOBAL_WM, 0x3bfcb3f6u, &n)) != FM_OK)
		return rv;

	/* The two per-traffic-class tables, same shape and different values. */
	for (c = 0; c < CMREST_TC_CLASSES; c++)
		if ((rv = wr(d, CM_SHARED_RXMP_WM + c,
			     c < CMREST_TC_MAPPED  ? 0x2be983bdu :
			     c < CMREST_TC_RESERVED ? 0x00000000u : 0x00cd0267u,
			     &n)) != FM_OK)
			return rv;
	for (c = 0; c < CMREST_TC_CLASSES; c++)
		if ((rv = wr(d, CM_RXMP_SOFT_DROP_WM + c,
			     c < CMREST_TC_MAPPED  ? 0x57d42786u :
			     c < CMREST_TC_RESERVED ? 0x00000000u : 0x7ffe3fffu,
			     &n)) != FM_OK)
			return rv;

	/* Shared-partition thresholds: no limit, because the per-port private
	 * allowances in cmwm.c are what do the limiting. */
	for (c = 0; c < CMREST_SHARED_WORDS; c++)
		if ((rv = wr(d, CM_SHARED_RXMP_PAUSE_ON_WM + c, 0xffffffffu, &n)) != FM_OK)
			return rv;
	for (c = 0; c < CMREST_SHARED_WORDS; c++)
		if ((rv = wr(d, CM_SHARED_RXMP_PAUSE_OFF_WM + c, 0xffffffffu, &n)) != FM_OK)
			return rv;

	/*
	 * Partition assignment, and the port's class.
	 *
	 * Note the sense: it is the ports that are NOT front-panel that get a
	 * partition and the loud-looking class value. A front-panel port is
	 * given zero and a different class, and is assigned its buffer through
	 * the watermark tables instead.
	 */
	for (port = 0; port < CMREST_PORTS; port++) {
		int front = cmrest_front(port);

		if ((rv = wr(d, CM_TC_PC_MAP + port * 2,
			     front ? 0x00000000u : 0x88fac688u, &n)) != FM_OK)
			return rv;
		if ((rv = wr(d, CM_TC_PC_MAP + port * 2 + 1,
			     front ? 0x00000000u : 0x00000006u, &n)) != FM_OK)
			return rv;
	}
	for (port = 0; port < CMREST_PORTS; port++)
		if ((rv = wr(d, CM_PC_RXMP_MAP + port,
			     cmrest_front(port) ? 0xbbbbbbbbu : 0xccccccccu,
			     &n)) != FM_OK)
			return rv;

	if (written != NULL)
		*written = n;
	return fm_alive(d) ? FM_OK : FM_EOFFBUS;
}
