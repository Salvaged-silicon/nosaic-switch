/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Access control lists: operator rules in the chip's ingress field processor.
 *
 * Shared between the Broadcom datapaths because it is written against bcm_field
 * alone, the same way l3sync.c is. What differs per chip is how many slices the
 * field processor has and which qualifiers fit in one; the SDK decides that at
 * group creation and this reports what it decided.
 *
 * Rules are properties, so they arrive the way every other setting does --
 * `nosaic config set acl_<seq> "<rule>"` -- and are re-read while the daemon
 * runs, so a change takes effect without restarting it. See acl.c for the
 * rule grammar.
 */
#ifndef NOSAIC_ACL_H
#define NOSAIC_ACL_H

#include <stdio.h>

/* Create the field group and install whatever rules are configured. Call once
 * the taps exist, because a rule may name a port. Returns 0 if the chip has a
 * group to hold rules in, -1 if it does not -- in which case the capability is
 * reported absent and every rule is reported as not installed. */
int nosaic_acl_start(int unit);

/* Re-read the rules and reprogram the chip if they changed. Cheap when they
 * have not; call it about once a second from the periodic thread. */
void nosaic_acl_poll(void);

/* Whether ACLs are available, and how much room the group has. Both zero when
 * nosaic_acl_start failed or was never called. */
void nosaic_acl_capability(int *available, int *entries_total, int *entries_free);

/* The "acl" query: every rule with its hit count, as one JSON response line. */
void nosaic_acl_query(FILE *out);

#endif
