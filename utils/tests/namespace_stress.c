// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct worker {
	const char *root;
	uint64_t operations;
	uint32_t id;
	atomic_uint_fast64_t *errors;
};

static void report_error(struct worker *worker, const char *operation,
			 const char *name)
{
	uint64_t number = atomic_fetch_add(worker->errors, 1);

	if (number < 32)
		fprintf(stderr, "worker=%u op=%s name=%s errno=%d (%s)\n",
			worker->id, operation, name, errno, strerror(errno));
}

static uint64_t random_next(uint64_t *state)
{
	uint64_t x = *state;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	*state = x;
	return x;
}

static void *run_worker(void *opaque)
{
	struct worker *worker = opaque;
	uint64_t state = UINT64_C(0x9e3779b97f4a7c15) ^ worker->id;
	uint64_t i;
	int dirfds[16];

	for (i = 0; i < 16; i++) {
		char path[1024];

		snprintf(path, sizeof(path), "%s/d%zu", worker->root, (size_t)i);
		dirfds[i] = open(path, O_RDONLY | O_DIRECTORY);
		if (dirfds[i] < 0) {
			report_error(worker, "open-dir", path);
			return NULL;
		}
	}

	for (i = 0; i < worker->operations; i++) {
		uint64_t value = random_next(&state);
		int dirfd = dirfds[value & 15];
		unsigned operation = (unsigned)(value % 100);
		char first[96], second[96];

		if (operation < 70) {
			struct stat st;

			snprintf(first, sizeof(first), "base.%u",
				 (unsigned)((value >> 8) & 127));
			if (fstatat(dirfd, first, &st, AT_SYMLINK_NOFOLLOW))
				report_error(worker, "lookup", first);
		} else if (operation < 92) {
			int fd;

			snprintf(first, sizeof(first), "tmp.%u.%llu", worker->id,
				 (unsigned long long)i);
			fd = openat(dirfd, first, O_CREAT | O_EXCL | O_WRONLY, 0644);
			if (fd < 0) {
				report_error(worker, "create", first);
				continue;
			}
			close(fd);
			if (unlinkat(dirfd, first, 0))
				report_error(worker, "unlink", first);
		} else {
			int fd;

			snprintf(first, sizeof(first), "rename.a.%u.%llu", worker->id,
				 (unsigned long long)i);
			snprintf(second, sizeof(second), "rename.b.%u.%llu", worker->id,
				 (unsigned long long)i);
			fd = openat(dirfd, first, O_CREAT | O_EXCL | O_WRONLY, 0644);
			if (fd < 0) {
				report_error(worker, "rename-create", first);
				continue;
			}
			close(fd);
			if (renameat(dirfd, first, dirfd, second))
				report_error(worker, "rename", first);
			else if (unlinkat(dirfd, second, 0))
				report_error(worker, "rename-unlink", second);
		}
	}
	for (i = 0; i < 16; i++)
		close(dirfds[i]);
	return NULL;
}

int main(int argc, char **argv)
{
	uint32_t threads;
	uint64_t operations;
	pthread_t *ids;
	struct worker *workers;
	atomic_uint_fast64_t errors = 0;
	uint32_t i;

	if (argc != 4) {
		fprintf(stderr, "Usage: %s ROOT THREADS OPS_PER_THREAD\n", argv[0]);
		return 2;
	}
	threads = (uint32_t)strtoul(argv[2], NULL, 10);
	operations = strtoull(argv[3], NULL, 10);
	if (!threads || threads > 256 || !operations)
		return 2;
	ids = calloc(threads, sizeof(*ids));
	workers = calloc(threads, sizeof(*workers));
	if (!ids || !workers)
		return 1;
	for (i = 0; i < threads; i++) {
		workers[i].root = argv[1];
		workers[i].operations = operations;
		workers[i].id = i;
		workers[i].errors = &errors;
		if (pthread_create(&ids[i], NULL, run_worker, &workers[i]))
			return 1;
	}
	for (i = 0; i < threads; i++)
		pthread_join(ids[i], NULL);
	printf("threads=%u operations=%llu errors=%llu\n", threads,
	       (unsigned long long)threads * operations,
	       (unsigned long long)atomic_load(&errors));
	free(workers);
	free(ids);
	return atomic_load(&errors) ? 1 : 0;
}
