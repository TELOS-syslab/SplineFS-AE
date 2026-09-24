/* SPDX-License-Identifier: GPL-2.0 */
/*
 * multidir_lookup_bench.c -- cold lookups across many directories.
 *
 * Synthetic trees are ROOT/d0..d{K-1}, each with M names "item.<index>".
 *
 *   prepare  ROOT K M               one inode per name
 *   preplink ROOT K M [POOL]        names hard-linked to POOL inodes
 *   preplist ROOT LIST POOL         a tree from a GUFI path list
 *   run | runneg | runneguniq ROOT K M LOOKUPS [seed]
 *                                   random hits, misses, or unique misses
 *   runlist | runlistneg ROOT LIST LOOKUPS [seed]
 *                                   random hits or misses over LIST's paths
 *   scanlist ROOT LIST              stat every listed path once, in order
 *   runmix ROOT K M OPS WRITE_PROB [seed]
 *                                   lookups mixed with create/unlink pairs
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static uint64_t parse_u64(const char *s)
{
	char *end;
	uint64_t v = strtoull(s, &end, 10);
	if (*end != '\0') { fprintf(stderr, "bad number %s\n", s); exit(1); }
	return v;
}

/* When NAME_LEN env is set (>0), emit fixed-length names with a long
 * shared 'a' prefix and the index as a 20-digit suffix.  All names in a
 * directory then share a long common prefix (forcing a long memcmp on the
 * htree leaf scan) and are equal length (so ext4's name_len pre-check never
 * short-circuits the compare).  Models templated/timestamped/encoded names
 * (ML checkpoints, log files, URL-encoded cache keys).  0 => short names. */
static long g_name_len; /* 0 by default */

static void format_name(char *buf, size_t len, uint64_t index)
{
	if (g_name_len > 0) {
		size_t total = (size_t)g_name_len;
		size_t sufw = 20, prew;
		if (total >= len) total = len - 1;
		if (sufw >= total) sufw = total - 1;
		prew = total - sufw;
		memset(buf, 'a', prew);
		snprintf(buf + prew, len - prew, "%0*" PRIu64, (int)sufw, index);
		return;
	}
	snprintf(buf, len, "item.%020" PRIu64, index);
}

static int mkdir_if_needed(const char *path)
{
	if (mkdir(path, 0755) == 0) return 0;
	if (errno == EEXIST) return 0;
	return -1;
}

static void do_prepare(const char *root, uint64_t K, uint64_t M)
{
	char path[1024];
	uint64_t i, j;

	if (mkdir_if_needed(root) != 0) {
		perror("mkdir root"); exit(1);
	}
	for (i = 0; i < K; i++) {
		snprintf(path, sizeof(path), "%s/d%" PRIu64, root, i);
		if (mkdir_if_needed(path) != 0) {
			perror("mkdir subdir"); exit(1);
		}
		int dirfd = open(path, O_RDONLY | O_DIRECTORY);
		if (dirfd < 0) {
			perror("open subdir"); exit(1);
		}
		for (j = 0; j < M; j++) {
			char name[256];
			format_name(name, sizeof(name), j);
			int fd = openat(dirfd, name,
				O_CREAT | O_TRUNC | O_WRONLY, 0644);
			if (fd < 0) { perror("openat create"); close(dirfd); exit(1); }
			close(fd);
		}
		close(dirfd);
	}
}

/* Routing-bound prepare: K dirs x M distinct names, but every name is a
 * hard link to one of POOL shared inodes (root/.pool/p<k>).  The namespace
 * therefore has K*M distinct names (full directory-index footprint) but only
 * POOL distinct inodes (a few hundred KB, always resident).  This removes the
 * inode-table floor that otherwise dominates the cache-pressure working set,
 * so the only resident-set difference between filesystems is the directory
 * index itself.  `run`/`runneg` are unchanged: they stat by name, resolving
 * through the index and reading a shared (resident) pool inode. */
static void do_prepare_link(const char *root, uint64_t K, uint64_t M,
			    uint64_t pool)
{
	char path[1024];
	uint64_t i, j;

	if (pool == 0) pool = 1;
	if (mkdir_if_needed(root) != 0) { perror("mkdir root"); exit(1); }

	snprintf(path, sizeof(path), "%s/.pool", root);
	if (mkdir_if_needed(path) != 0) { perror("mkdir pool"); exit(1); }
	int poolfd = open(path, O_RDONLY | O_DIRECTORY);
	if (poolfd < 0) { perror("open pool"); exit(1); }
	for (i = 0; i < pool; i++) {
		char pn[64];
		snprintf(pn, sizeof(pn), "p%" PRIu64, i);
		int fd = openat(poolfd, pn, O_CREAT | O_TRUNC | O_WRONLY, 0644);
		if (fd < 0) { perror("openat pool"); exit(1); }
		close(fd);
	}

	uint64_t linked = 0;

	for (i = 0; i < K; i++) {
		snprintf(path, sizeof(path), "%s/d%" PRIu64, root, i);
		if (mkdir_if_needed(path) != 0) { perror("mkdir subdir"); exit(1); }
		int dirfd = open(path, O_RDONLY | O_DIRECTORY);
		if (dirfd < 0) { perror("open subdir"); exit(1); }
		for (j = 0; j < M; j++) {
			char name[256], pn[64];
			/* Spread links over the whole pool: ext4 allows at
			 * most 65,000 links per inode. */
			format_name(name, sizeof(name), j);
			snprintf(pn, sizeof(pn), "p%" PRIu64, linked % pool);
			linked++;
			if (linkat(poolfd, pn, dirfd, name, 0) != 0 &&
			    errno != EEXIST) {
				perror("linkat"); close(dirfd); exit(1);
			}
		}
		close(dirfd);
	}
	close(poolfd);
}

/* Recursive mkdir (like mkdir -p) on a mutable path buffer. */
static int mkdirs(char *path)
{
	char *p;
	for (p = path + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(path, 0755) != 0 && errno != EEXIST) {
				*p = '/'; return -1;
			}
			*p = '/';
		}
	}
	if (mkdir(path, 0755) != 0 && errno != EEXIST) return -1;
	return 0;
}

/* Strip the trailing " d" / " f" type tag from a GUFI list line and return the
 * type char; trims newline.  Returns 0 (no tag) if malformed.  GUFI names are
 * base64 (no spaces), so the last space separates path from type. */
static char line_split_type(char *line)
{
	size_t L = strlen(line);
	char *sp, t;
	while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = '\0';
	if (!L) return 0;
	sp = strrchr(line, ' ');
	if (!sp || sp[1] == '\0') return 0;
	t = sp[1];
	*sp = '\0';
	return t;
}

/* Build a REAL namespace tree from a GUFI-style path list (lines
 * "<abs-path-under-root> d|f").  Directories are created with mkdir -p;
 * files are hard-linked to one of POOL shared inodes (root/.pool/p<k>) so the
 * inode table stays tiny and the only resident-set difference between
 * filesystems is the directory metadata itself.  Preserves the real hierarchy
 * and real filenames, so directory fanout/depth match the source corpus. */
static void do_prepare_list(const char *root, const char *listpath,
			    uint64_t pool)
{
	char line[4096], full[6144];
	FILE *f;
	int poolfd;
	uint64_t i, fi = 0, ndir = 0, nfile = 0;

	if (pool == 0) pool = 1;
	if (mkdir_if_needed(root) != 0) { perror("mkdir root"); exit(1); }
	snprintf(full, sizeof(full), "%s/.pool", root);
	if (mkdir_if_needed(full) != 0) { perror("mkdir pool"); exit(1); }
	poolfd = open(full, O_RDONLY | O_DIRECTORY);
	if (poolfd < 0) { perror("open pool"); exit(1); }
	for (i = 0; i < pool; i++) {
		char pn[64];
		int fd;
		snprintf(pn, sizeof(pn), "p%" PRIu64, i);
		fd = openat(poolfd, pn, O_CREAT | O_TRUNC | O_WRONLY, 0644);
		if (fd < 0) { perror("openat pool"); exit(1); }
		close(fd);
	}

	f = fopen(listpath, "r");
	if (!f) { perror("fopen list"); exit(1); }
	while (fgets(line, sizeof(line), f)) {
		char type = line_split_type(line);
		if (!type) continue;
		snprintf(full, sizeof(full), "%s%s", root, line);
		if (type == 'd') {
			if (mkdirs(full) != 0) {
				perror("mkdirs"); fprintf(stderr, "%s\n", full);
				exit(1);
			}
			ndir++;
		} else {
			char pn[64];
			snprintf(pn, sizeof(pn), "p%" PRIu64, fi % pool);
			fi++;
			if (linkat(poolfd, pn, AT_FDCWD, full, 0) != 0) {
				/* parent dir not yet created (list not strictly
				 * DFS-ordered): create it and retry once. */
				char *slash = strrchr(full, '/');
				if (errno == ENOENT && slash) {
					*slash = '\0';
					if (mkdirs(full) != 0) { perror("mkdirs parent"); exit(1); }
					*slash = '/';
					if (linkat(poolfd, pn, AT_FDCWD, full, 0) != 0 &&
					    errno != EEXIST) { perror("linkat"); fprintf(stderr, "%s\n", full); exit(1); }
				} else if (errno != EEXIST) {
					perror("linkat"); fprintf(stderr, "%s\n", full); exit(1);
				}
			}
			nfile++;
		}
	}
	fclose(f); close(poolfd);
	fprintf(stderr, "preplist: %" PRIu64 " dirs, %" PRIu64 " files\n", ndir, nfile);
}

/* xorshift64* */
static uint64_t xrand(uint64_t *s)
{
	uint64_t x = *s;
	x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
	*s = x;
	return x * 2685821657736338717ULL;
}

static void format_neg_name(char *buf, size_t len, uint64_t index)
{
	/* Different prefix from prepare(), so these never exist by
	 * construction. */
	snprintf(buf, len, "noexist.%020" PRIu64, index);
}

/* mode = 0: random uniform via xrand
 * mode = 1: sequential by name index (fi = 0,1,2,...) so lookups
 *           hit the dirent / index leaves in a cache-friendly order.
 *           Lets the FS-private index work surface above the
 *           inode/dentry cache pressure that swamps L1d at random
 *           access on large N. */
static void do_run(const char *root, uint64_t K, uint64_t M,
		   uint64_t lookups, uint64_t seed, int negative)
{
	int *dirfds;
	uint64_t i;
	struct timespec t0, t1;
	uint64_t hits = 0, misses = 0;
	double seconds, ops;
	uint64_t state = seed ? seed : 1;
	int mode = (seed == (uint64_t)-1) ? 1 : 0;

	dirfds = calloc(K, sizeof(int));
	if (!dirfds) { perror("calloc"); exit(1); }
	for (i = 0; i < K; i++) {
		char path[1024];
		snprintf(path, sizeof(path), "%s/d%" PRIu64, root, i);
		dirfds[i] = open(path, O_RDONLY | O_DIRECTORY);
		if (dirfds[i] < 0) {
			fprintf(stderr, "open %s: %s\n", path, strerror(errno));
			exit(1);
		}
	}

	if (clock_gettime(CLOCK_MONOTONIC_RAW, &t0) != 0) { perror("clock"); exit(1); }
	for (i = 0; i < lookups; i++) {
		uint64_t di, fi;
		char name[256];
		struct stat st;
		int ret;

		if (mode == 1) {
			di = i % K;
			fi = (i / K) % M;
		} else {
			uint64_t r = xrand(&state);
			di = r % K;
			fi = (r / K) % M;
		}

		if (negative)
			format_neg_name(name, sizeof(name),
				negative == 2 ? i + seed * lookups : fi);
		else
			format_name(name, sizeof(name), fi);
		ret = fstatat(dirfds[di], name, &st, AT_SYMLINK_NOFOLLOW);
		if (ret == 0) hits++;
		else misses++;
	}
	if (clock_gettime(CLOCK_MONOTONIC_RAW, &t1) != 0) { perror("clock"); exit(1); }

	seconds = (t1.tv_sec - t0.tv_sec) +
	          (t1.tv_nsec - t0.tv_nsec) / 1e9;
	ops = lookups / seconds;
	for (i = 0; i < K; i++) close(dirfds[i]);
	free(dirfds);

	fprintf(stdout, "lookups=%" PRIu64 " hits=%" PRIu64
		" misses=%" PRIu64 " seconds=%.6f ops_per_sec=%.3f\n",
		lookups, hits, misses, seconds, ops);
}

/* Random cold positive lookups over a REAL tree built by preplist.  Reads the
 * file paths from LISTFILE, reservoir-samples up to RUNLIST_MAX of them (env,
 * default 262144) so the bench's own RSS stays small relative to the memcg cap
 * under test, then issues `lookups` random full-path stats.  The sample spreads
 * across nearly all directories, so each filesystem's full directory working
 * set is exercised; only the per-target leaf differs. */
static void do_run_list(const char *root, const char *listpath,
			uint64_t lookups, uint64_t seed, int negative)
{
	char line[4096];
	FILE *f;
	char **paths;
	size_t cap, n = 0, seen = 0, rootlen = strlen(root);
	uint64_t state = seed ? seed : 1;
	uint64_t hits = 0, misses = 0, i;
	struct timespec t0, t1;
	double seconds;
	const char *envmax = getenv("RUNLIST_MAX");
	size_t maxp = envmax ? (size_t)strtoull(envmax, NULL, 10) : 262144;

	if (maxp == 0) maxp = 262144;
	cap = maxp;
	paths = malloc(cap * sizeof(char *));
	if (!paths) { perror("malloc"); exit(1); }

	f = fopen(listpath, "r");
	if (!f) { perror("fopen list"); exit(1); }
	while (fgets(line, sizeof(line), f)) {
		char type = line_split_type(line);
		size_t plen;
		char *full;
		if (type != 'f') continue;
		plen = strlen(line);
		/* reservoir sampling into a fixed-size set of `maxp` paths */
		if (seen < maxp) {
			full = malloc(rootlen + plen + 1);
			if (!full) { perror("malloc"); exit(1); }
			memcpy(full, root, rootlen);
			memcpy(full + rootlen, line, plen + 1);
			paths[n++] = full;
		} else {
			uint64_t r = xrand(&state) % (seen + 1);
			if (r < maxp) {
				full = malloc(rootlen + plen + 1);
				if (!full) { perror("malloc"); exit(1); }
				memcpy(full, root, rootlen);
				memcpy(full + rootlen, line, plen + 1);
				free(paths[r]);
				paths[r] = full;
			}
		}
		seen++;
	}
	fclose(f);
	if (n == 0) { fprintf(stderr, "runlist: no files in %s\n", listpath); exit(1); }

	if (clock_gettime(CLOCK_MONOTONIC_RAW, &t0) != 0) { perror("clock"); exit(1); }
	for (i = 0; i < lookups; i++) {
		struct stat st;
		const char *p = paths[xrand(&state) % n];
		if (negative) {
			/* descend the real (resident) path, miss at the leaf:
			 * exercises the Bloom-filter fast-negative vs ext4's
			 * dirent-block scan.  Append a suffix the namespace
			 * never contains by construction. */
			char nx[5120];
			snprintf(nx, sizeof(nx), "%s.noexist", p);
			if (stat(nx, &st) == 0) hits++;
			else misses++;
		} else if (stat(p, &st) == 0) hits++;
		else misses++;
	}
	if (clock_gettime(CLOCK_MONOTONIC_RAW, &t1) != 0) { perror("clock"); exit(1); }
	seconds = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	fprintf(stdout, "lookups=%" PRIu64 " hits=%" PRIu64 " misses=%" PRIu64
		" sampled=%zu of %zu seconds=%.6f ops_per_sec=%.3f\n",
		lookups, hits, misses, n, seen, seconds, lookups / seconds);
}


/* The path list is input, not workload.  Reading a 2.7 GB list inside a 2 GB
 * memcg would spend most of the cap on the list's own page cache and leave
 * the directory metadata this benchmark measures with whatever is left, and
 * by a different amount for each tree.  The list is read once and never
 * re-read, so drop it from the cache as it is consumed.
 */
static void drop_input_cache(FILE *f, uint64_t *consumed, size_t added)
{
	*consumed += added;
	if (*consumed < (64u << 20))
		return;
	*consumed = 0;
	fflush(f);
	posix_fadvise(fileno(f), 0, 0, POSIX_FADV_DONTNEED);
}

/* scanlist: stat every path in the list, in list order, exactly once.
 *
 * This is the record-and-replay scan behind the real-namespace figure.
 * do_run_list samples a bounded reservoir and probes it randomly, which
 * measures steady-state random lookup; a scan instead visits the whole tree
 * once in a fixed order.  Replaying a recorded order rather than letting each
 * filesystem walk in its own readdir order is what makes the comparison fair:
 * every filesystem then performs the identical sequence of operations.
 *
 * Directory entries are stat'ed too, since a tree walk stats what it visits.
 */
static void do_scan_list(const char *root, const char *listpath)
{
	char line[4096];
	FILE *f;
	size_t rootlen = strlen(root);
	uint64_t hits = 0, misses = 0, entries = 0, consumed = 0;
	struct timespec t0, t1;
	double seconds;
	char full[8192];

	f = fopen(listpath, "r");
	if (!f) { perror("fopen list"); exit(1); }
	posix_fadvise(fileno(f), 0, 0, POSIX_FADV_SEQUENTIAL);
	if (clock_gettime(CLOCK_MONOTONIC_RAW, &t0) != 0) { perror("clock"); exit(1); }
	while (fgets(line, sizeof(line), f)) {
		struct stat st;
		char type = line_split_type(line);
		drop_input_cache(f, &consumed, strlen(line));
		if (type != 'f' && type != 'd') continue;
		if (rootlen + strlen(line) + 1 > sizeof(full)) continue;
		memcpy(full, root, rootlen);
		memcpy(full + rootlen, line, strlen(line) + 1);
		if (lstat(full, &st) == 0) hits++;
		else misses++;
		entries++;
	}
	if (clock_gettime(CLOCK_MONOTONIC_RAW, &t1) != 0) { perror("clock"); exit(1); }
	fclose(f);
	if (entries == 0) { fprintf(stderr, "scanlist: no entries in %s\n", listpath); exit(1); }
	seconds = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	fprintf(stdout, "entries=%" PRIu64 " hits=%" PRIu64 " misses=%" PRIu64
		" seconds=%.6f ops_per_sec=%.3f\n",
		entries, hits, misses, seconds, entries / seconds);
}

/* runmix: each iteration with probability WRITE_PROB does
 * touch + unlink of a unique scratch name (exercises the
 * delta INSERT + DELETE path under stable backends; ordinary
 * dirent insert + delete elsewhere); otherwise stats an
 * existing name (positive hit).  Both paths report into the
 * same ops/sec count. */
static void do_run_mix(const char *root, uint64_t K, uint64_t M,
		       uint64_t ops_target, uint64_t seed,
		       double write_prob)
{
	int *dirfds;
	uint64_t i;
	struct timespec t0, t1;
	uint64_t hits = 0, writes = 0, errs = 0;
	double seconds, ops;
	uint64_t state = seed ? seed : 1;

	dirfds = calloc(K, sizeof(int));
	if (!dirfds) { perror("calloc"); exit(1); }
	for (i = 0; i < K; i++) {
		char path[1024];
		snprintf(path, sizeof(path), "%s/d%" PRIu64, root, i);
		dirfds[i] = open(path, O_RDONLY | O_DIRECTORY);
		if (dirfds[i] < 0) {
			fprintf(stderr, "open %s: %s\n", path, strerror(errno));
			exit(1);
		}
	}

	if (clock_gettime(CLOCK_MONOTONIC_RAW, &t0) != 0) { perror("clock"); exit(1); }
	for (i = 0; i < ops_target; i++) {
		uint64_t r = xrand(&state);
		uint64_t di = r % K;
		uint64_t fi = (r / K) % M;
		uint64_t roll = xrand(&state) % 1000000ULL;
		int do_write = (roll < (uint64_t)(write_prob * 1000000.0));
		char name[256];

		if (do_write) {
			int fd;
			snprintf(name, sizeof(name),
				"tmp.%020" PRIu64, i);
			fd = openat(dirfds[di], name,
				    O_CREAT | O_WRONLY | O_EXCL, 0644);
			if (fd >= 0) {
				close(fd);
				if (unlinkat(dirfds[di], name, 0) == 0)
					writes++;
				else
					errs++;
			} else {
				errs++;
			}
		} else {
			struct stat st;
			format_name(name, sizeof(name), fi);
			if (fstatat(dirfds[di], name, &st,
				    AT_SYMLINK_NOFOLLOW) == 0)
				hits++;
			else
				errs++;
		}
	}
	if (clock_gettime(CLOCK_MONOTONIC_RAW, &t1) != 0) { perror("clock"); exit(1); }

	seconds = (t1.tv_sec - t0.tv_sec) +
	          (t1.tv_nsec - t0.tv_nsec) / 1e9;
	ops = ops_target / seconds;
	for (i = 0; i < K; i++) close(dirfds[i]);
	free(dirfds);

	fprintf(stdout, "ops=%" PRIu64 " hits=%" PRIu64
		" writes=%" PRIu64 " errs=%" PRIu64
		" seconds=%.6f ops_per_sec=%.3f\n",
		ops_target, hits, writes, errs, seconds, ops);
}

int main(int argc, char **argv)
{
	const char *nl = getenv("NAME_LEN");
	if (nl) {
		g_name_len = strtol(nl, NULL, 10);
		if (g_name_len > 255) g_name_len = 255;   /* ext4 max name */
		if (g_name_len > 0 && g_name_len < 21) g_name_len = 21;
	}
	/* scanlist takes only ROOT and LISTFILE, so dispatch it before the
	 * generic arity guard below, which assumes at least four operands.
	 * Check argc first: with no arguments argv[1] is NULL. */
	if (argc >= 2 && strcmp(argv[1], "scanlist") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: %s scanlist ROOT LISTFILE\n", argv[0]);
			return 2;
		}
		do_scan_list(argv[2], argv[3]);
		return 0;
	}
	if (argc < 5) {
		fprintf(stderr,
			"Usage:\n"
			"  %s prepare ROOT K M\n"
			"  %s preplink ROOT K M [POOL]                    # distinct names, shared inode pool (routing-bound)\n"
			"  %s preplist ROOT LISTFILE [POOL]               # real tree from GUFI path list, files linked to pool\n"
			"  %s runlist ROOT LISTFILE LOOKUPS [seed]        # random cold full-path stats over a preplist tree\n"
			"  %s runlistneg ROOT LISTFILE LOOKUPS [seed]     # negative variant: descend real path, miss at leaf\n"
			"  %s scanlist ROOT LISTFILE                      # stat every entry once in list order (whole-tree scan)\n"
			"  %s run    ROOT K M LOOKUPS [seed]              # all hits\n"
			"  %s runneg ROOT K M LOOKUPS [seed]              # all misses\n"
			"  %s runneguniq ROOT K M LOOKUPS [seed]          # globally unique misses (policy settling)\n"
			"  %s runmix ROOT K M OPS WRITE_PROB [seed]       # mixed read+write\n"
			"             WRITE_PROB in [0.0, 1.0] (e.g. 0.10 = 10%% writes)\n",
			argv[0], argv[0], argv[0], argv[0], argv[0], argv[0],
			argv[0], argv[0], argv[0], argv[0]);
		return 2;
	}
	if (strcmp(argv[1], "prepare") == 0) {
		uint64_t K = parse_u64(argv[3]);
		uint64_t M = parse_u64(argv[4]);
		do_prepare(argv[2], K, M);
		return 0;
	}
	if (strcmp(argv[1], "preplink") == 0) {
		/* preplink ROOT K M [POOL]  -- routing-bound: distinct names,
		 * shared inode pool (default 1024). */
		uint64_t K = parse_u64(argv[3]);
		uint64_t M = parse_u64(argv[4]);
		uint64_t pool = (argc >= 6) ? parse_u64(argv[5]) : 1024;
		do_prepare_link(argv[2], K, M, pool);
		return 0;
	}
	if (strcmp(argv[1], "preplist") == 0) {
		/* preplist ROOT LISTFILE POOL -- build a real tree from a
		 * GUFI-style path list, files hard-linked to a shared pool. */
		uint64_t pool = (argc >= 5) ? parse_u64(argv[4]) : 1024;
		do_prepare_list(argv[2], argv[3], pool);
		return 0;
	}
	if (strcmp(argv[1], "runlist") == 0 || strcmp(argv[1], "runlistneg") == 0) {
		/* runlist[neg] ROOT LISTFILE LOOKUPS [seed] -- random cold
		 * full-path stats over a preplist tree (RUNLIST_MAX caps the
		 * path sample); runlistneg misses at the leaf (Bloom path). */
		int negative = (strcmp(argv[1], "runlistneg") == 0);
		uint64_t L = parse_u64(argv[4]);
		uint64_t seed = (argc >= 6) ? parse_u64(argv[5]) : 0;
		do_run_list(argv[2], argv[3], L, seed, negative);
		return 0;
	}
	if (strcmp(argv[1], "run") == 0 || strcmp(argv[1], "runneg") == 0 ||
	    strcmp(argv[1], "runneguniq") == 0) {
		int negative = strcmp(argv[1], "run") == 0 ? 0 :
			(strcmp(argv[1], "runneguniq") == 0 ? 2 : 1);
		uint64_t K, M, L, seed;
		if (argc < 6) {
			fprintf(stderr, "%s needs LOOKUPS arg\n", argv[1]);
			return 2;
		}
		K = parse_u64(argv[3]);
		M = parse_u64(argv[4]);
		L = parse_u64(argv[5]);
		seed = (argc >= 7) ? parse_u64(argv[6]) : 0;
		do_run(argv[2], K, M, L, seed, negative);
		return 0;
	}
	if (strcmp(argv[1], "runmix") == 0) {
		uint64_t K, M, OPS, seed;
		double wp;
		if (argc < 7) {
			fprintf(stderr, "runmix needs OPS WRITE_PROB args\n");
			return 2;
		}
		K = parse_u64(argv[3]);
		M = parse_u64(argv[4]);
		OPS = parse_u64(argv[5]);
		wp = strtod(argv[6], NULL);
		seed = (argc >= 8) ? parse_u64(argv[7]) : 0;
		if (wp < 0.0 || wp > 1.0) {
			fprintf(stderr, "WRITE_PROB out of range: %g\n", wp);
			return 2;
		}
		do_run_mix(argv[2], K, M, OPS, seed, wp);
		return 0;
	}
	fprintf(stderr, "unknown verb: %s\n", argv[1]);
	return 2;
}
