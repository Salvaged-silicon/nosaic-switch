/*
 * SDK configuration properties.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The SDK asks for its configuration one property at a time through
 * config_var_get, which is Broadcom's config.bcm mechanism seen from the
 * inside. This answers those questions from a plain key=value file supplied by
 * the board.
 *
 * It lives in the board directory rather than in this code on purpose. A port
 * map is a fact about how a particular switch is wired, and every board with
 * this ASIC wires it differently -- so a map compiled into the daemon would
 * make the one thing that must vary per board the one thing that cannot.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "props.h"

#define MAX_PROPS 2048
#define MAX_LINE  512

struct prop {
	char *name;
	char *value;
};

static struct prop props[MAX_PROPS];
static int nprops;

static char *trim(char *s)
{
	char *end;

	while (*s && isspace((unsigned char)*s))
		s++;
	if (*s == '\0')
		return s;
	end = s + strlen(s) - 1;
	while (end > s && isspace((unsigned char)*end))
		*end-- = '\0';
	return s;
}

int nosaic_props_load(const char *path)
{
	char line[MAX_LINE];
	FILE *f;
	int n = 0;

	f = fopen(path, "r");
	if (f == NULL)
		return -1;

	while (fgets(line, sizeof(line), f) != NULL) {
		char *eq, *name, *value;

		/* Comments and blank lines. */
		char *hash = strchr(line, '#');
		if (hash != NULL)
			*hash = '\0';
		eq = strchr(line, '=');
		if (eq == NULL)
			continue;
		*eq = '\0';
		name = trim(line);
		value = trim(eq + 1);
		if (*name == '\0')
			continue;

		if (nprops >= MAX_PROPS) {
			fprintf(stderr, "nosd-td2p: more than %d properties in %s; "
				"the rest are ignored\n", MAX_PROPS, path);
			break;
		}
		props[nprops].name = strdup(name);
		props[nprops].value = strdup(value);
		if (props[nprops].name == NULL || props[nprops].value == NULL) {
			fprintf(stderr, "nosd-td2p: out of memory reading %s\n", path);
			break;
		}
		nprops++;
		n++;
	}
	fclose(f);
	return n;
}

const char *nosaic_props_get(const char *name)
{
	int i;

	for (i = 0; i < nprops; i++)
		if (strcmp(props[i].name, name) == 0)
			return props[i].value;
	return NULL;
}

int nosaic_props_count(void)
{
	return nprops;
}
