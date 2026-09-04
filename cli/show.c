/* SPDX-License-Identifier: Apache-2.0 */
/*
 * `nosaic show` for architectures the Go toolchain cannot target.
 *
 * The AS5610 is 32-bit big-endian PowerPC, which gc has never supported, so
 * that board ships this CLI instead of the Go one. That is a fact about Go and
 * must not become a fact about the switch: the contract is the same, the
 * daemon is the same, and an operator moving between a PowerPC switch and an
 * x86 one should type the same words and read the same columns.
 *
 * So the output here is deliberately identical to the Go CLI's, down to the
 * column padding, rather than merely equivalent.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "query.h"
#include "show.h"

#define MAXPORTS 512
#define MAXCOLS  8

/* Text laid out the way Go's tabwriter lays it out: every column is as wide as
 * its widest cell plus two, and the last column is not padded. Matching it by
 * eye would drift the first time a port name got longer. */
struct table {
	char cell[MAXPORTS + 2][MAXCOLS][40];
	int  rows, cols;
};

static void put(struct table *t, int r, int c, const char *s)
{
	if (r >= MAXPORTS + 2 || c >= MAXCOLS)
		return;
	snprintf(t->cell[r][c], sizeof(t->cell[r][c]), "%s", s);
	if (r + 1 > t->rows)
		t->rows = r + 1;
	if (c + 1 > t->cols)
		t->cols = c + 1;
}

static void emit(const struct table *t)
{
	int w[MAXCOLS] = {0};
	int r, c;

	for (c = 0; c < t->cols; c++)
		for (r = 0; r < t->rows; r++) {
			int n = (int)strlen(t->cell[r][c]);

			if (n > w[c])
				w[c] = n;
		}
	for (r = 0; r < t->rows; r++) {
		for (c = 0; c < t->cols; c++) {
			if (c == t->cols - 1)
				printf("%s", t->cell[r][c]);
			else
				printf("%-*s", w[c] + 2, t->cell[r][c]);
		}
		printf("\n");
	}
}

static void no_datapath(void)
{
	fprintf(stderr,
		"nosaic: cannot reach the datapath on %s.\n"
		"The daemon serves it once the chip is up; if nosd is running and this\n"
		"is missing, it did not get that far.\n",
		NOSAIC_QUERY_SOCKET);
}

static int refused(const char *resp)
{
	if (strstr(resp, "\"ok\":true") != NULL)
		return 0;
	fprintf(stderr, "nosaic: the datapath refused: %s", resp);
	return 1;
}

int nosaic_show_caps(void)
{
	char *resp = nosaic_query_once(NOSAIC_QUERY_SOCKET, "{\"op\":\"capabilities\"}");
	struct table t;
	char s[40];
	int r = 0;

	if (resp == NULL) {
		no_datapath();
		return 1;
	}
	if (refused(resp)) {
		free(resp);
		return 1;
	}
	memset(&t, 0, sizeof(t));

	nosaic_jstr(resp, "Driver", s, sizeof(s));
	put(&t, r, 0, "driver");   put(&t, r++, 1, s);
	nosaic_jstr(resp, "Contract", s, sizeof(s));
	put(&t, r, 0, "contract"); put(&t, r++, 1, s);
	snprintf(s, sizeof(s), "%d max", nosaic_jint(resp, "MaxPorts", 0));
	put(&t, r, 0, "ports");    put(&t, r++, 1, s);
	put(&t, r, 0, "vlans");    put(&t, r++, 1, nosaic_jbool(resp, "VLANs", 0) ? "true" : "false");
	put(&t, r, 0, "l3");       put(&t, r++, 1, nosaic_jbool(resp, "L3", 0) ? "true" : "false");

	/* Stated even when absent, because an operator planning multipath needs to
	 * know before configuring it rather than after a route is refused. */
	if (nosaic_jbool(resp, "ECMP", 0)) {
		snprintf(s, sizeof(s), "yes, up to %d paths", nosaic_jint(resp, "MaxECMP", 0));
		put(&t, r, 0, "ecmp"); put(&t, r++, 1, s);
	} else {
		put(&t, r, 0, "ecmp"); put(&t, r++, 1, "no");
	}
	emit(&t);
	free(resp);
	return 0;
}

int nosaic_show_ports(void)
{
	int fd = nosaic_query_open(NOSAIC_QUERY_SOCKET);
	char names[MAXPORTS][40];
	struct table t;
	char *resp;
	const char *p;
	int n = 0, i, r = 1;

	if (fd < 0) {
		no_datapath();
		return 1;
	}
	if ((resp = nosaic_query_ask(fd, "{\"op\":\"ports\"}")) == NULL) {
		no_datapath();
		nosaic_query_close(fd);
		return 1;
	}
	if (refused(resp)) {
		free(resp);
		nosaic_query_close(fd);
		return 1;
	}
	/* From the array, not the envelope: starting at the document's first brace
	 * walks the envelope as a record and reports port one twice. */
	p = strstr(resp, "\"result\":[");
	p = p ? p + strlen("\"result\":[") : resp;
	while ((p = strchr(p, '{')) != NULL && n < MAXPORTS) {
		nosaic_jstr(p, "Name", names[n], sizeof(names[n]));
		if (names[n][0] != '\0')
			n++;
		p++;
	}
	free(resp);

	memset(&t, 0, sizeof(t));
	put(&t, 0, 0, "PORT"); put(&t, 0, 1, "ADMIN");
	put(&t, 0, 2, "OPER"); put(&t, 0, 3, "SPEED"); put(&t, 0, 4, "MTU");

	for (i = 0; i < n; i++) {
		char req[128], s[40];

		snprintf(req, sizeof(req),
			 "{\"op\":\"port.status\",\"name\":\"%s\"}", names[i]);
		if ((resp = nosaic_query_ask(fd, req)) == NULL) {
			no_datapath();
			nosaic_query_close(fd);
			return 1;
		}
		if (strstr(resp, "\"ok\":true") == NULL) {
			free(resp);
			continue;
		}
		put(&t, r, 0, names[i]);
		put(&t, r, 1, nosaic_jbool(resp, "AdminUp", 0) ? "up" : "down");
		put(&t, r, 2, nosaic_jbool(resp, "OperUp", 0) ? "up" : "down");
		snprintf(s, sizeof(s), "%d", nosaic_jint(resp, "SpeedMbps", 0));
		put(&t, r, 3, s);
		snprintf(s, sizeof(s), "%d", nosaic_jint(resp, "MTU", 0));
		put(&t, r, 4, s);
		r++;
		free(resp);
	}
	nosaic_query_close(fd);
	emit(&t);
	return 0;
}

int nosaic_show_routes(void)
{
	char *resp = nosaic_query_once(NOSAIC_QUERY_SOCKET, "{\"op\":\"l3.routes\"}");
	struct table t;
	const char *p;
	int r = 1;

	if (resp == NULL) {
		no_datapath();
		return 1;
	}
	if (refused(resp)) {
		free(resp);
		return 1;
	}
	memset(&t, 0, sizeof(t));
	put(&t, 0, 0, "PREFIX"); put(&t, 0, 1, "NEXT-HOPS");

	p = strstr(resp, "\"result\":[");
	p = p ? p + strlen("\"result\":[") : resp;
	while ((p = strchr(p, '{')) != NULL && r < MAXPORTS) {
		char pre[40], s[40];

		nosaic_jstr(p, "prefix", pre, sizeof(pre));
		if (pre[0] != '\0') {
			put(&t, r, 0, pre);
			/* The chip reports the egress interface it resolved to, not a
			 * gateway address: it holds an index, not the neighbour. Saying
			 * which is which beats printing a blank column. */
			snprintf(s, sizeof(s), "intf %d", nosaic_jint(p, "intf", 0));
			put(&t, r, 1, s);
			r++;
		}
		p++;
	}
	free(resp);
	if (r == 1) {
		printf("no routes\n");
		return 0;
	}
	emit(&t);
	return 0;
}
