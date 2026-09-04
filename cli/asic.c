/* SPDX-License-Identifier: Apache-2.0 */
/*
 * What Linux believes, what the chip actually holds, and where they differ.
 *
 * These are two different machines that happen to share a box. Linux has an
 * interface with an address, an MTU and a routing table; the chip has a port
 * with a VLAN, a spanning-tree state and a forwarding table. Nothing keeps them
 * in step except the daemon between them, and when that is wrong the switch
 * looks perfect from either side on its own:
 *
 *   ip addr        the interface is up with the right address
 *   ip route       the route is there
 *   the panel      the port is lit
 *   the chip       has none of it, and says nothing
 *
 * A day was spent on exactly that. The 40G ports had link, correct addresses,
 * correct VLANs, no errors and no discards, and formed no adjacency, because
 * frames the chip received were never punted to the CPU. Every tool on the
 * switch agreed everything was fine.
 *
 * So this asks both and prints them side by side. The verdict column is the
 * point: it says which of the two is wrong, in the vocabulary of whichever one
 * it is.
 */
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "asic.h"

/* ------------------------------------------------------- what Linux believes */

struct linux_state {
	int  present;
	int  up;          /* IFF_UP: what the operator asked for */
	int  running;     /* IFF_RUNNING: what the kernel sees */
	int  mtu;
	char addr[64];    /* "10.0.0.1/29", or empty */
};

static void linux_state(const char *name, struct linux_state *ls)
{
	struct ifreq r;
	int s;

	memset(ls, 0, sizeof(*ls));
	if ((s = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
		return;

	memset(&r, 0, sizeof(r));
	snprintf(r.ifr_name, sizeof(r.ifr_name), "%s", name);
	if (ioctl(s, SIOCGIFFLAGS, &r) != 0) {
		close(s);
		return;
	}
	ls->present = 1;
	ls->up = (r.ifr_flags & IFF_UP) != 0;
	ls->running = (r.ifr_flags & IFF_RUNNING) != 0;

	memset(&r, 0, sizeof(r));
	snprintf(r.ifr_name, sizeof(r.ifr_name), "%s", name);
	if (ioctl(s, SIOCGIFMTU, &r) == 0)
		ls->mtu = r.ifr_mtu;

	memset(&r, 0, sizeof(r));
	snprintf(r.ifr_name, sizeof(r.ifr_name), "%s", name);
	if (ioctl(s, SIOCGIFADDR, &r) == 0) {
		struct sockaddr_in *in = (struct sockaddr_in *)&r.ifr_addr;
		char ip[INET_ADDRSTRLEN];
		int bits = 0;

		inet_ntop(AF_INET, &in->sin_addr, ip, sizeof(ip));
		memset(&r, 0, sizeof(r));
		snprintf(r.ifr_name, sizeof(r.ifr_name), "%s", name);
		if (ioctl(s, SIOCGIFNETMASK, &r) == 0) {
			struct sockaddr_in *m = (struct sockaddr_in *)&r.ifr_addr;
			unsigned long v = ntohl(m->sin_addr.s_addr);

			while (v & 0x80000000UL) { bits++; v <<= 1; }
		}
		snprintf(ls->addr, sizeof(ls->addr), "%s/%d", ip, bits);
	}
	close(s);
}

/* ---------------------------------------------------- what the chip answers */

/* One flat JSON record's integer field. Enough for a response whose shape we
 * define, and far less code than a parser. */
static int jint(const char *rec, const char *key, int missing)
{
	char pat[64];
	const char *p;

	snprintf(pat, sizeof(pat), "\"%s\":", key);
	if ((p = strstr(rec, pat)) == NULL)
		return missing;
	return atoi(p + strlen(pat));
}

static void jstr(const char *rec, const char *key, char *out, size_t len)
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

static char *ask(const char *path, const char *op)
{
	struct sockaddr_un a;
	char *buf;
	size_t cap = 65536, n = 0;
	int fd;
	ssize_t r;

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return NULL;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
		close(fd);
		return NULL;
	}
	dprintf(fd, "{\"op\":\"%s\"}\n", op);

	if ((buf = malloc(cap)) == NULL) {
		close(fd);
		return NULL;
	}
	while ((r = read(fd, buf + n, cap - n - 1)) > 0) {
		n += (size_t)r;
		if (n + 1 >= cap)
			break;
	}
	buf[n] = '\0';
	close(fd);
	return buf;
}

/* --------------------------------------------------------------- the report */

static const char *stp_name(int v)
{
	switch (v) {
	case 0: return "disable";
	case 1: return "block";
	case 2: return "listen";
	case 3: return "learn";
	case 4: return "fwd";
	}
	return "?";
}

int nosaic_asic_ports(void)
{
	char *resp = ask(NOSAIC_QUERY_SOCKET, "asic.ports");
	const char *p;
	int n = 0, bad = 0;

	if (resp == NULL) {
		fprintf(stderr,
			"nosaic: cannot reach the datapath on %s.\n"
			"The daemon serves it once it is bridging ports; if nosd is "
			"running and this is missing, it did not get that far.\n",
			NOSAIC_QUERY_SOCKET);
		return 1;
	}
	if (strstr(resp, "\"ok\":true") == NULL) {
		fprintf(stderr, "nosaic: the datapath refused: %s", resp);
		free(resp);
		return 1;
	}

	/*
	 * Start at the array, not at the response's own opening brace.
	 *
	 * Iterating from the first '{' in the whole document walks the envelope
	 * as if it were a record, and every field lookup then finds the FIRST
	 * port's values -- so port one is reported twice and the count is one
	 * too high. The bug is invisible unless you count the rows.
	 */
	p = strstr(resp, "\"result\":[");
	p = p != NULL ? strchr(p, '[') : NULL;

	printf("%-7s %-30s %-42s %s\n", "port", "linux", "asic", "");
	for (p = p != NULL ? strchr(p, '{') : NULL; p != NULL; p = strchr(p + 1, '{')) {
		char name[32], mac[32], lin[80], asic[96], verdict[96];
		struct linux_state ls;
		int link, ena, pvid, want, stp, cpu, fmax;

		jstr(p, "name", name, sizeof(name));
		if (name[0] == '\0')
			continue;
		jstr(p, "mac", mac, sizeof(mac));
		link = jint(p, "link", -1);
		ena  = jint(p, "enabled", -1);
		pvid = jint(p, "pvid", 0);
		want = jint(p, "want_vlan", 0);
		stp  = jint(p, "stp", -1);
		cpu  = jint(p, "cpu_member", -1);
		fmax = jint(p, "frame_max", -1);

		linux_state(name, &ls);
		n++;

		snprintf(lin, sizeof(lin), "%-4s mtu%-5d %-18s",
			 !ls.present ? "ABS" : ls.up ? "up" : "down",
			 ls.mtu, ls.addr[0] ? ls.addr : "-");
		snprintf(asic, sizeof(asic), "link=%-4s vlan=%-5d stp=%-7s cpu=%-3s",
			 link == 1 ? "up" : link == 0 ? "DOWN" : "?",
			 pvid, stp_name(stp),
			 cpu == 1 ? "yes" : cpu == 0 ? "NO" : "?");

		/*
		 * One verdict, most serious first. Reporting every difference
		 * would bury the one that matters -- a port with no punt path is
		 * broken whatever its MTU says.
		 */
		verdict[0] = '\0';
		if (!ls.present)
			snprintf(verdict, sizeof(verdict), "NO LINUX INTERFACE");
		else if (cpu == 0)
			snprintf(verdict, sizeof(verdict),
				 "CPU NOT IN VLAN %d - nothing can reach Linux", pvid);
		else if (want != 0 && pvid != want)
			snprintf(verdict, sizeof(verdict),
				 "VLAN MISMATCH - chip has %d, daemon asked for %d",
				 pvid, want);
		else if (ena == 0)
			snprintf(verdict, sizeof(verdict), "PORT DISABLED IN THE CHIP");
		else if (stp >= 0 && stp != 4)
			snprintf(verdict, sizeof(verdict),
				 "NOT FORWARDING - stp is %s in vlan %d", stp_name(stp), pvid);
		else if (link == 0 && ls.up)
			snprintf(verdict, sizeof(verdict), "link down - cable or far end");
		else if (fmax > 0 && ls.mtu > 0 && fmax < ls.mtu)
			snprintf(verdict, sizeof(verdict),
				 "MTU %d exceeds what the chip will carry (%d)", ls.mtu, fmax);

		if (verdict[0] != '\0' && strncmp(verdict, "link down", 9) != 0)
			bad++;
		printf("%-7s %-30s %-42s %s\n", name, lin, asic, verdict);
	}
	free(resp);

	printf("\n%d port(s). \"linux\" is what the kernel has; \"asic\" is read back "
	       "from the chip.\n", n);
	if (bad > 0)
		printf("%d disagree in a way that stops traffic.\n", bad);
	return bad > 0 ? 1 : 0;
}

/* ------------------------------------------------------------------ routes */

#define MAX_ROUTES 512

struct rt {
	char prefix[24];
	char via[32];     /* the Linux interface, or empty */
	int  in_linux;
	int  in_asic;
	int  ecmp;
	/*
	 * A route with no gateway is directly attached, and the chip covers those
	 * with host entries rather than with a forwarding-table entry. Reporting
	 * them as missing would be true and wrong: it buries the routes that are
	 * missing and should not be, which is the only reason to run this.
	 */
	int  connected;
};

static struct rt routes[MAX_ROUTES];
static int nroutes;

static struct rt *rt_find(const char *prefix)
{
	int i;

	for (i = 0; i < nroutes; i++)
		if (strcmp(routes[i].prefix, prefix) == 0)
			return &routes[i];
	if (nroutes >= MAX_ROUTES)
		return NULL;
	memset(&routes[nroutes], 0, sizeof(routes[nroutes]));
	snprintf(routes[nroutes].prefix, sizeof(routes[nroutes].prefix), "%s", prefix);
	return &routes[nroutes++];
}

/*
 * /proc/net/route stores addresses as the in-memory 32-bit word printed as
 * hex, so what the digits mean depends on the host's byte order. Reading them
 * the same way everywhere gives correct routes on x86 and reversed ones on a
 * PowerPC switch -- which is a bug this tree has already had once, in the
 * datapath's own route mirror.
 */
static unsigned proc_hex_to_host(const char *h)
{
	unsigned v = (unsigned)strtoul(h, NULL, 16);

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	return v;
#else
	return ((v & 0xff) << 24) | ((v & 0xff00) << 8) |
	       ((v >> 8) & 0xff00) | ((v >> 24) & 0xff);
#endif
}

static void read_linux_routes(void)
{
	char line[512];
	FILE *f = fopen("/proc/net/route", "r");

	if (f == NULL)
		return;
	if (fgets(line, sizeof(line), f) == NULL) {   /* header */
		fclose(f);
		return;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		char iface[32], dest[16], mask[16], prefix[24];
		unsigned d, m;
		int bits = 0;
		struct rt *r;

		char gw[16];

		if (sscanf(line, "%31s %15s %15s %*s %*s %*s %*s %15s",
			   iface, dest, gw, mask) != 4)
			continue;
		d = proc_hex_to_host(dest);
		m = proc_hex_to_host(mask);
		while (m & 0x80000000u) { bits++; m <<= 1; }
		snprintf(prefix, sizeof(prefix), "%u.%u.%u.%u/%d",
			 (d >> 24) & 0xff, (d >> 16) & 0xff, (d >> 8) & 0xff, d & 0xff, bits);

		if ((r = rt_find(prefix)) == NULL)
			continue;
		r->in_linux = 1;
		if (proc_hex_to_host(gw) == 0)
			r->connected = 1;
		if (r->via[0] == '\0')
			snprintf(r->via, sizeof(r->via), "%s", iface);
	}
	fclose(f);
}

int nosaic_asic_routes(void)
{
	char *resp = ask(NOSAIC_QUERY_SOCKET, "l3.routes");
	const char *p;
	int i, missing = 0, only_asic = 0, connected = 0, partial;

	if (resp == NULL) {
		fprintf(stderr, "nosaic: cannot reach the datapath on %s\n",
			NOSAIC_QUERY_SOCKET);
		return 1;
	}
	if (strstr(resp, "\"ok\":true") == NULL) {
		fprintf(stderr, "nosaic: the datapath refused: %s", resp);
		free(resp);
		return 1;
	}
	partial = strstr(resp, "\"partial\":true") != NULL;

	read_linux_routes();

	p = strstr(resp, "\"result\":[");
	p = p != NULL ? strchr(p, '[') : NULL;
	for (p = p != NULL ? strchr(p, '{') : NULL; p != NULL; p = strchr(p + 1, '{')) {
		char prefix[24];
		struct rt *r;

		jstr(p, "prefix", prefix, sizeof(prefix));
		if (prefix[0] == '\0')
			continue;
		if ((r = rt_find(prefix)) == NULL)
			continue;
		r->in_asic = 1;
		r->ecmp = jint(p, "ecmp", 0);
	}
	free(resp);

	printf("%-22s %-18s %-14s %s\n", "prefix", "linux", "asic", "");
	for (i = 0; i < nroutes; i++) {
		struct rt *r = &routes[i];
		const char *verdict = "";

		/*
		 * Only the first case stops traffic. A route the chip has and
		 * Linux does not is usually the kernel having moved on and the
		 * mirror not having caught up yet, which is worth showing and is
		 * not a fault by itself.
		 */
		if (r->in_linux && !r->in_asic && r->connected) {
			verdict = "directly attached - covered by host entries";
			connected++;
		} else if (r->in_linux && !r->in_asic) {
			verdict = "NOT IN HARDWARE - forwarded by the CPU, if at all";
			missing++;
		} else if (!r->in_linux && r->in_asic) {
			verdict = "in the chip only - stale, or not yet withdrawn";
			only_asic++;
		}
		printf("%-22s %-18s %-14s %s\n", r->prefix,
		       r->in_linux ? r->via : "-",
		       r->in_asic ? (r->ecmp ? "present ecmp" : "present")
				  : (r->connected ? "-" : "MISSING"),
		       verdict);
	}

	printf("\n%d prefix(es). \"linux\" is the kernel's routing table; "
	       "\"asic\" is read back from the chip's own forwarding table.\n",
	       nroutes);
	if (missing > 0)
		printf("%d route(s) the kernel has and the chip does not, and should.\n",
		       missing);
	if (connected > 0)
		printf("%d directly attached prefix(es), which the chip covers with "
		       "host entries rather than routes.\n", connected);
	if (only_asic > 0)
		printf("%d route(s) the chip has and the kernel does not.\n", only_asic);
	if (partial)
		printf("The chip's table was only partly readable, so absences here "
		       "are not conclusive.\n");
	printf("IPv6 and per-nexthop detail are not compared: /proc/net/route "
	       "carries neither.\n");
	return missing > 0 ? 1 : 0;
}
