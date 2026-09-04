/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Answering "is what Linux thinks configured actually in the chip?"
 *
 * Every diagnostic this daemon had before this either read a log or restarted
 * the datapath. `--stats` re-initialises the chip, so it cannot be used on a
 * switch that is carrying traffic -- which is the only switch anyone needs to
 * ask about. So a whole class of fault was invisible from the box: the port is
 * up, the address is on the interface, the route is in the kernel, and the
 * chip has none of it.
 *
 * That is not hypothetical. The 40G ports on this board had link, correct
 * addresses, correct VLANs and no adjacency for a day, because frames the chip
 * received were never punted to the CPU. Nothing on the switch could have shown
 * that; it took reading counters out of a log and comparing them by hand.
 *
 * This serves the chip's own answers on a socket, read-only, so the CLI can
 * put them beside what Linux believes and say where the two differ.
 *
 * The protocol is the one internal/nosd/proto defines: newline-delimited JSON,
 * one response per request, as many requests per connection as the client
 * cares to send. Deliberately dull -- it has to be debuggable with the tools in
 * a minimal image, and `nc -U /run/nosd.sock` has to work.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <bcm/error.h>
#include <bcm/port.h>
#include <bcm/stg.h>
#include <bcm/vlan.h>
#include <bcm/l3.h>
#include <bcm/types.h>

#include "tapbridge.h"
#include "query.h"

static int query_unit;

/*
 * What the chip holds for one port.
 *
 * Every field is READ BACK from the hardware rather than remembered from what
 * was programmed. A daemon reporting its own intentions would agree with itself
 * no matter what the chip did, which is exactly the failure this exists to
 * catch.
 */
static void port_json(FILE *out, int i)
{
	const char *name = NULL;
	unsigned char mac[6];
	int port = 0, want_vlan = 0, want_mtu = 0;
	int link = -1, enabled = -1, stp = -1, frame_max = -1, speed = -1;
	bcm_vlan_t pvid = 0;
	bcm_stg_t stg = -1;
	bcm_pbmp_t pbm, ubm;
	int cpu_member = -1;

	if (nosaic_tap_info(i, &name, &port, &want_vlan, &want_mtu, mac) != 0)
		return;

	if (bcm_port_link_status_get(query_unit, port, &link) != BCM_E_NONE)
		link = -1;
	if (bcm_port_enable_get(query_unit, port, &enabled) != BCM_E_NONE)
		enabled = -1;
	if (bcm_port_untagged_vlan_get(query_unit, port, &pvid) != BCM_E_NONE)
		pvid = 0;
	if (bcm_port_frame_max_get(query_unit, port, &frame_max) != BCM_E_NONE)
		frame_max = -1;
	if (bcm_port_speed_get(query_unit, port, &speed) != BCM_E_NONE)
		speed = -1;

	/*
	 * The spanning-tree state that decides forwarding is the one in the
	 * port's OWN VLAN's group, not the default group's. bcm_port_stp_get
	 * answers for the default and a per-port service VLAN is not in it, so a
	 * port can read FORWARD there and be blocking where it counts -- with no
	 * drop counter on this chip and the MAC still counting frames in.
	 */
	if (pvid != 0 && bcm_vlan_stg_get(query_unit, pvid, &stg) == BCM_E_NONE) {
		if (bcm_stg_stp_get(query_unit, stg, port, &stp) != BCM_E_NONE)
			stp = -1;
	}

	/*
	 * Whether the CPU is in the port's VLAN, which is what decides if
	 * anything arriving here can reach Linux at all. A port can be up,
	 * forwarding and receiving and still punt nothing, and this is the field
	 * that says so.
	 */
	BCM_PBMP_CLEAR(pbm);
	BCM_PBMP_CLEAR(ubm);
	if (pvid != 0 && bcm_vlan_port_get(query_unit, pvid, &pbm, &ubm) == BCM_E_NONE)
		cpu_member = BCM_PBMP_MEMBER(pbm, 0) ? 1 : 0;

	fprintf(out,
		"%s{\"name\":\"%s\",\"port\":%d,\"link\":%d,\"enabled\":%d,"
		"\"pvid\":%d,\"want_vlan\":%d,\"stg\":%d,\"stp\":%d,"
		"\"cpu_member\":%d,\"frame_max\":%d,\"speed\":%d,"
		"\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}",
		i ? "," : "", name, port, link, enabled, (int)pvid, want_vlan,
		(int)stg, stp, cpu_member, frame_max, speed,
		mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/*
 * The chip's forwarding table, as the chip holds it.
 *
 * Traversed rather than remembered. The daemon knows what it asked the chip to
 * install and that is exactly the thing not worth reporting: a route that was
 * accepted and then evicted, or displaced by a longer prefix, looks identical
 * from the caller's side. Reading DEFIP back is the only answer that can
 * disagree with the daemon, and disagreeing is the point.
 */
struct route_dump {
	FILE *out;
	int   n;
};

static int route_cb(int unit, int index, bcm_l3_route_t *r, void *ud)
{
	struct route_dump *d = ud;
	unsigned s, m;
	int bits = 0;

	(void)unit;
	/* IPv6 entries share the table and have no l3a_subnet; skipping them is
	 * honest rather than reporting a v4 prefix made of the wrong bytes. */
	if (r->l3a_flags & BCM_L3_IP6)
		return BCM_E_NONE;

	s = (unsigned)r->l3a_subnet;
	m = (unsigned)r->l3a_ip_mask;
	while (m & 0x80000000u) { bits++; m <<= 1; }

	fprintf(d->out,
		"%s{\"prefix\":\"%u.%u.%u.%u/%d\",\"intf\":%d,\"index\":%d,"
		"\"ecmp\":%d}",
		d->n ? "," : "",
		(s >> 24) & 0xff, (s >> 16) & 0xff, (s >> 8) & 0xff, s & 0xff, bits,
		(int)r->l3a_intf, index,
		(r->l3a_flags & BCM_L3_MULTIPATH) ? 1 : 0);
	d->n++;
	return BCM_E_NONE;
}

static void handle(FILE *out, const char *req)
{
	int i;

	/*
	 * Matched by substring rather than parsed.
	 *
	 * A JSON parser in C, for a request with one field, is more code than
	 * everything else here and more ways to be wrong. The op names are ours,
	 * they contain no punctuation, and an unrecognised request is refused --
	 * so the worst a malformed one does is get an error back.
	 */
	if (strstr(req, "\"asic.ports\"") != NULL) {
		fprintf(out, "{\"ok\":true,\"result\":[");
		for (i = 0; i < nosaic_tap_count(); i++)
			port_json(out, i);
		fprintf(out, "]}\n");
		return;
	}
	/*
	 * The contract's own operations, so the CLI runs against this chip
	 * unmodified.
	 *
	 * asic.ports below reports what the chip holds and is a diagnostic; these
	 * are switchapi, and the point of them is that `nosaic show ports` cannot
	 * tell which silicon answered. That is the claim the whole abstraction
	 * rests on, and it is only worth anything if the same command works here
	 * and on the virtual platform without editing it.
	 */
	if (strstr(req, "\"capabilities\"") != NULL) {
		bcm_l3_info_t info;
		int maxv4 = 0;

		bcm_l3_info_t_init(&info);
		if (bcm_l3_info(query_unit, &info) == BCM_E_NONE)
			maxv4 = info.l3info_max_route;

		fprintf(out,
			"{\"ok\":true,\"result\":{\"Contract\":\"1\","
			"\"Driver\":\"%s\",\"MaxPorts\":%d,\"VLANs\":true,"
			"\"MaxVLANs\":4094,\"L2Learning\":true,\"L3\":true,"
			"\"MaxV4\":%d}}\n",
			NOSAIC_QUERY_DRIVER, nosaic_tap_count(), maxv4);
		return;
	}

	if (strstr(req, "\"ports\"") != NULL &&
	    strstr(req, "\"asic.ports\"") == NULL) {
		int i;

		fprintf(out, "{\"ok\":true,\"result\":[");
		for (i = 0; i < nosaic_tap_count(); i++) {
			const char *name = NULL;
			unsigned char mac[6];
			int port = 0, vlan = 0, mtu = 0;

			if (nosaic_tap_info(i, &name, &port, &vlan, &mtu, mac) != 0)
				continue;
			fprintf(out, "%s{\"Name\":\"%s\",\"Index\":%d}",
				i ? "," : "", name, port);
		}
		fprintf(out, "]}\n");
		return;
	}

	if (strstr(req, "\"port.status\"") != NULL) {
		const char *p = strstr(req, "\"name\":\"");
		char want[32];
		int i;

		want[0] = '\0';
		if (p != NULL) {
			const char *e;

			p += strlen("\"name\":\"");
			e = strchr(p, '"');
			if (e != NULL && (size_t)(e - p) < sizeof(want)) {
				memcpy(want, p, (size_t)(e - p));
				want[e - p] = '\0';
			}
		}
		for (i = 0; i < nosaic_tap_count(); i++) {
			const char *name = NULL;
			unsigned char mac[6];
			int port = 0, vlan = 0, mtu = 0;
			int link = 0, ena = 0, speed = 0, fmax = 0;

			if (nosaic_tap_info(i, &name, &port, &vlan, &mtu, mac) != 0)
				continue;
			if (strcmp(name, want) != 0)
				continue;

			if (bcm_port_link_status_get(query_unit, port, &link) != BCM_E_NONE)
				link = 0;
			if (bcm_port_enable_get(query_unit, port, &ena) != BCM_E_NONE)
				ena = 0;
			if (bcm_port_speed_get(query_unit, port, &speed) != BCM_E_NONE)
				speed = 0;
			if (bcm_port_frame_max_get(query_unit, port, &fmax) != BCM_E_NONE)
				fmax = 0;

			fprintf(out,
				"{\"ok\":true,\"result\":{\"Name\":\"%s\","
				"\"AdminUp\":%s,\"OperUp\":%s,\"SpeedMbps\":%d,"
				"\"FullDuplex\":true,\"MTU\":%d}}\n",
				name, ena ? "true" : "false", link ? "true" : "false",
				speed, mtu ? mtu : fmax);
			return;
		}
		fprintf(out, "{\"ok\":false,\"error\":\"no such port\"}\n");
		return;
	}

	if (strstr(req, "\"l3.routes\"") != NULL) {
		struct route_dump d;
		int rv;

		bcm_l3_info_t info;
		int last = 0;

		/*
		 * The range is index 0 to the table's last entry, and it has to be
		 * asked for. Passing 0 as the end traverses nothing and returns
		 * BCM_E_NONE, so the caller sees an empty forwarding table and
		 * concludes the chip has no routes -- which is indistinguishable
		 * from the fault this command exists to find.
		 */
		bcm_l3_info_t_init(&info);
		if (bcm_l3_info(query_unit, &info) == BCM_E_NONE)
			last = info.l3info_max_route;

		d.out = out;
		d.n = 0;
		fprintf(out, "{\"ok\":true,\"result\":[");
		rv = last > 0
			? bcm_l3_route_traverse(query_unit, 0, 0, (uint32)last, route_cb, &d)
			: BCM_E_UNAVAIL;
		fprintf(out, "]");
		if (rv != BCM_E_NONE)
			fprintf(out, ",\"partial\":true");
		fprintf(out, "}\n");
		return;
	}
	fprintf(out,
		"{\"ok\":false,\"error\":\"unsupported operation\","
		"\"unsupported\":true}\n");
}

static void *serve(void *arg)
{
	int fd = *(int *)arg;

	free(arg);
	for (;;) {
		int c = accept(fd, NULL, NULL);
		FILE *f;
		char line[1024];

		if (c < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if ((f = fdopen(c, "r+")) == NULL) {
			close(c);
			continue;
		}
		/*
		 * MANY requests per connection, until the client hangs up.
		 *
		 * The protocol was described as one exchange per connection, and
		 * the CLI does not work that way: it dials once and issues every
		 * call down the same socket -- `show ports` asks for the port list
		 * and then a status per port. A server that answers once and
		 * closes gets the first call right and breaks the second with
		 * "write: broken pipe", which reads as a network fault rather than
		 * as a protocol disagreement.
		 */
		while (fgets(line, sizeof(line), f) != NULL) {
			handle(f, line);
			fflush(f);
		}
		fclose(f);
	}
	close(fd);
	return NULL;
}

int nosaic_query_start(int unit, const char *path)
{
	struct sockaddr_un a;
	pthread_t th;
	int fd, *arg;

	query_unit = unit;

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
		fprintf(stderr, "query: socket: %s\n", strerror(errno));
		return -1;
	}
	/* A socket left behind by a daemon that did not shut down cleanly would
	 * make bind fail for ever, and the switch would run with no way to ask it
	 * anything. Nothing else owns this path. */
	unlink(path);

	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
		fprintf(stderr, "query: bind %s: %s\n", path, strerror(errno));
		close(fd);
		return -1;
	}
	/* Root only: this reports the chip's state and nothing else on the box
	 * needs it. */
	chmod(path, 0600);
	if (listen(fd, 4) != 0) {
		fprintf(stderr, "query: listen: %s\n", strerror(errno));
		close(fd);
		return -1;
	}

	arg = malloc(sizeof(int));
	if (arg == NULL) {
		close(fd);
		return -1;
	}
	*arg = fd;
	if (pthread_create(&th, NULL, serve, arg) != 0) {
		fprintf(stderr, "query: no thread; nothing can ask this daemon "
			"what the chip holds\n");
		free(arg);
		close(fd);
		return -1;
	}
	pthread_detach(th);
	printf("query        serving %s\n", path);
	fflush(stdout);
	return 0;
}
