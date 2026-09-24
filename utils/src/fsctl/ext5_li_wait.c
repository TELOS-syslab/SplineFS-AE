// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define EXT5_IOC_LI_WAIT_IDLE _IO('f', 0x87)

int main(int argc, char **argv)
{
	int fd;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s <path-on-filesystem>\n", argv[0]);
		return 2;
	}
	fd = open(argv[1], O_RDONLY | O_DIRECTORY);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %s\n", argv[1], strerror(errno));
		return 1;
	}
	if (ioctl(fd, EXT5_IOC_LI_WAIT_IDLE) < 0) {
		fprintf(stderr, "wait-idle(%s): %s\n", argv[1], strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);
	return 0;
}
