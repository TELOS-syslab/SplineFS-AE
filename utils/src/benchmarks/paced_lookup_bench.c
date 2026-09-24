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

static uint64_t parse_u64(const char *text, const char *what)
{
	char *end;
	unsigned long long value;

	errno = 0;
	value = strtoull(text, &end, 10);
	if (errno || end == text || *end) {
		fprintf(stderr, "invalid %s: %s\n", what, text);
		exit(2);
	}
	return value;
}

static uint64_t timespec_ns(const struct timespec *value)
{
	return (uint64_t)value->tv_sec * UINT64_C(1000000000) + value->tv_nsec;
}

static struct timespec ns_timespec(uint64_t value)
{
	struct timespec result = {
		.tv_sec = (time_t)(value / UINT64_C(1000000000)),
		.tv_nsec = (long)(value % UINT64_C(1000000000)),
	};

	return result;
}

int main(int argc, char **argv)
{
	struct timespec begin, end;
	uint64_t operations, rate, name_base, interval = 0, i;
	uint64_t hits = 0, misses = 0;
	int dirfd;

	if (argc != 5) {
		fprintf(stderr, "Usage: %s DIR OPERATIONS RATE_OPS_PER_SEC NAME_BASE\n",
			argv[0]);
		return 2;
	}
	operations = parse_u64(argv[2], "operations");
	rate = parse_u64(argv[3], "rate");
	name_base = parse_u64(argv[4], "name base");
	if (!operations)
		return 2;
	if (rate)
		interval = UINT64_C(1000000000) / rate;
	dirfd = open(argv[1], O_RDONLY | O_DIRECTORY);
	if (dirfd < 0) {
		perror("open directory");
		return 1;
	}
	if (clock_gettime(CLOCK_MONOTONIC, &begin)) {
		perror("clock_gettime");
		return 1;
	}
	for (i = 0; i < operations; i++) {
		char name[64];
		struct stat st;

		snprintf(name, sizeof(name), "paced-miss.%020" PRIu64,
			 name_base + i);
		if (!fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW))
			hits++;
		else if (errno == ENOENT)
			misses++;
		else {
			perror("fstatat");
			return 1;
		}
		if (interval) {
			struct timespec deadline = ns_timespec(
				timespec_ns(&begin) + (i + 1) * interval);
			int error;

			do {
				error = clock_nanosleep(CLOCK_MONOTONIC,
					TIMER_ABSTIME, &deadline, NULL);
			} while (error == EINTR);
			if (error) {
				errno = error;
				perror("clock_nanosleep");
				return 1;
			}
		}
	}
	if (clock_gettime(CLOCK_MONOTONIC, &end)) {
		perror("clock_gettime");
		return 1;
	}
	printf("operations=%" PRIu64 "\n", operations);
	printf("rate_limit=%" PRIu64 "\n", rate);
	printf("hits=%" PRIu64 "\n", hits);
	printf("misses=%" PRIu64 "\n", misses);
	printf("elapsed_ns=%" PRIu64 "\n",
	       timespec_ns(&end) - timespec_ns(&begin));
	printf("ops_per_sec=%.3f\n", (double)operations * 1e9 /
	       (timespec_ns(&end) - timespec_ns(&begin)));
	close(dirfd);
	return hits ? 1 : 0;
}
