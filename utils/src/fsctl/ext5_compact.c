/* SPDX-License-Identifier: GPL-2.0 */
/* ext5_compact.c -- userspace helper for EXT5_IOC_LI_FORCE_COMPACT. */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define EXT5_IOC_LI_FORCE_COMPACT _IO('f', 0x83)

int main(int argc, char **argv)
{
	int fd, ret, attempt;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s <directory>\n", argv[0]);
		return 2;
	}
	fd = open(argv[1], O_RDONLY | O_DIRECTORY);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %s\n", argv[1], strerror(errno));
		return 1;
	}
	/* A concurrent append can outrun the bounded tail catch-up without
	 * changing any live generation state. Hide that optimistic EAGAIN from
	 * this debug helper while leaving normal policy retries on the workqueue. */
	for (attempt = 0; attempt < 256; attempt++) {
		ret = ioctl(fd, EXT5_IOC_LI_FORCE_COMPACT);
		if (ret == 0 || errno != EAGAIN)
			break;
		usleep(10000);
	}
	if (ret < 0) {
		fprintf(stderr, "ioctl(EXT5_IOC_LI_FORCE_COMPACT): %s\n",
			strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);
	printf("OK: %s compacted\n", argv[1]);
	return 0;
}
