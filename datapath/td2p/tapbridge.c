/*
 * TAP bridge: put hardware switch ports on the Linux network stack.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * This is what lets an ordinary routing daemon run over ports the SDK owns.
 * Two directions, both plain:
 *
 *   wire -> Linux   a bcm_rx callback writes each received frame to a tap
 *   Linux -> wire   a poll loop reads the taps and calls bcm_tx
 *
 * The design and every caveat below are EdgeNOS's, which ran this on this
 * board; the code is NOSaic's own.
 *
 * WHY MORE THAN ONE PORT MATTERS. With a single routed port the chip cannot be
 * shown to forward anything: the only test path is out the interface the
 * packet arrived on, and Trident2+ drops that by design. Two ports remove the
 * ambiguity -- in one, out the other, with the CPU counter flat.
 *
 * EACH PORT GETS ITS OWN MAC. One router MAC across all routed ports is what
 * the vendor OS does and the chip is happy with it, but it puts two Linux
 * interfaces with the same hardware address on different subnets, and then the
 * kernel's ARP behaviour depends on arp_ignore and arp_announce. Distinct MACs
 * cost one MY_STATION entry each and avoid the question entirely.
 *
 * RUNTS ARE PADDED. bcm_tx refuses frames shorter than 60 bytes -- a 46-byte
 * ARP is rejected as a "tagged runt packet without higig header" -- and the
 * Linux stack hands us short frames as a matter of course. They are zero-padded
 * rather than dropped, because a dropped ARP is a link that looks up and
 * resolves nothing.
 *
 * THE TRANSMIT BUFFER MUST COME FROM THE SDK. bcm_tx hands the address
 * straight to the DMA engine, which resolves it against the reserved pool the
 * kernel command line carves out -- an ordinary stack or malloc buffer has no
 * mapping there, so the engine reads whatever happens to sit at the physical
 * address it computes. The frame still goes out with our exact timing, which
 * is what makes this so hard to see: the neighbour counted six frames for six
 * pings and every one arrived as all-zero MACs and "802.3, length 0". Allocate
 * with bcm_pkt_alloc and copy into pkt_data[0].data.
 *
 * ALLOCATE THAT BUFFER ONCE. The BDE's salloc is a bump allocator with no
 * free, so bcm_pkt_free returns nothing to it. EdgeNOS allocated per frame and
 * after ~2400 transmits the 64 MB pool was exhausted, transmit stopped, and an
 * OSPF adjacency fell back to Init -- the far end stopped hearing our Hellos
 * while we still heard its. bcm_tx is synchronous here (no async flag, NULL
 * cookie) and the pump loop is single threaded, so one buffer is safe to reuse
 * across every port.
 *
 * THE FRAME HANDED TO bcm_tx MUST CARRY A VLAN TAG. The transmit path assumes
 * a tag at offset 12, and because tx_upbmp marks the egress port untagged it
 * strips four bytes there on the way out. Give it the untagged frame Linux
 * produced and it deletes the ethertype and two payload bytes instead of a
 * tag: everything past the MAC header shifts left by four. So the tag goes on
 * here and the chip takes it back off, and the wire stays untagged. Frames
 * punted to us arrive tagged for the same reason and are stripped on receive.
 *
 * LINKSCAN MUST BE RUNNING. The transmit path ANDs its port bitmap with the
 * link bitmap that only linkscan maintains, and returns success having built
 * no descriptor when that is empty -- so every transmit silently vanishes.
 * nosaic_sdk_ports() starts it.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <sys/socket.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <sal/types.h>
#include <bcm/error.h>
#include <bcm/pkt.h>
#include <bcm/rx.h>
#include <bcm/tx.h>
#include <bcm/port.h>
#include <bcm/vlan.h>

#include "tapbridge.h"

#define TAP_MTU        9216
#define MIN_FRAME      60
#define MAX_TAPS       8
#define RX_PRIORITY    100

struct tap {
	char          name[IFNAMSIZ];
	int           fd;
	bcm_port_t    port;
	int           vlan;
	int           mtu;
	unsigned char mac[6];
};

static struct tap taps[MAX_TAPS];
static int ntaps;
static int tap_unit;
static bcm_pkt_t *tx_pkt;   /* allocated once; see the header comment */

/*
 * wire -> Linux.
 *
 * Called from the SDK's receive thread for every frame the chip punts to the
 * CPU. The source port decides which tap it belongs to; a frame from a port
 * with no tap is not ours and is left for whatever else may want it.
 */
static bcm_rx_t tap_rx(int unit, bcm_pkt_t *pkt, void *cookie)
{
	int i;

	for (i = 0; i < ntaps; i++) {
		unsigned char  flat[TAP_MTU];
		unsigned char *d;
		int            len;

		if (taps[i].port != pkt->src_port)
			continue;

		len = pkt->tot_len ? (int)pkt->tot_len : (int)pkt->pkt_data[0].len;
		if (len > (int)pkt->pkt_data[0].len)
			len = (int)pkt->pkt_data[0].len;   /* never read past the block */
		if (len <= 0)
			return BCM_RX_NOT_HANDLED;

		/* Punted frames arrive tagged; Linux wants what was on the wire. */
		d = pkt->pkt_data[0].data;
		if (len > 16 && d[12] == 0x81 && d[13] == 0x00 &&
		    len - 4 <= (int)sizeof(flat)) {
			memcpy(flat, d, 12);
			memcpy(flat + 12, d + 16, (size_t)(len - 16));
			len -= 4;
			d = flat;
		}
		if (write(taps[i].fd, d, (size_t)len) != len)
			return BCM_RX_NOT_HANDLED;
		return BCM_RX_HANDLED;
	}
	return BCM_RX_NOT_HANDLED;
}

/* Linux -> wire. */
static int tap_tx(struct tap *t, const unsigned char *buf, int len)
{
	unsigned char *frame;

	if (tx_pkt == NULL || len < 12 || len + 4 > TAP_MTU)
		return -1;

	frame = tx_pkt->pkt_data[0].data;
	memcpy(frame, buf, 12);                     /* destination + source MAC */
	if (buf[12] == 0x81 && buf[13] == 0x00) {
		memcpy(frame + 12, buf + 12, (size_t)(len - 12));
	} else {
		frame[12] = 0x81;                   /* TPID                     */
		frame[13] = 0x00;
		frame[14] = (unsigned char)((t->vlan >> 8) & 0x0f);
		frame[15] = (unsigned char)(t->vlan & 0xff);   /* prio 0, VID    */
		memcpy(frame + 16, buf + 12, (size_t)(len - 12));
		len += 4;
	}
	if (len < MIN_FRAME) {
		memset(frame + len, 0, (size_t)(MIN_FRAME - len));
		len = MIN_FRAME;
	}

	tx_pkt->pkt_data[0].len = (uint32)len;
	tx_pkt->pkt_len         = (uint32)len;
	tx_pkt->tot_len         = (uint32)len;
	tx_pkt->flags          |= BCM_TX_CRC_APPEND;
	BCM_PBMP_CLEAR(tx_pkt->tx_pbmp);
	BCM_PBMP_PORT_ADD(tx_pkt->tx_pbmp, t->port);
	BCM_PBMP_CLEAR(tx_pkt->tx_upbmp);
	BCM_PBMP_PORT_ADD(tx_pkt->tx_upbmp, t->port);  /* leave the wire untagged */

	if (bcm_tx(tap_unit, tx_pkt, NULL) != BCM_E_NONE)
		return -1;
	return 0;
}

/* Create one tap device, up, with its own MAC. */
static int tap_open(struct tap *t, const char *name, bcm_port_t port, int index,
		    int mtu)
{
	struct ifreq ifr;
	int fd, sock;

	fd = open("/dev/net/tun", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "tap: /dev/net/tun: %s (is CONFIG_TUN enabled?)\n",
			strerror(errno));
		return -1;
	}
	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
	if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
		fprintf(stderr, "tap: TUNSETIFF %s: %s\n", name, strerror(errno));
		close(fd);
		return -1;
	}

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock >= 0) {
		/* A distinct locally-administered MAC per port. */
		memset(&ifr, 0, sizeof(ifr));
		snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
		ifr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
		ifr.ifr_hwaddr.sa_data[0] = 0x02;
		ifr.ifr_hwaddr.sa_data[5] = (char)(0x50 + index);
		ioctl(sock, SIOCSIFHWADDR, &ifr);
		memcpy(t->mac, ifr.ifr_hwaddr.sa_data, 6);

		/* It has to match the neighbour. OSPF carries the MTU in its
		 * database description packets and refuses the adjacency when the
		 * two disagree -- it sits in ExStart, having already exchanged
		 * Hellos, and says nothing about why. A 1500 default against a
		 * neighbour at 1600 is exactly that. */
		memset(&ifr, 0, sizeof(ifr));
		snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
		ifr.ifr_mtu = mtu > 0 ? mtu : 1500;
		ioctl(sock, SIOCSIFMTU, &ifr);
		t->mtu = ifr.ifr_mtu;

		memset(&ifr, 0, sizeof(ifr));
		snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
		if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
			ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
			ioctl(sock, SIOCSIFFLAGS, &ifr);
		}
		close(sock);
	}

	snprintf(t->name, sizeof(t->name), "%s", name);
	t->fd = fd;
	t->port = port;
	return 0;
}

/*
 * Let the chip carry what the interface will send.
 *
 * The port's maximum frame is a separate setting from the interface MTU, and
 * the two failing to agree does not look like a size problem: small packets
 * pass, an adjacency forms, and then anything large disappears. Headroom is
 * the Ethernet header, the FCS, and the VLAN tag this bridge adds.
 */
static int tap_frame_max(int unit, bcm_port_t port, int mtu)
{
	int rv;

	if (mtu <= 0)
		return 0;

	rv = bcm_port_frame_max_set(unit, port, mtu + 14 + 4 + 4);
	if (rv != BCM_E_NONE && rv != BCM_E_UNAVAIL) {
		fprintf(stderr, "tap: bcm_port_frame_max_set %d: %d\n", mtu, rv);
		return -1;
	}
	return 0;
}

/*
 * Give a routed port its own VLAN, egressing untagged.
 *
 * Without this the chip tags what it sends and the neighbour drops it. The
 * failure is quiet and looks like nothing arriving at all: the frames reach
 * the far end and are counted there as received AND dropped, so a switch that
 * looks up on both sides moves nothing. That is exactly what happened -- six
 * pings out, six received by the neighbour, six dropped.
 *
 * A VLAN per port rather than one shared: these are routed interfaces, not
 * bridge members, and putting two of them in one broadcast domain would let
 * traffic between them bypass the routing this exists to make possible.
 */
static int tap_vlan_setup(int unit, struct tap *t, int vid)
{
	bcm_port_config_t cfg;
	bcm_pbmp_t pbm, upbm;
	int rv;

	if (vid <= 0)
		return 0;

	rv = bcm_port_config_get(unit, &cfg);
	if (rv != BCM_E_NONE) {
		fprintf(stderr, "tap: bcm_port_config_get: %d\n", rv);
		return -1;
	}

	rv = bcm_vlan_create(unit, (bcm_vlan_t)vid);
	if (rv != BCM_E_NONE && rv != BCM_E_EXISTS) {
		fprintf(stderr, "tap: bcm_vlan_create %d: %d\n", vid, rv);
		return -1;
	}

	/* The port and the CPU: the CPU has to be a member or frames punted to us
	 * and frames we inject both fall outside the VLAN. */
	BCM_PBMP_CLEAR(pbm);
	BCM_PBMP_CLEAR(upbm);
	BCM_PBMP_PORT_ADD(pbm, t->port);
	BCM_PBMP_OR(pbm, cfg.cpu);
	BCM_PBMP_PORT_ADD(upbm, t->port);
	rv = bcm_vlan_port_add(unit, (bcm_vlan_t)vid, pbm, upbm);
	if (rv != BCM_E_NONE) {
		fprintf(stderr, "tap: bcm_vlan_port_add %d: %d\n", vid, rv);
		return -1;
	}

	/* What an untagged frame arriving on this port is taken to belong to. */
	rv = bcm_port_untagged_vlan_set(unit, t->port, (bcm_vlan_t)vid);
	if (rv != BCM_E_NONE) {
		fprintf(stderr, "tap: bcm_port_untagged_vlan_set %d: %d\n", vid, rv);
		return -1;
	}
	printf("tap: %s port %d in vlan %d, untagged\n", t->name, t->port, vid);
	return 0;
}

int nosaic_tap_start(int unit, const struct tap_spec *specs, int n)
{
	int i, rv;

	if (n > MAX_TAPS)
		n = MAX_TAPS;
	tap_unit = unit;

	rv = bcm_pkt_alloc(unit, TAP_MTU + 8, BCM_TX_CRC_APPEND, &tx_pkt);
	if (rv != BCM_E_NONE || tx_pkt == NULL) {
		fprintf(stderr, "tap: bcm_pkt_alloc: %d\n", rv);
		return -1;
	}

	for (i = 0; i < n; i++) {
		if (tap_open(&taps[ntaps], specs[i].name, specs[i].port, ntaps,
			     specs[i].mtu) != 0)
			return -1;
		if (tap_frame_max(unit, specs[i].port, specs[i].mtu) != 0)
			return -1;
		taps[ntaps].vlan = specs[i].vlan;
		taps[ntaps].mtu = specs[i].mtu;
		if (tap_vlan_setup(unit, &taps[ntaps], specs[i].vlan) != 0)
			return -1;
		printf("tap: %s <-> port %d\n", taps[ntaps].name, taps[ntaps].port);
		ntaps++;
	}
	if (ntaps == 0)
		return 0;

	rv = bcm_rx_register(unit, "nosaic-tap", tap_rx, RX_PRIORITY, NULL,
			     BCM_RCO_F_ALL_COS);
	if (rv != BCM_E_NONE) {
		fprintf(stderr, "tap: bcm_rx_register: %d\n", rv);
		return -1;
	}
	if (!bcm_rx_active(unit)) {
		rv = bcm_rx_start(unit, NULL);
		if (rv != BCM_E_NONE) {
			fprintf(stderr, "tap: bcm_rx_start: %d\n", rv);
			return -1;
		}
	}
	return ntaps;
}

int nosaic_tap_count(void)
{
	return ntaps;
}

int nosaic_tap_info(int i, const char **name, int *port, int *vlan, int *mtu,
		    unsigned char mac[6])
{
	if (i < 0 || i >= ntaps)
		return -1;
	if (name != NULL)
		*name = taps[i].name;
	if (port != NULL)
		*port = taps[i].port;
	if (vlan != NULL)
		*vlan = taps[i].vlan;
	if (mtu != NULL)
		*mtu = taps[i].mtu;
	if (mac != NULL)
		memcpy(mac, taps[i].mac, 6);
	return 0;
}

void nosaic_tap_pump(void (*tick)(void), int tick_ms)
{
	struct pollfd fds[MAX_TAPS];
	unsigned char buf[TAP_MTU];
	int i;

	for (;;) {
		for (i = 0; i < ntaps; i++) {
			fds[i].fd = taps[i].fd;
			fds[i].events = POLLIN;
			fds[i].revents = 0;
		}
		if (poll(fds, (nfds_t)ntaps, tick != NULL ? tick_ms : -1) < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		if (tick != NULL)
			tick();
		for (i = 0; i < ntaps; i++) {
			ssize_t len;

			if (!(fds[i].revents & POLLIN))
				continue;
			len = read(taps[i].fd, buf, sizeof(buf));
			if (len > 0)
				tap_tx(&taps[i], buf, (int)len);
		}
	}
}
