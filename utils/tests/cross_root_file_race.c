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
#include <unistd.h>

struct race {
	int left_fd;
	int right_fd;
	unsigned iterations;
	ino_t expected_ino;
	atomic_bool stop;
	atomic_uint errors;
};

static void record_error(struct race *race, const char *operation)
{
	unsigned int count = atomic_fetch_add(&race->errors, 1);

	if (count < 16)
		fprintf(stderr, "%s: %s\n", operation, strerror(errno));
}

static void check_side(struct race *race, int dirfd)
{
	struct stat st;

	if (!fstatat(dirfd, "race-file", &st, AT_SYMLINK_NOFOLLOW)) {
		if (!S_ISREG(st.st_mode) || st.st_ino != race->expected_ino ||
		    st.st_size != 7) {
			errno = EIO;
			record_error(race, "reader observed wrong file");
		}
	} else if (errno != ENOENT) {
		record_error(race, "reader lookup");
	}
}

static void *reader(void *opaque)
{
	struct race *race = opaque;

	while (!atomic_load(&race->stop)) {
		check_side(race, race->left_fd);
		check_side(race, race->right_fd);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	struct race race = {0};
	struct stat st;
	pthread_t readers[4];
	unsigned int i;

	if (argc != 4) {
		fprintf(stderr, "Usage: %s LEFT_ROOT RIGHT_ROOT ITERATIONS\n",
			argv[0]);
		return 2;
	}
	race.iterations = (unsigned int)strtoul(argv[3], NULL, 10);
	if (!race.iterations)
		return 2;
	race.left_fd = open(argv[1], O_RDONLY | O_DIRECTORY);
	race.right_fd = open(argv[2], O_RDONLY | O_DIRECTORY);
	if (race.left_fd < 0 || race.right_fd < 0) {
		perror("open root");
		return 1;
	}
	if (fstatat(race.left_fd, "race-file", &st, AT_SYMLINK_NOFOLLOW)) {
		perror("fstatat race-file");
		return 1;
	}
	race.expected_ino = st.st_ino;
	for (i = 0; i < 4; i++) {
		if (pthread_create(&readers[i], NULL, reader, &race)) {
			perror("pthread_create");
			return 1;
		}
	}
	for (i = 0; i < race.iterations; i++) {
		if (renameat(race.left_fd, "race-file", race.right_fd,
			     "race-file")) {
			record_error(&race, "rename left to right");
			break;
		}
		if (renameat(race.right_fd, "race-file", race.left_fd,
			     "race-file")) {
			record_error(&race, "rename right to left");
			break;
		}
	}
	atomic_store(&race.stop, true);
	for (i = 0; i < 4; i++)
		pthread_join(readers[i], NULL);
	if (fstatat(race.left_fd, "race-file", &st, AT_SYMLINK_NOFOLLOW) ||
	    st.st_ino != race.expected_ino || st.st_size != 7) {
		errno = EIO;
		record_error(&race, "final file");
	}
	if (!faccessat(race.right_fd, "race-file", F_OK, 0) || errno != ENOENT) {
		errno = EIO;
		record_error(&race, "duplicate final file");
	}
	close(race.right_fd);
	close(race.left_fd);
	printf("renames=%u reader_errors=%u\n", race.iterations * 2,
	       atomic_load(&race.errors));
	return atomic_load(&race.errors) ? 1 : 0;
}
