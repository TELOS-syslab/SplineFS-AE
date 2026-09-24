/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ext5_promote_dir.c -- userspace helper to invoke EXT5_IOC_PROMOTE_DIR.
 *
 * Usage:
 *   ext5_promote_dir <directory>          promote one directory (verbose)
 *   ext5_promote_dir -                    promote every path read from stdin,
 *                                         one per line (quiet; for per-dir
 *                                         promotion of a whole tree via
 *                                         `find DIR -type d | ext5_promote_dir -`)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define EXT5_IOC_PROMOTE_DIR _IO('f', 0x80)

static int promote_one(const char *path, int verbose)
{
	int fd = open(path, O_RDONLY | O_DIRECTORY);
	if (fd < 0) {
		if (verbose)
			fprintf(stderr, "open(%s): %s\n", path, strerror(errno));
		return -1;
	}
	if (ioctl(fd, EXT5_IOC_PROMOTE_DIR) < 0) {
		if (verbose)
			fprintf(stderr, "ioctl(%s): %s\n", path, strerror(errno));
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "Usage: %s <directory> | %s - (paths on stdin)\n",
			argv[0], argv[0]);
		return 2;
	}
	if (strcmp(argv[1], "-") == 0) {
		char *line = NULL;
		size_t cap = 0;
		ssize_t n;
		unsigned long ok = 0, fail = 0;
		while ((n = getline(&line, &cap, stdin)) != -1) {
			if (n > 0 && line[n - 1] == '\n')
				line[n - 1] = '\0';
			if (line[0] == '\0')
				continue;
			if (promote_one(line, 0) == 0)
				ok++;
			else
				fail++;
		}
		free(line);
		fprintf(stderr, "promoted %lu dirs (%lu skipped/failed)\n", ok, fail);
		return 0;
	}
	if (promote_one(argv[1], 1) != 0)
		return 1;
	printf("OK: %s promoted\n", argv[1]);
	return 0;
}
