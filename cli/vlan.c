/*
 * nosaic vlan | svi | switchport | lag | show vlans | show lags -- the C
 * CLI's half of switchapi 1.2 and 1.3, for the board the Go CLI cannot run on.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Same words, same meaning as cmd/nosaic/vlan.go: a configuration line that
 * works on one switch works on every switch, which is what the single-CLI
 * commitment is for. `switchport` states a port's whole membership and
 * removes what the line does not name, so it can be applied over and over.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "query.h"
#include "show.h"

#define MAX_VID 4096

/* One request, and its error printed if it failed. 0 on success. */
static int ask(const char *req)
{
	char *resp = nosaic_query_once(NOSAIC_QUERY_SOCKET, req);

	if (resp == NULL) {
		nosaic_query_explain(NOSAIC_QUERY_SOCKET);
		return 1;
	}
	if (strstr(resp, "\"ok\":true") == NULL) {
		char err[256];

		nosaic_jstr(resp, "error", err, sizeof(err));
		fprintf(stderr, "nosaic: %s\n", err[0] ? err : resp);
		free(resp);
		return 1;
	}
	free(resp);
	return 0;
}

static int parse_vid(const char *s)
{
	char *end;
	long v = strtol(s, &end, 10);

	if (*s == '\0' || *end != '\0' || v < 1 || v > 4094) {
		fprintf(stderr, "nosaic: vlan \"%s\" is not a number from 1 to 4094\n", s);
		return -1;
	}
	return (int)v;
}

int nosaic_vlan_cmd(int argc, char **argv)
{
	char req[128];
	int vid;

	if (argc != 4 || (strcmp(argv[2], "add") != 0 && strcmp(argv[2], "del") != 0)) {
		fprintf(stderr, "usage: nosaic vlan add|del <vid>\n");
		return 2;
	}
	if ((vid = parse_vid(argv[3])) < 0)
		return 2;
	snprintf(req, sizeof(req), "{\"op\":\"vlan.%s\",\"args\":{\"vid\":%d}}",
		 argv[2], vid);
	return ask(req);
}

int nosaic_svi_cmd(int argc, char **argv)
{
	char req[128];
	int vid;

	if (argc != 4 || (strcmp(argv[2], "add") != 0 && strcmp(argv[2], "del") != 0)) {
		fprintf(stderr, "usage: nosaic svi add|del <vid>\n");
		return 2;
	}
	if ((vid = parse_vid(argv[3])) < 0)
		return 2;
	snprintf(req, sizeof(req), "{\"op\":\"svi.%s\",\"args\":{\"vid\":%d}}",
		 argv[2], vid);
	if (ask(req) != 0)
		return 1;
	if (strcmp(argv[2], "add") == 0)
		printf("vlan%d\n", vid);
	return 0;
}

/*
 * The `vlans` answer, walked for one port: have[vid] is 0 (not a member),
 * 1 (untagged) or 2 (tagged). The shape is ours --
 *   {"VID":10,"SVI":true,"Members":[{"Port":"swp1","Tagged":false},...]}
 * -- so finding each record by its key is enough.
 */
static char *vlans(void)
{
	char *resp = nosaic_query_once(NOSAIC_QUERY_SOCKET, "{\"op\":\"vlans\"}");

	if (resp == NULL) {
		nosaic_query_explain(NOSAIC_QUERY_SOCKET);
		return NULL;
	}
	if (strstr(resp, "\"ok\":true") == NULL) {
		char err[256];

		nosaic_jstr(resp, "error", err, sizeof(err));
		fprintf(stderr, "nosaic: %s\n", err[0] ? err : resp);
		free(resp);
		return NULL;
	}
	return resp;
}

static void membership(const char *resp, const char *port, unsigned char *have)
{
	const char *v = resp;
	char pat[96];

	memset(have, 0, MAX_VID);
	snprintf(pat, sizeof(pat), "{\"Port\":\"%s\",", port);
	while ((v = strstr(v, "{\"VID\":")) != NULL) {
		int vid = atoi(v + 7);
		const char *next = strstr(v + 1, "{\"VID\":");
		const char *m = strstr(v, pat);

		if (m != NULL && (next == NULL || m < next) && vid > 0 && vid < MAX_VID)
			have[vid] = strncmp(m + strlen(pat), "\"Tagged\":true", 13) == 0 ? 2 : 1;
		v++;
	}
}

int nosaic_switchport_cmd(int argc, char **argv)
{
	static const char usage[] =
		"usage: nosaic switchport <port> access <vid>\n"
		"       nosaic switchport <port> trunk <vid,...> [native <vid>]\n"
		"       nosaic switchport <port> none\n";
	unsigned char want[MAX_VID], have[MAX_VID];
	const char *port;
	char req[256], *resp;
	int vid, rc = 0;

	if (argc < 4) {
		fputs(usage, stderr);
		return 2;
	}
	port = argv[2];
	memset(want, 0, sizeof(want));
	if (strcmp(argv[3], "access") == 0 && argc == 5) {
		if ((vid = parse_vid(argv[4])) < 0)
			return 2;
		want[vid] = 1;
	} else if (strcmp(argv[3], "trunk") == 0 && (argc == 5 || argc == 7)) {
		char list[256], *tok, *save = NULL;

		snprintf(list, sizeof(list), "%s", argv[4]);
		for (tok = strtok_r(list, ",", &save); tok != NULL;
		     tok = strtok_r(NULL, ",", &save)) {
			if ((vid = parse_vid(tok)) < 0)
				return 2;
			want[vid] = 2;
		}
		if (argc == 7) {
			if (strcmp(argv[5], "native") != 0) {
				fputs(usage, stderr);
				return 2;
			}
			if ((vid = parse_vid(argv[6])) < 0)
				return 2;
			want[vid] = 1;
		}
	} else if (strcmp(argv[3], "none") != 0 || argc != 4) {
		fputs(usage, stderr);
		return 2;
	}

	if ((resp = vlans()) == NULL)
		return 1;
	membership(resp, port, have);
	free(resp);

	/* Removals first, so a native VLAN moving elsewhere is not briefly two. */
	for (vid = 1; vid < MAX_VID; vid++) {
		if (have[vid] && !want[vid]) {
			snprintf(req, sizeof(req), "{\"op\":\"vlan.port.del\",\"args\":"
				 "{\"vid\":%d,\"port\":\"%s\"}}", vid, port);
			rc |= ask(req);
		}
	}
	for (vid = 1; vid < MAX_VID; vid++) {
		if (!want[vid] || want[vid] == have[vid])
			continue;
		snprintf(req, sizeof(req), "{\"op\":\"vlan.port\",\"args\":"
			 "{\"vid\":%d,\"port\":\"%s\"%s}}", vid, port,
			 want[vid] == 2 ? ",\"tagged\":true" : "");
		rc |= ask(req);
	}
	return rc;
}

int nosaic_show_vlans(void)
{
	char *resp = vlans();
	const char *v;
	int any = 0;

	if (resp == NULL)
		return 1;
	for (v = resp; (v = strstr(v, "{\"VID\":")) != NULL; v++) {
		const char *next = strstr(v + 1, "{\"VID\":");
		const char *m = v;
		char untagged[512] = "", tagged[512] = "";
		int vid = atoi(v + 7);

		if (!any)
			printf("%-6s%-9s%-24s%s\n", "VLAN", "SVI", "UNTAGGED", "TAGGED");
		any = 1;
		while ((m = strstr(m, "{\"Port\":\"")) != NULL && (next == NULL || m < next)) {
			char name[64];
			const char *q = m + 9;
			size_t n = 0;
			char *dst;

			while (*q != '"' && *q != '\0' && n + 1 < sizeof(name))
				name[n++] = *q++;
			name[n] = '\0';
			/* {"Port":"<name>","Tagged":true|false} -- q is at the
			 * name's closing quote. */
			dst = strncmp(q, "\",\"Tagged\":true", 15) == 0 ? tagged : untagged;
			if (dst[0] != '\0')
				strncat(dst, ",", 511 - strlen(dst));
			strncat(dst, name, 511 - strlen(dst));
			m++;
		}
		{
			char svi[16] = "-";

			if (strstr(v, "\"SVI\":true") != NULL &&
			    (next == NULL || strstr(v, "\"SVI\":true") < next))
				snprintf(svi, sizeof(svi), "vlan%d", vid);
			printf("%-6d%-9s%-24s%s\n", vid, svi,
			       untagged[0] ? untagged : "-", tagged[0] ? tagged : "-");
		}
	}
	if (!any)
		printf("no vlans; add one with: nosaic vlan add <vid>\n");
	free(resp);
	return 0;
}

/*
 * nosaic lag <poN> lacp|static <port,...>  |  nosaic lag <poN> none
 *
 * The LAG's whole configuration, like cmd/nosaic/lag.go: members the line
 * does not name leave.
 */
int nosaic_lag_cmd(int argc, char **argv)
{
	static const char usage[] =
		"usage: nosaic lag <poN> lacp|static <port,...>\n"
		"       nosaic lag <poN> none\n";
	char req[1024], list[512], *tok, *save = NULL;
	size_t n;
	int first = 1;

	if (argc == 4 && strcmp(argv[3], "none") == 0) {
		snprintf(req, sizeof(req), "{\"op\":\"lag.del\",\"args\":"
			 "{\"name\":\"%s\"}}", argv[2]);
		return ask(req);
	}
	if (argc != 5 || (strcmp(argv[3], "lacp") != 0 && strcmp(argv[3], "static") != 0)) {
		fputs(usage, stderr);
		return 2;
	}
	snprintf(req, sizeof(req), "{\"op\":\"lag.add\",\"args\":"
		 "{\"name\":\"%s\"%s}}", argv[2],
		 strcmp(argv[3], "lacp") == 0 ? ",\"lacp\":true" : "");
	if (ask(req) != 0)
		return 1;
	n = (size_t)snprintf(req, sizeof(req), "{\"op\":\"lag.members\",\"args\":"
			     "{\"name\":\"%s\",\"ports\":[", argv[2]);
	snprintf(list, sizeof(list), "%s", argv[4]);
	for (tok = strtok_r(list, ",", &save); tok != NULL && n < sizeof(req);
	     tok = strtok_r(NULL, ",", &save)) {
		n += (size_t)snprintf(req + n, sizeof(req) - n, "%s\"%s\"",
				      first ? "" : ",", tok);
		first = 0;
	}
	if (n < sizeof(req))
		snprintf(req + n, sizeof(req) - n, "]}}");
	return ask(req);
}

int nosaic_show_lags(void)
{
	char *resp = nosaic_query_once(NOSAIC_QUERY_SOCKET, "{\"op\":\"lags\"}");
	const char *g;
	int any = 0;

	if (resp == NULL) {
		nosaic_query_explain(NOSAIC_QUERY_SOCKET);
		return 1;
	}
	if (strstr(resp, "\"ok\":true") == NULL) {
		char err[256];

		nosaic_jstr(resp, "error", err, sizeof(err));
		fprintf(stderr, "nosaic: %s\n", err[0] ? err : resp);
		free(resp);
		return 1;
	}
	/* {"Name":"po1","LACP":true,...,"Members":[{"Port":"swp1","Active":true,...}]} */
	for (g = resp; (g = strstr(g, "{\"Name\":\"")) != NULL; g++) {
		const char *next = strstr(g + 1, "{\"Name\":\"");
		const char *m = g;
		char name[16], act[512] = "", inact[512] = "";
		size_t n = 0;
		const char *q = g + 9;

		while (*q != '"' && *q != '\0' && n + 1 < sizeof(name))
			name[n++] = *q++;
		name[n] = '\0';
		if (!any)
			printf("%-6s%-8s%-24s%s\n", "LAG", "MODE", "ACTIVE", "INACTIVE");
		any = 1;
		while ((m = strstr(m, "{\"Port\":\"")) != NULL && (next == NULL || m < next)) {
			char port[64];
			char *dst;

			q = m + 9;
			n = 0;
			while (*q != '"' && *q != '\0' && n + 1 < sizeof(port))
				port[n++] = *q++;
			port[n] = '\0';
			dst = strncmp(q, "\",\"Active\":true", 15) == 0 ? act : inact;
			if (dst[0] != '\0')
				strncat(dst, ",", 511 - strlen(dst));
			strncat(dst, port, 511 - strlen(dst));
			m++;
		}
		printf("%-6s%-8s%-24s%s\n", name,
		       strncmp(g + 9 + strlen(name), "\",\"LACP\":true", 13) == 0 ?
		       "lacp" : "static", act[0] ? act : "-", inact[0] ? inact : "-");
	}
	if (!any)
		printf("no lags; add one with: nosaic lag po1 lacp <port,...>\n");
	free(resp);
	return 0;
}
