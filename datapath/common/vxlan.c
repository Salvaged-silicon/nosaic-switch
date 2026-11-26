/*
 * Broadcom hardware VXLAN / EVPN datapath (L2 + L3 VNIs).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sal/types.h>
#include <bcm/error.h>
#include <bcm/multicast.h>
#include <bcm/switch.h>
#include <bcm/vxlan.h>

#include "vxlan.h"

#define MAX_VNIS      256
#define VXLAN_UDP_PORT 4789

struct vni_entry {
	int             used;
	int             vni;
	int             vid;
	int             l3;
	char            vrf[32];
	bcm_vpn_t       vpn;
	bcm_multicast_t mcast;
};

static int vxlan_unit = -1;
static int vxlan_ok = 0;
static pthread_mutex_t vxlan_lock = PTHREAD_MUTEX_INITIALIZER;
static char vtep_ip[32];
static struct vni_entry vnis[MAX_VNIS];

int nosaic_vxlan_supported(void)
{
	return vxlan_ok;
}

int nosaic_vxlan_start(int unit)
{
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
	printf("vxlan: hardware VXLAN enabled (UDP port %d)\n", VXLAN_UDP_PORT);
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
	pthread_mutex_unlock(&vxlan_lock);
	printf("vxlan: local VTEP set to %s\n", ip);
	fflush(stdout);
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
			/* Idempotent update if identical. */
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

	bcm_vxlan_vpn_config_t_init(&cfg);
	cfg.vnid = (uint32)vni;
	cfg.flags = BCM_VXLAN_VPN_ELAN | BCM_VXLAN_VPN_WITH_VPNID;
	if (!l3) {
		rv = bcm_multicast_create(vxlan_unit, BCM_MULTICAST_TYPE_VXLAN, &mcast);
		if (rv != BCM_E_NONE) {
			pthread_mutex_unlock(&vxlan_lock);
			snprintf(err, errlen, "bcm_multicast_create(vni %d): %s", vni, bcm_errmsg(rv));
			return -1;
		}
		cfg.broadcast_group = mcast;
		cfg.unknown_unicast_group = mcast;
		cfg.unknown_multicast_group = mcast;
	} else {
		/* L3 VNI uses a dedicated transit VPN; BUM flooding is disabled
		 * (unicast routed L3 VXLAN encap/decap). */
		rv = bcm_multicast_create(vxlan_unit, BCM_MULTICAST_TYPE_VXLAN, &mcast);
		if (rv != BCM_E_NONE) {
			pthread_mutex_unlock(&vxlan_lock);
			snprintf(err, errlen, "bcm_multicast_create(l3 vni %d): %s", vni, bcm_errmsg(rv));
			return -1;
		}
		cfg.broadcast_group = mcast;
		cfg.unknown_unicast_group = mcast;
		cfg.unknown_multicast_group = mcast;
	}

	rv = bcm_vxlan_vpn_create(vxlan_unit, &cfg);
	if (rv != BCM_E_NONE) {
		if (mcast != 0)
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
	vnis[slot].vpn = cfg.vpn;
	vnis[slot].mcast = mcast;
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
	int i;

	if (!vxlan_ok) {
		snprintf(err, errlen, "VXLAN is not supported on this ASIC");
		return -2;
	}
	pthread_mutex_lock(&vxlan_lock);
	for (i = 0; i < MAX_VNIS; i++) {
		if (vnis[i].used && vnis[i].vni == vni) {
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
	int i, emitted = 0;

	pthread_mutex_lock(&vxlan_lock);
	fprintf(out, "{\"ok\":true,\"result\":{\"vtep\":\"%s\",\"vnis\":[", vtep_ip);
	for (i = 0; i < MAX_VNIS; i++) {
		if (!vnis[i].used)
			continue;
		fprintf(out, "%s{\"vni\":%d,\"vid\":%d,\"vrf\":\"%s\",\"l3\":%s}",
			emitted++ ? "," : "",
			vnis[i].vni, vnis[i].vid, vnis[i].vrf,
			vnis[i].l3 ? "true" : "false");
	}
	fprintf(out, "]}}\n");
	pthread_mutex_unlock(&vxlan_lock);
}
