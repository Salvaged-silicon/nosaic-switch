/*
 * Broadcom hardware VXLAN / EVPN datapath (L2 + L3 VNIs, remote VTEP tunnels,
 * BUM multicast replication, and Linux AF_BRIDGE netlink EVPN FDB sync).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <sal/types.h>
#include <bcm/error.h>
#include <bcm/l2.h>
#include <bcm/l3.h>
#include <bcm/multicast.h>
#include <bcm/port.h>
#include <bcm/switch.h>
#include <bcm/tunnel.h>
#include <bcm/vlan.h>
#include <bcm/vxlan.h>

#include "vxlan.h"

#define MAX_VNIS         256
#define MAX_VTEPS        128
#define MAX_VNI_VTEPS    64
#define MAX_ACCESS_PORTS 132
#define VXLAN_UDP_PORT   4789

#ifndef IFLA_VRF_TABLE
#define IFLA_VRF_TABLE 1
#endif

struct vtep_entry {
	int         used;
	uint32_t    dip;          /* host byte order */
	bcm_if_t    ul_egr;       /* underlay L3 egress object */
	bcm_if_t    tnl_intf;     /* L3 intf carrying the tunnel initiator */
	bcm_if_t    l3_egr;       /* L3 VXLAN overlay egress (for Type-5) */
	bcm_gport_t init_id;      /* tunnel initiator gport */
	bcm_gport_t term_id;      /* tunnel terminator gport */
};

struct vni_vtep {
	int         used;
	uint32_t    dip;
	bcm_gport_t vp_id;        /* network VXLAN gport */
	int         in_bum;       /* added to vni->mcast */
};

struct access_vp {
	int         used;
	bcm_port_t  port;
	bcm_gport_t vp_id;
};

struct vni_entry {
	int              used;
	int              vni;
	int              vid;
	int              l3;
	char             vrf[32];
	uint32_t         table_id;  /* Linux VRF routing table ID */
	bcm_vrf_t        vrf_id;    /* hardware bcm_vrf_t (1..255) */
	bcm_vpn_t        vpn;
	bcm_multicast_t  mcast;
	struct vni_vtep  peers[MAX_VNI_VTEPS];
	struct access_vp access[MAX_ACCESS_PORTS];
	unsigned long    macs_synced;
};

static int vxlan_unit = -1;
static int vxlan_ok = 0;
static pthread_mutex_t vxlan_lock = PTHREAD_MUTEX_INITIALIZER;
static char vtep_ip[32];
static uint32_t local_vtep_hbo = 0;
static struct vni_entry vnis[MAX_VNIS];
static struct vtep_entry vteps[MAX_VTEPS];

int nosaic_vxlan_supported(void)
{
	return vxlan_ok;
}

static int resolve_underlay_egr(uint32_t dip_hbo, bcm_if_t *egr_out)
{
	bcm_l3_route_t rt;
	bcm_l3_host_t host;

	bcm_l3_host_t_init(&host);
	host.l3a_ip_addr = dip_hbo;
	if (bcm_l3_host_find(vxlan_unit, &host) == BCM_E_NONE) {
		*egr_out = host.l3a_intf;
		return 0;
	}
	bcm_l3_route_t_init(&rt);
	rt.l3a_subnet = dip_hbo;
	rt.l3a_ip_mask = 0xffffffffU;
	if (bcm_l3_route_get(vxlan_unit, &rt) == BCM_E_NONE) {
		*egr_out = rt.l3a_intf;
		return 0;
	}
	return -1;
}

/* Ensure a hardware VXLAN tunnel initiator + terminator exists for remote VTEP dip_hbo.
 * Caller holds vxlan_lock. Returns index into vteps[] or -1. */
static int vtep_ensure_locked(uint32_t dip_hbo)
{
	int i, slot = -1, rv;
	bcm_if_t ul_egr = 0;
	bcm_l3_intf_t intf;
	bcm_tunnel_initiator_t tinit;
	bcm_tunnel_terminator_t tterm;
	struct in_addr ia;

	if (local_vtep_hbo == 0 || dip_hbo == 0 || dip_hbo == local_vtep_hbo)
		return -1;

	for (i = 0; i < MAX_VTEPS; i++) {
		if (vteps[i].used && vteps[i].dip == dip_hbo)
			return i;
		if (!vteps[i].used && slot < 0)
			slot = i;
	}
	if (slot < 0)
		return -1;

	if (resolve_underlay_egr(dip_hbo, &ul_egr) != 0)
		return -1;

	bcm_l3_intf_t_init(&intf);
	intf.l3a_vid = 4094;
	intf.l3a_mac_addr[0] = 0x02;
	intf.l3a_mac_addr[5] = (uint8)(slot + 1);
	rv = bcm_l3_intf_create(vxlan_unit, &intf);
	if (rv != BCM_E_NONE)
		return -1;

	bcm_tunnel_initiator_t_init(&tinit);
	tinit.type = bcmTunnelTypeVxlan;
	tinit.sip = local_vtep_hbo;
	tinit.dip = dip_hbo;
	tinit.udp_dst_port = VXLAN_UDP_PORT;
	tinit.udp_src_port = 0xffff;
	tinit.ttl = 64;
	tinit.flags = BCM_TUNNEL_INIT_USE_INNER_DF;
	rv = bcm_tunnel_initiator_set(vxlan_unit, &intf, &tinit);
	if (rv != BCM_E_NONE) {
		(void)bcm_l3_intf_delete(vxlan_unit, &intf);
		return -1;
	}

	bcm_tunnel_terminator_t_init(&tterm);
	tterm.type = bcmTunnelTypeVxlan;
	tterm.sip = dip_hbo;
	tterm.sip_mask = 0xffffffffU;
	tterm.dip = local_vtep_hbo;
	tterm.dip_mask = 0xffffffffU;
	tterm.udp_dst_port = VXLAN_UDP_PORT;
	rv = bcm_tunnel_terminator_add(vxlan_unit, &tterm);
	if (rv != BCM_E_NONE && rv != BCM_E_EXISTS) {
		(void)bcm_tunnel_initiator_clear(vxlan_unit, &intf);
		(void)bcm_l3_intf_delete(vxlan_unit, &intf);
		return -1;
	}

	memset(&vteps[slot], 0, sizeof(vteps[slot]));
	vteps[slot].used = 1;
	vteps[slot].dip = dip_hbo;
	vteps[slot].ul_egr = ul_egr;
	vteps[slot].tnl_intf = intf.l3a_intf_id;
	vteps[slot].init_id = tinit.tunnel_id;
	vteps[slot].term_id = tterm.tunnel_id;

	ia.s_addr = htonl(dip_hbo);
	printf("vxlan: created VTEP tunnel to %s (egr 0x%x)\n", inet_ntoa(ia), (unsigned)ul_egr);
	fflush(stdout);
	return slot;
}

/* Ensure a network VXLAN gport exists on vnis[vi] for remote VTEP dip_hbo.
 * Caller holds vxlan_lock. Returns pointer to vni_vtep or NULL. */
static struct vni_vtep *vni_peer_ensure_locked(int vi, uint32_t dip_hbo)
{
	struct vni_entry *ve = &vnis[vi];
	int i, slot = -1, ti, rv;
	bcm_vxlan_port_t vp;

	for (i = 0; i < MAX_VNI_VTEPS; i++) {
		if (ve->peers[i].used && ve->peers[i].dip == dip_hbo)
			return &ve->peers[i];
		if (!ve->peers[i].used && slot < 0)
			slot = i;
	}
	if (slot < 0)
		return NULL;

	ti = vtep_ensure_locked(dip_hbo);
	if (ti < 0)
		return NULL;

	bcm_vxlan_port_t_init(&vp);
	vp.flags = BCM_VXLAN_PORT_NETWORK | BCM_VXLAN_PORT_EGRESS_TUNNEL |
		   BCM_VXLAN_PORT_SERVICE_TAGGED;
	vp.criteria = BCM_VXLAN_PORT_MATCH_VN_ID;
	vp.match_tunnel_id = vteps[ti].term_id;
	vp.egress_tunnel_id = vteps[ti].init_id;
	vp.egress_if = vteps[ti].ul_egr;

	rv = bcm_vxlan_port_add(vxlan_unit, ve->vpn, &vp);
	if (rv != BCM_E_NONE)
		return NULL;

	ve->peers[slot].used = 1;
	ve->peers[slot].dip = dip_hbo;
	ve->peers[slot].vp_id = vp.vxlan_port_id;
	ve->peers[slot].in_bum = 0;
	return &ve->peers[slot];
}

static void handle_evpn_fdb(int is_add, int vni, const unsigned char mac[6],
			    uint32_t dip_hbo)
{
	static const unsigned char zero_mac[6] = { 0 };
	int vi = -1, i;
	struct vni_vtep *peer;

	pthread_mutex_lock(&vxlan_lock);
	for (i = 0; i < MAX_VNIS; i++) {
		if (vnis[i].used && vnis[i].vni == vni) {
			vi = i;
			break;
		}
	}
	if (vi < 0) {
		pthread_mutex_unlock(&vxlan_lock);
		return;
	}

	if (memcmp(mac, zero_mac, 6) == 0) {
		/* EVPN Type-3 IMET: remote VTEP BUM replication list. */
		if (is_add) {
			peer = vni_peer_ensure_locked(vi, dip_hbo);
			if (peer != NULL && !peer->in_bum && vnis[vi].mcast != 0) {
				bcm_if_t encap = 0;
				if (bcm_multicast_vxlan_encap_get(vxlan_unit, vnis[vi].mcast,
								  peer->vp_id, peer->vp_id,
								  &encap) == BCM_E_NONE) {
					if (bcm_multicast_egress_add(vxlan_unit, vnis[vi].mcast,
								     peer->vp_id, encap) == BCM_E_NONE)
						peer->in_bum = 1;
				}
			}
		} else {
			for (i = 0; i < MAX_VNI_VTEPS; i++) {
				if (vnis[vi].peers[i].used &&
				    vnis[vi].peers[i].dip == dip_hbo &&
				    vnis[vi].peers[i].in_bum) {
					bcm_if_t encap = 0;
					if (bcm_multicast_vxlan_encap_get(vxlan_unit, vnis[vi].mcast,
									  vnis[vi].peers[i].vp_id,
									  vnis[vi].peers[i].vp_id,
									  &encap) == BCM_E_NONE) {
						(void)bcm_multicast_egress_delete(vxlan_unit,
										  vnis[vi].mcast,
										  vnis[vi].peers[i].vp_id,
										  encap);
					}
					vnis[vi].peers[i].in_bum = 0;
				}
			}
		}
		pthread_mutex_unlock(&vxlan_lock);
		return;
	}

	/* EVPN Type-2 MAC entry pointing to remote VTEP. */
	if (is_add) {
		bcm_l2_addr_t l2;
		peer = vni_peer_ensure_locked(vi, dip_hbo);
		if (peer != NULL) {
			bcm_l2_addr_t_init(&l2, (uint8 *)mac, vnis[vi].vpn);
			l2.port = peer->vp_id;
			l2.flags = BCM_L2_STATIC;
			if (bcm_l2_addr_add(vxlan_unit, &l2) == BCM_E_NONE)
				vnis[vi].macs_synced++;
		}
	} else {
		(void)bcm_l2_addr_delete(vxlan_unit, (uint8 *)mac, vnis[vi].vpn);
	}
	pthread_mutex_unlock(&vxlan_lock);
}

static void parse_nl_neigh(const struct nlmsghdr *nlh)
{
	const struct ndmsg *ndm;
	const struct rtattr *rta;
	int rlen, vni;
	char ifname[IF_NAMESIZE];
	const unsigned char *mac = NULL;
	uint32_t dip_hbo = 0;

	if (nlh->nlmsg_type != RTM_NEWNEIGH && nlh->nlmsg_type != RTM_DELNEIGH)
		return;
	ndm = (const struct ndmsg *)NLMSG_DATA(nlh);
	if (ndm->ndm_family != AF_BRIDGE)
		return;
	if (if_indextoname((unsigned)ndm->ndm_ifindex, ifname) == NULL)
		return;
	if (strncmp(ifname, "vxlan", 5) != 0)
		return;
	vni = atoi(ifname + 5);
	if (vni <= 0)
		return;

	rlen = (int)nlh->nlmsg_len - (int)NLMSG_LENGTH(sizeof(*ndm));
	for (rta = (const struct rtattr *)((const char *)ndm + NLMSG_ALIGN(sizeof(*ndm)));
	     RTA_OK(rta, rlen); rta = RTA_NEXT(rta, rlen)) {
		if (rta->rta_type == NDA_LLADDR && RTA_PAYLOAD(rta) == 6) {
			mac = (const unsigned char *)RTA_DATA(rta);
		} else if (rta->rta_type == NDA_DST && RTA_PAYLOAD(rta) == 4) {
			uint32_t net_ip;
			memcpy(&net_ip, RTA_DATA(rta), 4);
			dip_hbo = ntohl(net_ip);
		}
	}
	if (mac != NULL && dip_hbo != 0)
		handle_evpn_fdb(nlh->nlmsg_type == RTM_NEWNEIGH, vni, mac, dip_hbo);
}

static void request_fdb_dump(int fd)
{
	struct {
		struct nlmsghdr nlh;
		struct ndmsg    ndm;
	} req;

	memset(&req, 0, sizeof(req));
	req.nlh.nlmsg_len = sizeof(req);
	req.nlh.nlmsg_type = RTM_GETNEIGH;
	req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	req.ndm.ndm_family = AF_BRIDGE;
	(void)send(fd, &req, sizeof(req), 0);
}

static void *vxlan_nl_thread(void *arg)
{
	struct sockaddr_nl sa;
	unsigned char buf[16384];
	int fd, ticks = 0;

	(void)arg;
	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd < 0)
		return NULL;
	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	sa.nl_groups = RTMGRP_NEIGH;
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		close(fd);
		return NULL;
	}

	request_fdb_dump(fd);
	for (;;) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int pr = poll(&pfd, 1, 5000);
		if (pr > 0 && (pfd.revents & POLLIN)) {
			ssize_t n = recv(fd, buf, sizeof(buf), 0);
			if (n > 0) {
				const struct nlmsghdr *nlh = (const struct nlmsghdr *)buf;
				int rem = (int)n;
				for (; NLMSG_OK(nlh, rem); nlh = NLMSG_NEXT(nlh, rem))
					parse_nl_neigh(nlh);
			}
		} else {
			ticks++;
			if (ticks >= 6) { /* periodic reconcile every ~30s */
				request_fdb_dump(fd);
				ticks = 0;
			}
		}
	}
	close(fd);
	return NULL;
}

int nosaic_vxlan_start(int unit)
{
	pthread_t th;
	int rv;

	vxlan_unit = unit;
	rv = bcm_vxlan_init(unit);
	if (rv == BCM_E_UNAVAIL) {
		vxlan_ok = 0;
		return 0;
	}
	if (rv != BCM_E_NONE) {
		fprintf(stderr, "vxlan: bcm_vxlan_init: %s (disabled)\n", bcm_errmsg(rv));
		vxlan_ok = 0;
		return -1;
	}
	(void)bcm_switch_control_set(unit, bcmSwitchL3EgressMode, 1);
	(void)bcm_switch_control_set(unit, bcmSwitchVxlanUdpDestPortSet, VXLAN_UDP_PORT);
	(void)bcm_switch_control_set(unit, bcmSwitchVxlanTunnelMissToCpu, 1);
	vxlan_ok = 1;
	if (pthread_create(&th, NULL, vxlan_nl_thread, NULL) == 0)
		pthread_detach(th);
	printf("vxlan: hardware VXLAN + EVPN netlink sync enabled (UDP port %d)\n",
	       VXLAN_UDP_PORT);
	fflush(stdout);
	return 0;
}

int nosaic_vxlan_set_vtep(const char *ip, char *err, size_t errlen)
{
	struct in_addr a;

	if (!vxlan_ok) {
		snprintf(err, errlen, "VXLAN is not supported on this ASIC");
		return -2;
	}
	if (ip == NULL || inet_pton(AF_INET, ip, &a) != 1) {
		snprintf(err, errlen, "invalid VTEP IPv4 address \"%s\"", ip ? ip : "");
		return -1;
	}
	pthread_mutex_lock(&vxlan_lock);
	snprintf(vtep_ip, sizeof(vtep_ip), "%s", ip);
	local_vtep_hbo = ntohl(a.s_addr);
	pthread_mutex_unlock(&vxlan_lock);
	printf("vxlan: local VTEP set to %s\n", ip);
	fflush(stdout);
	return 0;
}

/* Query Linux netlink (RTM_GETLINK) for a VRF interface's routing table ID. */
static uint32_t vrf_table_id(const char *vrf_name)
{
	struct {
		struct nlmsghdr  nlh;
		struct ifinfomsg ifi;
	} req;
	unsigned char buf[8192];
	int fd, ifx;
	ssize_t n;

	if (vrf_name == NULL || vrf_name[0] == '\0')
		return 0;
	ifx = (int)if_nametoindex(vrf_name);
	if (ifx <= 0)
		return 0;

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd < 0)
		return 0;
	memset(&req, 0, sizeof(req));
	req.nlh.nlmsg_len = sizeof(req);
	req.nlh.nlmsg_type = RTM_GETLINK;
	req.nlh.nlmsg_flags = NLM_F_REQUEST;
	req.ifi.ifi_family = AF_UNSPEC;
	req.ifi.ifi_index = ifx;
	if (send(fd, &req, sizeof(req), 0) < 0) {
		close(fd);
		return 0;
	}
	n = recv(fd, buf, sizeof(buf), 0);
	close(fd);
	if (n <= 0)
		return 0;

	{
		const struct nlmsghdr *nlh = (const struct nlmsghdr *)buf;
		int rem = (int)n;
		for (; NLMSG_OK(nlh, rem); nlh = NLMSG_NEXT(nlh, rem)) {
			const struct ifinfomsg *ifi;
			const struct rtattr *rta;
			int rlen;

			if (nlh->nlmsg_type != RTM_NEWLINK)
				continue;
			ifi = (const struct ifinfomsg *)NLMSG_DATA(nlh);
			rta = (const struct rtattr *)((const char *)ifi + NLMSG_ALIGN(sizeof(*ifi)));
			rlen = (int)nlh->nlmsg_len - (int)NLMSG_LENGTH(sizeof(*ifi));
			for (; RTA_OK(rta, rlen); rta = RTA_NEXT(rta, rlen)) {
				if (rta->rta_type != IFLA_LINKINFO)
					continue;
				const struct rtattr *li = (const struct rtattr *)RTA_DATA(rta);
				int lilen = (int)RTA_PAYLOAD(rta);
				for (; RTA_OK(li, lilen); li = RTA_NEXT(li, lilen)) {
					if (li->rta_type != IFLA_INFO_DATA)
						continue;
					const struct rtattr *da = (const struct rtattr *)RTA_DATA(li);
					int dalen = (int)RTA_PAYLOAD(li);
					for (; RTA_OK(da, dalen); da = RTA_NEXT(da, dalen)) {
						if (da->rta_type == IFLA_VRF_TABLE && RTA_PAYLOAD(da) == 4) {
							uint32_t tbl = 0;
							memcpy(&tbl, RTA_DATA(da), 4);
							return tbl;
						}
					}
				}
			}
		}
	}
	return 0;
}

/* Bind all front-panel ports currently in ve->vid as local VXLAN access ports
 * on ve->vpn and add them to ve->mcast so remote BUM traffic floods locally.
 * Caller holds vxlan_lock. */
static void vni_bind_access_ports_locked(int vi)
{
	struct vni_entry *ve = &vnis[vi];
	bcm_pbmp_t pbmp, ubmp;
	bcm_port_t p;

	if (!ve->used || ve->vid <= 0)
		return;
	BCM_PBMP_CLEAR(pbmp);
	BCM_PBMP_CLEAR(ubmp);
	if (bcm_vlan_port_get(vxlan_unit, (bcm_vlan_t)ve->vid, &pbmp, &ubmp) != BCM_E_NONE)
		return;

	BCM_PBMP_ITER(pbmp, p) {
		bcm_gport_t gp;
		bcm_vxlan_port_t vp;
		bcm_if_t encap = 0;
		int i, exists = 0, slot = -1;

		if (p == 0) /* skip CPU port */
			continue;
		for (i = 0; i < MAX_ACCESS_PORTS; i++) {
			if (ve->access[i].used && ve->access[i].port == p) {
				exists = 1;
				break;
			}
			if (!ve->access[i].used && slot < 0)
				slot = i;
		}
		if (exists || slot < 0)
			continue;
		if (bcm_port_gport_get(vxlan_unit, p, &gp) != BCM_E_NONE)
			continue;
		bcm_vxlan_port_t_init(&vp);
		vp.flags = BCM_VXLAN_PORT_SERVICE_TAGGED;
		vp.criteria = BCM_VXLAN_PORT_MATCH_PORT_VLAN;
		vp.match_port = gp;
		vp.match_vlan = (bcm_vlan_t)ve->vid;
		vp.egress_service_tpid = 0x8100;
		vp.egress_service_vlan = (bcm_vlan_t)ve->vid;
		if (bcm_vxlan_port_add(vxlan_unit, ve->vpn, &vp) != BCM_E_NONE)
			continue;
		ve->access[slot].used = 1;
		ve->access[slot].port = p;
		ve->access[slot].vp_id = vp.vxlan_port_id;
		if (ve->mcast != 0 &&
		    bcm_multicast_vxlan_encap_get(vxlan_unit, ve->mcast, gp,
						  vp.vxlan_port_id, &encap) == BCM_E_NONE) {
			(void)bcm_multicast_egress_add(vxlan_unit, ve->mcast, gp, encap);
		}
	}
}

void nosaic_vxlan_sync_vlan(int vid)
{
	int i;

	if (!vxlan_ok || vid <= 0)
		return;
	pthread_mutex_lock(&vxlan_lock);
	for (i = 0; i < MAX_VNIS; i++) {
		if (vnis[i].used && vnis[i].vid == vid)
			vni_bind_access_ports_locked(i);
	}
	pthread_mutex_unlock(&vxlan_lock);
}

int nosaic_vxlan_vrf_for_table(uint32_t table_id, bcm_vrf_t *vrf_out)
{
	int i;

	if (!vxlan_ok || table_id == 0)
		return -1;
	pthread_mutex_lock(&vxlan_lock);
	for (i = 0; i < MAX_VNIS; i++) {
		if (!vnis[i].used || !vnis[i].l3)
			continue;
		if (vnis[i].table_id == 0 && vnis[i].vrf[0] != '\0')
			vnis[i].table_id = vrf_table_id(vnis[i].vrf);
		if (vnis[i].table_id == table_id && vnis[i].vrf_id > 0) {
			if (vrf_out != NULL)
				*vrf_out = vnis[i].vrf_id;
			pthread_mutex_unlock(&vxlan_lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&vxlan_lock);
	return -1;
}

int nosaic_vxlan_l3_egress(bcm_vrf_t vrf, uint32_t gw_hbo, bcm_if_t *egr_out)
{
	int ti;
	bcm_l3_egress_t egr;

	if (!vxlan_ok || vrf <= 0 || gw_hbo == 0 || egr_out == NULL)
		return -1;
	pthread_mutex_lock(&vxlan_lock);
	ti = vtep_ensure_locked(gw_hbo);
	if (ti < 0) {
		pthread_mutex_unlock(&vxlan_lock);
		return -1;
	}
	if (vteps[ti].l3_egr == 0) {
		bcm_l3_egress_t_init(&egr);
		egr.intf = vteps[ti].tnl_intf;
		egr.port = vteps[ti].init_id;
		egr.flags = BCM_L3_VXLAN_ONLY;
		if (bcm_l3_egress_create(vxlan_unit, 0, &egr, &vteps[ti].l3_egr) != BCM_E_NONE) {
			/* Fallback to underlay egress if chip does not support BCM_L3_VXLAN_ONLY. */
			vteps[ti].l3_egr = vteps[ti].ul_egr;
		}
	}
	*egr_out = vteps[ti].l3_egr;
	pthread_mutex_unlock(&vxlan_lock);
	return 0;
}

int nosaic_vxlan_add_vni(int vni, int vid, const char *vrf, int l3,
			 char *err, size_t errlen)
{
	bcm_vxlan_vpn_config_t cfg;
	bcm_multicast_t mcast = 0;
	int i, slot = -1, rv;

	if (!vxlan_ok) {
		snprintf(err, errlen, "VXLAN is not supported on this ASIC");
		return -2;
	}
	if (vni < 1 || vni > 16777215) {
		snprintf(err, errlen, "VNI %d out of range (1..16777215)", vni);
		return -1;
	}
	if (vid < 1 || vid > 4094) {
		snprintf(err, errlen, "VLAN %d out of range (1..4094)", vid);
		return -1;
	}

	pthread_mutex_lock(&vxlan_lock);
	for (i = 0; i < MAX_VNIS; i++) {
		if (vnis[i].used && vnis[i].vni == vni) {
			if (vnis[i].vid == vid && vnis[i].l3 == (l3 ? 1 : 0) &&
			    strcmp(vnis[i].vrf, vrf ? vrf : "") == 0) {
				pthread_mutex_unlock(&vxlan_lock);
				return 0;
			}
			pthread_mutex_unlock(&vxlan_lock);
			snprintf(err, errlen, "VNI %d already exists on VLAN %d", vni, vnis[i].vid);
			return -1;
		}
		if (!vnis[i].used && slot < 0)
			slot = i;
	}
	if (slot < 0) {
		pthread_mutex_unlock(&vxlan_lock);
		snprintf(err, errlen, "all %d VNI slots are in use", MAX_VNIS);
		return -1;
	}

	rv = bcm_multicast_create(vxlan_unit, BCM_MULTICAST_TYPE_VXLAN, &mcast);
	if (rv != BCM_E_NONE) {
		pthread_mutex_unlock(&vxlan_lock);
		snprintf(err, errlen, "bcm_multicast_create(vni %d): %s", vni, bcm_errmsg(rv));
		return -1;
	}

	bcm_vxlan_vpn_config_t_init(&cfg);
	cfg.vnid = (uint32)vni;
	cfg.flags = BCM_VXLAN_VPN_ELAN | BCM_VXLAN_VPN_WITH_VPNID;
	cfg.broadcast_group = mcast;
	cfg.unknown_unicast_group = mcast;
	cfg.unknown_multicast_group = mcast;

	rv = bcm_vxlan_vpn_create(vxlan_unit, &cfg);
	if (rv != BCM_E_NONE) {
		(void)bcm_multicast_destroy(vxlan_unit, mcast);
		pthread_mutex_unlock(&vxlan_lock);
		snprintf(err, errlen, "bcm_vxlan_vpn_create(vni %d): %s", vni, bcm_errmsg(rv));
		return -1;
	}

	memset(&vnis[slot], 0, sizeof(vnis[slot]));
	vnis[slot].used = 1;
	vnis[slot].vni = vni;
	vnis[slot].vid = vid;
	vnis[slot].l3 = l3 ? 1 : 0;
	if (vrf != NULL)
		snprintf(vnis[slot].vrf, sizeof(vnis[slot].vrf), "%s", vrf);
	if (l3) {
		vnis[slot].vrf_id = (bcm_vrf_t)(slot + 1);
		vnis[slot].table_id = vrf_table_id(vrf);
	}
	vnis[slot].vpn = cfg.vpn;
	vnis[slot].mcast = mcast;
	vni_bind_access_ports_locked(slot);
	pthread_mutex_unlock(&vxlan_lock);

	printf("vxlan: added %s VNI %d (vlan %d%s%s, vpn 0x%x)\n",
	       l3 ? "L3" : "L2", vni, vid,
	       (vrf && vrf[0]) ? ", vrf " : "", (vrf && vrf[0]) ? vrf : "",
	       (unsigned)cfg.vpn);
	fflush(stdout);
	return 0;
}

int nosaic_vxlan_del_vni(int vni, char *err, size_t errlen)
{
	int i, j;

	if (!vxlan_ok) {
		snprintf(err, errlen, "VXLAN is not supported on this ASIC");
		return -2;
	}
	pthread_mutex_lock(&vxlan_lock);
	for (i = 0; i < MAX_VNIS; i++) {
		if (vnis[i].used && vnis[i].vni == vni) {
			for (j = 0; j < MAX_VNI_VTEPS; j++) {
				if (vnis[i].peers[j].used) {
					(void)bcm_vxlan_port_delete(vxlan_unit, vnis[i].vpn,
								    vnis[i].peers[j].vp_id);
				}
			}
			(void)bcm_vxlan_vpn_destroy(vxlan_unit, vnis[i].vpn);
			if (vnis[i].mcast != 0)
				(void)bcm_multicast_destroy(vxlan_unit, vnis[i].mcast);
			vnis[i].used = 0;
			pthread_mutex_unlock(&vxlan_lock);
			printf("vxlan: removed VNI %d\n", vni);
			fflush(stdout);
			return 0;
		}
	}
	pthread_mutex_unlock(&vxlan_lock);
	snprintf(err, errlen, "VNI %d not found", vni);
	return -1;
}

void nosaic_vxlan_query(FILE *out)
{
	int i, j, emitted = 0;

	pthread_mutex_lock(&vxlan_lock);
	fprintf(out, "{\"ok\":true,\"result\":{\"vtep\":\"%s\",\"vnis\":[", vtep_ip);
	for (i = 0; i < MAX_VNIS; i++) {
		int npeers = 0;

		if (!vnis[i].used)
			continue;
		for (j = 0; j < MAX_VNI_VTEPS; j++)
			if (vnis[i].peers[j].used)
				npeers++;
		fprintf(out,
			"%s{\"vni\":%d,\"vid\":%d,\"vrf\":\"%s\",\"l3\":%s,"
			"\"peers\":%d,\"macs\":%lu}",
			emitted++ ? "," : "",
			vnis[i].vni, vnis[i].vid, vnis[i].vrf,
			vnis[i].l3 ? "true" : "false",
			npeers, vnis[i].macs_synced);
	}
	fprintf(out, "]}}\n");
	pthread_mutex_unlock(&vxlan_lock);
}
