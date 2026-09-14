/* Tests for the property table.
 *
 * Build and run:
 *     cc -I. -o /tmp/props_test props_test.c props.c && /tmp/props_test
 *
 * There is no C unit-test harness in this tree yet, so this is a plain main()
 * that exits non-zero on failure. It exists because the bug it covers took a
 * switch down: a property defined in both the image and the persistent layer
 * produced two entries, nosd created the same tap twice, the second TUNSETIFF
 * returned EBUSY, and s6 restarted it into the same wall eleven times.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "props.h"

static int failures;

static void check(int ok, const char *what)
{
	printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok)
		failures++;
}

static void write_file(const char *path, const char *body)
{
	FILE *f = fopen(path, "w");

	if (f == NULL) {
		perror(path);
		exit(2);
	}
	fputs(body, f);
	fclose(f);
}

int main(void)
{
	const char *image = "/tmp/props_test_image.conf";
	const char *persist = "/tmp/props_test_persist.conf";
	int i, seen, count;
	const char *v;

	/* The shipped layer, then the per-switch layer that overrides it. */
	write_file(image,
		   "tap_et1=1:1006:1600\n"
		   "tap_et52=61:1052:1600\n"
		   "port_init_speed=10000\n");
	write_file(persist,
		   "# this switch\n"
		   "tap_et52=61:1052:1600\n"
		   "port_init_speed=40000\n");

	nosaic_props_load(image);
	nosaic_props_load(persist);

	/* Lookup has always taken the last definition; that is the layering. */
	v = nosaic_props_get("port_init_speed");
	check(v != NULL && strcmp(v, "40000") == 0,
	      "a redefined property reads as the LAST value");

	/* And now enumeration agrees with it. Walking the table forwards used to
	 * yield both copies, which is what created the tap twice. */
	seen = 0;
	count = nosaic_props_count();
	for (i = 0; i < count; i++) {
		const char *name = nosaic_props_name(i);

		if (name != NULL && strcmp(name, "tap_et52") == 0)
			seen++;
	}
	check(seen == 1, "a property defined in BOTH layers is enumerated ONCE");

	seen = 0;
	for (i = 0; i < count; i++) {
		const char *name = nosaic_props_name(i);

		if (name != NULL && strcmp(name, "port_init_speed") == 0)
			seen++;
	}
	check(seen == 1, "the same, for a non-tap property");

	/* A property only the image defines must survive. Deduplicating must not
	 * turn "override one key" into "replace the file". */
	v = nosaic_props_get("tap_et1");
	check(v != NULL && strcmp(v, "1:1006:1600") == 0,
	      "a property the override does not mention is kept");

	/* Three distinct names went in; three must come out. */
	check(count == 3, "the table holds one entry per distinct name");

	remove(image);
	remove(persist);
	printf("\n%s\n", failures ? "FAILED" : "all passed");
	return failures ? 1 : 0;
}
