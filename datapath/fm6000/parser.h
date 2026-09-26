/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_FM6000_PARSER_H
#define NOSAIC_FM6000_PARSER_H

struct fm6000;

/*
 * Clear the parser's per-port seed for every port that does not carry
 * traffic, and the unused second entry for every port.
 *
 * ⚠ THIS DOES NOT SEED THE PORTS THAT DO CARRY TRAFFIC. See parser.c.
 */
int fm_parser_fields_clear(struct fm6000 *d, unsigned *written);

#endif /* NOSAIC_FM6000_PARSER_H */
