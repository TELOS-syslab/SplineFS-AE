// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static uint64_t ns_now(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts)) {
		perror("clock_gettime");
		exit(1);
	}
	return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
}

static int compare_u64(const void *left, const void *right)
{
	uint64_t a = *(const uint64_t *)left;
	uint64_t b = *(const uint64_t *)right;

	return (a > b) - (a < b);
}

int main(int argc, char **argv)
{
	uint64_t operations, *latency, start, end, i, errors = 0;
	int dirfd;

	if (argc != 3 && argc != 4) {
		fprintf(stderr, "Usage: %s DIRECTORY OPERATIONS [PREFIX]\n", argv[0]);
		return 2;
	}
	operations = strtoull(argv[2], NULL, 10);
	if (!operations || operations > UINT32_MAX)
		return 2;
	latency = calloc((size_t)operations, sizeof(*latency));
	if (!latency) {
		perror("calloc");
		return 1;
	}
	dirfd = open(argv[1], O_RDONLY | O_DIRECTORY);
	if (dirfd < 0) {
		perror("open directory");
		return 1;
	}
	start = ns_now();
	for (i = 0; i < operations; i++) {
		struct stat st;
		char name[64];
		uint64_t before = ns_now();

		snprintf(name, sizeof(name), "%s.%020" PRIu64,
			 argc == 4 ? argv[3] : "transition-miss", i);
		if (!fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) ||
		    errno != ENOENT)
			errors++;
		latency[i] = ns_now() - before;
	}
	end = ns_now();
	close(dirfd);
	qsort(latency, (size_t)operations, sizeof(*latency), compare_u64);
	printf("operations=%" PRIu64 "\n", operations);
	printf("errors=%" PRIu64 "\n", errors);
	printf("elapsed_sec=%.6f\n", (double)(end - start) / 1e9);
	printf("ops_per_sec=%.3f\n", (double)operations * 1e9 / (end - start));
	printf("p50_ns=%" PRIu64 "\n", latency[operations / 2]);
	printf("p99_ns=%" PRIu64 "\n", latency[(operations * 99) / 100]);
	printf("p999_ns=%" PRIu64 "\n", latency[(operations * 999) / 1000]);
	printf("max_ns=%" PRIu64 "\n", latency[operations - 1]);
	free(latency);
	return errors ? 1 : 0;
}
