/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The client half of the datapath's query socket.
 *
 * Shared, because two different commands ask the same daemon the same way and
 * the copy that existed in one of them read its responses incorrectly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "query.h"

int nosaic_query_open(const char *path)
{
	struct sockaddr_un a;
	int fd;

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return -1;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

void nosaic_query_close(int fd)
{
	if (fd >= 0)
		close(fd);
}

char *nosaic_query_ask(int fd, const char *request)
{
	char *buf;
	size_t cap = 65536, n = 0;
	ssize_t r;

	if (fd < 0)
		return NULL;
	if (dprintf(fd, "%s\n", request) < 0)
		return NULL;

	if ((buf = malloc(cap)) == NULL)
		return NULL;
	/* One line. See the header for why this is not read-to-EOF. */
	while (n + 1 < cap) {
		if ((r = read(fd, buf + n, 1)) <= 0)
			break;
		if (buf[n] == '\n') {
			n++;
			break;
		}
		n++;
	}
	if (n == 0) {
		free(buf);
		return NULL;
	}
	buf[n] = '\0';
	return buf;
}

char *nosaic_query_once(const char *path, const char *request)
{
	int fd = nosaic_query_open(path);
	char *resp;

	if (fd < 0)
		return NULL;
	resp = nosaic_query_ask(fd, request);
	nosaic_query_close(fd);
	return resp;
}

int nosaic_jint(const char *rec, const char *key, int missing)
{
	char pat[64];
	const char *p;

	snprintf(pat, sizeof(pat), "\"%s\":", key);
	if ((p = strstr(rec, pat)) == NULL)
		return missing;
	return atoi(p + strlen(pat));
}

void nosaic_jstr(const char *rec, const char *key, char *out, size_t len)
{
	char pat[64];
	const char *p, *e;

	*out = '\0';
	snprintf(pat, sizeof(pat), "\"%s\":\"", key);
	if ((p = strstr(rec, pat)) == NULL)
		return;
	p += strlen(pat);
	if ((e = strchr(p, '"')) == NULL)
		return;
	if ((size_t)(e - p) >= len)
		return;
	memcpy(out, p, (size_t)(e - p));
	out[e - p] = '\0';
}

int nosaic_jbool(const char *rec, const char *key, int missing)
{
	char pat[64];
	const char *p;

	snprintf(pat, sizeof(pat), "\"%s\":", key);
	if ((p = strstr(rec, pat)) == NULL)
		return missing;
	p += strlen(pat);
	while (*p == ' ')
		p++;
	if (strncmp(p, "true", 4) == 0)
		return 1;
	if (strncmp(p, "false", 5) == 0)
		return 0;
	return missing;
}
