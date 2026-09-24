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

static uint64_t now_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts)) {
		perror("clock_gettime");
		exit(1);
	}
	return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
}

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

static int compare_u64(const void *left, const void *right)
{
	uint64_t a = *(const uint64_t *)left;
	uint64_t b = *(const uint64_t *)right;

	return (a > b) - (a < b);
}

static uint64_t percentile(const uint64_t *values, uint64_t count,
			   uint64_t numerator, uint64_t denominator)
{
	uint64_t index = (count * numerator + denominator - 1) / denominator;

	if (!index)
		index = 1;
	return values[index - 1];
}

static int run_once(const char *old_path, const char *new_path)
{
	uint64_t start, end;
	int result, saved_errno;

	start = now_ns();
	result = rename(old_path, new_path);
	saved_errno = result ? errno : 0;
	end = now_ns();
	printf("result=%s\nerrno=%d\nlatency_ns=%" PRIu64 "\n",
	       result ? (saved_errno == EAGAIN ? "eagain" : "error") : "success",
	       saved_errno, end - start);
	if (!result || saved_errno == EAGAIN)
		return 0;
	errno = saved_errno;
	perror("rename");
	return 1;
}

static int run_complete(const char *old_path, const char *new_path)
{
	uint64_t start_all, end_all, first_ns, retry_ns = 0;
	uint64_t wait_start = 0, wait_ns = 0;
	uint64_t attempts = 0;
	uint64_t backoff_ns = 50000;
	int result, saved_errno;

	start_all = now_ns();
	result = rename(old_path, new_path);
	saved_errno = result ? errno : 0;
	first_ns = now_ns() - start_all;
	if (!result) {
		printf("result=success\nfirst_result=success\nerrno=0\n"
		       "first_ns=%" PRIu64 "\nwait_ns=0\nretry_ns=0\n"
		       "latency_ns=%" PRIu64 "\neagain_count=0\n",
		       first_ns, first_ns);
		return 0;
	}
	if (saved_errno != EAGAIN) {
		errno = saved_errno;
		perror("rename");
		return 1;
	}

	wait_start = now_ns();
	for (;;) {
		struct timespec retry_pause = { .tv_nsec = backoff_ns };
		uint64_t retry_start;

		if (nanosleep(&retry_pause, NULL) && errno != EINTR) {
			perror("nanosleep");
			return 1;
		}
		retry_start = now_ns();
		result = rename(old_path, new_path);
		saved_errno = result ? errno : 0;
		retry_ns = now_ns() - retry_start;
		attempts++;
		if (!result)
			break;
		if (saved_errno != EAGAIN) {
			errno = saved_errno;
			perror("rename retry");
			return 1;
		}
		if (now_ns() - start_all > UINT64_C(120000000000)) {
			fprintf(stderr, "rename retry timed out\n");
			return 1;
		}
		if (backoff_ns < UINT64_C(500000))
			backoff_ns = backoff_ns * 2 > UINT64_C(500000) ?
				UINT64_C(500000) : backoff_ns * 2;
	}
	end_all = now_ns();
	wait_ns = end_all - wait_start - retry_ns;
	printf("result=success\nfirst_result=eagain\nerrno=0\n"
	       "first_ns=%" PRIu64 "\nwait_ns=%" PRIu64 "\n"
	       "retry_ns=%" PRIu64 "\nlatency_ns=%" PRIu64 "\n"
	       "eagain_count=%" PRIu64 "\n",
	       first_ns, wait_ns, retry_ns, end_all - start_all, attempts);
	return 0;
}

static int run_pingpong(const char *left_path, const char *right_path,
			const char *name, uint64_t operations)
{
	uint64_t *latencies;
	uint64_t start_all, end_all, i;
	struct stat before, after;
	int left_fd, right_fd;

	if (!operations) {
		fprintf(stderr, "operations must be positive\n");
		return 2;
	}
	left_fd = open(left_path, O_RDONLY | O_DIRECTORY);
	right_fd = open(right_path, O_RDONLY | O_DIRECTORY);
	if (left_fd < 0 || right_fd < 0) {
		perror("open directory");
		return 1;
	}
	if (fstatat(left_fd, name, &before, AT_SYMLINK_NOFOLLOW)) {
		perror("fstatat initial file");
		return 1;
	}
	latencies = calloc(operations, sizeof(*latencies));
	if (!latencies) {
		perror("calloc");
		return 1;
	}
	start_all = now_ns();
	for (i = 0; i < operations; i++) {
		uint64_t start = now_ns();
		int source = (i & 1) ? right_fd : left_fd;
		int destination = (i & 1) ? left_fd : right_fd;

		if (renameat(source, name, destination, name)) {
			fprintf(stderr, "renameat operation %" PRIu64 ": %s\n",
				i, strerror(errno));
			return 1;
		}
		latencies[i] = now_ns() - start;
	}
	end_all = now_ns();
	if (fstatat((operations & 1) ? right_fd : left_fd, name, &after,
		    AT_SYMLINK_NOFOLLOW)) {
		perror("fstatat final file");
		return 1;
	}
	if (before.st_ino != after.st_ino) {
		fprintf(stderr, "inode changed from %ju to %ju\n",
			(uintmax_t)before.st_ino, (uintmax_t)after.st_ino);
		return 1;
	}
	qsort(latencies, operations, sizeof(*latencies), compare_u64);
	printf("operations=%" PRIu64 "\n", operations);
	printf("elapsed_ns=%" PRIu64 "\n", end_all - start_all);
	printf("ops_per_sec=%.3f\n",
	       (double)operations * 1e9 / (double)(end_all - start_all));
	printf("p50_ns=%" PRIu64 "\n",
	       percentile(latencies, operations, 50, 100));
	printf("p95_ns=%" PRIu64 "\n",
	       percentile(latencies, operations, 95, 100));
	printf("p99_ns=%" PRIu64 "\n",
	       percentile(latencies, operations, 99, 100));
	printf("p999_ns=%" PRIu64 "\n",
	       percentile(latencies, operations, 999, 1000));
	printf("max_ns=%" PRIu64 "\n", latencies[operations - 1]);
	printf("inode=%ju\n", (uintmax_t)after.st_ino);
	free(latencies);
	close(right_fd);
	close(left_fd);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 4 && !strcmp(argv[1], "once"))
		return run_once(argv[2], argv[3]);
	if (argc == 4 && !strcmp(argv[1], "complete"))
		return run_complete(argv[2], argv[3]);
	if (argc == 6 && !strcmp(argv[1], "pingpong"))
		return run_pingpong(argv[2], argv[3], argv[4],
				    parse_u64(argv[5], "operations"));
	fprintf(stderr,
		"Usage:\n"
		"  %s once OLD_PATH NEW_PATH\n"
		"  %s complete OLD_PATH NEW_PATH\n"
		"  %s pingpong LEFT_DIR RIGHT_DIR NAME OPERATIONS\n",
		argv[0], argv[0], argv[0]);
	return 2;
}
