/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_FM6000_MEMFILL_H
#define NOSAIC_FM6000_MEMFILL_H

#include <stdint.h>

struct fm6000;

/*
 * Full ordered memory initialisation for the FM6000.
 *
 * The chip has 134 ECC-protected SRAM banks that must all be written before
 * the sweeper runs. A cold chip holds uninitialized ECC syndromes in every
 * bank the boot controller does not touch; the sweeper walks all of them,
 * hits an invalid syndrome, and triggers a fatal watchdog reset before the
 * scheduler ring can ever start.
 *
 * The fills MUST run in order: earlier fills are prereqs for later ones
 * (PARSER before MAPPER, MAPPER before CM, etc.).
 *
 * Called after --boot. Returns FM_OK if every fill completed and the chip
 * is still answering, FM_EOFFBUS if the chip reset itself mid-run.
 */
int fm_memfill_all(struct fm6000 *d);

/* Like fm_memfill_all, but prints each bank's address, size, and ok/DIED. */
int fm_memfill_verbose(struct fm6000 *d);

/*
 * Fill exactly the first `n` banks (0..n-1) in verbose mode, then return.
 * Used for context-sensitive probing: fill up to a problem bank, then probe
 * the next region word-by-word to distinguish burst-timing from context.
 */
int fm_memfill_n_verbose(struct fm6000 *d, unsigned n);

#endif /* NOSAIC_FM6000_MEMFILL_H */
