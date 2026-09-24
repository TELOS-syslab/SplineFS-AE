// SPDX-License-Identifier: GPL-2.0
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static struct dirent *next_real(DIR *dir)
{
	struct dirent *de;

	while ((de = readdir(dir)) != NULL)
		if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
			return de;
	return NULL;
}

static int expect_after(DIR *dir, long cookie, const char *expected)
{
	struct dirent *de;

	seekdir(dir, cookie);
	errno = 0;
	de = next_real(dir);
	if (!de) {
		fprintf(stderr, "readdir after cookie failed: %s\n",
			errno ? strerror(errno) : "end of directory");
		return 1;
	}
	if (strcmp(de->d_name, expected)) {
		fprintf(stderr,
			"cookie resumed at %s (next cookie %ld), expected %s from %ld\n",
			de->d_name, telldir(dir), expected, cookie);
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	char previous[NAME_MAX + 1] = "";
	char expected[NAME_MAX + 1];
	struct dirent *de;
	long cookie;
	long expected_after;
	long previous_cookie = 0;
	unsigned int i;
	pid_t pid;
	int status;
	DIR *dir;

	if (argc != 3) {
		fprintf(stderr, "Usage: %s DIR COMPACTION_TOOL\n", argv[0]);
		return 2;
	}
	dir = opendir(argv[1]);
	if (!dir) {
		perror("opendir");
		return 1;
	}
	for (i = 0; i < 512; i++) {
		de = next_real(dir);
		if (!de) {
			fprintf(stderr, "directory ended before test cursor\n");
			closedir(dir);
			return 1;
		}
		strncpy(previous, de->d_name, sizeof(previous));
		previous[sizeof(previous) - 1] = '\0';
		cookie = telldir(dir);
		if (cookie <= previous_cookie) {
			fprintf(stderr,
				"nonmonotonic cookie at %s: %ld after %ld\n",
				de->d_name, cookie, previous_cookie);
			closedir(dir);
			return 1;
		}
		previous_cookie = cookie;
	}
	cookie = telldir(dir);
	if (cookie < 0) {
		perror("telldir");
		closedir(dir);
		return 1;
	}
	de = next_real(dir);
	if (!de) {
		fprintf(stderr, "directory ended at test cursor\n");
		closedir(dir);
		return 1;
	}
	strncpy(expected, de->d_name, sizeof(expected));
	expected[sizeof(expected) - 1] = '\0';
	expected_after = telldir(dir);
	if (expect_after(dir, cookie, expected)) {
		fprintf(stderr,
			"previous entry was %s, cookie=%ld, expected-next=%ld\n",
			previous, cookie, expected_after);
		closedir(dir);
		return 1;
	}

	pid = fork();
	if (pid < 0) {
		perror("fork");
		closedir(dir);
		return 1;
	}
	if (!pid) {
		execl(argv[2], argv[2], argv[1], (char *)NULL);
		perror("exec compaction tool");
		_exit(127);
	}
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
	    WEXITSTATUS(status)) {
		fprintf(stderr, "compaction child failed\n");
		closedir(dir);
		return 1;
	}
	if (expect_after(dir, cookie, expected)) {
		closedir(dir);
		return 1;
	}
	if (closedir(dir)) {
		perror("closedir");
		return 1;
	}
	return 0;
}
