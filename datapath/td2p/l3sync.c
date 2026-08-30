/*
 * Hardware L3: mirror the Linux routing table into the chip.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Without this the tap bridge already routes -- every forwarded packet goes up
 * to the CPU, through the kernel, and back down. It works, and it is about
 * three orders of magnitude slower than the silicon it is running on. This is
 * what makes the chip do it.
 *
 * The routing table stays the kernel's. Nothing here decides anything: zebra
 * owns the FIB and this copies what is already there, which is what keeps one
 * routing daemon working the same way on every board.
 *
 * What the chip needs:
 *
 *   bcm_l3_intf_create    a router interface holding our MAC for the VLAN --
 *                         the source MAC the chip puts on routed frames.
 *   bcm_l2_station_add    MY_STATION. Without it a frame sent to our MAC is
 *                         treated as an L2 frame to be switched, and routing
 *                         is never considered.
 *   bcm_l3_egress_create  a next hop: destination MAC, port and VLAN.
 *   bcm_l3_route_add      the LPM entry in DEFIP, pointing at that next hop.
 *   bcm_l3_host_add       directly attached neighbours, which have no gateway
 *                         and so never appear as a route.
 *   bcm_field_*           the punt rules that keep the control plane alive
 *                         once MY_STATION exists.
 *
 * The design and every caveat here are EdgeNOS's, proven on this board; the
 * code is NOSaic's own.
 *
 * MY_STATION BREAKS THE CONTROL PLANE UNLESS YOU PUT IT BACK. This is the part
 * that is not obvious and cost EdgeNOS a week. Telling the chip to route
 * frames addressed to our MAC also hands it packets aimed AT US, which the
 * routing engine has no business terminating -- so it drops them. The symptom
 * is an OSPF adjacency that reaches ExStart and stops, because the unicast
 * Database Description packets are being routed instead of delivered. Three
 * things put it back, and all three are needed:
 *
 *   - a field-processor rule on IP protocol 89, copying all OSPF to the CPU;
 *   - a rule per interface matching our own address;
 *   - a host entry for our own address with BCM_L3_L2TOCPU, meaning "deliver
 *     to the CPU, unrouted".
 *
 * MULTICAST NEEDS ITS OWN STATION ENTRY. MY_STATION also stops the bridge's
 * path for multicast, so hellos to 224.0.0.5 stop arriving and the adjacency
 * stalls in the same place for a different reason. A second station entry on
 * 01:00:5e:00:00:00/24 restores it -- the vendor OS carries exactly this pair.
 *
 * A ROUTE THAT MOVES MUST BE REPLACED, NOT RE-ADDED. Keying the cache on
 * (prefix, mask, next hop) makes a prefix that changed next hop look new; the
 * add then collides with the existing DEFIP entry, fails, and the chip goes on
 * forwarding to the old next hop. A hardware table that lags the routing table
 * is worse than none at all: traffic is blackholed while everything reports
 * healthy. So the lookup is on (prefix, mask) alone and a changed next hop is
 * a BCM_L3_REPLACE.
 *
 * COUNTERS ARE OFF BY DEFAULT AND THAT IS DELIBERATE. Attaching a statistic to
 * a field-processor rule starts the SDK collecting it, and each collection
 * takes a DMA buffer from a pool that is a bump allocator with no free. It
 * looks healthy for hours and then the pool is gone -- after which the bridge
 * cannot get a buffer to transmit with, and the adjacencies die while every
 * process stays up. The rules punt without the counters. l3_fp_stats=1 turns
 * them on for as long as you are watching.
 *
 * IPv4 ONLY, for now. The v6 equivalent needs the NDP cache over netlink,
 * because there is no /proc file for it, and a second field-processor group,
 * because DstIp and DstIp6 are different widths and one group cannot carry
 * both keys. Stated rather than silently missing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>

#include <sal/types.h>
#include <bcm/types.h>
#include <bcm/error.h>
#include <bcm/l2.h>
#include <bcm/l3.h>
#include <bcm/field.h>
#include <bcm/switch.h>
#include <bcm/port.h>

#include "l3sync.h"
#include "props.h"

#define MAX_IF   8
#define MAX_NH   64
#define MAX_RT   1024
#define MAX_HOST 256

struct l3if {
	char       ifname[IFNAMSIZ];
	int        port;
	bcm_vlan_t vlan;
	bcm_mac_t  mac;
	bcm_if_t   intf;
	int        station;
	bcm_if_t   cpu_eg;      /* egress object aimed at the CPU port */
	int        self_done;   /* the "this is me" host entry is installed */
	bcm_field_entry_t fp_self;
	int        fp_stat_self;
};

static struct l3if ifs[MAX_IF];
static int nif;
static int l3_unit;
static int l3_on;
static int fp_stats;        /* see the note above: off unless asked for */

static struct { int ifx; uint32_t gw; bcm_if_t eg; } nh[MAX_NH];
static int nnh;
static struct { uint32_t dst, mask; bcm_if_t eg; int seen; } rt[MAX_RT];
static int nrt;
static struct { int ifx; uint32_t ip; } hs[MAX_HOST];
static int nhs;

static unsigned long st_added, st_moved, st_gone, st_failed, st_unresolved, st_host;

static bcm_field_group_t fp_grp = -1;
static bcm_field_entry_t fp_ospf = -1;
static int fp_stat_ospf = -1;

static struct l3if *if_by_name(const char *n)
{
	int i;

	for (i = 0; i < nif; i++) {
		if (strcmp(ifs[i].ifname, n) == 0)
			return &ifs[i];
	}
	return NULL;
}

/*
 * /proc/net/route prints the in-kernel big-endian address read back as a host
 * u32, so on a little-endian box the bytes come out reversed: 10.101.101.0
 * appears as 0065650A.
 */
static uint32_t be_hex_to_host(const char *h)
{
	uint32_t v = (uint32_t)strtoul(h, NULL, 16);

	return ((v & 0xff) << 24) | ((v & 0xff00) << 8) |
	       ((v >> 8) & 0xff00) | ((v >> 24) & 0xff);
}

/* Our own address on an interface, for the "this is me" entries. */
static int iface_ipv4(const char *ifname, uint32_t *ip)
{
	struct ifreq ifr;
	int fd = socket(AF_INET, SOCK_DGRAM, 0), rv;

	if (fd < 0)
		return -1;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
	ifr.ifr_addr.sa_family = AF_INET;
	rv = ioctl(fd, SIOCGIFADDR, &ifr);
	if (rv == 0) {
		struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;

		*ip = ntohl(sin->sin_addr.s_addr);
	}
	close(fd);
	return rv;
}

/* The kernel exports the ARP cache as text; resolution is its job, not ours. */
static int arp_lookup(const char *dev, uint32_t gw, uint8_t *mac)
{
	char line[256], ip[64], hw[64], d[64];
	unsigned int m[6];
	FILE *f = fopen("/proc/net/arp", "r");
	int found = 0;

	if (f == NULL)
		return 0;
	if (fgets(line, sizeof(line), f) == NULL) {   /* header */
		fclose(f);
		return 0;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		unsigned a, b, c, e;
		int i;

		if (sscanf(line, "%63s %*s %*s %63s %*s %63s", ip, hw, d) != 3)
			continue;
		if (strcmp(d, dev) != 0)
			continue;
		if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &e) != 4)
			continue;
		if (((a << 24) | (b << 16) | (c << 8) | e) != gw)
			continue;
		if (sscanf(hw, "%x:%x:%x:%x:%x:%x",
			   &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6)
			continue;
		for (i = 0; i < 6; i++)
			mac[i] = (uint8_t)m[i];
		found = 1;
		break;
	}
	fclose(f);
	return found;
}

/* One egress object per (interface, gateway), created the first time it is
 * wanted and kept, because creating a second for the same next hop would spend
 * a table entry to say the same thing. */
static int nexthop(struct l3if *ifp, int ifx, uint32_t gw, bcm_if_t *eg)
{
	bcm_l3_egress_t egr;
	uint8_t mac[6];
	int rv, i;

	for (i = 0; i < nnh; i++) {
		if (nh[i].ifx == ifx && nh[i].gw == gw) {
			*eg = nh[i].eg;
			return BCM_E_NONE;
		}
	}
	if (nnh >= MAX_NH)
		return BCM_E_RESOURCE;
	if (!arp_lookup(ifp->ifname, gw, mac))
		return BCM_E_NOT_FOUND;

	bcm_l3_egress_t_init(&egr);
	memcpy(egr.mac_addr, mac, 6);
	egr.intf = ifp->intf;
	egr.vlan = ifp->vlan;
	egr.port = ifp->port;
	egr.module = 0;

	rv = bcm_l3_egress_create(l3_unit, 0, &egr, eg);
	if (rv != BCM_E_NONE)
		return rv;

	nh[nnh].ifx = ifx;
	nh[nnh].gw = gw;
	nh[nnh].eg = *eg;
	nnh++;
	printf("l3: next hop %u.%u.%u.%u dev %s via "
	       "%02x:%02x:%02x:%02x:%02x:%02x -> egress %d\n",
	       gw >> 24, (gw >> 16) & 0xff, (gw >> 8) & 0xff, gw & 0xff,
	       ifp->ifname, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], *eg);
	fflush(stdout);
	return BCM_E_NONE;
}

static uint32_t q_self_ip;

static int q_ospf(bcm_field_entry_t e)
{
	return bcm_field_qualify_IpProtocol(l3_unit, e, 89, 0xff);
}

static int q_self(bcm_field_entry_t e)
{
	return bcm_field_qualify_DstIp(l3_unit, e, (bcm_ip_t)q_self_ip, 0xffffffff);
}

static int fp_add(const char *what, bcm_field_entry_t *ent, int *stat_id,
		  int (*qual)(bcm_field_entry_t))
{
	int rv;

	if (fp_grp < 0)
		return -1;
	rv = bcm_field_entry_create(l3_unit, fp_grp, ent);
	if (rv != BCM_E_NONE) {
		fprintf(stderr, "fp: %s entry_create: %d\n", what, rv);
		return -1;
	}
	rv = qual(*ent);
	if (rv != BCM_E_NONE) {
		fprintf(stderr, "fp: %s qualify: %d\n", what, rv);
		return -1;
	}
	rv = bcm_field_action_add(l3_unit, *ent, bcmFieldActionCopyToCpu, 0, 0);
	if (rv != BCM_E_NONE) {
		fprintf(stderr, "fp: %s action_add: %d\n", what, rv);
		return -1;
	}
	if (fp_stats && stat_id != NULL) {
		bcm_field_stat_t s[1] = { bcmFieldStatPackets };

		if (bcm_field_stat_create(l3_unit, fp_grp, 1, s, stat_id) == BCM_E_NONE)
			bcm_field_entry_stat_attach(l3_unit, *ent, *stat_id);
	}
	rv = bcm_field_entry_install(l3_unit, *ent);
	if (rv != BCM_E_NONE) {
		fprintf(stderr, "fp: %s install: %d\n", what, rv);
		return -1;
	}
	printf("fp: %s installed\n", what);
	fflush(stdout);
	return 0;
}

static void fp_setup(void)
{
	bcm_field_qset_t q;
	int rv;

	BCM_FIELD_QSET_INIT(q);
	BCM_FIELD_QSET_ADD(q, bcmFieldQualifyStageIngress);
	BCM_FIELD_QSET_ADD(q, bcmFieldQualifyIpProtocol);
	BCM_FIELD_QSET_ADD(q, bcmFieldQualifyDstIp);
	rv = bcm_field_group_create(l3_unit, q, BCM_FIELD_GROUP_PRIO_ANY, &fp_grp);
	if (rv != BCM_E_NONE) {
		fprintf(stderr, "fp: group_create: %d -- the control plane will not "
			"survive MY_STATION\n", rv);
		fp_grp = -1;
		return;
	}
	fp_add("ospf (ip proto 89)", &fp_ospf, &fp_stat_ospf, q_ospf);
}

static void fp_show(const char *tag, int stat_id)
{
	uint64 v;

	if (stat_id < 0)
		return;
	if (bcm_field_stat_get(l3_unit, stat_id, bcmFieldStatPackets, &v) != BCM_E_NONE)
		return;
	printf("fp: %-28s punted %llu packets\n", tag,
	       (unsigned long long)COMPILER_64_LO(v) |
	       ((unsigned long long)COMPILER_64_HI(v) << 32));
}

/*
 * Our own address has to punt.
 *
 * Done from the poll loop rather than at start-up, because the tap exists
 * before anything has given it an address: at start-up there is nothing to
 * read.
 *
 * The host entry's l3a_intf is an EGRESS OBJECT id, not an L3 interface id.
 * Passing the interface id is accepted and installs an entry that points
 * nowhere useful, which is the worst of both -- it reports success.
 */
static void self_punt(struct l3if *ifp)
{
	bcm_l3_host_t h;
	bcm_l3_egress_t egr;
	bcm_port_config_t cfg;
	uint32_t ip = 0;
	int rv;

	if (iface_ipv4(ifp->ifname, &ip) != 0 || ip == 0)
		return;

	if (ifp->fp_self < 0 && fp_grp >= 0) {
		char b[80];

		snprintf(b, sizeof(b), "self %s (%u.%u.%u.%u)", ifp->ifname,
			 ip >> 24, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
		q_self_ip = ip;
		fp_add(b, &ifp->fp_self, &ifp->fp_stat_self, q_self);
	}
	if (ifp->self_done)
		return;

	if (ifp->cpu_eg < 0) {
		if (bcm_port_config_get(l3_unit, &cfg) != BCM_E_NONE)
			return;
		bcm_l3_egress_t_init(&egr);
		egr.intf = ifp->intf;
		egr.vlan = ifp->vlan;
		BCM_PBMP_ITER(cfg.cpu, egr.port) {
			break;
		}
		egr.flags = BCM_L3_L2TOCPU;
		memset(egr.mac_addr, 0, 6);
		rv = bcm_l3_egress_create(l3_unit, 0, &egr, &ifp->cpu_eg);
		if (rv != BCM_E_NONE) {
			fprintf(stderr, "l3: %s cpu egress: %d\n", ifp->ifname, rv);
			ifp->cpu_eg = -1;
			return;
		}
	}

	bcm_l3_host_t_init(&h);
	h.l3a_ip_addr = ip;
	h.l3a_flags = BCM_L3_L2TOCPU;
	h.l3a_intf = ifp->cpu_eg;
	rv = bcm_l3_host_add(l3_unit, &h);
	printf("l3: %s self %u.%u.%u.%u -> CPU: %d\n", ifp->ifname,
	       ip >> 24, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff, rv);
	fflush(stdout);
	if (rv == BCM_E_NONE)
		ifp->self_done = 1;
}

static int rt_find(uint32_t dst, uint32_t mask)
{
	int i;

	for (i = 0; i < nrt; i++) {
		if (rt[i].dst == dst && rt[i].mask == mask)
			return i;
	}
	return -1;
}

/* Withdraw what the kernel no longer has. Without this the chip keeps
 * forwarding to a next hop the routing protocol has already given up on. */
static void rt_sweep(void)
{
	int i, gone = 0;

	for (i = 0; i < nrt; ) {
		bcm_l3_route_t r;

		if (rt[i].seen) {
			rt[i].seen = 0;
			i++;
			continue;
		}
		bcm_l3_route_t_init(&r);
		r.l3a_subnet = rt[i].dst;
		r.l3a_ip_mask = rt[i].mask;
		r.l3a_intf = rt[i].eg;
		bcm_l3_route_delete(l3_unit, &r);
		st_gone++;
		gone++;

		rt[i] = rt[nrt - 1];
		nrt--;
	}
	if (gone) {
		printf("l3: -%d routes withdrawn from DEFIP\n", gone);
		fflush(stdout);
	}
}

static void poll_routes(void)
{
	char line[512], iface[64], dst[32], gw[32], mask[32];
	FILE *f = fopen("/proc/net/route", "r");
	int flags, added = 0;

	if (f == NULL)
		return;
	if (fgets(line, sizeof(line), f) == NULL) {   /* header */
		fclose(f);
		return;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		bcm_l3_route_t r;
		struct l3if *ifp;
		bcm_if_t eg;
		uint32_t d, m, g;
		int rv, idx, moved = 0;

		if (sscanf(line, "%63s %31s %31s %x %*d %*d %*d %31s",
			   iface, dst, gw, &flags, mask) != 5)
			continue;
		ifp = if_by_name(iface);
		if (ifp == NULL)
			continue;
		if (!(flags & 0x1))     /* RTF_UP */
			continue;
		if (!(flags & 0x2))     /* RTF_GATEWAY; the rest are host entries */
			continue;

		d = be_hex_to_host(dst);
		m = be_hex_to_host(mask);
		g = be_hex_to_host(gw);

		if (nexthop(ifp, (int)(ifp - ifs), g, &eg) != BCM_E_NONE) {
			st_unresolved++;
			continue;
		}

		idx = rt_find(d, m);
		if (idx >= 0) {
			rt[idx].seen = 1;
			if (rt[idx].eg == eg)
				continue;       /* unchanged */
			moved = 1;              /* same prefix, different next hop */
		}

		bcm_l3_route_t_init(&r);
		r.l3a_subnet = d;
		r.l3a_ip_mask = m;
		r.l3a_intf = eg;
		if (moved)
			r.l3a_flags |= BCM_L3_REPLACE;

		rv = bcm_l3_route_add(l3_unit, &r);
		if (rv != BCM_E_NONE) {
			st_failed++;
			continue;
		}
		if (moved) {
			rt[idx].eg = eg;
			st_moved++;
		} else if (nrt < MAX_RT) {
			rt[nrt].dst = d;
			rt[nrt].mask = m;
			rt[nrt].eg = eg;
			rt[nrt].seen = 1;
			nrt++;
			st_added++;
			added++;
		}
	}
	fclose(f);
	rt_sweep();
	if (added) {
		printf("l3: +%d routes into DEFIP (total %lu, moved %lu, failed %lu, "
		       "unresolved %lu)\n",
		       added, st_added, st_moved, st_failed, st_unresolved);
		fflush(stdout);
	}
}

static int host_seen(int ifx, uint32_t ip)
{
	int i;

	for (i = 0; i < nhs; i++) {
		if (hs[i].ifx == ifx && hs[i].ip == ip)
			return 1;
	}
	return 0;
}

/*
 * Directly attached hosts need host entries.
 *
 * Only routes with a gateway go into DEFIP above, which leaves a hole a router
 * cannot have: a destination on one of our own subnets has no gateway, so
 * nothing is programmed and the chip cannot reach it. That is not an edge case
 * -- the peer at the far end of each link is exactly this, and it is the first
 * thing anyone pings.
 *
 * Our own address is skipped: it already has an L2TOCPU entry, and replacing
 * that with a forwarding entry would send packets meant for us back out of the
 * port they arrived on.
 */
static void poll_hosts(void)
{
	char line[256], ip[64], hw[64], dev[64];
	FILE *f = fopen("/proc/net/arp", "r");
	int added = 0;

	if (f == NULL)
		return;
	if (fgets(line, sizeof(line), f) == NULL) {
		fclose(f);
		return;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		bcm_l3_host_t h;
		struct l3if *ifp;
		bcm_if_t eg;
		unsigned a, b, c, d;
		uint32_t v, self = 0;
		int ifx, rv;

		if (sscanf(line, "%63s %*s %*s %63s %*s %63s", ip, hw, dev) != 3)
			continue;
		if (strcmp(hw, "00:00:00:00:00:00") == 0)
			continue;                       /* not resolved yet */
		ifp = if_by_name(dev);
		if (ifp == NULL)
			continue;
		if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
			continue;
		v = (a << 24) | (b << 16) | (c << 8) | d;
		ifx = (int)(ifp - ifs);

		if (iface_ipv4(ifp->ifname, &self) == 0 && v == self)
			continue;
		if (host_seen(ifx, v))
			continue;
		if (nhs >= MAX_HOST)
			break;
		if (nexthop(ifp, ifx, v, &eg) != BCM_E_NONE)
			continue;

		bcm_l3_host_t_init(&h);
		h.l3a_ip_addr = v;
		h.l3a_intf = eg;
		rv = bcm_l3_host_add(l3_unit, &h);
		if (rv != BCM_E_NONE)
			continue;

		hs[nhs].ifx = ifx;
		hs[nhs].ip = v;
		nhs++;
		st_host++;
		added++;
		printf("l3: host %u.%u.%u.%u dev %s -> egress %d\n",
		       a, b, c, d, ifp->ifname, eg);
	}
	fclose(f);
	if (added)
		fflush(stdout);
}

void nosaic_l3_poll(void)
{
	static unsigned long ticks;
	static unsigned long last;
	unsigned long now;
	int i;

	if (!l3_on)
		return;
	for (i = 0; i < nif; i++)
		self_punt(&ifs[i]);
	poll_routes();
	poll_hosts();

	/* Report periodically, and only when something has changed.
	 *
	 * What is reported is the chip's own accounting, not ours. Our counters
	 * say what we asked for; only the chip says what it has. This project has
	 * already closed one lead on the strength of a return code that meant
	 * nothing, and "the route was accepted" and "the route is in the ASIC"
	 * are different claims. */
	now = st_added + st_moved + st_gone + st_failed + st_host;
	if (++ticks % 60 == 0 && now != last) {
		last = now;
		nosaic_l3_stats();
	}
}

int nosaic_l3_add_intf(int unit, const char *ifname, int port, int vlan,
		       const bcm_mac_t mac, int mtu)
{
	bcm_l3_intf_t intf;
	bcm_l2_station_t st;
	struct l3if *ifp;
	int rv;

	if (nif >= MAX_IF)
		return -1;
	l3_unit = unit;

	if (nif == 0) {
		const char *v = nosaic_props_get("l3_fp_stats");

		fp_stats = v != NULL && atoi(v) != 0;

		/* Egress objects need advanced egress management turned on first.
		 * Without it bcm_l3_egress_create returns BCM_E_DISABLED, and the
		 * failure surfaces much later as unicast to our own address being
		 * dropped -- the CPU egress object it needed was never built. */
		rv = bcm_switch_control_set(unit, bcmSwitchL3EgressMode, 1);
		if (rv != BCM_E_NONE) {
			fprintf(stderr, "l3: L3EgressMode: %d\n", rv);
			return -1;
		}
	}

	ifp = &ifs[nif];
	memset(ifp, 0, sizeof(*ifp));
	snprintf(ifp->ifname, IFNAMSIZ, "%s", ifname);
	ifp->port = port;
	ifp->vlan = (bcm_vlan_t)vlan;
	ifp->intf = -1;
	ifp->cpu_eg = -1;
	ifp->fp_self = -1;
	ifp->fp_stat_self = -1;
	memcpy(ifp->mac, mac, 6);

	bcm_l3_intf_t_init(&intf);
	memcpy(intf.l3a_mac_addr, mac, 6);
	intf.l3a_vid = ifp->vlan;
	intf.l3a_mtu = mtu > 0 ? mtu : 1500;
	rv = bcm_l3_intf_create(unit, &intf);
	if (rv != BCM_E_NONE) {
		fprintf(stderr, "l3: %s bcm_l3_intf_create: %d\n", ifname, rv);
		return -1;
	}
	ifp->intf = intf.l3a_intf_id;

	/* MY_STATION: without this the chip never routes frames sent to our MAC. */
	bcm_l2_station_t_init(&st);
	memcpy(st.dst_mac, mac, 6);
	memset(st.dst_mac_mask, 0xff, 6);
	st.flags = BCM_L2_STATION_IPV4 | BCM_L2_STATION_IPV6 |
		   BCM_L2_STATION_ARP_RARP;
	rv = bcm_l2_station_add(unit, &ifp->station, &st);
	if (rv != BCM_E_NONE) {
		fprintf(stderr, "l3: %s bcm_l2_station_add: %d\n", ifname, rv);
		return -1;
	}

	if (nif == 0) {
		/* Multicast must still terminate. MY_STATION diverts frames for our
		 * MAC into the routing engine, and the bridge's path for multicast
		 * no longer gets a look in -- so OSPF hellos to 224.0.0.5 stop
		 * arriving and the adjacency stalls. The vendor OS carries this same
		 * pair of station entries beside the router MAC. */
		static const bcm_mac_t mc4 = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x00 };
		static const bcm_mac_t mc4m = { 0xff, 0xff, 0xff, 0x00, 0x00, 0x00 };
		static const bcm_mac_t mc6 = { 0x33, 0x33, 0x00, 0x00, 0x00, 0x00 };
		static const bcm_mac_t mc6m = { 0xff, 0xff, 0x00, 0x00, 0x00, 0x00 };
		bcm_l2_station_t mc;
		int id;

		bcm_l2_station_t_init(&mc);
		memcpy(mc.dst_mac, mc4, 6);
		memcpy(mc.dst_mac_mask, mc4m, 6);
		mc.flags = BCM_L2_STATION_IPV4 | BCM_L2_STATION_ARP_RARP;
		rv = bcm_l2_station_add(unit, &id, &mc);
		printf("l3: station 01:00:5e:00:00:00/24 (v4 multicast): %d\n", rv);

		bcm_l2_station_t_init(&mc);
		memcpy(mc.dst_mac, mc6, 6);
		memcpy(mc.dst_mac_mask, mc6m, 6);
		mc.flags = BCM_L2_STATION_IPV6;
		rv = bcm_l2_station_add(unit, &id, &mc);
		printf("l3: station 33:33:00:00:00:00/16 (v6 multicast): %d\n", rv);

		fp_setup();
	}

	nif++;
	l3_on = 1;
	printf("l3: %s interface %d (port %d, vlan %d, mtu %d), my_station %d\n",
	       ifname, ifp->intf, port, ifp->vlan, intf.l3a_mtu, ifp->station);
	fflush(stdout);
	return 0;
}

void nosaic_l3_stats(void)
{
	bcm_l3_info_t info;
	int i;

	if (!l3_on)
		return;
	printf("l3: %lu routes / %d next hops (moved %lu, gone %lu, failed %lu, "
	       "unresolved %lu), %lu host entries\n",
	       st_added, nnh, st_moved, st_gone, st_failed, st_unresolved, st_host);

	fp_show("ospf (ip proto 89)", fp_stat_ospf);
	for (i = 0; i < nif; i++) {
		char b[64];

		snprintf(b, sizeof(b), "self %s", ifs[i].ifname);
		fp_show(b, ifs[i].fp_stat_self);
	}

	/* The chip's own accounting, not ours. This is what answers "are the
	 * routes actually in the ASIC" -- our counters only say what we asked
	 * for, and this project has already closed one lead on the strength of a
	 * return code that meant nothing. */
	if (bcm_l3_info(l3_unit, &info) == BCM_E_NONE) {
		printf("l3: CHIP route %d/%d  intf %d/%d  host %d/%d\n",
		       info.l3info_used_route, info.l3info_max_route,
		       info.l3info_used_intf, info.l3info_max_intf,
		       info.l3info_used_host, info.l3info_max_host);
	}
	fflush(stdout);
}
