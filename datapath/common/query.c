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
#include <bcm/switch.h>
#include <bcm/types.h>

#include "dmapool.h"
#include "tapbridge.h"
#include "acl.h"
#include "query.h"

static int query_unit;

/* The BDE's pool, if the datapath handed it over. NULL is a legitimate state:
 * see nosaic_query_set_dmapool. */
static struct nosaic_dmapool *query_pool;

void nosaic_query_set_dmapool(struct nosaic_dmapool *p)
{
	query_pool = p;
}

/* The datapath's PHY register dumper, if it has one. See query.h. */
static void (*query_phydump)(FILE *out);
static void (*query_phyread)(FILE *out, int port, int devad, int reg, int count);
static void (*query_phywrite)(FILE *out, int port, int devad, int reg, int val);

void nosaic_query_set_phydump(void (*fn)(FILE *out))
{
	query_phydump = fn;
}

void nosaic_query_set_phyread(void (*fn)(FILE *out, int port, int devad,
					 int reg, int count))
{
	query_phyread = fn;
}

void nosaic_query_set_phywrite(void (*fn)(FILE *out, int port, int devad,
					  int reg, int val))
{
	query_phywrite = fn;
}

/*
 * One integer field out of a request, without a JSON parser.
 *
 * The same trade as the substring matching above and for the same reason:
 * the keys are ours, the values are integers, and a request that does not
 * contain the key gets the default rather than an error. Accepts 0x-prefixed
 * values, because register numbers are read and written in hex everywhere
 * else and a diagnostic that demanded decimal would be used wrongly.
 */
static int req_int(const char *req, const char *key, int dflt)
{
	char pat[32];
	const char *p;

	snprintf(pat, sizeof(pat), "\"%s\":", key);
	p = strstr(req, pat);
	if (p == NULL)
		return dflt;
	p += strlen(pat);
	while (*p == ' ')
		p++;
	if (*p == '\0')
		return dflt;
	return (int)strtol(p, NULL, 0);
}

/* JSON string escaping for allocation names.
 *
 * The names come from the SDK, not from us, so they are not ours to assume
 * anything about. One containing a quote would produce a response the CLI
 * cannot parse, and the fault would look like the socket rather than the
 * name. */
static void json_str(FILE *out, const char *s)
{
	fputc('"', out);
	for (; *s != '\0'; s++) {
		if (*s == '"' || *s == '\\')
			fprintf(out, "\\%c", *s);
		else if ((unsigned char)*s < 0x20)
			fprintf(out, "\\u%04x", (unsigned char)*s);
		else
			fputc(*s, out);
	}
	fputc('"', out);
}

/*
 * A request's string arguments. The protocol is flat enough that finding a
 * key and reading what follows it is a parser: the CLI writes the JSON and
 * nothing here needs more than a number or a string out of it. The number
 * half is req_int above.
 */
static void req_str(const char *req, const char *key, char *out, size_t len)
{
	char pat[40];
	const char *p;
	size_t n = 0;

	out[0] = '\0';
	snprintf(pat, sizeof(pat), "\"%s\":\"", key);
	if ((p = strstr(req, pat)) == NULL)
		return;
	for (p += strlen(pat); *p != '\0' && *p != '"' && n + 1 < len; p++) {
		if (*p == '\\' && p[1] != '\0')
			p++;
		out[n++] = *p;
	}
	out[n] = '\0';
}

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
	/*
	 * The switch die's own temperature.
	 *
	 * ⚠ THIS IS THE HOTTEST THING IN THE BOX AND NOTHING ELSE CAN SEE IT.
	 *
	 * Board sensors are i2c parts near the ASIC, not on it -- on the Nexus
	 * 3172TQ the vendor records the die at 56 C while the three board diodes
	 * idle between 31 and 38, and the die trips at 100. The only way to the
	 * die is the SDK over PCIe, which means this daemon, which is why a
	 * reading that belongs to the platform HAL is served from here.
	 *
	 * The SDK reports 0.1 C units; millidegrees are what the HAL and hwmon
	 * both speak, so the conversion happens once, here, rather than in each
	 * consumer.
	 *
	 * A chip with no monitors answers with an empty list rather than an
	 * error: "this chip cannot tell you" and "the query failed" are
	 * different, and a cooling loop must be able to distinguish them.
	 */
	if (strstr(req, "\"asic.temp\"") != NULL) {
		bcm_switch_temperature_monitor_t mon[8];
		int n = 0, k, rv;

		rv = bcm_switch_temperature_monitor_get(query_unit,
			(int)(sizeof(mon) / sizeof(mon[0])), mon, &n);
		if (rv != BCM_E_NONE) {
			fprintf(out, "{\"ok\":false,\"error\":\"temperature monitors: %s\"}\n",
				bcm_errmsg(rv));
			return;
		}
		if (n < 0)
			n = 0;
		if (n > (int)(sizeof(mon) / sizeof(mon[0])))
			n = (int)(sizeof(mon) / sizeof(mon[0]));
		fprintf(out, "{\"ok\":true,\"result\":[");
		for (k = 0; k < n; k++)
			fprintf(out, "%s{\"Index\":%d,\"MilliC\":%d,\"PeakMilliC\":%d}",
				k ? "," : "", k, mon[k].curr * 100, mon[k].peak * 100);
		fprintf(out, "]}\n");
		return;
	}

	/*
	 * The external PHYs' own view of the line.
	 *
	 * Served raw -- register numbers and the words read out of them --
	 * because the whole point is to see what the part says rather than
	 * what a driver concluded from it. The decoding belongs in whoever is
	 * reading, who knows which part this is.
	 */
	if (strstr(req, "\"phy.dump\"") != NULL) {
		if (query_phydump == NULL) {
			fprintf(out, "{\"ok\":false,\"error\":"
				"\"this datapath has no external PHY driver bound\"}\n");
			return;
		}
		fprintf(out, "{\"ok\":true,\"result\":[");
		query_phydump(out);
		fprintf(out, "]}\n");
		return;
	}

	/*
	 * One PHY register, or a run of them, by address.
	 *
	 * Separate from phy.dump because the dump answers "what is wrong" and
	 * this answers "what is actually there" -- which registers a part
	 * implements, what it calls itself, whether its firmware is running.
	 * Those questions are not known in advance, and a diagnostic that
	 * needs a rebuild to ask a new one is a diagnostic that does not get
	 * asked.
	 */
	if (strstr(req, "\"phy.read\"") != NULL) {
		int port  = req_int(req, "port", 0);
		int devad = req_int(req, "devad", 1);
		int reg   = req_int(req, "reg", 0);
		int count = req_int(req, "count", 1);

		if (query_phyread == NULL) {
			fprintf(out, "{\"ok\":false,\"error\":"
				"\"this datapath has no external PHY driver bound\"}\n");
			return;
		}
		if (port <= 0 || count <= 0 || count > 256) {
			fprintf(out, "{\"ok\":false,\"error\":"
				"\"need a port, and a count between 1 and 256\"}\n");
			return;
		}
		fprintf(out, "{\"ok\":true,\"result\":[");
		query_phyread(out, port, devad, reg, count);
		fprintf(out, "]}\n");
		return;
	}

	/*
	 * Put a port into, or take it out of, one of the chip's loopbacks.
	 *
	 * ⚠ THIS IS A BRING-UP TOOL AND IT BREAKS TRAFFIC ON THE PORT.
	 *
	 * It exists because "the far end links and we never receive" is not a
	 * question the switch API can answer: link is one bit and it is down,
	 * and everything upstream of the failure looks correct. A loopback
	 * splits the path. PHY_REMOTE loops what the PHY receives on the line
	 * straight back out of it, so the far end sees its own transmission
	 * returned -- if the far end holds link, light IS reaching our PMD and
	 * being recovered, and the fault is downstream toward the ASIC. If it
	 * drops, we really are receiving nothing.
	 *
	 * Modes are the SDK's: 0 none, 1 MAC, 2 PHY, 3 PHY remote, 4 MAC
	 * remote, 5 EDB. Passed through rather than named, because which ones
	 * a given PHY implements is the PHY's business and a name we invented
	 * for one that is refused would only obscure the refusal.
	 */
	if (strstr(req, "\"port.loopback\"") != NULL) {
		int port = req_int(req, "port", 0);
		int mode = req_int(req, "mode", -1);
		int have = -1, rv;

		if (port <= 0) {
			fprintf(out, "{\"ok\":false,\"error\":\"need a port\"}\n");
			return;
		}
		if (mode >= 0) {
			rv = bcm_port_loopback_set(query_unit, port, mode);
			if (rv != BCM_E_NONE) {
				fprintf(out, "{\"ok\":false,\"error\":"
					"\"loopback %d on port %d: %s\"}\n",
					mode, port, bcm_errmsg(rv));
				return;
			}
		}
		/* Read back rather than echo: a mode the PHY quietly declined is
		 * the thing most worth seeing here. */
		if (bcm_port_loopback_get(query_unit, port, &have) != BCM_E_NONE)
			have = -1;
		fprintf(out, "{\"ok\":true,\"result\":{\"Port\":%d,\"Mode\":%d}}\n",
			port, have);
		return;
	}

	/*
	 * Write one PHY register, by address.
	 *
	 * ⚠ A BRING-UP TOOL. IT CAN TAKE A WORKING PORT DOWN.
	 *
	 * The counterpart of phy.read, and it earns its risk on exactly the
	 * questions read cannot answer. The one it was added for: a far end
	 * reporting link tells you its RECEIVER locked, and says nothing
	 * about its transmitter -- so "the far end links, therefore our
	 * transmit is good and only our receive is broken" is an inference,
	 * not a measurement. Disabling our own PMD transmitter (1.9 bit 0)
	 * and watching whether the far end drops turns it into one.
	 *
	 * Writes nothing on its own initiative and reads the register back
	 * afterwards, because a register that ignored the write is the
	 * interesting case.
	 */
	if (strstr(req, "\"phy.write\"") != NULL) {
		int port  = req_int(req, "port", 0);
		int devad = req_int(req, "devad", 1);
		int reg   = req_int(req, "reg", -1);
		int val   = req_int(req, "value", -1);

		if (query_phywrite == NULL) {
			fprintf(out, "{\"ok\":false,\"error\":"
				"\"this datapath has no external PHY driver bound\"}\n");
			return;
		}
		if (port <= 0 || reg < 0 || reg > 0xffff ||
		    val < 0 || val > 0xffff) {
			fprintf(out, "{\"ok\":false,\"error\":"
				"\"need a port, a register and a 16-bit value\"}\n");
			return;
		}
		fprintf(out, "{\"ok\":true,\"result\":[");
		query_phywrite(out, port, devad, reg, val);
		fprintf(out, "]}\n");
		return;
	}

	if (strstr(req, "\"asic.ports\"") != NULL) {
		fprintf(out, "{\"ok\":true,\"result\":[");
		for (i = 0; i < nosaic_tap_count(); i++)
			port_json(out, i);
		fprintf(out, "]}\n");
		return;
	}
	/*
	 * What the DMA pool holds, and who is holding it.
	 *
	 * `used` and `largest` together say whether a pool that cannot satisfy
	 * an allocation is full or merely fragmented, which are different
	 * faults. The per-name table says which caller to go and look at: a
	 * name whose outstanding total climbs with uptime is a leak, and that
	 * is exactly the shape this daemon shipped with for two boards.
	 */
	if (strstr(req, "\"asic.dma\"") != NULL) {
		struct nosaic_dma_stat st[NOSAIC_DMA_NAMES];
		int n, k;

		if (query_pool == NULL) {
			fprintf(out, "{\"ok\":false,\"error\":\"this datapath has no DMA pool registered\"}\n");
			return;
		}
		n = nosaic_dmapool_stats(query_pool, st, NOSAIC_DMA_NAMES);
		fprintf(out,
			"{\"ok\":true,\"result\":{\"Bytes\":%zu,\"Used\":%zu,"
			"\"Largest\":%zu,\"Peak\":%zu,\"Fails\":%llu,\"Callers\":[",
			query_pool->len, nosaic_dmapool_used(query_pool),
			nosaic_dmapool_largest(query_pool), query_pool->peak,
			(unsigned long long)query_pool->fails);
		for (k = 0; k < n; k++) {
			fprintf(out, "%s{\"Name\":", k ? "," : "");
			json_str(out, st[k].name);
			fprintf(out,
				",\"Outstanding\":%zu,\"Peak\":%zu,"
				"\"Allocs\":%llu,\"Frees\":%llu,\"Fails\":%llu}",
				st[k].outstanding, st[k].peak,
				(unsigned long long)st[k].allocs,
				(unsigned long long)st[k].frees,
				(unsigned long long)st[k].fails);
		}
		fprintf(out, "]}}\n");
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
		struct nosaic_acl_caps acl;
		int maxv4 = 0, maxecmp = 0;

		bcm_l3_info_t_init(&info);
		if (bcm_l3_info(query_unit, &info) == BCM_E_NONE) {
			maxv4 = info.l3info_max_route;
			maxecmp = info.l3info_max_ecmp;
		}
		nosaic_acl_capability(&acl);

		/*
		 * ⚠ ECMP WAS NEVER REPORTED, AND IT HAD BEEN WORKING ALL ALONG.
		 *
		 * This response simply omitted the field, so the Go side unmarshalled
		 * the zero value and `show caps` answered "ecmp no" -- on a switch
		 * whose log says `l3: ecmp group of 2 -> egress 200000`, with a real
		 * bcm_l3_egress_ecmp group in the chip carrying both members of an
		 * equal-cost pair. An operator reading that capability would have
		 * concluded the board could not do multipath and designed around a
		 * limitation it does not have.
		 *
		 * The width comes from the chip rather than from a constant here: it
		 * is what the silicon reports it can do, which is the same rule the
		 * port speeds follow.
		 *
		 * ⚠ AND VLANS WERE REPORTED THAT WERE NEVER THERE. This said
		 * "VLANs":true and "L2Learning":true while every vlan.* and l2.fdb
		 * request below falls through to "unsupported" -- the capability
		 * model's one rule broken the other way round from ECMP. They are
		 * false until this server implements them, and the contract stays
		 * 1.1 here because 1.2's VLAN listing and SVIs are not implemented
		 * either.
		 */
		fprintf(out,
			"{\"ok\":true,\"result\":{\"Contract\":\"1.1\","
			"\"Driver\":\"%s\",\"MaxPorts\":%d,\"VLANs\":false,"
			"\"MaxVLANs\":0,\"L2Learning\":false,\"L3\":true,"
			"\"MaxV4\":%d,\"ECMP\":%s,\"MaxECMP\":%d,"
			"\"ACL\":%s,\"ACLEntries\":%d,"
			"\"ACL6\":%s,\"ACL6Entries\":%d}}\n",
			NOSAIC_QUERY_DRIVER, nosaic_tap_count(), maxv4,
			maxecmp > 1 ? "true" : "false", maxecmp,
			acl.v4 ? "true" : "false", acl.v4_total,
			acl.v6 ? "true" : "false", acl.v6_total);
		return;
	}

	/* The rules and what each has matched, straight from the chip's
	 * counters. Read-only like everything else here: rules are set through
	 * configuration, so that what the chip holds and what the switch was
	 * told to hold cannot be two different things. */
	if (strstr(req, "\"acl.set\"") != NULL || strstr(req, "\"acl.del\"") != NULL) {
		char rule[256], err[128];
		int seq = req_int(req, "seq", 0), rv;

		if (strstr(req, "\"acl.set\"") != NULL) {
			req_str(req, "rule", rule, sizeof(rule));
			rv = nosaic_acl_set(seq, rule, err, sizeof(err));
		} else {
			rv = nosaic_acl_del(seq, err, sizeof(err));
		}
		if (rv == 0) {
			fprintf(out, "{\"ok\":true}\n");
		} else {
			fprintf(out, "{\"ok\":false,\"error\":");
			json_str(out, err);
			fprintf(out, "%s}\n", rv == -2 ? ",\"unsupported\":true" : "");
		}
		return;
	}

	if (strstr(req, "\"acl\"") != NULL) {
		nosaic_acl_query(out);
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
