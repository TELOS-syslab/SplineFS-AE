/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Locality sweep over subtree regions.
 *
 * Layout:
 *   ROOT/g0000/d000/item.000...
 *
 * Every leaf directory is smaller than the per-directory promotion gate.
 * A group reaches the subtree gate, so learned indexing is profitable only
 * through subtree aggregation. The workload churns a selected fraction of
 * groups while reading uniformly across the whole namespace.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct ext5_li_dir_mode {
	uint32_t mode;
	uint32_t _pad;
	uint64_t subtree_root_ino;
};

#define EXT5_IOC_LI_DIR_MODE _IOR('f', 0x86, struct ext5_li_dir_mode)

static uint64_t parse_u64(const char *s)
{
	char *end;
	uint64_t value = strtoull(s, &end, 10);

	if (!s[0] || *end) {
		fprintf(stderr, "invalid integer: %s\n", s);
		exit(2);
	}
	return value;
}

static uint64_t nsec_between(const struct timespec *start,
			     const struct timespec *end)
{
	return (uint64_t)(end->tv_sec - start->tv_sec) * 1000000000ULL +
	       (uint64_t)(end->tv_nsec - start->tv_nsec);
}

static void group_name(char *buf, size_t size, uint64_t group)
{
	snprintf(buf, size, "g%04" PRIu64, group);
}

static void leaf_name(char *buf, size_t size, uint64_t leaf)
{
	snprintf(buf, size, "d%03" PRIu64, leaf);
}

static void item_name(char *buf, size_t size, uint64_t item)
{
	snprintf(buf, size, "item.%020" PRIu64, item);
}

static int mkdir_if_needed(const char *path)
{
	if (mkdir(path, 0755) == 0 || errno == EEXIST)
		return 0;
	return -1;
}

static void prepare_tree(const char *root, uint64_t groups, uint64_t leaves,
			 uint64_t entries, uint64_t pool_size)
{
	char path[1024];
	uint64_t group;
	uint64_t leaf;
	uint64_t item;
	int pool_fd;

	if (!pool_size)
		pool_size = 1;
	if (mkdir_if_needed(root)) {
		perror("mkdir root");
		exit(1);
	}
	snprintf(path, sizeof(path), "%s/.pool", root);
	if (mkdir_if_needed(path)) {
		perror("mkdir pool");
		exit(1);
	}
	pool_fd = open(path, O_RDONLY | O_DIRECTORY);
	if (pool_fd < 0) {
		perror("open pool");
		exit(1);
	}
	for (item = 0; item < pool_size; item++) {
		char name[64];
		int fd;

		snprintf(name, sizeof(name), "p%" PRIu64, item);
		fd = openat(pool_fd, name, O_CREAT | O_EXCL | O_WRONLY, 0644);
		if (fd < 0 && errno != EEXIST) {
			perror("create pool inode");
			exit(1);
		}
		if (fd >= 0)
			close(fd);
	}

	for (group = 0; group < groups; group++) {
		char gname[32];

		group_name(gname, sizeof(gname), group);
		snprintf(path, sizeof(path), "%s/%s", root, gname);
		if (mkdir_if_needed(path)) {
			perror("mkdir group");
			exit(1);
		}
		for (leaf = 0; leaf < leaves; leaf++) {
			char lname[32];
			int leaf_fd;

			leaf_name(lname, sizeof(lname), leaf);
			snprintf(path, sizeof(path), "%s/%s/%s", root, gname,
				 lname);
			if (mkdir_if_needed(path)) {
				perror("mkdir leaf");
				exit(1);
			}
			leaf_fd = open(path, O_RDONLY | O_DIRECTORY);
			if (leaf_fd < 0) {
				perror("open leaf");
				exit(1);
			}
			for (item = 0; item < entries; item++) {
				char name[64];
				char pool_name[64];

				item_name(name, sizeof(name), item);
				snprintf(pool_name, sizeof(pool_name), "p%" PRIu64,
					 item % pool_size);
				if (linkat(pool_fd, pool_name, leaf_fd, name, 0) &&
				    errno != EEXIST) {
					perror("linkat");
					exit(1);
				}
			}
			close(leaf_fd);
		}
	}
	close(pool_fd);
	printf("prepared_groups=%" PRIu64 " leaves_per_group=%" PRIu64
	       " entries_per_leaf=%" PRIu64 " total_leaf_dirs=%" PRIu64
	       " total_entries=%" PRIu64 "\n",
	       groups, leaves, entries, groups * leaves,
	       groups * leaves * entries);
}

static void drop_caches(int value)
{
	char text[4];
	int length;
	int fd;

	sync();
	fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
	if (fd < 0) {
		perror("open drop_caches");
		exit(1);
	}
	length = snprintf(text, sizeof(text), "%d\n", value);
	if (write(fd, text, length) != length) {
		perror("write drop_caches");
		exit(1);
	}
	close(fd);
}

static int mutate_dir(int fd, uint64_t seed, uint64_t sequence,
		      uint64_t *elapsed_ns)
{
	char name[96];
	struct timespec start;
	struct timespec end;
	int file_fd;
	int error = 0;

	snprintf(name, sizeof(name), "mut.%016" PRIx64 ".%016" PRIx64,
		 seed, sequence);
	clock_gettime(CLOCK_MONOTONIC_RAW, &start);
	file_fd = openat(fd, name, O_CREAT | O_EXCL | O_WRONLY, 0644);
	if (file_fd < 0) {
		error = 1;
	} else {
		close(file_fd);
		if (unlinkat(fd, name, 0))
			error = 1;
	}
	clock_gettime(CLOCK_MONOTONIC_RAW, &end);
	if (elapsed_ns)
		*elapsed_ns += nsec_between(&start, &end);
	return error;
}

static uint64_t next_random(uint64_t *state)
{
	uint64_t x = *state;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	*state = x;
	return x;
}

static uint64_t *make_group_order(uint64_t groups, uint64_t seed)
{
	uint64_t *order;
	uint64_t state = seed ? seed : 1;
	uint64_t i;

	order = malloc(groups * sizeof(*order));
	if (!order) {
		perror("malloc group order");
		exit(1);
	}
	for (i = 0; i < groups; i++)
		order[i] = i;
	for (i = groups; i > 1; i--) {
		uint64_t j = next_random(&state) % i;
		uint64_t tmp = order[i - 1];

		order[i - 1] = order[j];
		order[j] = tmp;
	}
	return order;
}

static int *open_tree(const char *root, uint64_t groups, uint64_t leaves,
		      int **group_fds_out, int *root_fd_out)
{
	char path[1024];
	int *group_fds;
	int *leaf_fds;
	uint64_t group;
	uint64_t leaf;

	group_fds = calloc(groups, sizeof(*group_fds));
	leaf_fds = calloc(groups * leaves, sizeof(*leaf_fds));
	if (!group_fds || !leaf_fds) {
		perror("calloc fds");
		exit(1);
	}
	*root_fd_out = open(root, O_RDONLY | O_DIRECTORY);
	if (*root_fd_out < 0) {
		perror("open root");
		exit(1);
	}
	for (group = 0; group < groups; group++) {
		char gname[32];

		group_name(gname, sizeof(gname), group);
		snprintf(path, sizeof(path), "%s/%s", root, gname);
		group_fds[group] = open(path, O_RDONLY | O_DIRECTORY);
		if (group_fds[group] < 0) {
			perror("open group");
			exit(1);
		}
		for (leaf = 0; leaf < leaves; leaf++) {
			char lname[32];
			uint64_t index = group * leaves + leaf;

			leaf_name(lname, sizeof(lname), leaf);
			snprintf(path, sizeof(path), "%s/%s/%s", root, gname,
				 lname);
			leaf_fds[index] = open(path, O_RDONLY | O_DIRECTORY);
			if (leaf_fds[index] < 0) {
				perror("open leaf");
				exit(1);
			}
		}
	}
	*group_fds_out = group_fds;
	return leaf_fds;
}

static void trigger_policy(int root_fd, int *group_fds, uint64_t groups)
{
	struct stat st;
	uint64_t group;

	drop_caches(2);
	if (fstatat(root_fd, ".splinefs-policy-probe", &st,
		    AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
		fprintf(stderr, "root policy probe did not return ENOENT\n");
		exit(1);
	}
	for (group = 0; group < groups; group++) {
		if (fstatat(group_fds[group], ".splinefs-policy-probe", &st,
			    AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
			fprintf(stderr,
				"group policy probe did not return ENOENT\n");
			exit(1);
		}
	}
}

static uint64_t run_phase(int *leaf_fds, uint64_t total_leaves,
			  uint64_t leaves_per_group, uint64_t entries,
			  uint64_t offset, uint64_t count, uint64_t gap,
			  uint64_t active_groups, const uint64_t *group_order,
			  uint64_t seed, uint64_t *mutation_sequence,
			  uint64_t *lookup_ns, uint64_t *mutation_ns)
{
	uint64_t errors = 0;
	uint64_t i;

	for (i = 0; i < count; i++) {
		uint64_t ordinal = offset + i;
		uint64_t leaf_index = ordinal % total_leaves;
		uint64_t file_index = ordinal / total_leaves;
		char name[64];
		struct stat st;
		struct timespec start;
		struct timespec end;

		if (file_index >= entries) {
			fprintf(stderr, "lookup ordinal exceeds namespace\n");
			exit(2);
		}
		if (active_groups && i && i % gap == 0) {
			uint64_t sequence = *mutation_sequence;
			uint64_t active_slot = sequence % active_groups;
			uint64_t leaf = (sequence / active_groups) %
					leaves_per_group;
			uint64_t group = group_order[active_slot];
			uint64_t target = group * leaves_per_group + leaf;

			errors += mutate_dir(leaf_fds[target], seed, sequence,
					     mutation_ns);
			(*mutation_sequence)++;
		}

		item_name(name, sizeof(name), file_index);
		clock_gettime(CLOCK_MONOTONIC_RAW, &start);
		if (fstatat(leaf_fds[leaf_index], name, &st,
			    AT_SYMLINK_NOFOLLOW))
			errors++;
		clock_gettime(CLOCK_MONOTONIC_RAW, &end);
		if (lookup_ns)
			*lookup_ns += nsec_between(&start, &end);
	}
	return errors;
}

static void run_sweep(const char *root, uint64_t groups, uint64_t leaves,
		      uint64_t entries, uint64_t warmup, uint64_t measured,
		      uint64_t gap, uint64_t active_groups,
		      uint64_t settle_seconds, uint64_t seed)
{
	uint64_t total_leaves = groups * leaves;
	uint64_t *group_order;
	int *group_fds;
	int *leaf_fds;
	int root_fd;
	uint64_t mutation_sequence = 0;
	uint64_t warmup_errors = 0;
	uint64_t measured_errors = 0;
	uint64_t lookup_ns = 0;
	uint64_t mutation_ns = 0;
	uint64_t measured_mutations;
	uint64_t i;
	struct timespec start;
	struct timespec end;
	uint64_t total_ns;
	uint64_t logical_ops;

	if (!groups || !leaves || !entries || !gap ||
	    active_groups > groups || warmup + measured > total_leaves * entries) {
		fprintf(stderr, "invalid sweep dimensions\n");
		exit(2);
	}
	group_order = make_group_order(groups, seed);
	leaf_fds = open_tree(root, groups, leaves, &group_fds, &root_fd);

	for (i = 0; i < total_leaves; i++)
		warmup_errors += mutate_dir(leaf_fds[i],
					    seed ^ 0xfeed0000ULL, i, NULL);
	drop_caches(3);
	warmup_errors += run_phase(leaf_fds, total_leaves, leaves, entries,
				   0, warmup, gap, active_groups, group_order,
				   seed ^ 0x10000000ULL, &mutation_sequence,
				   NULL, NULL);

	trigger_policy(root_fd, group_fds, groups);
	if (settle_seconds)
		sleep(settle_seconds);

	if (active_groups) {
		for (i = 0; i < active_groups; i++) {
			uint64_t group = group_order[i];
			uint64_t leaf;

			for (leaf = 0; leaf < leaves; leaf++) {
				uint64_t target = group * leaves + leaf;

				warmup_errors += mutate_dir(
					leaf_fds[target],
					seed ^ 0xc10a0000ULL,
					mutation_sequence++, NULL);
			}
		}
	}
	drop_caches(3);

	measured_mutations = mutation_sequence;
	clock_gettime(CLOCK_MONOTONIC_RAW, &start);
	measured_errors += run_phase(leaf_fds, total_leaves, leaves, entries,
				     warmup, measured, gap, active_groups,
				     group_order, seed, &mutation_sequence,
				     &lookup_ns, &mutation_ns);
	clock_gettime(CLOCK_MONOTONIC_RAW, &end);
	measured_mutations = mutation_sequence - measured_mutations;

	if (active_groups) {
		for (i = 0; i < active_groups; i++) {
			uint64_t group = group_order[i];
			uint64_t leaf;

			for (leaf = 0; leaf < leaves; leaf++) {
				uint64_t target = group * leaves + leaf;

				measured_errors += mutate_dir(
					leaf_fds[target],
					seed ^ 0xc1050000ULL,
					mutation_sequence++, NULL);
			}
		}
	}
	total_ns = nsec_between(&start, &end);
	logical_ops = measured + measured_mutations;

	for (i = 0; i < total_leaves; i++)
		close(leaf_fds[i]);
	for (i = 0; i < groups; i++)
		close(group_fds[i]);
	close(root_fd);
	free(leaf_fds);
	free(group_fds);
	free(group_order);

	printf("groups=%" PRIu64 " active_groups=%" PRIu64
	       " warmup=%" PRIu64 " lookups=%" PRIu64
	       " mutations=%" PRIu64 " warmup_errors=%" PRIu64
	       " errors=%" PRIu64 " seconds=%.6f ops_per_sec=%.3f"
	       " lookup_us=%.3f mutation_pair_us=%.3f\n",
	       groups, active_groups, warmup, measured, measured_mutations,
	       warmup_errors, measured_errors, total_ns / 1e9,
	       total_ns ? logical_ops * 1e9 / total_ns : 0.0,
	       measured ? lookup_ns / 1000.0 / measured : 0.0,
	       measured_mutations ?
			mutation_ns / 1000.0 / measured_mutations : 0.0);
	if (warmup_errors || measured_errors)
		exit(1);
}

static const char *mode_name(uint32_t mode)
{
	if (mode == 1)
		return "ROOT";
	if (mode == 2)
		return "INTERIOR";
	return "MUTABLE";
}

static void print_mode(int fd, const char *scope, int64_t group,
		       int64_t leaf, uint64_t *roots, uint64_t *interiors,
		       uint64_t *mutable)
{
	struct ext5_li_dir_mode mode = {0};

	if (ioctl(fd, EXT5_IOC_LI_DIR_MODE, &mode)) {
		perror("EXT5_IOC_LI_DIR_MODE");
		exit(1);
	}
	if (mode.mode == 1)
		(*roots)++;
	else if (mode.mode == 2)
		(*interiors)++;
	else
		(*mutable)++;
	printf("%s,%" PRId64 ",%" PRId64 ",%s,%" PRIu64 "\n",
	       scope, group, leaf, mode_name(mode.mode),
	       mode.subtree_root_ino);
}

static void show_modes(const char *root, uint64_t groups, uint64_t leaves)
{
	char path[1024];
	uint64_t roots = 0;
	uint64_t interiors = 0;
	uint64_t mutable = 0;
	uint64_t group;
	uint64_t leaf;
	int fd;

	printf("scope,group,leaf,mode,subtree_root_ino\n");
	fd = open(root, O_RDONLY | O_DIRECTORY);
	if (fd < 0) {
		perror("open root");
		exit(1);
	}
	print_mode(fd, "root", -1, -1, &roots, &interiors, &mutable);
	close(fd);

	for (group = 0; group < groups; group++) {
		char gname[32];

		group_name(gname, sizeof(gname), group);
		snprintf(path, sizeof(path), "%s/%s", root, gname);
		fd = open(path, O_RDONLY | O_DIRECTORY);
		if (fd < 0) {
			perror("open group");
			exit(1);
		}
		print_mode(fd, "group", group, -1, &roots, &interiors,
			   &mutable);
		close(fd);
		for (leaf = 0; leaf < leaves; leaf++) {
			char lname[32];

			leaf_name(lname, sizeof(lname), leaf);
			snprintf(path, sizeof(path), "%s/%s/%s", root,
				 gname, lname);
			fd = open(path, O_RDONLY | O_DIRECTORY);
			if (fd < 0) {
				perror("open leaf");
				exit(1);
			}
			print_mode(fd, "leaf", group, leaf, &roots,
				   &interiors, &mutable);
			close(fd);
		}
	}
	fprintf(stderr, "mode_summary roots=%" PRIu64
		" interiors=%" PRIu64 " mutable=%" PRIu64 "\n",
		roots, interiors, mutable);
}

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s prepare ROOT GROUPS LEAVES ENTRIES [POOL]\n"
		"  %s run ROOT GROUPS LEAVES ENTRIES WARMUP MEASURE GAP"
		" ACTIVE_GROUPS SETTLE_SECONDS [SEED]\n"
		"  %s modes ROOT GROUPS LEAVES\n",
		program, program, program);
	exit(2);
}

int main(int argc, char **argv)
{
	if (argc < 2)
		usage(argv[0]);
	if (!strcmp(argv[1], "prepare")) {
		if (argc < 6 || argc > 7)
			usage(argv[0]);
		prepare_tree(argv[2], parse_u64(argv[3]), parse_u64(argv[4]),
			     parse_u64(argv[5]),
			     argc == 7 ? parse_u64(argv[6]) : 1024);
		return 0;
	}
	if (!strcmp(argv[1], "run")) {
		if (argc < 11 || argc > 12)
			usage(argv[0]);
		run_sweep(argv[2], parse_u64(argv[3]), parse_u64(argv[4]),
			  parse_u64(argv[5]), parse_u64(argv[6]),
			  parse_u64(argv[7]), parse_u64(argv[8]),
			  parse_u64(argv[9]), parse_u64(argv[10]),
			  argc == 12 ? parse_u64(argv[11]) : 1);
		return 0;
	}
	if (!strcmp(argv[1], "modes")) {
		if (argc != 5)
			usage(argv[0]);
		show_modes(argv[2], parse_u64(argv[3]), parse_u64(argv[4]));
		return 0;
	}
	usage(argv[0]);
	return 2;
}
