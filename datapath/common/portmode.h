/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_PORTMODE_H
#define NOSAIC_PORTMODE_H

/*
 * QSFP cage mode: one 40G port, or four 10G ports.
 *
 * A QSFP cage is four SerDes lanes. The chip can drive them as a single 40G
 * port whose PCS stripes across all four, or as four independent 10G ports.
 * Which one it is is decided by the PORT MAP, before the SDK attaches -- there
 * is no runtime switch on this generation, so a change takes a reboot.
 *
 * ⚠ THIS IS NOT A PREFERENCE, IT IS A PROPERTY OF THE LINK.
 *
 * Both ends must agree. Four lanes in 4x10G mode each achieve PCS lock on
 * their own; the same four lanes in 40G mode must additionally DESKEW and
 * align as one PCS. So a pair that links fine at 4x10G can fail at 40G, with
 * every lane locked, no symbol errors and no CRC errors -- and both ends
 * reporting something different from each other.
 *
 * That is not hypothetical: it is why this exists. On the 7050TX-64 two cages
 * to a neighbour linked on all eight lanes at 10G and would not carry a single
 * frame at 40G, under NOSaic and under the vendor's own OS alike.
 */

/*
 * Rewrite the port map for any cage the board asks to be broken out.
 *
 * Call after the configuration is loaded and BEFORE the SDK attaches, because
 * the map is read during attach and never again. Returns the number of cages
 * broken out, or -1 if the configuration asks for something impossible.
 *
 * The board states this as `nosaic_portmode_<port>=4x10g` (or `40g`, the
 * default) where <port> is the cage's first logical port -- 49, 53, 57, 61 on
 * a board whose cages start there.
 */
int nosaic_portmode_apply(int unit);

#endif
