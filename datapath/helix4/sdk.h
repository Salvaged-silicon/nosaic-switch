/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_HELIX4_SDK_H
#define NOSAIC_HELIX4_SDK_H

#include <stdint.h>
#include "bde.h"

/*
 * Bring the chip up as far as soc_attach and return its unit number, or -1.
 *
 * Little-endian throughout: this build sets SYS_BE_PIO=0 and the host is
 * little-endian, so unlike the AS5610 there is no CMIC_ENDIAN_SELECT to write
 * before the first register can be believed. The chip's default already
 * matches the CPU.
 */
int nosaic_helix4_sdk_attach(struct nosaic_helix4_bde *b, uint16_t dev_id, uint8_t rev_id);

/* Reset the chip and bring the SOC layer up. Separate from the BCM layer
 * because a failure in one says something different from a failure in the
 * other, and because the reset has to happen exactly once. */
int nosaic_helix4_sdk_soc_init(int unit);

/* The rest: misc, MMU, and the BCM layer above them. */
int nosaic_helix4_sdk_bcm_init(int unit);

/*
 * Per-port service VLANs: port N alone in VLAN 3300+N, untagged, CPU tagged.
 *
 * Cumulus's layout on Broadcom hardware, by way of EdgeNOS, and the resting
 * state both of the two pieces of software known to work on this class of
 * board boot into. A port is forwarding and fully usable while alone in its
 * VLAN, so there is nothing to bridge to and no loop to form whatever the
 * cabling looks like — which matters more here than on the AS5610, because
 * this is a 48-port access switch and somebody will plug both ends of a patch
 * lead into it.
 */
#define NOSAIC_HELIX4_SERVICE_VLAN_BASE 3300

/* Enable every port and put it in spanning-tree forwarding. bcm_init leaves
 * both off, so nothing forwards until this runs.
 *
 * forward bridges every port together in the default VLAN instead, which is
 * what demonstrates the chip moving frames — and is a loop wherever two ports
 * reach the same neighbour. Bring-up on a known topology only. */
int nosaic_helix4_sdk_ports_up(int unit, int forward);

/* Sample every port's counters twice, `seconds` apart, and print what moved.
 * A delta is the only form of this that answers whether traffic is flowing. */
int nosaic_helix4_sdk_stats(int unit, int seconds);

/* Bring the declared taps up, give each a router interface, and pump frames
 * between the chip and Linux until something fails. Only returns on failure. */
int nosaic_helix4_sdk_run(int unit);

/*
 * How many iProc-space registers the SDK asked for that were not mapped, and
 * the first few addresses.
 *
 * Not a statistic — a bring-up instrument. See the long comment in sdk.c: this
 * board's SDK build reaches some registers through the SoC's own address space
 * rather than the CMIC window, and which ones is not knowable from here. The
 * daemon refuses to guess, records what was asked for, and reports it, so the
 * set is discovered in one boot instead of by reading vendor source.
 */
int nosaic_helix4_sdk_iproc_misses(uint32_t *first, int max);

#endif
