/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fstrace_replay.c -- replay a trace produced by fstrace_record.so
 *
 * Reads the line-oriented format documented at the top of
 * fstrace_record.c and re-issues each syscall against a target tree,
 * maintaining a recorded-fd -> live-fd mapping so that READ/WRITE/
 * FSYNC/CLOSE land on the right kernel fd.
 *
 * Path rewriting:
 *   --map FROM=TO       Rewrite paths whose prefix is FROM to TO.
 *                       May be repeated; first match wins.  The last
 *                       map "/=<target>" can serve as a catch-all.
 *
 * Modes:
 *   --warmup            Build the tree by replaying creates/writes
 *                       only; useful as a "prepare" pass.  Equivalent
 *                       to dropping STAT/LSTAT/READ/PREAD/GETDENTS64
 *                       from the trace.
 *   --no-data           Skip data-only ops (READ/WRITE/PREAD/PWRITE/
 *                       GETDENTS64).  Useful to time metadata-only
 *                       portion of a trace.
 *   --hold-fds          Don't honour CLOSE; leak fds.  Use with
 *                       caution; lets back-to-back FSTAT(rfd) replay
 *                       on the same fd even if the original closed
 *                       it just before.
 *   --bytes-pattern N   Fill the write-buffer with byte N (default
 *                       0xab).
 *
 * Output:
 *   summary line on stderr; one ERROR line per mismatch on stderr;
 *   a single CSV-friendly result on stdout:
 *     replay_ops_per_sec=<rate> replay_mb_per_sec=<bw>
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define FDMAP_SIZE (1 << 20) /* up to 1M unique recorded fds */
static int *g_fdmap;
static char **g_fdpath;
static char *g_writebuf;
static size_t g_writebuf_cap;
static unsigned char g_byte_pattern = 0xab;

struct rewrite { char *from; char *to; size_t from_len; };
static struct rewrite g_rewrites[16];
static int g_nrewrites;

static int g_skip_data;     /* --no-data */
static int g_warmup;        /* --warmup */
static int g_hold_fds;      /* --hold-fds */
/*
 * Periodic-sync throttle.  Big prep replays (millions of writes
 * with no fsync) fill the dirty-page queue faster than writeback
 * drains, eventually triggering 21-25s soft lockups in
 * bvec_alloc/clear_page_erms.  Calling sync_file_range on the
 * just-closed fds every N writes lets writeback keep up without
 * changing the semantic timing of the trace much (we don't fsync,
 * just nudge writeback).  Set to 0 to disable.  Default off.
 */
static unsigned long g_throttle_writes;

static uint64_t s_total, s_done, s_skipped;
static uint64_t s_open, s_close, s_read, s_write, s_fsync;
static uint64_t s_rename, s_unlink, s_mkdir, s_rmdir;
static uint64_t s_stat, s_truncate, s_getdents;
static uint64_t s_bytes_read, s_bytes_written;
static uint64_t s_unexpected_failures;

static void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(2);
}

/*
 * Parse a quoted token from *cursor.  The opening quote must be at
 * **cursor.  Updates *cursor to point past the closing quote.
 * Returns the (in-place modified, NUL-terminated) string content,
 * which is a pointer into the caller's buffer.
 */
static char *parse_quoted(char **cursor)
{
	char *p = *cursor;
	char *start, *r, *w;
	if (*p != '"') return NULL;
	p++;
	start = p;
	w = p;
	r = p;
	while (*r && *r != '"') {
		if (*r == '\\') {
			if (!r[1]) return NULL;
			switch (r[1]) {
			case '\\': *w++ = '\\'; r += 2; break;
			case '"':  *w++ = '"';  r += 2; break;
			case 'n':  *w++ = '\n'; r += 2; break;
			case 'r':  *w++ = '\r'; r += 2; break;
			case 't':  *w++ = '\t'; r += 2; break;
			case 'x': {
				char h[3] = { r[2], r[3], 0 };
				if (!isxdigit((unsigned char)h[0]) ||
				    !isxdigit((unsigned char)h[1]))
					return NULL;
				*w++ = (char)strtol(h, NULL, 16);
				r += 4;
				break;
			}
			default: return NULL;
			}
		} else {
			*w++ = *r++;
		}
	}
	if (*r != '"') return NULL;
	*w = 0;
	*cursor = r + 1;
	return start;
}

static char *skip_ws(char *p)
{
	while (*p == ' ' || *p == '\t') p++;
	return p;
}

/* Apply rewrite to dst (size cap).  Returns 1 on hit, 0 on no-match. */
static int rewrite_path(const char *src, char *dst, size_t cap)
{
	int i;
	for (i = 0; i < g_nrewrites; i++) {
		struct rewrite *r = &g_rewrites[i];
		if (strncmp(src, r->from, r->from_len) == 0) {
			snprintf(dst, cap, "%s%s", r->to, src + r->from_len);
			return 1;
		}
	}
	if (cap > 0)
		snprintf(dst, cap, "%s", src);
	return 0;
}

static int store_fd(uint64_t rfd, int live, const char *abs)
{
	if (rfd >= FDMAP_SIZE) {
		fprintf(stderr, "rfd %" PRIu64 " out of range\n", rfd);
		return -1;
	}
	if (g_fdmap[rfd] >= 0)
		(void)!close(g_fdmap[rfd]);
	g_fdmap[rfd] = live;
	if (g_fdpath[rfd]) { free(g_fdpath[rfd]); g_fdpath[rfd] = NULL; }
	if (abs)
		g_fdpath[rfd] = strdup(abs);
	return 0;
}

static int lookup_fd(uint64_t rfd)
{
	if (rfd >= FDMAP_SIZE) return -1;
	return g_fdmap[rfd];
}

static void drop_fd(uint64_t rfd)
{
	if (rfd >= FDMAP_SIZE) return;
	if (g_fdmap[rfd] >= 0 && !g_hold_fds) {
		(void)!close(g_fdmap[rfd]);
		g_fdmap[rfd] = -1;
	}
	if (g_fdpath[rfd]) { free(g_fdpath[rfd]); g_fdpath[rfd] = NULL; }
}

static void ensure_writebuf(size_t need)
{
	if (need <= g_writebuf_cap) return;
	free(g_writebuf);
	g_writebuf = malloc(need);
	if (!g_writebuf) die("oom write buf %zu", need);
	memset(g_writebuf, g_byte_pattern, need);
	g_writebuf_cap = need;
}

/* Drop the trailing " -> <ret> [errno=N]" tail and parse the ret. */
static int extract_ret(char *line, long long *out_ret, int *out_errno)
{
	char *arrow = strstr(line, " -> ");
	char *p;
	*out_ret = 0;
	*out_errno = 0;
	if (!arrow) return -1;
	*arrow = 0;
	p = arrow + 4;
	*out_ret = strtoll(p, &p, 10);
	while (*p == ' ') p++;
	if (strncmp(p, "errno=", 6) == 0) {
		*out_errno = (int)strtol(p + 6, NULL, 10);
	}
	return 0;
}

/*
 * Replay one parsed line.  `cursor` points at the verb name; we've
 * already stripped the seq prefix and the trailing " -> N" tail.
 */
static void replay_line(char *cursor, long long rec_ret, int rec_errno)
{
	char *verb = cursor;
	char *p;

	(void)rec_errno; /* used only for grading */

	while (*cursor && *cursor != ' ') cursor++;
	if (!*cursor) return;
	*cursor++ = 0;
	cursor = skip_ws(cursor);

	s_total++;

	if (strcmp(verb, "OPEN") == 0) {
		char *qpath, dst[PATH_MAX];
		int flags;
		unsigned mode;
		long long rfd_ll;
		uint64_t rfd;
		int fd;

		qpath = parse_quoted(&cursor);
		if (!qpath) goto bad;
		cursor = skip_ws(cursor);
		flags = (int)strtol(cursor, &p, 10);
		cursor = skip_ws(p);
		mode = (unsigned)strtoul(cursor, &p, 10);
		cursor = p;

		if (rec_ret < 0) {
			s_skipped++;
			return;
		}
		rfd_ll = rec_ret;
		rfd = (uint64_t)rfd_ll;

		rewrite_path(qpath, dst, sizeof(dst));
		fd = open(dst, flags, mode);
		if (fd < 0) {
			s_unexpected_failures++;
			fprintf(stderr, "OPEN \"%s\" flags=%d mode=%o: %s\n",
				dst, flags, mode, strerror(errno));
			return;
		}
		store_fd(rfd, fd, dst);
		s_open++; s_done++;
		return;
	}

	if (strcmp(verb, "CREAT") == 0) {
		char *qpath, dst[PATH_MAX];
		unsigned mode;
		long long rfd_ll;
		uint64_t rfd;
		int fd;

		qpath = parse_quoted(&cursor);
		if (!qpath) goto bad;
		cursor = skip_ws(cursor);
		mode = (unsigned)strtoul(cursor, &p, 10);
		cursor = p;
		if (rec_ret < 0) { s_skipped++; return; }
		rfd_ll = rec_ret;
		rfd = (uint64_t)rfd_ll;
		rewrite_path(qpath, dst, sizeof(dst));
		fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
		if (fd < 0) {
			s_unexpected_failures++;
			fprintf(stderr, "CREAT \"%s\": %s\n",
				dst, strerror(errno));
			return;
		}
		store_fd(rfd, fd, dst);
		s_open++; s_done++;
		return;
	}

	if (strcmp(verb, "CLOSE") == 0) {
		uint64_t rfd = strtoull(cursor, &p, 10);
		drop_fd(rfd);
		s_close++; s_done++;
		return;
	}

	if (strcmp(verb, "READ") == 0 || strcmp(verb, "PREAD") == 0) {
		uint64_t rfd;
		size_t n;
		long long off = 0;
		int fd;
		ssize_t r;

		if (g_skip_data || g_warmup) { s_skipped++; return; }
		rfd = strtoull(cursor, &p, 10);
		cursor = skip_ws(p);
		n = strtoull(cursor, &p, 10);
		cursor = skip_ws(p);
		if (verb[0] == 'P') off = strtoll(cursor, &p, 10);
		fd = lookup_fd(rfd);
		if (fd < 0) { s_skipped++; return; }
		ensure_writebuf(n);
		r = (verb[0] == 'P')
		    ? pread(fd, g_writebuf, n, (off_t)off)
		    : read(fd, g_writebuf, n);
		if (r < 0) {
			s_unexpected_failures++;
			fprintf(stderr, "%s rfd=%" PRIu64 " n=%zu: %s\n",
				verb, rfd, n, strerror(errno));
			return;
		}
		s_bytes_read += r;
		s_read++; s_done++;
		return;
	}

	if (strcmp(verb, "WRITE") == 0 || strcmp(verb, "PWRITE") == 0) {
		uint64_t rfd;
		size_t n;
		long long off = 0;
		int fd;
		ssize_t r;
		size_t total = 0;

		if (g_skip_data) { s_skipped++; return; }
		rfd = strtoull(cursor, &p, 10);
		cursor = skip_ws(p);
		n = strtoull(cursor, &p, 10);
		cursor = skip_ws(p);
		if (verb[0] == 'P') off = strtoll(cursor, &p, 10);
		fd = lookup_fd(rfd);
		if (fd < 0) { s_skipped++; return; }
		ensure_writebuf(n);
		while (total < n) {
			r = (verb[0] == 'P')
			    ? pwrite(fd, g_writebuf + total, n - total,
				     (off_t)off + total)
			    : write(fd, g_writebuf + total, n - total);
			if (r <= 0) {
				s_unexpected_failures++;
				fprintf(stderr, "%s rfd=%" PRIu64
					" n=%zu wrote=%zu: %s\n",
					verb, rfd, n, total, strerror(errno));
				return;
			}
			total += r;
		}
		s_bytes_written += total;
		s_write++; s_done++;
		/* Periodic writeback nudge — see g_throttle_writes
		 * comment.  Use sync_file_range with WRITE so we don't
		 * change ordering semantics. */
		if (g_throttle_writes &&
		    (s_write % g_throttle_writes) == 0) {
			(void)!sync_file_range(fd, 0, 0,
				SYNC_FILE_RANGE_WRITE);
		}
		return;
	}

	if (strcmp(verb, "FSYNC") == 0 || strcmp(verb, "FDATASYNC") == 0) {
		uint64_t rfd = strtoull(cursor, &p, 10);
		int fd = lookup_fd(rfd);
		if (fd < 0) { s_skipped++; return; }
		if (verb[0] == 'F' && verb[1] == 'D')
			(void)!fdatasync(fd);
		else
			(void)!fsync(fd);
		s_fsync++; s_done++;
		return;
	}

	if (strcmp(verb, "RENAME") == 0) {
		char *qa = parse_quoted(&cursor);
		char *qb;
		char a[PATH_MAX], b[PATH_MAX];
		if (!qa) goto bad;
		cursor = skip_ws(cursor);
		qb = parse_quoted(&cursor);
		if (!qb) goto bad;
		rewrite_path(qa, a, sizeof(a));
		rewrite_path(qb, b, sizeof(b));
		if (rename(a, b) != 0 && rec_ret == 0) {
			s_unexpected_failures++;
			fprintf(stderr, "RENAME \"%s\" -> \"%s\": %s\n",
				a, b, strerror(errno));
			return;
		}
		s_rename++; s_done++;
		return;
	}

	if (strcmp(verb, "UNLINK") == 0) {
		char *q = parse_quoted(&cursor);
		char abs[PATH_MAX];
		if (!q) goto bad;
		rewrite_path(q, abs, sizeof(abs));
		if (unlink(abs) != 0 && rec_ret == 0) {
			s_unexpected_failures++;
			fprintf(stderr, "UNLINK \"%s\": %s\n",
				abs, strerror(errno));
			return;
		}
		s_unlink++; s_done++;
		return;
	}

	if (strcmp(verb, "MKDIR") == 0) {
		char *q = parse_quoted(&cursor);
		char abs[PATH_MAX];
		unsigned mode;
		if (!q) goto bad;
		cursor = skip_ws(cursor);
		mode = (unsigned)strtoul(cursor, &p, 10);
		rewrite_path(q, abs, sizeof(abs));
		if (mkdir(abs, mode) != 0 && rec_ret == 0 && errno != EEXIST) {
			s_unexpected_failures++;
			fprintf(stderr, "MKDIR \"%s\": %s\n",
				abs, strerror(errno));
			return;
		}
		s_mkdir++; s_done++;
		return;
	}

	if (strcmp(verb, "RMDIR") == 0) {
		char *q = parse_quoted(&cursor);
		char abs[PATH_MAX];
		if (!q) goto bad;
		rewrite_path(q, abs, sizeof(abs));
		if (rmdir(abs) != 0 && rec_ret == 0) {
			s_unexpected_failures++;
			fprintf(stderr, "RMDIR \"%s\": %s\n",
				abs, strerror(errno));
			return;
		}
		s_rmdir++; s_done++;
		return;
	}

	if (strcmp(verb, "STAT") == 0 || strcmp(verb, "LSTAT") == 0 ||
	    strcmp(verb, "FSTATAT") == 0) {
		char *q = parse_quoted(&cursor);
		char abs[PATH_MAX];
		struct stat st;
		int rc;
		if (g_warmup) { s_skipped++; return; }
		if (!q) goto bad;
		rewrite_path(q, abs, sizeof(abs));
		rc = (verb[0] == 'L')
		    ? lstat(abs, &st)
		    : stat(abs, &st);
		(void)rc;
		s_stat++; s_done++;
		return;
	}

	if (strcmp(verb, "FSTAT") == 0) {
		uint64_t rfd = strtoull(cursor, &p, 10);
		int fd = lookup_fd(rfd);
		struct stat st;
		if (fd < 0) { s_skipped++; return; }
		if (g_warmup) { s_skipped++; return; }
		(void)!fstat(fd, &st);
		s_stat++; s_done++;
		return;
	}

	if (strcmp(verb, "TRUNCATE") == 0) {
		char *q = parse_quoted(&cursor);
		char abs[PATH_MAX];
		long long len;
		if (!q) goto bad;
		cursor = skip_ws(cursor);
		len = strtoll(cursor, &p, 10);
		rewrite_path(q, abs, sizeof(abs));
		(void)!truncate(abs, (off_t)len);
		s_truncate++; s_done++;
		return;
	}

	if (strcmp(verb, "FTRUNCATE") == 0) {
		uint64_t rfd = strtoull(cursor, &p, 10);
		long long len;
		int fd = lookup_fd(rfd);
		cursor = skip_ws(p);
		len = strtoll(cursor, &p, 10);
		if (fd < 0) { s_skipped++; return; }
		(void)!ftruncate(fd, (off_t)len);
		s_truncate++; s_done++;
		return;
	}

	if (strcmp(verb, "GETDENTS64") == 0) {
		uint64_t rfd = strtoull(cursor, &p, 10);
		size_t n;
		int fd = lookup_fd(rfd);
		cursor = skip_ws(p);
		n = strtoull(cursor, &p, 10);
		if (g_skip_data || g_warmup) { s_skipped++; return; }
		if (fd < 0) { s_skipped++; return; }
		ensure_writebuf(n);
		(void)!syscall(SYS_getdents64, fd, g_writebuf, n);
		s_getdents++; s_done++;
		return;
	}

	/* unknown verb */
	s_skipped++;
	return;

bad:
	s_skipped++;
	fprintf(stderr, "parse error: %s ...\n", verb);
}

static void usage(const char *p)
{
	fprintf(stderr,
"Usage: %s [opts] <trace-file>\n"
"  --map FROM=TO          rewrite paths under FROM to live at TO\n"
"  --warmup               replay only ops that materialize the tree\n"
"  --no-data              skip READ/WRITE/PREAD/PWRITE/GETDENTS\n"
"  --hold-fds             do not close fds even when CLOSE is replayed\n"
"  --bytes-pattern N      fill the write-buffer with byte N (default 0xab)\n"
"  --throttle-writes N    sync_file_range every N writes (0=off; recommended\n"
"                         100000 for >10M-write prep traces to avoid 21-25s\n"
"                         soft lockups in bvec_alloc under writeback storm)\n"
, p);
}

static void add_rewrite(const char *spec)
{
	char *eq = strchr(spec, '=');
	if (!eq) die("bad --map: %s (need FROM=TO)", spec);
	if (g_nrewrites >= (int)(sizeof(g_rewrites) / sizeof(g_rewrites[0])))
		die("too many --map");
	g_rewrites[g_nrewrites].from = strndup(spec, eq - spec);
	g_rewrites[g_nrewrites].to = strdup(eq + 1);
	g_rewrites[g_nrewrites].from_len = eq - spec;
	g_nrewrites++;
}

int main(int argc, char **argv)
{
	const char *trace_path = NULL;
	FILE *fp;
	char *line = NULL;
	size_t cap = 0;
	ssize_t n;
	struct timespec t0, t1;
	double elapsed;
	int i;
	int saved_failures;
	uint64_t trace_consumed;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
			add_rewrite(argv[++i]);
		} else if (strcmp(argv[i], "--warmup") == 0) {
			g_warmup = 1;
		} else if (strcmp(argv[i], "--no-data") == 0) {
			g_skip_data = 1;
		} else if (strcmp(argv[i], "--hold-fds") == 0) {
			g_hold_fds = 1;
		} else if (strcmp(argv[i], "--bytes-pattern") == 0 &&
			   i + 1 < argc) {
			g_byte_pattern = (unsigned char)strtoul(
				argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "--throttle-writes") == 0 &&
			   i + 1 < argc) {
			g_throttle_writes = strtoul(argv[++i], NULL, 0);
		} else if (argv[i][0] == '-') {
			usage(argv[0]);
			return 2;
		} else if (!trace_path) {
			trace_path = argv[i];
		} else {
			usage(argv[0]);
			return 2;
		}
	}
	if (!trace_path) { usage(argv[0]); return 2; }

	g_fdmap = malloc(sizeof(int) * FDMAP_SIZE);
	g_fdpath = calloc(FDMAP_SIZE, sizeof(char *));
	if (!g_fdmap || !g_fdpath) die("oom fdmap");
	for (i = 0; i < FDMAP_SIZE; i++) g_fdmap[i] = -1;

	fp = fopen(trace_path, "r");
	if (!fp) die("open %s: %s", trace_path, strerror(errno));
	posix_fadvise(fileno(fp), 0, 0, POSIX_FADV_SEQUENTIAL);

	clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
	saved_failures = 0;
	trace_consumed = 0;
	while ((n = getline(&line, &cap, fp)) > 0) {
		long long ret;
		int en;
		char *cursor;

		/* The trace is input, not workload.  Replaying a multi-gigabyte
		 * trace inside a memcg cap would spend the cap on the trace's
		 * own page cache rather than on the filesystem being measured.
		 * It is read once and never re-read, so drop it as it goes.
		 */
		trace_consumed += (uint64_t)n;
		if (trace_consumed >= (64u << 20)) {
			trace_consumed = 0;
			posix_fadvise(fileno(fp), 0, 0, POSIX_FADV_DONTNEED);
		}

		if (n > 0 && line[n - 1] == '\n') line[n - 1] = 0;
		/* strip seq number */
		cursor = line;
		while (*cursor && *cursor >= '0' && *cursor <= '9') cursor++;
		while (*cursor == ' ' || *cursor == '\t') cursor++;
		if (extract_ret(cursor, &ret, &en) != 0) {
			s_skipped++;
			continue;
		}
		replay_line(cursor, ret, en);
		/* High failure threshold: real workloads (postgres
		 * pgstat tmp file deletion, transient RENAMEs) can
		 * legitimately produce thousands of expected
		 * failures.  Replay should still finish them so the
		 * timing reflects real syscall throughput. */
		if (s_unexpected_failures > 1000000 + (uint64_t)saved_failures) {
			fprintf(stderr,
			    "fstrace_replay: too many failures, aborting\n");
			break;
		}
	}
	clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
	elapsed = (t1.tv_sec - t0.tv_sec) +
	          (t1.tv_nsec - t0.tv_nsec) / 1e9;

	for (i = 0; i < FDMAP_SIZE; i++) {
		if (g_fdmap[i] >= 0) (void)!close(g_fdmap[i]);
	}

	fprintf(stderr,
"== fstrace_replay summary ==\n"
"trace=%s\n"
"total_lines=%" PRIu64 " replayed=%" PRIu64 " skipped=%" PRIu64
" unexpected_failures=%" PRIu64 "\n"
"open=%" PRIu64 " close=%" PRIu64
" read=%" PRIu64 " write=%" PRIu64 " fsync=%" PRIu64
" rename=%" PRIu64 " unlink=%" PRIu64
" mkdir=%" PRIu64 " rmdir=%" PRIu64
" stat=%" PRIu64 " trunc=%" PRIu64 " getdents=%" PRIu64 "\n"
"bytes_read=%" PRIu64 " bytes_written=%" PRIu64 " elapsed=%.3f s\n",
		trace_path,
		s_total, s_done, s_skipped, s_unexpected_failures,
		s_open, s_close, s_read, s_write, s_fsync,
		s_rename, s_unlink, s_mkdir, s_rmdir,
		s_stat, s_truncate, s_getdents,
		s_bytes_read, s_bytes_written, elapsed);

	printf("replay_ops_per_sec=%.3f replay_mb_per_sec=%.3f"
	       " elapsed=%.6f total=%" PRIu64 " replayed=%" PRIu64
	       " unexpected_failures=%" PRIu64 "\n",
	       elapsed > 0 ? (double)s_done / elapsed : 0.0,
	       elapsed > 0 ? (double)(s_bytes_read + s_bytes_written) /
	                     elapsed / (1024.0 * 1024.0) : 0.0,
	       elapsed, s_total, s_done, s_unexpected_failures);

	free(line);
	fclose(fp);
	free(g_writebuf);
	return s_unexpected_failures > 0 ? 1 : 0;
}
