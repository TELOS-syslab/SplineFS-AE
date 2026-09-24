// SPDX-License-Identifier: GPL-2.0
/* ext5_demote_dir.c -- userspace helper for EXT5_IOC_LI_DEMOTE_DIR. */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define EXT5_IOC_LI_DEMOTE_DIR _IO('f', 0x85)

int main(int argc, char **argv)
{
	int fd, ret;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <stable-dir>\n", argv[0]);
		return 2;
	}
	fd = open(argv[1], O_RDONLY | O_DIRECTORY);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %s\n", argv[1], strerror(errno));
		return 1;
	}
	ret = ioctl(fd, EXT5_IOC_LI_DEMOTE_DIR);
	if (ret < 0) {
		int saved_errno = errno;

		fprintf(stderr, "ioctl(EXT5_IOC_LI_DEMOTE_DIR): %s\n",
			strerror(saved_errno));
		close(fd);
		return saved_errno == EAGAIN ? 3 : 1;
	}
	close(fd);
	printf("OK: %s demoted\n", argv[1]);
	return 0;
}
