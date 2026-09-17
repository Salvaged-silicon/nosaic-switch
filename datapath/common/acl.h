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

/* Whether ACLs are available, per address family, and how much room each
 * group has. All zero when nosaic_acl_start failed or was never called. */
struct nosaic_acl_caps {
	int v4, v4_total, v4_free;
	int v6, v6_total, v6_free;
	int v6_l4;   /* the v6 group can match L4 ports */
};
void nosaic_acl_capability(struct nosaic_acl_caps *c);

/* The "acl" query: every rule with its hit count, as one JSON response line. */
void nosaic_acl_query(FILE *out);

/*
 * The contract's SetACL and DelACL. The rule is persisted as the acl_<seq>
 * setting in the switch's own configuration and then installed by the same
 * reload every change goes through. Returns 0; -1 with err saying what was
 * wrong with the rule or why the chip refused it; -2 when this datapath has
 * no field group for that family, which the socket reports as unsupported.
 */
int nosaic_acl_set(int seq, const char *text, char *err, size_t errlen);
int nosaic_acl_del(int seq, char *err, size_t errlen);

#endif
