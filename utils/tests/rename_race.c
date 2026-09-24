// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1U << 1)
#endif

struct race {
	int rootfd;
	unsigned iterations;
	atomic_bool stop;
	atomic_uint errors;
};

static void fail(struct race *race, const char *what)
{
	unsigned nr = atomic_fetch_add(&race->errors, 1);

	if (nr < 16)
		fprintf(stderr, "%s: errno=%d (%s)\n", what, errno,
			strerror(errno));
}

static void *reader(void *opaque)
{
	struct race *race = opaque;
	const char *paths[] = { "a/moving/payload", "b/moving/payload" };

	while (!atomic_load(&race->stop)) {
		unsigned i;

		for (i = 0; i < 2; i++) {
			struct stat st;

			if (!fstatat(race->rootfd, paths[i], &st,
				     AT_SYMLINK_NOFOLLOW)) {
				if (!S_ISREG(st.st_mode) || st.st_size != 7) {
					errno = EIO;
					fail(race, "reader observed corrupt payload");
				}
			} else if (errno != ENOENT) {
				fail(race, "reader lookup");
			}
		}
	}
	return NULL;
}

int main(int argc, char **argv)
{
	struct race race = {0};
	pthread_t readers[4];
	unsigned i;

	if (argc != 3) {
		fprintf(stderr, "Usage: %s STABLE_ROOT ITERATIONS\n", argv[0]);
		return 2;
	}
	race.iterations = (unsigned)strtoul(argv[2], NULL, 10);
	if (!race.iterations)
		return 2;
	race.rootfd = open(argv[1], O_RDONLY | O_DIRECTORY);
	if (race.rootfd < 0) {
		perror("open root");
		return 1;
	}
	for (i = 0; i < 4; i++) {
		if (pthread_create(&readers[i], NULL, reader, &race)) {
			perror("pthread_create");
			return 1;
		}
	}
	for (i = 0; i < race.iterations; i++) {
		if (renameat(race.rootfd, "a/moving", race.rootfd,
			     "b/moving")) {
			fail(&race, "rename a->b");
			break;
		}
		if (renameat(race.rootfd, "b/moving", race.rootfd,
			     "a/moving")) {
			fail(&race, "rename b->a");
			break;
		}
	}
	atomic_store(&race.stop, true);
	for (i = 0; i < 4; i++)
		pthread_join(readers[i], NULL);
	if (faccessat(race.rootfd, "a/moving/payload", F_OK, 0))
		fail(&race, "final payload");
	errno = 0;
	if (syscall(SYS_renameat2, race.rootfd, "source", race.rootfd,
		    "destination", RENAME_EXCHANGE) != -1 || errno != EOPNOTSUPP) {
		errno = EIO;
		fail(&race, "stable RENAME_EXCHANGE was not rejected");
	}
	close(race.rootfd);
	printf("renames=%u reader_errors=%u\n", race.iterations * 2,
	       atomic_load(&race.errors));
	return atomic_load(&race.errors) ? 1 : 0;
}
