/* SPDX-License-Identifier: Apache-2.0 */
/*
 * `nosaic acl add` and `nosaic acl del`: the contract's SetACL and DelACL,
 * from the CLI the PowerPC boards run. Same words, same answers as the Go one.
 *
 * The rule goes over the socket as text. The datapath parses it, refuses it
 * with a reason if it is wrong, persists it as the acl_<seq> setting so that
 * `config show` lists it and it survives an upgrade, and installs it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "query.h"
#include "show.h"

static const char usage[] =
"usage: nosaic acl add <seq> deny|permit [ipv4|ipv6] [in <port>] [proto <p>]\n"
"                      [src <prefix>] [dst <prefix>] [sport <n>] [dport <n>]\n"
"       nosaic acl del <seq>\n";

static void jquote(char *out, size_t len, const char *s)
{
	size_t n = 0;

	for (; *s != '\0' && n + 2 < len; s++) {
		if (*s == '"' || *s == '\\')
			out[n++] = '\\';
		out[n++] = *s;
	}
	out[n] = '\0';
}

int nosaic_acl_cmd(int argc, char **argv)
{
	char req[2048], rule[512], q[1024], *resp;
	int seq, i;
	size_t n = 0;

	if (argc < 4) {
		fputs(usage, stderr);
		return 2;
	}
	seq = atoi(argv[3]);
	if (seq <= 0) {
		fprintf(stderr, "nosaic: sequence '%s' is not a number\n", argv[3]);
		return 2;
	}
	if (strcmp(argv[2], "add") == 0) {
		if (argc < 5) {
			fputs(usage, stderr);
			return 2;
		}
		rule[0] = '\0';
		for (i = 4; i < argc; i++)
			n += snprintf(rule + n, sizeof(rule) - n, "%s%s", i > 4 ? " " : "", argv[i]);
		jquote(q, sizeof(q), rule);
		snprintf(req, sizeof(req), "{\"op\":\"acl.set\",\"args\":{\"seq\":%d,\"rule\":\"%s\"}}",
			 seq, q);
	} else if (strcmp(argv[2], "del") == 0) {
		snprintf(req, sizeof(req), "{\"op\":\"acl.del\",\"args\":{\"seq\":%d}}", seq);
	} else {
		fputs(usage, stderr);
		return 2;
	}

	resp = nosaic_query_once(NOSAIC_QUERY_SOCKET, req);
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
	if (strcmp(argv[2], "add") == 0)
		printf("acl_%d=%s\n", seq, rule);
	else
		printf("acl_%d removed\n", seq);
	return 0;
}
