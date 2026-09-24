// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Used only in a disposable, one-vCPU QEMU guest. The FIFO caller prevents
 * a maintenance worker from consuming the boundary between rename records. */
int main(int argc, char **argv)
{
	int dirfd, dstfd, i;

	if (argc < 3 || argc > 4)
		return 2;
	dirfd = open(argv[2], O_RDONLY | O_DIRECTORY);
	if (dirfd < 0)
		return 1;
	if (!strcmp(argv[1], "fill")) {
		for (i = 0; i < 63; i++) {
			char name[40];
			int fd;

			if (i < 62)
				snprintf(name, sizeof(name), "n.%030d", i);
			else
				strcpy(name, "x.0000000000000000000000");
			fd = openat(dirfd, name, O_WRONLY | O_CREAT | O_EXCL, 0600);
			if (fd < 0) {
				perror("fill");
				return 1;
			}
			close(fd);
		}
	} else if (!strcmp(argv[1], "rename")) {
		struct sched_param param = { .sched_priority = 1 };
		int err;

		dstfd = argc == 4 ? open(argv[3], O_RDONLY | O_DIRECTORY) : dirfd;
		if (dstfd < 0 || sched_setscheduler(0, SCHED_FIFO, &param)) {
			perror("rename setup");
			return 1;
		}
		err = renameat(dirfd, "s", dstfd, "t") ? errno : 0;
		if (dstfd != dirfd)
			close(dstfd);
		if (err != ENOSPC) {
			fprintf(stderr, "rename returned %d, expected ENOSPC\n", err);
			return 1;
		}
	} else {
		return 2;
	}
	close(dirfd);
	return 0;
}
