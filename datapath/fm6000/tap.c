/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/if.h>
#include <linux/if_tun.h>

#include "tap.h"

int fm_tap_create(const char *name)
{
	struct ifreq ifr;
	int fd;

	fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "tap: open /dev/net/tun: %s\n", strerror(errno));
		return -1;
	}

	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
	if (name)
		strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);

	if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
		fprintf(stderr, "tap: TUNSETIFF %s: %s\n", name, strerror(errno));
		close(fd);
		return -1;
	}
	fprintf(stderr, "nosd-fm6000: TAP %s created (fd=%d)\n", ifr.ifr_name, fd);
	return fd;
}

int fm_tap_write(int fd, const uint8_t *frame, size_t len)
{
	ssize_t n = write(fd, frame, len);
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;
		return -1;
	}
	return 0;
}

ssize_t fm_tap_read(int fd, uint8_t *buf, size_t bufsz)
{
	ssize_t n = read(fd, buf, bufsz);
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;
		return -1;
	}
	return n;
}
