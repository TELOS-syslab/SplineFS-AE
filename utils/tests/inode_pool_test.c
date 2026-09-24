// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define EXT5_IOC_LI_RESERVE_POOL _IOW('f', 0x82, uint32_t)

int main(int argc, char **argv)
{
	uint32_t count;
	int fd, err, expected;

	if (argc != 4) {
		fprintf(stderr, "usage: %s DIR COUNT EXPECTED_ERRNO\n", argv[0]);
		return 2;
	}
	count = strtoul(argv[2], NULL, 0);
	expected = strtol(argv[3], NULL, 0);
	fd = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) {
		perror("open pool directory");
		return 1;
	}
	err = ioctl(fd, EXT5_IOC_LI_RESERVE_POOL, &count) ? errno : 0;
	close(fd);
	if (err != expected) {
		fprintf(stderr, "pool count=%u: expected errno=%d, got %d (%s)\n",
			count, expected, err, strerror(err));
		return 1;
	}
	printf("POOL_RESERVE count=%u errno=%d\n", count, err);
	return 0;
}
