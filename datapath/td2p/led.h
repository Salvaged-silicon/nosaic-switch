/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_TD2P_LED_H
#define NOSAIC_TD2P_LED_H

/* Front-panel port LEDs, driven from the chip's own link state.
 *
 * Returns 0 if the board controller was found and the panel is being driven,
 * -1 otherwise -- which is not fatal: a switch with a dark panel still
 * forwards, and a board with no SCD has no panel to drive.
 */
int  nosaic_led_start(int unit);
void nosaic_led_poll(void);
void nosaic_led_stop(void);

#endif
