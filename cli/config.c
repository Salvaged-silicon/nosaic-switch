/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The switch's configuration, as files.
 *
 * Two layers, and which is which is the whole design:
 *
 *   /etc/nosaic/*.conf        what the IMAGE shipped. Replaced by every
 *                             upgrade, read-only in practice, and the same on
 *                             every switch built from that image.
 *   /mnt/data/config/*.conf   what THIS SWITCH is. On the shared data
 *                             partition, so it survives an upgrade and also
 *                             survives a rollback.
 *
 * Loaded in that order, and the last definition of a key wins -- so a setting
 * here overrides the image without editing the image. That is what lets a
 * switch be configured at all without rebuilding it, and it is why `set`
 * refuses to write anywhere but the second directory.
 *
 * Everything is key=value text. A configuration you can read with cat, diff
 * between two switches, and restore by copying a directory is worth more on
 * this hardware than a database.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"

#define MAX_SETTINGS 512

struct setting {
	char *name;
	char *value;
	char *file;      /* where the winning definition came from */
	int   from_site; /* 1 if it came from the switch's own config */
};

static struct setting settings[MAX_SETTINGS];
static int nsettings;

static struct setting *find(const char *name)
{
	int i;

	for (i = 0; i < nsettings; i++)
		if (strcmp(settings[i].name, name) == 0)
			return &settings[i];
	return NULL;
}

/* Later definitions replace earlier ones in place, so the array holds the
 * EFFECTIVE configuration and remembers which file won. */
static void record(const char *name, const char *value, const char *file, int site)
{
	struct setting *s = find(name);

	if (s != NULL) {
		free(s->value);
		free(s->file);
		s->value = strdup(value);
		s->file = strdup(file);
		s->from_site = site;
		return;
	}
	if (nsettings >= MAX_SETTINGS)
		return;
	settings[nsettings].name = strdup(name);
	settings[nsettings].value = strdup(value);
	settings[nsettings].file = strdup(file);
	settings[nsettings].from_site = site;
	nsettings++;
}

static void load_file(const char *path, int site)
{
	char line[1024];
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return;
	while (fgets(line, sizeof(line), f) != NULL) {
		char *eq, *name, *value, *end;

		name = line;
		while (*name == ' ' || *name == '\t')
			name++;
		if (*name == '#' || *name == '\n' || *name == '\0')
			continue;
		eq = strchr(name, '=');
		if (eq == NULL)
			continue;
		*eq = '\0';
		value = eq + 1;

		for (end = eq - 1; end >= name && (*end == ' ' || *end == '\t'); end--)
			*end = '\0';
		while (*value == ' ' || *value == '\t')
			value++;
		end = value + strlen(value);
		while (end > value && (end[-1] == '\n' || end[-1] == '\r' ||
				       end[-1] == ' ' || end[-1] == '\t'))
			*--end = '\0';

		record(name, value, path, site);
	}
	fclose(f);
}

/* Every *.conf in a directory, in name order -- which is why the files are
 * named so that the order reads sensibly rather than being an accident. */
static void load_dir(const char *dir, int site)
{
	char *names[128];
	int n = 0, i, j;
	struct dirent *e;
	DIR *d = opendir(dir);

	if (d == NULL)
		return;
	while ((e = readdir(d)) != NULL && n < (int)(sizeof(names) / sizeof(names[0]))) {
		size_t len = strlen(e->d_name);

		if (len < 6 || strcmp(e->d_name + len - 5, ".conf") != 0)
			continue;
		names[n++] = strdup(e->d_name);
	}
	closedir(d);

	for (i = 1; i < n; i++) {          /* insertion sort: n is tiny */
		char *k = names[i];
		for (j = i - 1; j >= 0 && strcmp(names[j], k) > 0; j--)
			names[j + 1] = names[j];
		names[j + 1] = k;
	}
	for (i = 0; i < n; i++) {
		char path[512];

		snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
		load_file(path, site);
		free(names[i]);
	}
}

void nosaic_config_load(void)
{
	nsettings = 0;
	load_dir(NOSAIC_CONFIG_IMAGE_DIR, 0);
	load_dir(NOSAIC_CONFIG_SITE_DIR, 1);
}

int nosaic_config_count(void) { return nsettings; }
const char *nosaic_config_name(int i)  { return i < nsettings ? settings[i].name : NULL; }
const char *nosaic_config_value(int i) { return i < nsettings ? settings[i].value : NULL; }
const char *nosaic_config_file(int i)  { return i < nsettings ? settings[i].file : NULL; }
int nosaic_config_is_site(int i)       { return i < nsettings ? settings[i].from_site : 0; }

const char *nosaic_config_get(const char *name)
{
	struct setting *s = find(name);

	return s != NULL ? s->value : NULL;
}

/*
 * Write a setting into this switch's own configuration.
 *
 * Only ever into NOSAIC_CONFIG_SITE_DIR, never into the image's copy: the
 * image is read-only in intent and replaced wholesale by an upgrade, so a
 * change written there would be lost by the next one and would not be visible
 * to anyone comparing two switches.
 *
 * The file is rewritten whole rather than appended to, so setting a key twice
 * leaves one line rather than two and the file stays something a person can
 * read. Written to a temporary and renamed, because a switch that loses power
 * mid-write should come back with the old configuration rather than half of
 * the new one.
 */
int nosaic_config_set(const char *name, const char *value, char *err, size_t errlen)
{
	char path[512], tmp[512], line[1024];
	FILE *in, *out;
	int written = 0;

	if (name == NULL || *name == '\0' || strchr(name, '=') != NULL) {
		snprintf(err, errlen, "a setting name cannot be empty or contain '='");
		return -1;
	}
	if (mkdir(NOSAIC_CONFIG_SITE_DIR, 0755) != 0 && errno != EEXIST) {
		snprintf(err, errlen, "%s: %s", NOSAIC_CONFIG_SITE_DIR, strerror(errno));
		return -1;
	}
	snprintf(path, sizeof(path), "%s/%s", NOSAIC_CONFIG_SITE_DIR, NOSAIC_CONFIG_SITE_FILE);
	snprintf(tmp, sizeof(tmp), "%s.new", path);

	if ((out = fopen(tmp, "w")) == NULL) {
		snprintf(err, errlen, "%s: %s", tmp, strerror(errno));
		return -1;
	}
	fprintf(out, "# This switch's own configuration. Written by `nosaic config set`.\n"
		     "# Overrides the image's defaults in %s.\n", NOSAIC_CONFIG_IMAGE_DIR);

	if ((in = fopen(path, "r")) != NULL) {
		while (fgets(line, sizeof(line), in) != NULL) {
			char *eq;
			size_t nlen = strlen(name);

			if (line[0] == '#')
				continue;    /* the header is rewritten above */
			eq = strchr(line, '=');
			if (eq != NULL && (size_t)(eq - line) == nlen &&
			    strncmp(line, name, nlen) == 0)
				continue;    /* replaced below */
			fputs(line, out);
		}
		fclose(in);
	}
	if (value != NULL) {
		fprintf(out, "%s=%s\n", name, value);
		written = 1;
	}
	if (fflush(out) != 0 || fsync(fileno(out)) != 0) {
		snprintf(err, errlen, "%s: %s", tmp, strerror(errno));
		fclose(out);
		return -1;
	}
	fclose(out);
	if (rename(tmp, path) != 0) {
		snprintf(err, errlen, "%s: %s", path, strerror(errno));
		return -1;
	}
	(void)written;
	return 0;
}
