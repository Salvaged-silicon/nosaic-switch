/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_TDP_LED_H
#define NOSAIC_TDP_LED_H

#include "bde.h"

/* Front-panel port LEDs, driven from the chip's own link state.
 *
 * The same three states nosd-td2p shows on the 7050SX2 -- dark, green, amber --
 * because an operator should not have to remember which switch they are looking
 * at to read its panel. What differs is only how the light is reached: there the
 * lamps hang off a board controller, here off the chip's own LED processors.
 *
 * Takes the BDE rather than finding its own way to the registers: the LED
 * processors are inside the switch chip, on the mapping the BDE already owns.
 *
 * Returns 0 if the panel is being driven, -1 otherwise -- which is not fatal.
 * A switch with a dark panel still forwards.
 */
int  nosaic_led_start(int unit, struct nosaic_tdp_bde *b);
void nosaic_led_poll(void);
void nosaic_led_stop(void);

#endif
