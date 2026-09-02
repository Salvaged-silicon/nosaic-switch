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
#include <dirent.h>

#include "props.h"

#define MAX_PROPS 2048
#define MAX_LINE  512

struct prop {
	char *name;
	char *value;
	/* Whether the SDK ever asked for this one. A property nobody reads is
	 * indistinguishable from one that took effect, and both look like a
	 * correctly configured switch right up until the traffic does not flow. */
	int used;
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

/*
 * Look up a property. The LAST definition wins.
 *
 * Files are loaded in the order given, so a later one overrides an earlier
 * one -- which is what makes layering work: the board ships its defaults and
 * the operator's generated map, or a local override, comes after. Returning
 * the first match instead would mean a later file could be loaded, counted,
 * and quietly ignored.
 */
const char *nosaic_props_get(const char *name)
{
	int i;

	for (i = nprops - 1; i >= 0; i--)
		if (strcmp(props[i].name, name) == 0) {
			props[i].used = 1;
			return props[i].value;
		}
	return NULL;
}

/*
 * Report properties the SDK never asked for.
 *
 * This exists because a misspelt or mistimed property is silent. It is loaded,
 * counted, reported as configuration, and never read -- and the switch then
 * behaves exactly as if it had not been set, which for something like a
 * polarity flip means links that come up carrying garbage. Sixty-two polarity
 * properties that the SDK never looked at and sixty-two that it applied are
 * the same picture from outside, and this is the difference.
 */
void nosaic_props_report_unused(void)
{
	int i, unused = 0;

	for (i = 0; i < nprops; i++)
		if (!props[i].used)
			unused++;
	if (unused == 0) {
		printf("config     all %d properties were read by the SDK\n", nprops);
		return;
	}

	printf("config     %d of %d properties were NEVER read by the SDK:\n",
	       unused, nprops);
	for (i = 0; i < nprops; i++) {
		if (props[i].used)
			continue;
		printf("             %s\n", props[i].name);
		/* Enough to identify the pattern without pages of output. */
		if (--unused == 0)
			break;
		if (i > 0 && (i % 400) == 0)
			break;
	}
	printf("           A property the SDK does not read has no effect, and\n"
	       "           looks identical to one that worked.\n");
}

int nosaic_props_count(void)
{
	return nprops;
}

static int cmp_names(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/*
 * Load every *.conf in a directory, in name order.
 *
 * Name order rather than readdir order so two switches with the same files
 * load them the same way. Later files override earlier ones, so the ordering
 * is not cosmetic: a directory read in whatever sequence the filesystem
 * happens to return would give different configuration on different boots of
 * the same machine.
 *
 * A directory that is not there is not an error. The shipped defaults and the
 * operator's generated files live in different places and either may be
 * absent.
 */
int nosaic_props_load_dir(const char *dir)
{
	char *names[256];
	int nn = 0, total = 0, i;
	struct dirent *e;
	DIR *d;

	d = opendir(dir);
	if (d == NULL)
		return -1;
	while ((e = readdir(d)) != NULL && nn < (int)(sizeof(names) / sizeof(names[0]))) {
		size_t len = strlen(e->d_name);

		if (len < 6 || strcmp(e->d_name + len - 5, ".conf") != 0)
			continue;
		names[nn] = strdup(e->d_name);
		if (names[nn] == NULL)
			break;
		nn++;
	}
	closedir(d);

	qsort(names, (size_t)nn, sizeof(names[0]), cmp_names);
	for (i = 0; i < nn; i++) {
		char path[512];
		int c;

		snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
		c = nosaic_props_load(path);
		if (c >= 0) {
			printf("config     %d properties from %s\n", c, path);
			total += c;
		}
		free(names[i]);
	}
	return total;
}

/* Iterate the loaded properties, so a caller can find every property matching
 * a prefix without this file knowing what prefixes mean. */
const char *nosaic_props_name(int i)
{
	if (i < 0 || i >= nprops)
		return NULL;
	return props[i].name;
}

const char *nosaic_props_value(int i)
{
	if (i < 0 || i >= nprops)
		return NULL;
	return props[i].value;
}
