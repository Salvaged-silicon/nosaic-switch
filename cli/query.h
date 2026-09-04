/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_CLI_QUERY_H
#define NOSAIC_CLI_QUERY_H

#include <stddef.h>

/* Where the datapath serves its state. Matches datapath/common/query.h and
 * internal/nosd/proto.SocketPath. */
#define NOSAIC_QUERY_SOCKET "/run/nosd.sock"

/* Open a connection to the datapath. Returns -1 if it is not there. */
int nosaic_query_open(const char *path);
void nosaic_query_close(int fd);

/* Send one request and read one response.
 *
 * The caller owns the returned buffer. NULL means the connection failed.
 *
 * One request, one LINE back -- deliberately not "read until the peer closes".
 * The server multiplexes: it keeps the connection open and waits for the next
 * request, so a client reading to EOF blocks forever while the server blocks
 * waiting for it. That deadlock does not exist against an older daemon that
 * closed after answering, which is exactly the kind of bug that appears the
 * day a switch is upgraded and not before.
 */
char *nosaic_query_ask(int fd, const char *request);

/* Open, ask once, close. For the commands that need a single answer. */
char *nosaic_query_once(const char *path, const char *request);

/* One flat JSON record's fields. Enough for responses whose shape we define,
 * and far less code than a parser. */
int nosaic_jint(const char *rec, const char *key, int missing);
void nosaic_jstr(const char *rec, const char *key, char *out, size_t len);
int nosaic_jbool(const char *rec, const char *key, int missing);

#endif
