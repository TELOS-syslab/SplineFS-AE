// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <dirent.h>
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

static uint64_t timespec_ns(const struct timespec *value)
{
	return (uint64_t)value->tv_sec * UINT64_C(1000000000) + value->tv_nsec;
}

static struct timespec ns_timespec(uint64_t value)
{
	struct timespec result = {
		.tv_sec = value / UINT64_C(1000000000),
		.tv_nsec = value % UINT64_C(1000000000),
	};

	return result;
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

static uint64_t random_u64(uint64_t *state)
{
	uint64_t value = *state;

	value ^= value >> 12;
	value ^= value << 25;
	value ^= value >> 27;
	*state = value;
	return value * UINT64_C(2685821657736338717);
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

static void print_latency_summary(uint64_t *latencies, uint64_t count,
				  uint64_t elapsed_ns)
{
	qsort(latencies, count, sizeof(*latencies), compare_u64);
	printf("operations=%" PRIu64 "\n", count);
	printf("elapsed_ns=%" PRIu64 "\n", elapsed_ns);
	printf("ops_per_sec=%.3f\n", (double)count * 1e9 / elapsed_ns);
	printf("p50_ns=%" PRIu64 "\n", percentile(latencies, count, 50, 100));
	printf("p95_ns=%" PRIu64 "\n", percentile(latencies, count, 95, 100));
	printf("p99_ns=%" PRIu64 "\n", percentile(latencies, count, 99, 100));
	printf("p999_ns=%" PRIu64 "\n",
	       percentile(latencies, count, 999, 1000));
	printf("max_ns=%" PRIu64 "\n", latencies[count - 1]);
}

static void format_delta(char *name, size_t bytes, uint64_t index)
{
	snprintf(name, bytes, "delta.%020" PRIu64, index);
}

static void format_temp(char *name, size_t bytes, uint64_t index)
{
	snprintf(name, bytes, "temp.%021" PRIu64, index);
}

static void format_base(char *name, size_t bytes, uint64_t index)
{
	snprintf(name, bytes, "item.%020" PRIu64, index);
}

static void format_absent(char *name, size_t bytes, uint64_t index)
{
	snprintf(name, bytes, "absent.%018" PRIu64, index);
}

static int open_directory(const char *path)
{
	int fd = open(path, O_RDONLY | O_DIRECTORY);

	if (fd < 0) {
		perror("open directory");
		exit(1);
	}
	return fd;
}

static int run_fill_rate(const char *path, uint64_t start_index, uint64_t count,
			 uint64_t rate)
{
	uint64_t *latencies, start_all, end_all, i;
	struct timespec begin;
	uint64_t interval = rate ? UINT64_C(1000000000) / rate : 0;
	int dirfd;

	if (!count)
		return 2;
	dirfd = open_directory(path);
	latencies = calloc(count, sizeof(*latencies));
	if (!latencies) {
		perror("calloc");
		return 1;
	}
	if (clock_gettime(CLOCK_MONOTONIC, &begin)) {
		perror("clock_gettime");
		free(latencies);
		close(dirfd);
		return 1;
	}
	start_all = now_ns();
	for (i = 0; i < count; i++) {
		char name[64];
		uint64_t start;
		int fd;

		format_delta(name, sizeof(name), start_index + i);
		start = now_ns();
		fd = openat(dirfd, name, O_CREAT | O_EXCL | O_WRONLY, 0644);
		if (fd < 0) {
			fprintf(stderr, "create %s: %s\n", name, strerror(errno));
			return 1;
		}
		close(fd);
		latencies[i] = now_ns() - start;
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
				free(latencies);
				close(dirfd);
				return 1;
			}
		}
	}
	end_all = now_ns();
	print_latency_summary(latencies, count, end_all - start_all);
	printf("first_index=%" PRIu64 "\n", start_index);
	printf("last_index=%" PRIu64 "\n", start_index + count - 1);
	printf("rate_limit=%" PRIu64 "\n", rate);
	free(latencies);
	close(dirfd);
	return 0;
}

static int run_fill(const char *path, uint64_t start_index, uint64_t count)
{
	return run_fill_rate(path, start_index, count, 0);
}

static int run_remove(const char *path, uint64_t start_index, uint64_t count)
{
	uint64_t *latencies, start_all, end_all, i;
	int dirfd;

	if (!count)
		return 2;
	dirfd = open_directory(path);
	latencies = calloc(count, sizeof(*latencies));
	if (!latencies) {
		perror("calloc");
		close(dirfd);
		return 1;
	}
	start_all = now_ns();
	for (i = 0; i < count; i++) {
		char name[64];
		uint64_t start;

		format_delta(name, sizeof(name), start_index + i);
		start = now_ns();
		if (unlinkat(dirfd, name, 0)) {
			fprintf(stderr, "unlink %s: %s\n", name, strerror(errno));
			free(latencies);
			close(dirfd);
			return 1;
		}
		latencies[i] = now_ns() - start;
	}
	end_all = now_ns();
	print_latency_summary(latencies, count, end_all - start_all);
	printf("first_index=%" PRIu64 "\n", start_index);
	printf("last_index=%" PRIu64 "\n", start_index + count - 1);
	free(latencies);
	close(dirfd);
	return 0;
}

static int run_verify_absent(const char *path, uint64_t start_index,
			     uint64_t count)
{
	uint64_t i;
	int dirfd;

	if (!count)
		return 2;
	dirfd = open_directory(path);
	for (i = 0; i < count; i++) {
		char name[64];
		struct stat st;

		format_delta(name, sizeof(name), start_index + i);
		if (!fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) ||
		    errno != ENOENT) {
			fprintf(stderr, "expected absent %s: %s\n", name,
				errno ? strerror(errno) : "name exists");
			close(dirfd);
			return 1;
		}
	}
	printf("verified_absent=%" PRIu64 "\n", count);
	close(dirfd);
	return 0;
}

static int parse_base_name(const char *name, uint64_t *index)
{
	uint64_t value = 0;
	size_t i;

	if (strlen(name) != 25 || memcmp(name, "item.", 5))
		return -1;
	for (i = 5; i < 25; i++) {
		if (name[i] < '0' || name[i] > '9')
			return -1;
		value = value * 10 + (uint64_t)(name[i] - '0');
	}
	*index = value;
	return 0;
}

static int parse_delta_name(const char *name, uint64_t *index)
{
	uint64_t value = 0;
	size_t i;

	if (strlen(name) != 26 || memcmp(name, "delta.", 6))
		return -1;
	for (i = 6; i < 26; i++) {
		if (name[i] < '0' || name[i] > '9')
			return -1;
		value = value * 10 + (uint64_t)(name[i] - '0');
	}
	*index = value;
	return 0;
}

static int run_verify_tree(const char *path, uint64_t base_count,
			   uint64_t delta_start, uint64_t delta_count)
{
	unsigned char *base_seen, *delta_seen;
	struct dirent *entry;
	uint64_t found = 0;
	DIR *dir;

	if (!base_count || !delta_count)
		return 2;
	dir = opendir(path);
	if (!dir) {
		perror("opendir");
		return 1;
	}
	base_seen = calloc(base_count, sizeof(*base_seen));
	delta_seen = calloc(delta_count, sizeof(*delta_seen));
	if (!base_seen || !delta_seen) {
		perror("calloc");
		free(base_seen);
		free(delta_seen);
		closedir(dir);
		return 1;
	}
	errno = 0;
	while ((entry = readdir(dir)) != NULL) {
		uint64_t index;
		unsigned char *slot;

		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		if (!parse_base_name(entry->d_name, &index) && index < base_count)
			slot = &base_seen[index];
		else if (!parse_delta_name(entry->d_name, &index) &&
			 index >= delta_start && index - delta_start < delta_count)
			slot = &delta_seen[index - delta_start];
		else {
			fprintf(stderr, "unexpected readdir name: %s\n", entry->d_name);
			goto fail;
		}
		if (*slot) {
			fprintf(stderr, "duplicate readdir name: %s\n", entry->d_name);
			goto fail;
		}
		*slot = 1;
		found++;
	}
	if (errno) {
		perror("readdir");
		goto fail;
	}
	if (found != base_count + delta_count) {
		fprintf(stderr, "readdir count: expected=%" PRIu64
			" observed=%" PRIu64 "\n", base_count + delta_count,
			found);
		goto fail;
	}
	printf("verified_tree=%" PRIu64 "\n", found);
	free(base_seen);
	free(delta_seen);
	closedir(dir);
	return 0;
fail:
	free(base_seen);
	free(delta_seen);
	closedir(dir);
	return 1;
}

static int run_verify_base(const char *path, uint64_t count)
{
	unsigned char *seen;
	struct dirent *entry;
	uint64_t found = 0;
	DIR *dir;

	if (!count)
		return 2;
	dir = opendir(path);
	if (!dir) {
		perror("opendir");
		return 1;
	}
	seen = calloc(count, sizeof(*seen));
	if (!seen) {
		perror("calloc");
		closedir(dir);
		return 1;
	}
	errno = 0;
	while ((entry = readdir(dir)) != NULL) {
		uint64_t index;

		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		if (parse_base_name(entry->d_name, &index) || index >= count) {
			fprintf(stderr, "unexpected readdir name: %s\n", entry->d_name);
			free(seen);
			closedir(dir);
			return 1;
		}
		if (seen[index]) {
			fprintf(stderr, "duplicate readdir name: %s\n", entry->d_name);
			free(seen);
			closedir(dir);
			return 1;
		}
		seen[index] = 1;
		found++;
	}
	if (errno) {
		perror("readdir");
		free(seen);
		closedir(dir);
		return 1;
	}
	free(seen);
	closedir(dir);
	if (found != count) {
		fprintf(stderr, "readdir count: expected=%" PRIu64
			" observed=%" PRIu64 "\n", count, found);
		return 1;
	}
	printf("verified_base=%" PRIu64 "\n", found);
	return 0;
}

static int run_temp(const char *path, uint64_t start_index, uint64_t pairs)
{
	uint64_t *latencies, start_all, end_all, i;
	int dirfd;

	if (!pairs)
		return 2;
	dirfd = open_directory(path);
	latencies = calloc(pairs, sizeof(*latencies));
	if (!latencies) {
		perror("calloc");
		return 1;
	}
	start_all = now_ns();
	for (i = 0; i < pairs; i++) {
		char name[64];
		uint64_t start;
		int fd;

		format_temp(name, sizeof(name), start_index + i);
		start = now_ns();
		fd = openat(dirfd, name, O_CREAT | O_EXCL | O_WRONLY, 0644);
		if (fd < 0 || close(fd) || unlinkat(dirfd, name, 0)) {
			fprintf(stderr, "temporary pair %s: %s\n", name,
				strerror(errno));
			return 1;
		}
		latencies[i] = now_ns() - start;
	}
	end_all = now_ns();
	print_latency_summary(latencies, pairs, end_all - start_all);
	printf("namespace_operations=%" PRIu64 "\n", pairs * 2);
	free(latencies);
	close(dirfd);
	return 0;
}

/* Create the complete batch before deleting it in creation order.  The first
 * DELETE is behind every INSERT, so physical-tail cancellation is deliberately
 * impossible.  This is the balanced-log control; run_temp() is the adjacent
 * transient-name control. */
static int run_temp_batch(const char *path, uint64_t start_index, uint64_t pairs)
{
	uint64_t start_all, end_all, i;
	int dirfd;

	if (!pairs)
		return 2;
	dirfd = open_directory(path);
	start_all = now_ns();
	for (i = 0; i < pairs; i++) {
		char name[64];
		int fd;

		format_temp(name, sizeof(name), start_index + i);
		fd = openat(dirfd, name, O_CREAT | O_EXCL | O_WRONLY, 0644);
		if (fd < 0 || close(fd)) {
			fprintf(stderr, "batched temporary create %s: %s\n", name,
				strerror(errno));
			return 1;
		}
	}
	for (i = 0; i < pairs; i++) {
		char name[64];

		format_temp(name, sizeof(name), start_index + i);
		if (unlinkat(dirfd, name, 0)) {
			fprintf(stderr, "batched temporary unlink %s: %s\n", name,
				strerror(errno));
			return 1;
		}
	}
	end_all = now_ns();
	printf("operations=%" PRIu64 "\n", pairs);
	printf("elapsed_ns=%" PRIu64 "\n", end_all - start_all);
	printf("namespace_operations=%" PRIu64 "\n", pairs * 2);
	close(dirfd);
	return 0;
}

static int run_sparse_fill(const char *path, uint64_t start_index,
			   uint64_t count, uint64_t lookups_per_insert)
{
	uint64_t *latencies, start_all, end_all, i;
	int dirfd;

	if (!count || !lookups_per_insert)
		return 2;
	dirfd = open_directory(path);
	latencies = calloc(count, sizeof(*latencies));
	if (!latencies) {
		perror("calloc");
		return 1;
	}
	start_all = now_ns();
	for (i = 0; i < count; i++) {
		char name[64];
		uint64_t start, j;
		int fd;

		format_delta(name, sizeof(name), start_index + i);
		start = now_ns();
		fd = openat(dirfd, name, O_CREAT | O_EXCL | O_WRONLY, 0644);
		if (fd < 0 || close(fd)) {
			fprintf(stderr, "sparse create %s: %s\n", name,
				strerror(errno));
			return 1;
		}
		for (j = 0; j < lookups_per_insert; j++) {
			struct stat st;
			uint64_t index = (start_index + i) * lookups_per_insert + j;

			format_absent(name, sizeof(name), index);
			if (!fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) ||
			    errno != ENOENT) {
				errno = EIO;
				perror("sparse negative lookup");
				return 1;
			}
		}
		latencies[i] = now_ns() - start;
	}
	end_all = now_ns();
	print_latency_summary(latencies, count, end_all - start_all);
	printf("namespace_operations=%" PRIu64 "\n",
	       count * (lookups_per_insert + 1));
	printf("lookups_per_insert=%" PRIu64 "\n", lookups_per_insert);
	free(latencies);
	close(dirfd);
	return 0;
}

static int run_lookup(const char *path, const char *population,
		      uint64_t lookups, uint64_t base_files,
		      uint64_t delta_start, uint64_t delta_count, uint64_t seed,
		      int unique)
{
	uint64_t state = seed ? seed : 1;
	uint64_t start, end, hits = 0, misses = 0, i;
	int dirfd;

	if (!lookups || (!strcmp(population, "base") && !base_files) ||
	    (!strcmp(population, "delta") && !delta_count))
		return 2;
	if (strcmp(population, "base") && strcmp(population, "delta") &&
	    strcmp(population, "negative")) {
		fprintf(stderr, "unknown population: %s\n", population);
		return 2;
	}
	dirfd = open_directory(path);
	start = now_ns();
	for (i = 0; i < lookups; i++) {
		char name[64];
		struct stat st;
		uint64_t value = unique ? i + seed * lookups : random_u64(&state);
		int result;

		if (!strcmp(population, "base"))
			format_base(name, sizeof(name), value % base_files);
		else if (!strcmp(population, "delta"))
			format_delta(name, sizeof(name),
				     delta_start + value % delta_count);
		else
			format_absent(name, sizeof(name), value);
		result = fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW);
		if (!result)
			hits++;
		else if (errno == ENOENT)
			misses++;
		else {
			perror("fstatat");
			return 1;
		}
	}
	end = now_ns();
	printf("population=%s\n", population);
	printf("lookups=%" PRIu64 "\n", lookups);
	printf("hits=%" PRIu64 "\n", hits);
	printf("misses=%" PRIu64 "\n", misses);
	printf("elapsed_ns=%" PRIu64 "\n", end - start);
	printf("ops_per_sec=%.3f\n", (double)lookups * 1e9 / (end - start));
	close(dirfd);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 5 && !strcmp(argv[1], "fill"))
		return run_fill(argv[2], parse_u64(argv[3], "start"),
				parse_u64(argv[4], "count"));
	if (argc == 5 && !strcmp(argv[1], "remove"))
		return run_remove(argv[2], parse_u64(argv[3], "start"),
				  parse_u64(argv[4], "count"));
	if (argc == 5 && !strcmp(argv[1], "verify-absent"))
		return run_verify_absent(argv[2], parse_u64(argv[3], "start"),
					 parse_u64(argv[4], "count"));
	if (argc == 4 && !strcmp(argv[1], "verify-base"))
		return run_verify_base(argv[2], parse_u64(argv[3], "count"));
	if (argc == 6 && !strcmp(argv[1], "verify-tree"))
		return run_verify_tree(argv[2], parse_u64(argv[3], "base count"),
			parse_u64(argv[4], "delta start"),
			parse_u64(argv[5], "delta count"));
	if (argc == 6 && !strcmp(argv[1], "fill-paced"))
		return run_fill_rate(argv[2], parse_u64(argv[3], "start"),
			parse_u64(argv[4], "count"),
			parse_u64(argv[5], "rate"));
	if (argc == 5 && !strcmp(argv[1], "temp"))
		return run_temp(argv[2], parse_u64(argv[3], "start"),
				parse_u64(argv[4], "pairs"));
	if (argc == 5 && !strcmp(argv[1], "temp-batch"))
		return run_temp_batch(argv[2], parse_u64(argv[3], "start"),
				      parse_u64(argv[4], "pairs"));
	if (argc == 6 && !strcmp(argv[1], "sparse-fill"))
		return run_sparse_fill(argv[2], parse_u64(argv[3], "start"),
			parse_u64(argv[4], "count"),
			parse_u64(argv[5], "lookups per insert"));
	if (argc == 9 && (!strcmp(argv[1], "lookup") ||
			   !strcmp(argv[1], "lookup-unique")))
		return run_lookup(argv[2], argv[3], parse_u64(argv[4], "lookups"),
				  parse_u64(argv[5], "base files"),
				  parse_u64(argv[6], "delta start"),
				  parse_u64(argv[7], "delta count"),
				  parse_u64(argv[8], "seed"),
				  !strcmp(argv[1], "lookup-unique"));
	fprintf(stderr,
		"Usage:\n"
		"  %s fill DIR START COUNT\n"
		"  %s remove DIR START COUNT\n"
		"  %s verify-absent DIR START COUNT\n"
		"  %s verify-base DIR COUNT\n"
		"  %s verify-tree DIR BASE_COUNT DELTA_START DELTA_COUNT\n"
		"  %s fill-paced DIR START COUNT RATE\n"
		"  %s temp DIR START PAIRS\n"
		"  %s temp-batch DIR START PAIRS\n"
		"  %s sparse-fill DIR START COUNT LOOKUPS_PER_INSERT\n"
		"  %s lookup[-unique] DIR base|delta|negative LOOKUPS BASE_FILES "
		"DELTA_START DELTA_COUNT SEED\n",
		argv[0], argv[0], argv[0], argv[0], argv[0], argv[0], argv[0],
		argv[0], argv[0], argv[0]);
	return 2;
}
