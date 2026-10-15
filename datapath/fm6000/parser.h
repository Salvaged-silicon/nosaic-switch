/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_FM6000_PARSER_H
#define NOSAIC_FM6000_PARSER_H

struct fm6000;

/*
 * Write the parser's per-port seed: a GLORT for every port that carries
 * traffic, zero for every port that does not, and zero in the unused second
 * entry everywhere.
 */
int fm_parser_fields_init(struct fm6000 *d, unsigned *written);
int fm_parser_port_link_up(struct fm6000 *d, unsigned fp_port);

#endif /* NOSAIC_FM6000_PARSER_H */
