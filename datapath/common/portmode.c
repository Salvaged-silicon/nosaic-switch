/*
 * QSFP cage mode: one 40G port, or four 10G ports. See portmode.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "props.h"
#include "portmode.h"

/* A cage is four lanes, always. */
#define LANES 4

/*
 * One cage, from `portmap_<base>.<unit>=<physical>:<speed>`.
 *
 * The physical port is the cage's first lane and the remaining three follow it
 * consecutively -- that is what makes a QSFP a QSFP. The LOGICAL ports follow
 * consecutively too, which is why a 40G cage occupies four logical numbers and
 * the next cage starts four along: on the 7050TX-64 the cages are 49, 53, 57
 * and 61, not 49, 50, 51, 52.
 */
static int parse_portmap(const char *v, int *phys, int *speed)
{
	char *end;
	long p, s;

	p = strtol(v, &end, 10);
	if (end == v || *end != ':')
		return -1;
	s = strtol(end + 1, &end, 10);
	if (s <= 0)
		return -1;
	*phys = (int)p;
	*speed = (int)s;
	return 0;
}

int nosaic_portmode_apply(int unit)
{
	int base, broken = 0;

	/*
	 * Walk every logical port that could be a cage. Cheap, and it avoids
	 * the board having to state where its cages are a second time: a port
	 * with no portmap entry is not a port, and one whose mode is unset or
	 * "40g" is left exactly as the generated map has it.
	 */
	for (base = 1; base <= 128; base++) {
		char key[48], val[64];
		const char *mode, *map;
		int phys = 0, speed = 0, i;

		snprintf(key, sizeof(key), "nosaic_portmode_%d", base);
		mode = nosaic_props_get_unit(key, unit);
		if (mode == NULL || strcmp(mode, "4x10g") != 0) {
			if (mode != NULL && strcmp(mode, "40g") != 0) {
				fprintf(stderr, "portmode: %s=%s is not a mode I know; "
					"use 40g or 4x10g\n", key, mode);
				return -1;
			}
			continue;
		}

		snprintf(key, sizeof(key), "portmap_%d", base);
		map = nosaic_props_get_unit(key, unit);
		if (map == NULL) {
			fprintf(stderr, "portmode: port %d is asked to break out and has "
				"no portmap entry; there is no cage there\n", base);
			return -1;
		}
		if (parse_portmap(map, &phys, &speed) != 0) {
			fprintf(stderr, "portmode: port %d has an unreadable portmap "
				"entry \"%s\"\n", base, map);
			return -1;
		}
		if (speed != 40000 && speed != 40) {
			fprintf(stderr, "portmode: port %d is %d, not 40G; only a 40G "
				"cage can be broken out\n", base, speed);
			return -1;
		}

		/*
		 * Prefer base+1..3 when free (the Arista layout: 49, 53, 57, 61).
		 * If occupied (e.g. Dell S6000-ON where 32 cages are 1..32), allocate
		 * three free logical ports in 1..128.
		 */
		int subs[LANES];
		subs[0] = base;
		int need_alloc = 0;
		for (i = 1; i < LANES; i++) {
			snprintf(key, sizeof(key), "portmap_%d", base + i);
			if (nosaic_props_get_unit(key, unit) != NULL) {
				need_alloc = 1;
				break;
			}
			subs[i] = base + i;
		}
		if (need_alloc) {
			int cand = 33, found = 1;
			for (; cand <= 128 && found < LANES; cand++) {
				snprintf(key, sizeof(key), "portmap_%d", cand);
				if (nosaic_props_get_unit(key, unit) == NULL) {
					subs[found++] = cand;
				}
			}
			if (found < LANES) {
				fprintf(stderr, "portmode: no free logical ports to break out port %d\n",
					base);
				return -1;
			}
		}

		for (i = 0; i < LANES; i++) {
			snprintf(key, sizeof(key), "portmap_%d.%d", subs[i], unit);
			snprintf(val, sizeof(val), "%d:10", phys + i);
			if (nosaic_props_set(key, val) != 0) {
				fprintf(stderr, "portmode: no room to break out port %d\n",
					base);
				return -1;
			}
			if (i > 0) {
				snprintf(key, sizeof(key), "tap_swp%d", subs[i]);
				if (nosaic_props_get(key) == NULL) {
					snprintf(val, sizeof(val), "%d:%d:1500",
						 subs[i], 1000 + subs[i]);
					nosaic_props_set(key, val);
				}
			}
		}
		/* The unsuffixed form, if the generated map used one, would now
		 * disagree with what we just wrote. */
		snprintf(key, sizeof(key), "portmap_%d", base);
		nosaic_props_unset(key);

		printf("portmode: port %d broken out -- %d:10 %d:10 %d:10 %d:10 "
		       "on logical %d %d %d %d\n", base,
		       phys, phys + 1, phys + 2, phys + 3,
		       subs[0], subs[1], subs[2], subs[3]);
		broken++;
	}
	if (broken)
		fflush(stdout);
	return broken;
}
