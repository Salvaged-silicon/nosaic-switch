/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The switch-api socket, for the FM6000.
 *
 * Newline-delimited JSON on /run/nosd.sock, exactly as every other datapath in
 * this tree serves it -- the CLI, the config model and the HAL above must not
 * be able to tell which silicon answered. internal/nosd/proto and
 * internal/switchapi are the specification; datapath/common/query.c is another
 * implementation of it, not the definition, and it cannot be reused here
 * because every answer in it comes from a bcm_* call.
 *
 * WHAT THIS ANSWERS TODAY IS MOSTLY "NO", AND THAT IS THE POINT. A datapath
 * that reports capabilities it does not have produces a switch that accepts
 * configuration and silently does not apply it. Capabilities here are derived
 * from what the chip has actually been brought up to do, so they become true
 * one at a time as the port progresses, and `nosaic show caps` is worth
 * reading from the first boot.
 */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "pci.h"
#include "regs.h"
#include "serdes.h"
#include "sock.h"

#ifndef NOSAIC_QUERY_DRIVER
#define NOSAIC_QUERY_DRIVER "fm6000"
#endif

static struct fm6000 *sock_dev;
static const char *sock_path;

/* Admin state for ports 1..8; index 0 unused. */
static bool port_admin_up[9];

/* Pull an integer argument out of a request. The protocol is deliberately dull
 * and so is this: a real JSON parser here would be more code than the whole
 * server. Same approach as the Broadcom datapath's query.c. */
static long req_int(const char *req, const char *key, long dflt)
{
	char pat[64];
	const char *p;

	snprintf(pat, sizeof(pat), "\"%s\":", key);
	if ((p = strstr(req, pat)) == NULL)
		return dflt;
	return strtol(p + strlen(pat), NULL, 0);
}

/* Pull a string argument out of a request into buf[len]. Returns buf on
 * success or NULL if the key is absent or the value does not fit. */
static char *req_str(const char *req, const char *key, char *buf, size_t len)
{
	char pat[64];
	const char *p, *q;
	size_t n;

	snprintf(pat, sizeof(pat), "\"%s\":\"", key);
	if ((p = strstr(req, pat)) == NULL)
		return NULL;
	p += strlen(pat);
	if ((q = strchr(p, '"')) == NULL)
		return NULL;
	n = (size_t)(q - p);
	if (n >= len)
		return NULL;
	memcpy(buf, p, n);
	buf[n] = '\0';
	return buf;
}

/* Parse "etN" -> N. Returns 0 on failure. */
static int port_from_name(const char *name)
{
	long n;

	if (name == NULL || (name[0] != 'e' || name[1] != 't'))
		return 0;
	n = strtol(name + 2, NULL, 10);
	if (n < 1 || n > 8)
		return 0;
	return (int)n;
}

/* Read PORT_STATUS and LANE_STATUS for a port. Both are 0 on failure. */
static void read_port_hw(const struct fm_port *p,
			 uint32_t *port_status, uint32_t *lane_status)
{
	uint32_t lb = FM6000_EPL_LANE(p->epl, p->lane);

	*port_status = 0;
	*lane_status = 0;
	if (sock_dev == NULL)
		return;
	fm_rd(sock_dev, lb, port_status);
	fm_rd(sock_dev, lb + FM6000_EPL_LANE_STATUS_OFF, lane_status);
}

static void answer(FILE *out, const char *req)
{
	/*
	 * capabilities. Every one of these is false or zero because none of it
	 * has been demonstrated on this chip yet, and the honest answer to
	 * "can you route" on a board whose ports have never linked is no.
	 *
	 * The fields become true as milestones land, and each should be turned
	 * on by the code that makes it true rather than in advance.
	 */
	if (strstr(req, "\"capabilities\"") != NULL) {
		/*
		 * EVERY field of switchapi.Capabilities, including the ones
		 * that are false -- not just the interesting ones.
		 *
		 * Omitting a field is not neutral: the Go side unmarshals the
		 * zero value and the CLI reports it as a definite "no". The
		 * Trident2+ datapath left ECMP out of this response and `show
		 * caps` answered "ecmp no" on a switch that had a working ECMP
		 * group in the chip, so an operator reading it would have
		 * designed around a limitation the board did not have. Here the
		 * zeroes happen to be true, which is exactly when the habit of
		 * omitting them takes hold.
		 */
		fprintf(out,
			"{\"ok\":true,\"result\":{\"Contract\":\"1.1\","
			"\"Driver\":\"%s\",\"MaxPorts\":8,"
			"\"VLANs\":false,\"MaxVLANs\":0,"
			"\"L2Learning\":false,\"MaxFDB\":0,"
			"\"L3\":false,\"MaxV4\":0,\"MaxV6\":0,\"IPv6\":false,"
			"\"ECMP\":false,\"MaxECMP\":0,"
			"\"ACL\":false,\"ACLEntries\":0,\"ACLSlices\":0,"
			"\"ACL6\":false,\"ACL6Entries\":0,"
			"\"Counters\":false,\"SFP\":false,\"Breakout\":false}}\n",
			NOSAIC_QUERY_DRIVER);
		return;
	}

	/* ports: enumerate the eight known front-panel ports. */
	if (strstr(req, "\"ports\"") != NULL &&
	    strstr(req, "\"asic.ports\"") == NULL) {
		int i;
		const char *sep = "";

		fprintf(out, "{\"ok\":true,\"result\":[");
		for (i = 1; i <= 8; i++) {
			const struct fm_port *p = fm_port_lookup(i);

			if (p == NULL)
				continue;
			fprintf(out, "%s{\"Name\":\"et%d\",\"Index\":%d}",
				sep, p->port, p->port);
			sep = ",";
		}
		fprintf(out, "]}\n");
		return;
	}

	/* port.status: read PORT_STATUS and LANE_STATUS from hardware. */
	if (strstr(req, "\"port.status\"") != NULL) {
		char name[32];
		const struct fm_port *p;
		uint32_t ps, ls;
		bool admin_up, oper_up;
		int port;

		if (req_str(req, "name", name, sizeof(name)) == NULL) {
			fprintf(out, "{\"ok\":false,\"error\":\"missing name\"}\n");
			return;
		}
		if ((port = port_from_name(name)) == 0 ||
		    (p = fm_port_lookup(port)) == NULL) {
			fprintf(out, "{\"ok\":false,\"error\":\"unknown port\"}\n");
			return;
		}
		read_port_hw(p, &ps, &ls);
		admin_up = port_admin_up[port];
		oper_up = (ps & FM6000_PORT_STATUS_UP) == FM6000_PORT_STATUS_UP &&
			  ls == 0x940u;
		fprintf(out,
			"{\"ok\":true,\"result\":{"
			"\"Name\":\"%s\","
			"\"AdminUp\":%s,"
			"\"OperUp\":%s,"
			"\"SpeedMbps\":10000,"
			"\"FullDuplex\":true,"
			"\"MTU\":9216}}\n",
			name,
			admin_up ? "true" : "false",
			oper_up  ? "true" : "false");
		return;
	}

	/* port.admin: bring a port up or down. */
	if (strstr(req, "\"port.admin\"") != NULL) {
		char name[32];
		const struct fm_port *p;
		bool up;
		int port;

		if (req_str(req, "name", name, sizeof(name)) == NULL) {
			fprintf(out, "{\"ok\":false,\"error\":\"missing name\"}\n");
			return;
		}
		if ((port = port_from_name(name)) == 0 ||
		    (p = fm_port_lookup(port)) == NULL) {
			fprintf(out, "{\"ok\":false,\"error\":\"unknown port\"}\n");
			return;
		}
		up = strstr(req, "\"up\":true") != NULL;
		if (up) {
			struct fm_lane_report rep;
			int rv;

			if (sock_dev == NULL) {
				fprintf(out,
					"{\"ok\":false,\"error\":\"no chip attached\"}\n");
				return;
			}
			memset(&rep, 0, sizeof(rep));
			rv = fm_lane_enable(sock_dev, p, &rep);
			if (rv != FM_OK) {
				fprintf(out,
					"{\"ok\":false,\"error\":\"lane enable failed at step %d\"}\n",
					rep.reached);
				return;
			}
		}
		port_admin_up[port] = up;
		fprintf(out, "{\"ok\":true,\"result\":{}}\n");
		return;
	}

	/*
	 * asic.state -- this board's own diagnostic, not part of the contract.
	 *
	 * It exists because the single most important fact about this chip is
	 * one the contract has no field for: whether it is still on the PCIe
	 * bus. An operator looking at a switch that has stopped working needs
	 * that before anything else, and needs it without running a tool that
	 * might itself touch the chip.
	 */
	if (strstr(req, "\"asic.state\"") != NULL) {
		int off = sock_dev ? fm_is_offbus(sock_dev) : 1;

		fprintf(out,
			"{\"ok\":true,\"result\":{\"Slot\":\"%s\","
			"\"BarBytes\":%zu,\"OnBus\":%s,\"BanksInitialised\":%s,"
			"\"Reads\":%llu,\"Writes\":%llu,\"Refused\":%llu}}\n",
			sock_dev ? sock_dev->slot : "",
			sock_dev ? sock_dev->bar_bytes : 0,
			off ? "false" : "true",
			(sock_dev && sock_dev->banks_ready) ? "true" : "false",
			sock_dev ? sock_dev->reads : 0,
			sock_dev ? sock_dev->writes : 0,
			sock_dev ? sock_dev->refused : 0);
		return;
	}

	/*
	 * asic.reg -- read one register by word address.
	 *
	 * Read-only, and it goes through the same guard as everything else, so
	 * asking for a bank memory on a cold chip gets a refusal with a reason
	 * rather than a dead switch. That refusal is the feature.
	 */
	if (strstr(req, "\"asic.reg\"") != NULL) {
		uint32_t word = (uint32_t)req_int(req, "word", 0), v = 0;
		const char *why;
		int rv;

		if (sock_dev == NULL) {
			fprintf(out, "{\"ok\":false,\"error\":\"no chip attached\"}\n");
			return;
		}
		if ((why = fm_hazard(sock_dev, word, 0)) != NULL) {
			fprintf(out, "{\"ok\":false,\"error\":\"refused: %s\"}\n", why);
			return;
		}
		rv = fm_rd(sock_dev, word, &v);
		if (rv == FM_OK)
			fprintf(out,
				"{\"ok\":true,\"result\":{\"Word\":%u,"
				"\"Block\":\"%s\",\"Value\":%u}}\n",
				word, fm_block_name(word), v);
		else if (rv == FM_EOFFBUS)
			fprintf(out,
				"{\"ok\":false,\"error\":\"chip is off the PCIe bus\"}\n");
		else
			fprintf(out, "{\"ok\":false,\"error\":\"read failed\"}\n");
		return;
	}

	/*
	 * Everything else. Unsupported rather than an error, because the
	 * difference survives the wire and is what stops the capability model
	 * quietly ceasing to mean anything: "this hardware cannot yet" is not
	 * "this went wrong".
	 */
	fprintf(out,
		"{\"ok\":false,\"error\":\"not supported by this datapath: "
		"the fm6000 port does not forward yet\",\"unsupported\":true}\n");
}

static void *serve(void *arg)
{
	int lfd = *(int *)arg;

	for (;;) {
		int cfd = accept(lfd, NULL, NULL);
		FILE *f;
		char line[4096];

		if (cfd < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if ((f = fdopen(cfd, "r+")) == NULL) {
			close(cfd);
			continue;
		}
		/* As many requests per connection as the client sends: the CLI
		 * dials once and issues every call down the same socket, and a
		 * server that answers one and closes breaks the second with a
		 * write error that looks like a network fault. */
		while (fgets(line, sizeof(line), f) != NULL) {
			answer(f, line);
			fflush(f);
		}
		fclose(f);
	}
	return NULL;
}

int fm_sock_start(struct fm6000 *d, const char *path)
{
	static int lfd;
	struct sockaddr_un sa;
	pthread_t th;

	sock_dev = d;
	sock_path = path ? path : NOSAIC_QUERY_SOCKET;

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", sock_path);
	unlink(sock_path);

	if ((lfd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return -1;
	if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		close(lfd);
		return -1;
	}
	/* The CLI runs as a non-root operator; the daemon does not. */
	chmod(sock_path, 0666);
	if (listen(lfd, 8) != 0) {
		close(lfd);
		return -1;
	}
	if (pthread_create(&th, NULL, serve, &lfd) != 0) {
		close(lfd);
		return -1;
	}
	pthread_detach(th);
	return 0;
}
