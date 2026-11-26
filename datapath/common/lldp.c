/*
 * IEEE 802.1AB LLDP: receive neighbor TLVs and advertise local ports
 * (front-panel ASIC ports + optional eth0 management port).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <sal/types.h>
#include <bcm/error.h>
#include <bcm/l2.h>
#include <bcm/pkt.h>
#include <bcm/port.h>
#include <bcm/rx.h>

#include "lldp.h"
#include "tapbridge.h"

#define LLDP_RX_PRIORITY 115   /* above tapbridge (100) */
#define LLDP_MAX_PORT    160
#define LLDP_TX_INTERVAL 30
#define LLDP_TX_TTL      120
#define LLDP_ETHERTYPE   0x88cc

static const unsigned char lldp_da[6] = { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x0e };

struct lldp_nbr {
	int    valid;
	char   local[32];
	char   chassis[64];
	char   port[64];
	char   sysname[64];
	char   sysdesc[96];
	char   portdesc[64];
	char   mgmt[48];
	time_t expires;
};

static int lldp_unit = -1;
static pthread_mutex_t lldp_lock = PTHREAD_MUTEX_INITIALIZER;
static int lldp_enabled = 1;
static int eth0_enabled = 0;
static int eth0_fd = -1;
static int eth0_ifindex = 0;
static unsigned char eth0_mac[6];
static unsigned char port_disabled[LLDP_MAX_PORT];
static struct lldp_nbr nbrs[LLDP_MAX_PORT];
static struct lldp_nbr eth0_nbr;

static time_t mono_sec(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
		return ts.tv_sec;
	return time(NULL);
}

static void copy_printable(char *dst, size_t dlen, const unsigned char *src, size_t slen)
{
	size_t i, n = 0;

	if (dlen == 0)
		return;
	for (i = 0; i < slen && n + 1 < dlen; i++) {
		unsigned char c = src[i];
		if (c == '\0')
			break;
		if (c == '"' || c == '\\')
			c = ' ';
		dst[n++] = isprint(c) ? (char)c : '?';
	}
	dst[n] = '\0';
}

static void format_mac(char *dst, size_t dlen, const unsigned char *m)
{
	snprintf(dst, dlen, "%02x:%02x:%02x:%02x:%02x:%02x",
		 m[0], m[1], m[2], m[3], m[4], m[5]);
}

static int lldp_rx_port(int unit, bcm_pkt_t *pkt)
{
	bcm_gport_t gp;
	bcm_port_t local;

	BCM_GPORT_MODPORT_SET(gp, pkt->src_mod, pkt->src_port);
	if (bcm_port_local_get(unit, gp, &local) != BCM_E_NONE)
		return pkt->src_port;
	return local;
}

static void find_tap_name(int port, char *out, size_t len)
{
	int i;

	for (i = 0; i < nosaic_tap_count(); i++) {
		const char *nm = NULL;
		int p = -1;

		if (nosaic_tap_info(i, &nm, &p, NULL, NULL, NULL) == 0 &&
		    p == port && nm != NULL) {
			snprintf(out, len, "%s", nm);
			return;
		}
	}
	snprintf(out, len, "port%d", port);
}

/* Parse a raw Ethernet frame carrying an LLDPDU into out; returns TTL (>=0) or -1. */
static int parse_lldpdu(const unsigned char *p, int len, const char *local,
			struct lldp_nbr *out)
{
	int off = 12, ttl = -1;

	if (len < 18 || memcmp(p, lldp_da, 6) != 0)
		return -1;
	if (p[12] == 0x81 && p[13] == 0x00)
		off = 16;
	if (len < off + 2 || p[off] != 0x88 || p[off + 1] != 0xcc)
		return -1;
	off += 2;

	memset(out, 0, sizeof(*out));
	snprintf(out->local, sizeof(out->local), "%s", local);

	while (off + 2 <= len) {
		unsigned hdr = ((unsigned)p[off] << 8) | p[off + 1];
		unsigned type = (hdr >> 9) & 0x7f;
		unsigned tlen = hdr & 0x1ff;
		const unsigned char *val = p + off + 2;

		off += 2;
		if (type == 0)
			break;
		if (off + (int)tlen > len)
			break;

		switch (type) {
		case 1: /* Chassis ID */
			if (tlen >= 2) {
				if (val[0] == 4 && tlen == 7)
					format_mac(out->chassis, sizeof(out->chassis), val + 1);
				else
					copy_printable(out->chassis, sizeof(out->chassis), val + 1, tlen - 1);
			}
			break;
		case 2: /* Port ID */
			if (tlen >= 2) {
				if (val[0] == 3 && tlen == 7)
					format_mac(out->port, sizeof(out->port), val + 1);
				else
					copy_printable(out->port, sizeof(out->port), val + 1, tlen - 1);
			}
			break;
		case 3: /* TTL */
			if (tlen == 2)
				ttl = ((int)val[0] << 8) | val[1];
			break;
		case 4: /* Port Description */
			copy_printable(out->portdesc, sizeof(out->portdesc), val, tlen);
			break;
		case 5: /* System Name */
			copy_printable(out->sysname, sizeof(out->sysname), val, tlen);
			break;
		case 6: /* System Description */
			copy_printable(out->sysdesc, sizeof(out->sysdesc), val, tlen);
			break;
		case 8: /* Management Address */
			if (tlen >= 6 && val[0] == 5 && val[1] == 1) {
				snprintf(out->mgmt, sizeof(out->mgmt), "%u.%u.%u.%u",
					 val[2], val[3], val[4], val[5]);
			}
			break;
		}
		off += (int)tlen;
	}
	return ttl;
}

static bcm_rx_t lldp_rx(int unit, bcm_pkt_t *pkt, void *cookie)
{
	const unsigned char *p = pkt->pkt_data[0].data;
	int len = (int)pkt->pkt_data[0].len, port, ttl;
	char local[32];
	struct lldp_nbr n;

	(void)cookie;
	if (len < 18 || memcmp(p, lldp_da, 6) != 0)
		return BCM_RX_NOT_HANDLED;
	if (!((p[12] == 0x88 && p[13] == 0xcc) ||
	      (p[12] == 0x81 && p[13] == 0x00 && p[16] == 0x88 && p[17] == 0xcc)))
		return BCM_RX_NOT_HANDLED;

	port = lldp_rx_port(unit, pkt);
	if (port < 0 || port >= LLDP_MAX_PORT)
		return BCM_RX_HANDLED;

	find_tap_name(port, local, sizeof(local));
	ttl = parse_lldpdu(p, len, local, &n);
	if (ttl < 0)
		return BCM_RX_HANDLED;

	pthread_mutex_lock(&lldp_lock);
	if (!lldp_enabled || port_disabled[port]) {
		pthread_mutex_unlock(&lldp_lock);
		return BCM_RX_HANDLED;
	}
	if (ttl == 0) {
		nbrs[port].valid = 0;
	} else {
		n.valid = 1;
		n.expires = mono_sec() + ttl;
		nbrs[port] = n;
	}
	pthread_mutex_unlock(&lldp_lock);
	return BCM_RX_HANDLED;
}

static int put_tlv(unsigned char *buf, int pos, int cap, unsigned type,
		   const void *data, unsigned len)
{
	if (pos + 2 + (int)len > cap || len > 511)
		return pos;
	buf[pos]     = (unsigned char)(((type & 0x7f) << 1) | ((len >> 8) & 1));
	buf[pos + 1] = (unsigned char)(len & 0xff);
	if (len > 0)
		memcpy(buf + pos + 2, data, len);
	return pos + 2 + (int)len;
}

static int build_lldpdu(unsigned char *frame, int cap,
			const unsigned char src_mac[6],
			const unsigned char chassis[6],
			const char *ifname, const char *host,
			int have_ip4, const unsigned char ip4[4])
{
	unsigned char tmp[128];
	int pos = 14;
	size_t nlen;

	if (cap < 64)
		return 0;
	memcpy(frame, lldp_da, 6);
	memcpy(frame + 6, src_mac, 6);
	frame[12] = 0x88;
	frame[13] = 0xcc;

	/* TLV 1: Chassis ID (subtype 4 = MAC) */
	tmp[0] = 4;
	memcpy(tmp + 1, chassis, 6);
	pos = put_tlv(frame, pos, cap, 1, tmp, 7);

	/* TLV 2: Port ID (subtype 5 = interface name) */
	tmp[0] = 5;
	nlen = strlen(ifname);
	if (nlen > sizeof(tmp) - 1)
		nlen = sizeof(tmp) - 1;
	memcpy(tmp + 1, ifname, nlen);
	pos = put_tlv(frame, pos, cap, 2, tmp, (unsigned)(1 + nlen));

	/* TLV 3: TTL */
	tmp[0] = (LLDP_TX_TTL >> 8) & 0xff;
	tmp[1] = LLDP_TX_TTL & 0xff;
	pos = put_tlv(frame, pos, cap, 3, tmp, 2);

	/* TLV 5: System Name */
	pos = put_tlv(frame, pos, cap, 5, host, (unsigned)strlen(host));

	/* TLV 6: System Description */
	pos = put_tlv(frame, pos, cap, 6, "NOSaic", 6);

	/* TLV 7: System Capabilities (Bridge + Router supported & enabled = 0x0014) */
	{
		unsigned char caps[4] = { 0x00, 0x14, 0x00, 0x14 };
		pos = put_tlv(frame, pos, cap, 7, caps, sizeof(caps));
	}

	/* TLV 8: Management Address (IPv4) */
	if (have_ip4) {
		unsigned char mgmt[12];
		mgmt[0] = 5;
		mgmt[1] = 1;
		memcpy(mgmt + 2, ip4, 4);
		mgmt[6] = 1;
		mgmt[7] = 0; mgmt[8] = 0; mgmt[9] = 0; mgmt[10] = 0;
		mgmt[11] = 0;
		pos = put_tlv(frame, pos, cap, 8, mgmt, sizeof(mgmt));
	}

	/* TLV 0: End of LLDPDU */
	pos = put_tlv(frame, pos, cap, 0, NULL, 0);
	return pos;
}

static int get_eth0_ipv4(unsigned char ip[4])
{
	struct ifreq ifr;
	int fd;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "eth0");
	if (ioctl(fd, SIOCGIFADDR, &ifr) != 0) {
		close(fd);
		return -1;
	}
	close(fd);
	memcpy(ip, &((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr, 4);
	return 0;
}

/* Open an AF_PACKET socket bound to eth0 for EtherType 0x88cc (caller holds lldp_lock). */
static int eth0_open_locked(char *err, size_t errlen)
{
	struct ifreq ifr;
	struct sockaddr_ll sll;
	struct packet_mreq mreq;
	int fd;

	if (eth0_fd >= 0)
		return 0;
	fd = socket(AF_PACKET, SOCK_RAW, htons(LLDP_ETHERTYPE));
	if (fd < 0) {
		snprintf(err, errlen, "eth0 lldp socket: %s", strerror(errno));
		return -1;
	}
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "eth0");
	if (ioctl(fd, SIOCGIFINDEX, &ifr) != 0) {
		snprintf(err, errlen, "eth0 ifindex: %s", strerror(errno));
		close(fd);
		return -1;
	}
	eth0_ifindex = ifr.ifr_ifindex;
	if (ioctl(fd, SIOCGIFHWADDR, &ifr) == 0)
		memcpy(eth0_mac, ifr.ifr_hwaddr.sa_data, 6);

	memset(&sll, 0, sizeof(sll));
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(LLDP_ETHERTYPE);
	sll.sll_ifindex = eth0_ifindex;
	if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) != 0) {
		snprintf(err, errlen, "eth0 lldp bind: %s", strerror(errno));
		close(fd);
		return -1;
	}

	memset(&mreq, 0, sizeof(mreq));
	mreq.mr_ifindex = eth0_ifindex;
	mreq.mr_type = PACKET_MR_MULTICAST;
	mreq.mr_alen = 6;
	memcpy(mreq.mr_address, lldp_da, 6);
	(void)setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

	eth0_fd = fd;
	return 0;
}

static void eth0_close_locked(void)
{
	if (eth0_fd >= 0) {
		close(eth0_fd);
		eth0_fd = -1;
	}
	eth0_nbr.valid = 0;
}

static void lldp_tx_all(void)
{
	unsigned char chassis[6] = { 0 };
	unsigned char ip4[4];
	int have_ip4, i, n = nosaic_tap_count();
	int glb, do_eth0, efd, eidx;
	unsigned char emac[6];
	char host[64];

	pthread_mutex_lock(&lldp_lock);
	glb = lldp_enabled;
	do_eth0 = glb && eth0_enabled && eth0_fd >= 0;
	efd = eth0_fd;
	eidx = eth0_ifindex;
	memcpy(emac, eth0_mac, 6);
	pthread_mutex_unlock(&lldp_lock);

	if (!glb)
		return;

	for (i = 0; i < n; i++) {
		if (nosaic_tap_info(i, NULL, NULL, NULL, NULL, chassis) == 0)
			break;
	}
	if (gethostname(host, sizeof(host)) != 0)
		snprintf(host, sizeof(host), "nosaic");
	host[sizeof(host) - 1] = '\0';
	have_ip4 = (get_eth0_ipv4(ip4) == 0);

	for (i = 0; i < n; i++) {
		const char *name = NULL;
		unsigned char mac[6];
		unsigned char frame[512];
		int port = -1, vid = 1, link = 0, pos = 0, dis;

		if (nosaic_tap_info(i, &name, &port, &vid, NULL, mac) != 0 ||
		    name == NULL || port <= 0 || port >= LLDP_MAX_PORT)
			continue;
		pthread_mutex_lock(&lldp_lock);
		dis = port_disabled[port];
		pthread_mutex_unlock(&lldp_lock);
		if (dis)
			continue;
		if (bcm_port_link_status_get(lldp_unit, port, &link) != BCM_E_NONE || !link)
			continue;

		pos = build_lldpdu(frame, sizeof(frame), mac, chassis, name, host, have_ip4, ip4);
		if (pos > 0)
			nosaic_tap_xmit(port, vid > 0 ? vid : 1, frame, pos);
	}

	if (do_eth0) {
		unsigned char frame[512];
		struct sockaddr_ll sll;
		int pos;

		pos = build_lldpdu(frame, sizeof(frame), emac, chassis, "eth0", host, have_ip4, ip4);
		if (pos > 0) {
			memset(&sll, 0, sizeof(sll));
			sll.sll_family = AF_PACKET;
			sll.sll_protocol = htons(LLDP_ETHERTYPE);
			sll.sll_ifindex = eidx;
			sll.sll_halen = 6;
			memcpy(sll.sll_addr, lldp_da, 6);
			(void)sendto(efd, frame, (size_t)pos, 0,
				     (struct sockaddr *)&sll, sizeof(sll));
		}
	}
}

static void *lldp_thread(void *arg)
{
	time_t next_tx = mono_sec() + 3;

	(void)arg;
	for (;;) {
		int fd;
		time_t now = mono_sec();

		if (now >= next_tx) {
			lldp_tx_all();
			next_tx = mono_sec() + LLDP_TX_INTERVAL;
		}

		pthread_mutex_lock(&lldp_lock);
		fd = (lldp_enabled && eth0_enabled) ? eth0_fd : -1;
		pthread_mutex_unlock(&lldp_lock);

		if (fd >= 0) {
			struct pollfd pfd = { .fd = fd, .events = POLLIN };
			if (poll(&pfd, 1, 1000) > 0 && (pfd.revents & POLLIN)) {
				unsigned char buf[1500];
				struct sockaddr_ll sll;
				socklen_t slen = sizeof(sll);
				ssize_t r = recvfrom(fd, buf, sizeof(buf), 0,
						     (struct sockaddr *)&sll, &slen);
				if (r > 0 && sll.sll_pkttype != PACKET_OUTGOING) {
					struct lldp_nbr n;
					int ttl = parse_lldpdu(buf, (int)r, "eth0", &n);
					if (ttl >= 0) {
						pthread_mutex_lock(&lldp_lock);
						if (ttl == 0) {
							eth0_nbr.valid = 0;
						} else {
							n.valid = 1;
							n.expires = mono_sec() + ttl;
							eth0_nbr = n;
						}
						pthread_mutex_unlock(&lldp_lock);
					}
				}
			}
		} else {
			sleep(1);
		}
	}
	return NULL;
}

int nosaic_lldp_set(const char *iface, int enabled, char *err, size_t errlen)
{
	int i;

	pthread_mutex_lock(&lldp_lock);
	if (iface == NULL || iface[0] == '\0') {
		lldp_enabled = enabled ? 1 : 0;
		if (!lldp_enabled) {
			for (i = 0; i < LLDP_MAX_PORT; i++)
				nbrs[i].valid = 0;
			eth0_nbr.valid = 0;
		}
		pthread_mutex_unlock(&lldp_lock);
		return 0;
	}
	if (strcmp(iface, "eth0") == 0) {
		if (enabled) {
			if (eth0_open_locked(err, errlen) != 0) {
				pthread_mutex_unlock(&lldp_lock);
				return -1;
			}
			eth0_enabled = 1;
		} else {
			eth0_enabled = 0;
			eth0_close_locked();
		}
		pthread_mutex_unlock(&lldp_lock);
		return 0;
	}

	for (i = 0; i < nosaic_tap_count(); i++) {
		const char *nm = NULL;
		int p = -1;

		if (nosaic_tap_info(i, &nm, &p, NULL, NULL, NULL) == 0 &&
		    nm != NULL && strcmp(nm, iface) == 0 &&
		    p > 0 && p < LLDP_MAX_PORT) {
			port_disabled[p] = enabled ? 0 : 1;
			if (!enabled)
				nbrs[p].valid = 0;
			pthread_mutex_unlock(&lldp_lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&lldp_lock);
	snprintf(err, errlen, "no such interface %s", iface);
	return -1;
}

int nosaic_lldp_start(int unit)
{
	bcm_l2_cache_addr_t addr;
	int idx = -1, rv;
	pthread_t th;

	lldp_unit = unit;

	/* Trap 01:80:c2:00:00:0e to CPU across all VLANs without flooding.
	 * Do NOT set BCM_L2_CACHE_DISCARD: on Trident II, DST_DISCARD drops
	 * the frame before CPU delivery. BCM_L2_CACHE_CPU | BCM_L2_CACHE_BPDU
	 * traps to CPU without forwarding. */
	bcm_l2_cache_addr_t_init(&addr);
	memcpy(addr.mac, lldp_da, 6);
	memset(addr.mac_mask, 0xff, 6);
	addr.vlan = 0;
	addr.vlan_mask = 0;
	addr.flags = BCM_L2_CACHE_CPU | BCM_L2_CACHE_BPDU;
	rv = bcm_l2_cache_set(unit, -1, &addr, &idx);
	if (rv != BCM_E_NONE)
		fprintf(stderr, "lldp: bcm_l2_cache_set: %s (continuing)\n", bcm_errmsg(rv));
	else
		printf("lldp: trapping 01:80:c2:00:00:0e to CPU (l2 cache idx %d)\n", idx);
	fflush(stdout);

	rv = bcm_rx_register(unit, "nosaic-lldp", lldp_rx, LLDP_RX_PRIORITY, NULL,
			     BCM_RCO_F_ALL_COS);
	if (rv != BCM_E_NONE) {
		fprintf(stderr, "lldp: bcm_rx_register: %s\n", bcm_errmsg(rv));
		return -1;
	}
	if (pthread_create(&th, NULL, lldp_thread, NULL) != 0) {
		fprintf(stderr, "lldp: pthread_create: %s\n", strerror(errno));
		return -1;
	}
	pthread_detach(th);
	return 0;
}

static void emit_nbr(FILE *out, struct lldp_nbr *n, time_t now, int *emitted)
{
	long rem;

	if (!n->valid)
		return;
	rem = (long)(n->expires - now);
	if (rem <= 0) {
		n->valid = 0;
		return;
	}
	fprintf(out,
		"%s{\"local\":\"%s\",\"chassis\":\"%s\",\"port\":\"%s\","
		"\"sysname\":\"%s\",\"sysdesc\":\"%s\",\"portdesc\":\"%s\",\"mgmt\":\"%s\",\"ttl\":%ld}",
		(*emitted)++ ? "," : "",
		n->local, n->chassis, n->port,
		n->sysname, n->sysdesc, n->portdesc, n->mgmt, rem);
}

void nosaic_lldp_query(FILE *out)
{
	time_t now = mono_sec();
	int i, emitted = 0;

	fprintf(out, "{\"ok\":true,\"result\":[");
	pthread_mutex_lock(&lldp_lock);
	emit_nbr(out, &eth0_nbr, now, &emitted);
	for (i = 0; i < LLDP_MAX_PORT; i++)
		emit_nbr(out, &nbrs[i], now, &emitted);
	pthread_mutex_unlock(&lldp_lock);
	fprintf(out, "]}\n");
}

void nosaic_lldp_config_query(FILE *out)
{
	int i, emitted = 0;

	pthread_mutex_lock(&lldp_lock);
	fprintf(out, "{\"ok\":true,\"result\":{\"enabled\":%s,\"eth0\":%s,\"disabled\":[",
		lldp_enabled ? "true" : "false",
		eth0_enabled ? "true" : "false");
	for (i = 0; i < nosaic_tap_count(); i++) {
		const char *nm = NULL;
		int p = -1;

		if (nosaic_tap_info(i, &nm, &p, NULL, NULL, NULL) == 0 &&
		    nm != NULL && p > 0 && p < LLDP_MAX_PORT && port_disabled[p]) {
			fprintf(out, "%s\"%s\"", emitted++ ? "," : "", nm);
		}
	}
	fprintf(out, "]}}\n");
	pthread_mutex_unlock(&lldp_lock);
}
