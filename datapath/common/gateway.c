/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Virtual gateways. switchapi 1.6.
 *
 * A shared address on an SVI -- 10.0.10.254 on vlan10 -- that both switches
 * of an MLAG pair answer for with the same MAC, and both route for, so a
 * host's default gateway outlives either switch and whichever of them a
 * host's traffic reaches forwards it. What Arista calls VARP and Cumulus VRR.
 *
 * THREE PIECES
 *
 *   The chip.   A MY_STATION entry for the virtual MAC, so a frame sent to it
 *               is routed in the chip exactly as one sent to the SVI's own MAC
 *               is. Station entries are not per VLAN, so one serves every
 *               gateway.
 *
 *   ARP.        tapbridge answers an ARP request for a gateway address itself,
 *               with the virtual MAC, and does not pass it to the kernel -- the
 *               kernel would answer with the SVI's own MAC, and a host would
 *               cache whichever came last. Both switches of a pair answer; the
 *               host gets the same MAC twice.
 *
 *   The kernel. The address is put on the SVI as a /32, so the kernel answers
 *               a ping to it and forwards what the chip punts, and l3sync punts
 *               it like every other address of the SVI. A /32 never becomes the
 *               interface's primary address, so the kernel's own traffic --
 *               its ARP requests, its OSPF -- keeps coming from the SVI's own.
 *               A frame to the virtual MAC that the chip punts is handed to
 *               the SVI's tap with its destination rewritten to the tap's MAC,
 *               or the kernel would drop it as someone else's.
 *
 * No macvlan: none of the switch kernels has one, and the kernel is outside
 * the A/B slot.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bcm/error.h>
#include <bcm/l2.h>

#include "gateway.h"
#include "tapbridge.h"

#define MAX_GW 256

struct gw { int vid; uint32_t ip; int plen; };      /* ip in network order */

static pthread_mutex_t gw_lock = PTHREAD_MUTEX_INITIALIZER;
static int gw_unit = -1;
static volatile unsigned char vmac[6] = { 0x00, 0x00, 0x5e, 0x00, 0x01, 0x01 };
static struct gw gws[MAX_GW];
static volatile int ngw;
static volatile unsigned char on_vid[4096];
static int station = -1;

/* ---------------------------------------------------------------------- */
/* The kernel: the address on the SVI, over rtnetlink. */

static void rta_add(struct nlmsghdr *n, int type, const void *data, int len)
{
	struct rtattr *r = (struct rtattr *)((char *)n + NLMSG_ALIGN(n->nlmsg_len));

	r->rta_type = (unsigned short)type;
	r->rta_len = (unsigned short)RTA_LENGTH(len);
	memcpy(RTA_DATA(r), data, (size_t)len);
	n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(r->rta_len);
}

static int kernel_addr(int add, int vid, uint32_t ip)
{
	struct { struct nlmsghdr n; struct ifaddrmsg a; char buf[64]; } req;
	char name[IFNAMSIZ], ack[256];
	unsigned idx;
	int fd, rv = -1;
	ssize_t r;

	snprintf(name, sizeof(name), "vlan%d", vid);
	if ((idx = if_nametoindex(name)) == 0)
		return -1;
	if ((fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE)) < 0)
		return -1;
	memset(&req, 0, sizeof(req));
	req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
	req.n.nlmsg_type = add ? RTM_NEWADDR : RTM_DELADDR;
	req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | (add ? NLM_F_CREATE | NLM_F_REPLACE : 0);
	req.a.ifa_family = AF_INET;
	req.a.ifa_prefixlen = 32;
	req.a.ifa_index = idx;
	rta_add(&req.n, IFA_LOCAL, &ip, 4);
	rta_add(&req.n, IFA_ADDRESS, &ip, 4);
	if (send(fd, &req, req.n.nlmsg_len, 0) > 0 && (r = recv(fd, ack, sizeof(ack), 0)) > 0) {
		struct nlmsghdr *h = (struct nlmsghdr *)ack;

		if (h->nlmsg_type == NLMSG_ERROR) {
			struct nlmsgerr *e = NLMSG_DATA(h);

			rv = e->error == 0 || (!add && e->error == -EADDRNOTAVAIL) ? 0 : -1;
		}
	}
	close(fd);
	return rv;
}

/*
 * ⚠ AND THE KERNEL MUST NOT ARP FROM THE GATEWAY ADDRESS.
 *
 * Answering a ping to 10.99.40.254, the kernel sources the reply from .254,
 * and if it has to ARP for the host first, its request says "10.99.40.254 is
 * at <the SVI's own MAC>" -- and the host believes it, over the virtual MAC
 * it was just told. Measured: a Nexus resolved the gateway to the 7050SX2's
 * SVI MAC. arp_announce 2 makes the kernel's requests carry the SVI's own
 * address instead, the best one in the target's subnet, which a /32 never is.
 */
static void quiet_arp(int vid)
{
	char path[96];
	FILE *f;

	snprintf(path, sizeof(path), "/proc/sys/net/ipv4/conf/vlan%d/arp_announce", vid);
	if ((f = fopen(path, "w")) != NULL) {
		fputs("2\n", f);
		fclose(f);
	}
}

/* ---------------------------------------------------------------------- */
/* The chip: MY_STATION for the virtual MAC. Caller holds gw_lock. */

static void station_set(int want)
{
	bcm_l2_station_t st;
	int rv;

	if (station >= 0 && !want) {
		bcm_l2_station_delete(gw_unit, station);
		station = -1;
		return;
	}
	if (station >= 0 || !want)
		return;
	bcm_l2_station_t_init(&st);
	memcpy(st.dst_mac, (const unsigned char *)vmac, 6);
	memset(st.dst_mac_mask, 0xff, 6);
	st.flags = BCM_L2_STATION_IPV4 | BCM_L2_STATION_IPV6 | BCM_L2_STATION_ARP_RARP;
	rv = bcm_l2_station_add(gw_unit, &station, &st);
	if (rv != BCM_E_NONE) {
		fprintf(stderr, "gateway: bcm_l2_station_add: %s; the chip will not route "
			"frames sent to the virtual MAC\n", bcm_errmsg(rv));
		station = -1;
	}
}

/* A gratuitous ARP from the virtual MAC, so hosts that already know the
 * gateway -- from the other switch, or from before -- have it right. */
static void garp(int vid, uint32_t ip)
{
	unsigned char f[42];

	memset(f, 0xff, 6);
	memcpy(f + 6, (const unsigned char *)vmac, 6);
	f[12] = 0x08; f[13] = 0x06;
	f[14] = 0; f[15] = 1; f[16] = 0x08; f[17] = 0; f[18] = 6; f[19] = 4;
	f[20] = 0; f[21] = 1;                               /* request */
	memcpy(f + 22, (const unsigned char *)vmac, 6);
	memcpy(f + 28, &ip, 4);
	memset(f + 32, 0, 6);
	memcpy(f + 38, &ip, 4);
	nosaic_tap_svi_xmit(vid, f, sizeof(f));
}

/* ---------------------------------------------------------------------- */

static int parse(const char *svi, const char *prefix, int *vid, uint32_t *ip, int *plen,
		 char *err, size_t n)
{
	char a[64], *slash;
	struct in_addr in;

	if (svi == NULL || sscanf(svi, "vlan%d", vid) != 1 || *vid < 1 || *vid > 4094) {
		if (err != NULL)
			snprintf(err, n, "%s is not a routed vlan interface", svi ? svi : "");
		return -1;
	}
	snprintf(a, sizeof(a), "%s", prefix ? prefix : "");
	*plen = 32;
	if ((slash = strchr(a, '/')) != NULL) {
		*slash = '\0';
		*plen = atoi(slash + 1);
	}
	if (inet_pton(AF_INET, a, &in) != 1 || *plen < 1 || *plen > 32) {
		if (err != NULL)
			snprintf(err, n, "virtual gateway %s: IPv4 only", prefix ? prefix : "");
		return -1;
	}
	*ip = in.s_addr;
	return 0;
}

int nosaic_gw_supported(void)
{
	return gw_unit >= 0;
}

int nosaic_gw_set_mac(const char *mac, char *err, size_t n)
{
	unsigned m[6];
	int i;

	if (!nosaic_gw_supported())
		return -2;
	if (mac == NULL || sscanf(mac, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3],
				  &m[4], &m[5]) != 6) {
		if (err != NULL)
			snprintf(err, n, "virtual mac \"%s\" is not a MAC address", mac ? mac : "");
		return -1;
	}
	if (m[0] & 1) {
		if (err != NULL)
			snprintf(err, n, "virtual mac %s is multicast; it must be unicast", mac);
		return -1;
	}
	pthread_mutex_lock(&gw_lock);
	station_set(0);
	for (i = 0; i < 6; i++)
		vmac[i] = (unsigned char)m[i];
	station_set(ngw > 0);
	for (i = 0; i < ngw; i++)
		garp(gws[i].vid, gws[i].ip);
	pthread_mutex_unlock(&gw_lock);
	return 0;
}

int nosaic_gw_add(const char *svi, const char *prefix, char *err, size_t n)
{
	char name[IFNAMSIZ];
	uint32_t ip;
	int vid, plen, i;

	if (!nosaic_gw_supported())
		return -2;
	if (parse(svi, prefix, &vid, &ip, &plen, err, n) != 0)
		return -1;
	snprintf(name, sizeof(name), "vlan%d", vid);
	if (if_nametoindex(name) == 0) {
		if (err != NULL)
			snprintf(err, n, "%s is not a routed vlan interface", svi);
		return -1;
	}
	pthread_mutex_lock(&gw_lock);
	for (i = 0; i < ngw; i++) {
		if (gws[i].vid == vid && gws[i].ip == ip) {
			gws[i].plen = plen;
			kernel_addr(1, vid, ip);        /* again, in case the tap was remade */
			quiet_arp(vid);
			pthread_mutex_unlock(&gw_lock);
			return 0;
		}
	}
	if (ngw >= MAX_GW) {
		pthread_mutex_unlock(&gw_lock);
		if (err != NULL)
			snprintf(err, n, "at most %d virtual gateways", MAX_GW);
		return -1;
	}
	if (kernel_addr(1, vid, ip) != 0) {
		pthread_mutex_unlock(&gw_lock);
		if (err != NULL)
			snprintf(err, n, "%s: could not put %s on it", svi, prefix);
		return -1;
	}
	quiet_arp(vid);
	gws[ngw].vid = vid;
	gws[ngw].ip = ip;
	gws[ngw].plen = plen;
	ngw++;
	on_vid[vid] = 1;
	station_set(1);
	garp(vid, ip);
	pthread_mutex_unlock(&gw_lock);
	printf("gateway: %s %s, answered with %02x:%02x:%02x:%02x:%02x:%02x\n", svi, prefix,
	       vmac[0], vmac[1], vmac[2], vmac[3], vmac[4], vmac[5]);
	fflush(stdout);
	return 0;
}

static void remove_at(int i)
{
	int vid = gws[i].vid, j, still = 0;

	gws[i] = gws[--ngw];
	for (j = 0; j < ngw; j++)
		still = still || gws[j].vid == vid;
	on_vid[vid] = (unsigned char)still;
	station_set(ngw > 0);
}

int nosaic_gw_del(const char *svi, const char *prefix, char *err, size_t n)
{
	uint32_t ip;
	int vid, plen, i;

	if (!nosaic_gw_supported())
		return -2;
	if (parse(svi, prefix, &vid, &ip, &plen, err, n) != 0)
		return -1;
	pthread_mutex_lock(&gw_lock);
	for (i = 0; i < ngw; i++) {
		if (gws[i].vid == vid && gws[i].ip == ip) {
			kernel_addr(0, vid, ip);
			remove_at(i);
			break;
		}
	}
	pthread_mutex_unlock(&gw_lock);
	return 0;
}

void nosaic_gw_svi_gone(int vid)
{
	int i;

	pthread_mutex_lock(&gw_lock);
	for (i = 0; i < ngw; )
		if (gws[i].vid == vid)
			remove_at(i);
		else
			i++;
	pthread_mutex_unlock(&gw_lock);
}

void nosaic_gw_query(FILE *out)
{
	char a[INET_ADDRSTRLEN];
	int i;

	pthread_mutex_lock(&gw_lock);
	fprintf(out, "{\"ok\":true,\"result\":[");
	for (i = 0; i < ngw; i++) {
		inet_ntop(AF_INET, &gws[i].ip, a, sizeof(a));
		fprintf(out, "%s{\"SVI\":\"vlan%d\",\"Address\":\"%s/%d\","
			"\"MAC\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}", i ? "," : "",
			gws[i].vid, a, gws[i].plen, vmac[0], vmac[1], vmac[2], vmac[3],
			vmac[4], vmac[5]);
	}
	fprintf(out, "]}\n");
	pthread_mutex_unlock(&gw_lock);
}

/* ---------------------------------------------------------------------- */
/* For tapbridge: lock-free, on the receive path. */

int nosaic_gw_on(int vid)
{
	return vid > 0 && vid < 4096 && on_vid[vid];
}

int nosaic_gw_is(int vid, uint32_t ip_be)
{
	int i, n = ngw;

	for (i = 0; i < n && i < MAX_GW; i++)
		if (gws[i].vid == vid && gws[i].ip == ip_be)
			return 1;
	return 0;
}

void nosaic_gw_mac(unsigned char mac[6])
{
	int i;

	for (i = 0; i < 6; i++)
		mac[i] = vmac[i];
}

int nosaic_gw_start(int unit)
{
	gw_unit = unit;
	return 0;
}
