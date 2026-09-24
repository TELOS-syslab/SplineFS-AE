// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	uint32_t pairs, gap, i, j;
	int dirfd;

	if (argc != 4) {
		fprintf(stderr, "Usage: %s DIR PAIRS GAP_LOOKUPS\n", argv[0]);
		return 2;
	}
	pairs = (uint32_t)strtoul(argv[2], NULL, 10);
	gap = (uint32_t)strtoul(argv[3], NULL, 10);
	dirfd = open(argv[1], O_RDONLY | O_DIRECTORY);
	if (dirfd < 0) {
		perror("open dir");
		return 1;
	}
	for (i = 0; i < pairs; i++) {
		char mutation[64];

		for (j = 0; j < gap; j++) {
			char missing[96];
			struct stat st;

			snprintf(missing, sizeof(missing), "missing.%u.%u", i, j);
			if (fstatat(dirfd, missing, &st, AT_SYMLINK_NOFOLLOW) == 0 ||
			    errno != ENOENT) {
				fprintf(stderr, "unexpected lookup result for %s\n", missing);
				return 1;
			}
		}
		snprintf(mutation, sizeof(mutation), "mutation.%u", i);
		{
			int fd = openat(dirfd, mutation,
				O_CREAT | O_EXCL | O_WRONLY, 0644);

			if (fd < 0) {
				perror("create mutation");
				return 1;
			}
			close(fd);
		}
		if (unlinkat(dirfd, mutation, 0)) {
			perror("unlink mutation");
			return 1;
		}
	}
	close(dirfd);
	return 0;
}

