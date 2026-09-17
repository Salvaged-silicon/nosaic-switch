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
#define MAXCOLS  NOSAIC_TABLE_COLS

/* The layout itself is in show.h, shared with optics.c: see the note there. */
#define table nosaic_table

void nosaic_table_put(struct table *t, int r, int c, const char *s)
{
	if (r >= NOSAIC_TABLE_ROWS || c >= MAXCOLS)
		return;
	snprintf(t->cell[r][c], sizeof(t->cell[r][c]), "%s", s);
	if (r + 1 > t->rows)
		t->rows = r + 1;
	if (c + 1 > t->cols)
		t->cols = c + 1;
}

void nosaic_table_emit(const struct table *t)
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

#define put  nosaic_table_put
#define emit nosaic_table_emit

static void no_datapath(void)
{
	/* Reads errno, so nothing may intervene between the failed call and this.
	 * The cases it separates -- root-only socket, absent socket, stale socket
	 * -- used to share one message that described only the second. */
	nosaic_query_explain(NOSAIC_QUERY_SOCKET);
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
	if (nosaic_jbool(resp, "ACL", 0)) {
		snprintf(s, sizeof(s), "yes, %d rules", nosaic_jint(resp, "ACLEntries", 0));
		put(&t, r, 0, "acl"); put(&t, r++, 1, s);
	} else {
		put(&t, r, 0, "acl"); put(&t, r++, 1, "no");
	}

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

/* Bytes an operator can read at a glance. 64 MiB is a size people recognise;
 * 67108864 is one they have to count the digits of -- and counting them wrong
 * is how a full pool gets mistaken for an empty one. */
static void human_bytes(char *out, size_t len, unsigned long long n)
{
	if (n >= (1ULL << 20))
		snprintf(out, len, "%.1f MiB", (double)n / (double)(1ULL << 20));
	else if (n >= (1ULL << 10))
		snprintf(out, len, "%.1f KiB", (double)n / (double)(1ULL << 10));
	else
		snprintf(out, len, "%llu B", n);
}

int nosaic_show_dma(void)
{
	char *resp = nosaic_query_once(NOSAIC_QUERY_SOCKET, "{\"op\":\"asic.dma\"}");
	char b1[32], b2[32], b3[32];
	unsigned long long bytes, used, largest, peak, fails;
	const char *p;

	if (resp == NULL) {
		no_datapath();
		return 1;
	}
	if (refused(resp)) {
		free(resp);
		return 1;
	}

	bytes   = (unsigned long long)nosaic_jint(resp, "Bytes", 0);
	used    = (unsigned long long)nosaic_jint(resp, "Used", 0);
	largest = (unsigned long long)nosaic_jint(resp, "Largest", 0);
	peak    = (unsigned long long)nosaic_jint(resp, "Peak", 0);
	fails   = (unsigned long long)nosaic_jint(resp, "Fails", 0);

	human_bytes(b1, sizeof(b1), bytes);
	human_bytes(b2, sizeof(b2), used);
	human_bytes(b3, sizeof(b3), largest);
	printf("pool                %s\n", b1);
	printf("used                %s (%llu%%)\n", b2,
	       bytes ? used * 100ULL / bytes : 0ULL);
	human_bytes(b2, sizeof(b2), peak);
	printf("peak                %s\n", b2);
	/* Beside `used`, this is what separates a full pool from a fragmented
	 * one -- two faults with different fixes that the totals alone cannot
	 * tell apart. */
	printf("largest free        %s\n", b3);
	printf("failed allocations  %llu\n", fails);

	/* A name whose outstanding total climbs with uptime is a leak. That is
	 * the question this table exists to answer, and the reason it exists is
	 * that it could not be answered from the switch the first time. */
	if ((p = strstr(resp, "\"Callers\":[")) != NULL && strstr(p, "{") != NULL) {
		printf("\n%-24s %12s %12s %10s %10s %8s\n",
		       "CALLER", "OUTSTANDING", "PEAK", "ALLOCS", "FREES", "FAILS");
		for (p = strchr(p, '{'); p != NULL; p = strchr(p + 1, '{')) {
			char name[NOSAIC_DMA_NAME_MAX];

			nosaic_jstr(p, "Name", name, sizeof(name));
			human_bytes(b1, sizeof(b1),
				    (unsigned long long)nosaic_jint(p, "Outstanding", 0));
			human_bytes(b2, sizeof(b2),
				    (unsigned long long)nosaic_jint(p, "Peak", 0));
			printf("%-24s %12s %12s %10d %10d %8d\n", name, b1, b2,
			       nosaic_jint(p, "Allocs", 0),
			       nosaic_jint(p, "Frees", 0),
			       nosaic_jint(p, "Fails", 0));
		}
	}
	free(resp);
	return 0;
}

/* A hit count is a 64-bit number and nosaic_jint is not; a counter past two
 * billion would read as garbage, and a busy port gets there in an afternoon. */
static unsigned long long jcount(const char *rec, const char *key)
{
	char pat[48];
	const char *p;

	snprintf(pat, sizeof(pat), "\"%s\":", key);
	p = strstr(rec, pat);
	return p ? strtoull(p + strlen(pat), NULL, 10) : 0;
}

int nosaic_show_acl(void)
{
	char *resp = nosaic_query_once(NOSAIC_QUERY_SOCKET, "{\"op\":\"acl\"}");
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
	if (!nosaic_jbool(resp, "Available", 0)) {
		/* Stated as a fact about the silicon, because it is one: the
		 * datapath asked for a field group and was refused. */
		fprintf(stderr, "nosaic: this switch's datapath has no field group "
			"for access lists\n");
		free(resp);
		return 1;
	}
	memset(&t, 0, sizeof(t));
	put(&t, 0, 0, "SEQ"); put(&t, 0, 1, "ACTION"); put(&t, 0, 2, "MATCH");
	put(&t, 0, 3, "PACKETS"); put(&t, 0, 4, "STATUS");

	p = strstr(resp, "\"Rules\":[");
	p = p ? p + strlen("\"Rules\":[") : resp;
	while ((p = strchr(p, '{')) != NULL && r < NOSAIC_TABLE_ROWS) {
		char rule[160], err[80], s[40], *sp;

		nosaic_jstr(p, "Rule", rule, sizeof(rule));
		nosaic_jstr(p, "Error", err, sizeof(err));
		snprintf(s, sizeof(s), "%d", nosaic_jint(p, "Seq", 0));
		put(&t, r, 0, s);
		/* "deny in swp6 proto icmp" is an action and a match, and they
		 * read better apart. A rule that failed to parse may have
		 * neither, in which case the text is shown as it was written. */
		sp = strchr(rule, ' ');
		if (sp != NULL) {
			*sp = '\0';
			put(&t, r, 1, rule);
			put(&t, r, 2, sp + 1);
		} else {
			put(&t, r, 1, rule);
			put(&t, r, 2, "any");
		}
		snprintf(s, sizeof(s), "%llu", jcount(p, "Packets"));
		put(&t, r, 3, s);
		if (nosaic_jbool(p, "Installed", 0))
			put(&t, r, 4, err[0] ? err : "in chip");
		else
			put(&t, r, 4, err[0] ? err : "not installed");
		r++;
		p++;
	}
	free(resp);
	if (r == 1) {
		printf("no rules; set one with: nosaic config set acl_<seq> "
		       "\"deny|permit [in <port>] [proto <p>] [src <cidr>] "
		       "[dst <cidr>] [sport <n>] [dport <n>]\"\n");
		return 0;
	}
	emit(&t);
	return 0;
}
