/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fstrace_record.c -- LD_PRELOAD recorder for FS-syscall replay.
 *
 *   LD_PRELOAD=./libfstrace_record.so FSTRACE_OUT=/tmp/x.trace <prog>
 *
 * Captures the canonical FS-mutation/read syscalls libc dispatches:
 *
 *   open openat creat close
 *   read pread write pwrite
 *   fsync fdatasync
 *   rename renameat renameat2
 *   unlink unlinkat mkdir mkdirat rmdir
 *   stat lstat fstat fstatat (glibc __xstat / __fxstatat family)
 *   truncate ftruncate
 *   getdents64
 *
 * Trace format -- one verb per line, paths C-string-escaped.  Tokens
 * are whitespace-separated, with literal `->` separating args from
 * return value.  An optional `errno=N` follows the return on failure.
 *
 *   <seq> OPEN     "abs_path"      <flags>     <mode>     -> <rfd>   [errno=N]
 *   <seq> CREAT    "abs_path"      <mode>                 -> <rfd>   [errno=N]
 *   <seq> CLOSE    <rfd>                                  -> <ret>   [errno=N]
 *   <seq> READ     <rfd>           <nbytes>               -> <n>     [errno=N]
 *   <seq> PREAD    <rfd>           <nbytes>    <offset>   -> <n>     [errno=N]
 *   <seq> WRITE    <rfd>           <nbytes>               -> <n>     [errno=N]
 *   <seq> PWRITE   <rfd>           <nbytes>    <offset>   -> <n>     [errno=N]
 *   <seq> FSYNC    <rfd>                                  -> <ret>   [errno=N]
 *   <seq> FDATASYNC <rfd>                                 -> <ret>   [errno=N]
 *   <seq> RENAME   "old"           "new"                  -> <ret>   [errno=N]
 *   <seq> UNLINK   "abs_path"                             -> <ret>   [errno=N]
 *   <seq> MKDIR    "abs_path"      <mode>                 -> <ret>   [errno=N]
 *   <seq> RMDIR    "abs_path"                             -> <ret>   [errno=N]
 *   <seq> STAT     "abs_path"                             -> <ret>   [errno=N]
 *   <seq> LSTAT    "abs_path"                             -> <ret>   [errno=N]
 *   <seq> FSTAT    <rfd>                                  -> <ret>   [errno=N]
 *   <seq> FSTATAT  "abs_path"                             -> <ret>   [errno=N]
 *   <seq> TRUNCATE  "abs_path"     <length>               -> <ret>   [errno=N]
 *   <seq> FTRUNCATE <rfd>          <length>               -> <ret>   [errno=N]
 *   <seq> GETDENTS64 <rfd>         <nbytes>               -> <n>     [errno=N]
 *
 * Accuracy contract:
 *  - Paths are resolved to absolute at the moment of the call (relative
 *    paths via `/proc/self/cwd`, *at-form via `/proc/self/fd/<dirfd>`).
 *    No CWD tracking inside the recorder.
 *  - <rfd> is the kernel-returned fd at record time.  The replayer
 *    maintains a recorded-fd -> live-fd mapping.
 *  - <seq> is a globally monotonic atomic counter; gives total order
 *    across threads.  The output is mutex-serialised so each line is
 *    atomic.
 *  - errno is logged whenever the syscall returned <0 (or -1 for
 *    open/creat).  Replayer can use it to grade outcomes.
 *  - Re-entry from inside a hook (e.g. malloc -> writev -> ...) is
 *    suppressed via thread-local guard; we never log our own logs.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

/*
 * glibc >= 2.33 dropped the visible _STAT_VER / __xstat ABI from the
 * default headers; the symbols are still in libc.so but the constant
 * isn't.  Define a fallback that matches the x86_64 ABI (1).
 */
#ifndef _STAT_VER
# define _STAT_VER 1
#endif

/* ---- global state ---- */
static int g_trace_fd = -1;
static atomic_uint_fast64_t g_seq;
static pthread_mutex_t g_write_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread int t_in_hook;

/*
 * Optional prefix filter — only log syscalls whose abs-path falls
 * under FSTRACE_PATH (and fd-only ops whose fd was opened under it).
 * This is essential when recording programs that drag in a giant
 * path search (e.g. Python's site-packages walk): without it the
 * trace is dominated by failed OPENs of irrelevant paths.
 */
static char *g_path_prefix;
static size_t g_path_prefix_len;
/* fd-tracking: g_fd_in_scope[fd] == 1 if the fd was opened under
 * the prefix.  Sized for typical workloads; oversize fds are treated
 * as "in scope" to avoid false drops. */
#define FD_TABLE_SIZE 8192
static unsigned char g_fd_in_scope[FD_TABLE_SIZE];

static int path_in_scope(const char *abs)
{
	if (!g_path_prefix)
		return 1;
	if (!abs)
		return 0;
	return strncmp(abs, g_path_prefix, g_path_prefix_len) == 0;
}

static void mark_fd(int fd, int in_scope)
{
	if (fd < 0 || fd >= FD_TABLE_SIZE)
		return;
	g_fd_in_scope[fd] = (unsigned char)(in_scope ? 1 : 0);
}

static int fd_in_scope(int fd)
{
	if (!g_path_prefix)
		return 1;
	if (fd < 0)
		return 0;
	if (fd >= FD_TABLE_SIZE)
		return 1; /* be conservative for unusually high fds */
	return g_fd_in_scope[fd];
}

static int     (*real_open)(const char *, int, ...);
static int     (*real_open64)(const char *, int, ...);
static int     (*real_openat)(int, const char *, int, ...);
static int     (*real_openat64)(int, const char *, int, ...);
static int     (*real_creat)(const char *, mode_t);
static int     (*real_close)(int);
static ssize_t (*real_read)(int, void *, size_t);
static ssize_t (*real_pread)(int, void *, size_t, off_t);
static ssize_t (*real_pread64)(int, void *, size_t, off64_t);
static ssize_t (*real_write)(int, const void *, size_t);
static ssize_t (*real_pwrite)(int, const void *, size_t, off_t);
static ssize_t (*real_pwrite64)(int, const void *, size_t, off64_t);
static int     (*real_fsync)(int);
static int     (*real_fdatasync)(int);
static int     (*real_rename)(const char *, const char *);
static int     (*real_renameat)(int, const char *, int, const char *);
static int     (*real_renameat2)(int, const char *, int, const char *, unsigned);
static int     (*real_unlink)(const char *);
static int     (*real_unlinkat)(int, const char *, int);
static int     (*real_mkdir)(const char *, mode_t);
static int     (*real_mkdirat)(int, const char *, mode_t);
static int     (*real_rmdir)(const char *);
static int     (*real___xstat)(int, const char *, struct stat *);
static int     (*real___lxstat)(int, const char *, struct stat *);
static int     (*real___fxstat)(int, int, struct stat *);
static int     (*real___fxstatat)(int, int, const char *, struct stat *, int);
/*
 * 64-bit-off variants.  glibc <2.33 exports these for binaries
 * compiled with _FILE_OFFSET_BITS=64; Python (anaconda) uses them.
 * We pass through opaque struct stat64 *.
 */
static int     (*real___xstat64)(int, const char *, void *);
static int     (*real___lxstat64)(int, const char *, void *);
static int     (*real___fxstat64)(int, int, void *);
static int     (*real___fxstatat64)(int, int, const char *, void *, int);
static int     (*real_stat)(const char *, struct stat *);
static int     (*real_lstat)(const char *, struct stat *);
static int     (*real_fstat)(int, struct stat *);
static int     (*real_fstatat)(int, const char *, struct stat *, int);
static int     (*real_truncate)(const char *, off_t);
static int     (*real_ftruncate)(int, off_t);

#define BIND(name) \
	do { \
		if (!real_##name) \
			real_##name = dlsym(RTLD_NEXT, #name); \
	} while (0)

static void __attribute__((constructor)) fstrace_init(void)
{
	const char *path = getenv("FSTRACE_OUT");
	int fd;

	BIND(open); BIND(open64); BIND(openat); BIND(openat64);
	BIND(creat); BIND(close);
	BIND(read); BIND(pread); BIND(pread64);
	BIND(write); BIND(pwrite); BIND(pwrite64);
	BIND(fsync); BIND(fdatasync);
	BIND(rename); BIND(renameat); BIND(renameat2);
	BIND(unlink); BIND(unlinkat);
	BIND(mkdir); BIND(mkdirat); BIND(rmdir);
	BIND(__xstat); BIND(__lxstat); BIND(__fxstat); BIND(__fxstatat);
	BIND(__xstat64); BIND(__lxstat64); BIND(__fxstat64); BIND(__fxstatat64);
	BIND(stat); BIND(lstat); BIND(fstat); BIND(fstatat);
	BIND(truncate); BIND(ftruncate);

	if (!path)
		path = "/tmp/fstrace.out";
	/*
	 * Open with O_APPEND so every process spawned under the same
	 * LD_PRELOAD shares one trace.  The caller is expected to
	 * truncate the file BEFORE launching the workload (e.g.
	 * `: > trace.txt`).  Two writes interleave at line granularity
	 * because POSIX append-mode is per-write atomic up to PIPE_BUF
	 * (4 KiB) — well above our line size.
	 */
	fd = real_open ? real_open(path, O_WRONLY | O_CREAT | O_APPEND, 0644)
	               : open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd < 0) {
		const char *msg = "fstrace: cannot open trace output\n";
		(void)!write(2, msg, strlen(msg));
		_exit(1);
	}
	g_trace_fd = fd;
	atomic_store(&g_seq, 0);
	{
		const char *pp = getenv("FSTRACE_PATH");
		if (pp && pp[0]) {
			g_path_prefix = strdup(pp);
			g_path_prefix_len = strlen(pp);
		}
	}
}

static void __attribute__((destructor)) fstrace_fini(void)
{
	if (g_trace_fd >= 0) {
		fsync(g_trace_fd);
		close(g_trace_fd);
		g_trace_fd = -1;
	}
}

static void emit(const char *buf, size_t n)
{
	if (g_trace_fd < 0)
		return;
	pthread_mutex_lock(&g_write_lock);
	(void)!real_write(g_trace_fd, buf, n);
	pthread_mutex_unlock(&g_write_lock);
}

static size_t escape_into(const char *src, char *dst, size_t cap)
{
	size_t i = 0, j = 0;
	if (!src) {
		if (cap > 0)
			dst[0] = 0;
		return 0;
	}
	while (src[i] && j + 5 < cap) {
		unsigned char c = (unsigned char)src[i++];
		switch (c) {
		case '\\': dst[j++] = '\\'; dst[j++] = '\\'; break;
		case '"':  dst[j++] = '\\'; dst[j++] = '"';  break;
		case '\n': dst[j++] = '\\'; dst[j++] = 'n';  break;
		case '\r': dst[j++] = '\\'; dst[j++] = 'r';  break;
		case '\t': dst[j++] = '\\'; dst[j++] = 't';  break;
		default:
			if (c >= 0x20 && c < 0x7f) {
				dst[j++] = c;
			} else {
				if (j + 4 >= cap)
					goto out;
				j += snprintf(dst + j, cap - j, "\\x%02x", c);
			}
		}
	}
out:
	dst[j] = 0;
	return j;
}

static void resolve_path(int dirfd, const char *path, char *out, size_t cap)
{
	char proc[64];
	ssize_t n;
	size_t plen;

	if (!path) {
		snprintf(out, cap, "(null)");
		return;
	}
	if (path[0] == '/') {
		snprintf(out, cap, "%s", path);
		return;
	}
	if (dirfd == AT_FDCWD) {
		snprintf(proc, sizeof(proc), "/proc/self/cwd");
	} else {
		snprintf(proc, sizeof(proc), "/proc/self/fd/%d", dirfd);
	}
	n = readlink(proc, out, cap - 1);
	if (n < 0) {
		snprintf(out, cap, "(unresolved)/%s", path);
		return;
	}
	out[n] = 0;
	plen = strlen(path);
	if ((size_t)n + 1 + plen + 1 >= cap)
		return;
	out[n] = '/';
	memcpy(out + n + 1, path, plen + 1);
}

#define LINEBUF 8192
#define ENTER() if (!t_in_hook && (t_in_hook = 1, 1))
#define LEAVE() do { t_in_hook = 0; } while (0)

/*
 * Append errno suffix to a line buffer when the syscall failed.
 * `_errno` is taken from `errno` at hook return time.  Caller should
 * have saved errno before any libc call we make.
 */
#define LOG_LINE_HEAD(line_buf, fmt, ...) \
	snprintf(line_buf, sizeof(line_buf), \
	    "%" PRIu64 " " fmt, \
	    (uint64_t)atomic_fetch_add(&g_seq, 1), ##__VA_ARGS__)

#define LOG_OK_RAW(fmt, ...) do { \
	char _line[LINEBUF]; \
	int _n = LOG_LINE_HEAD(_line, fmt "\n", ##__VA_ARGS__); \
	if (_n > 0) emit(_line, (size_t)_n > sizeof(_line) ? sizeof(_line) : (size_t)_n); \
} while (0)

#define LOG_OP(verb_args_fmt, retval, saved_errno, ...) do { \
	char _line[LINEBUF]; \
	int _n; \
	if ((retval) < 0) { \
		_n = snprintf(_line, sizeof(_line), \
		    "%" PRIu64 " " verb_args_fmt " -> %lld errno=%d\n", \
		    (uint64_t)atomic_fetch_add(&g_seq, 1), \
		    ##__VA_ARGS__, (long long)(retval), saved_errno); \
	} else { \
		_n = snprintf(_line, sizeof(_line), \
		    "%" PRIu64 " " verb_args_fmt " -> %lld\n", \
		    (uint64_t)atomic_fetch_add(&g_seq, 1), \
		    ##__VA_ARGS__, (long long)(retval)); \
	} \
	if (_n > 0) emit(_line, (size_t)_n > sizeof(_line) ? sizeof(_line) : (size_t)_n); \
} while (0)

/* --- helpers that build a single resolved+escaped path token -------- */
#define MKABS(dst, dirfd, path) do { \
	char _abs[PATH_MAX]; \
	resolve_path((dirfd), (path), _abs, sizeof(_abs)); \
	escape_into(_abs, (dst), PATH_MAX * 2); \
} while (0)

/*
 * For each open-form syscall, we resolve the abs path, decide whether
 * it's in scope, mark the resulting fd accordingly, and only emit a
 * trace line if in scope.
 */
#define HOOK_OPEN(call_real) do { \
	char p[PATH_MAX * 2]; \
	int in_scope; \
	MKABS(p, dirfd, path); \
	in_scope = path_in_scope(p); \
	if (rc >= 0) mark_fd(rc, in_scope); \
	if (in_scope) \
		LOG_OP("OPEN \"%s\" %d %u", rc, sv, p, flags, (unsigned)mode); \
} while (0)

int open(const char *path, int flags, ...)
{
	mode_t mode = 0;
	int rc, sv;
	int dirfd = AT_FDCWD;
	if (flags & (O_CREAT | O_TMPFILE)) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}
	BIND(open);
	rc = real_open(path, flags, mode);
	sv = errno;
	ENTER() {
		HOOK_OPEN();
		LEAVE();
	}
	errno = sv;
	return rc;
}

int open64(const char *path, int flags, ...)
{
	mode_t mode = 0;
	int rc, sv;
	int dirfd = AT_FDCWD;
	if (flags & (O_CREAT | O_TMPFILE)) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}
	BIND(open64);
	rc = real_open64(path, flags, mode);
	sv = errno;
	ENTER() {
		HOOK_OPEN();
		LEAVE();
	}
	errno = sv;
	return rc;
}

int openat(int dirfd, const char *path, int flags, ...)
{
	mode_t mode = 0;
	int rc, sv;
	if (flags & (O_CREAT | O_TMPFILE)) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}
	BIND(openat);
	rc = real_openat(dirfd, path, flags, mode);
	sv = errno;
	ENTER() {
		HOOK_OPEN();
		LEAVE();
	}
	errno = sv;
	return rc;
}

int openat64(int dirfd, const char *path, int flags, ...)
{
	mode_t mode = 0;
	int rc, sv;
	if (flags & (O_CREAT | O_TMPFILE)) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}
	BIND(openat64);
	rc = real_openat64(dirfd, path, flags, mode);
	sv = errno;
	ENTER() {
		HOOK_OPEN();
		LEAVE();
	}
	errno = sv;
	return rc;
}

int creat(const char *path, mode_t mode)
{
	int rc, sv;
	BIND(creat);
	rc = real_creat(path, mode);
	sv = errno;
	ENTER() {
		char p[PATH_MAX * 2];
		int in_scope;
		MKABS(p, AT_FDCWD, path);
		in_scope = path_in_scope(p);
		if (rc >= 0) mark_fd(rc, in_scope);
		if (in_scope)
			LOG_OP("CREAT \"%s\" %u", rc, sv, p, (unsigned)mode);
		LEAVE();
	}
	errno = sv;
	return rc;
}

int close(int fd)
{
	int rc, sv;
	int in_scope;
	BIND(close);
	in_scope = fd_in_scope(fd);
	rc = real_close(fd);
	sv = errno;
	ENTER() {
		if (in_scope)
			LOG_OP("CLOSE %d", rc, sv, fd);
		mark_fd(fd, 0);
		LEAVE();
	}
	errno = sv;
	return rc;
}

/* ===== I/O — gated on fd's in-scope flag ===== */
#define LOG_IF_FD_IN_SCOPE(verb_fmt, ...) do { \
	if (fd_in_scope(fd)) \
		LOG_OP(verb_fmt, rc, sv, ##__VA_ARGS__); \
} while (0)

ssize_t read(int fd, void *buf, size_t n)
{
	ssize_t rc;
	int sv;
	BIND(read);
	rc = real_read(fd, buf, n);
	sv = errno;
	ENTER() {
		LOG_IF_FD_IN_SCOPE("READ %d %zu", fd, n);
		LEAVE();
	}
	errno = sv;
	return rc;
}

ssize_t pread(int fd, void *buf, size_t n, off_t off)
{
	ssize_t rc;
	int sv;
	BIND(pread);
	rc = real_pread(fd, buf, n, off);
	sv = errno;
	ENTER() {
		LOG_IF_FD_IN_SCOPE("PREAD %d %zu %lld",
				   fd, n, (long long)off);
		LEAVE();
	}
	errno = sv;
	return rc;
}

ssize_t pread64(int fd, void *buf, size_t n, off64_t off)
{
	ssize_t rc;
	int sv;
	BIND(pread64);
	rc = real_pread64(fd, buf, n, off);
	sv = errno;
	ENTER() {
		LOG_IF_FD_IN_SCOPE("PREAD %d %zu %lld",
				   fd, n, (long long)off);
		LEAVE();
	}
	errno = sv;
	return rc;
}

ssize_t write(int fd, const void *buf, size_t n)
{
	ssize_t rc;
	int sv;
	BIND(write);
	rc = real_write(fd, buf, n);
	sv = errno;
	if (fd == g_trace_fd)
		return rc;
	ENTER() {
		LOG_IF_FD_IN_SCOPE("WRITE %d %zu", fd, n);
		LEAVE();
	}
	errno = sv;
	return rc;
}

ssize_t pwrite(int fd, const void *buf, size_t n, off_t off)
{
	ssize_t rc;
	int sv;
	BIND(pwrite);
	rc = real_pwrite(fd, buf, n, off);
	sv = errno;
	ENTER() {
		LOG_IF_FD_IN_SCOPE("PWRITE %d %zu %lld",
				   fd, n, (long long)off);
		LEAVE();
	}
	errno = sv;
	return rc;
}

ssize_t pwrite64(int fd, const void *buf, size_t n, off64_t off)
{
	ssize_t rc;
	int sv;
	BIND(pwrite64);
	rc = real_pwrite64(fd, buf, n, off);
	sv = errno;
	ENTER() {
		LOG_IF_FD_IN_SCOPE("PWRITE %d %zu %lld",
				   fd, n, (long long)off);
		LEAVE();
	}
	errno = sv;
	return rc;
}

int fsync(int fd)
{
	int rc, sv;
	BIND(fsync);
	rc = real_fsync(fd);
	sv = errno;
	ENTER() {
		LOG_IF_FD_IN_SCOPE("FSYNC %d", fd);
		LEAVE();
	}
	errno = sv;
	return rc;
}

int fdatasync(int fd)
{
	int rc, sv;
	BIND(fdatasync);
	rc = real_fdatasync(fd);
	sv = errno;
	ENTER() {
		LOG_IF_FD_IN_SCOPE("FDATASYNC %d", fd);
		LEAVE();
	}
	errno = sv;
	return rc;
}

#define LOG_IF_PATH_IN_SCOPE(p_token, verb_fmt, ...) do { \
	if (path_in_scope(p_token)) \
		LOG_OP(verb_fmt, rc, sv, ##__VA_ARGS__); \
} while (0)

/* ===== rename — emit if EITHER side is in scope (caller may want to
 * see cross-region renames). ===== */
int rename(const char *oldp, const char *newp)
{
	int rc, sv;
	BIND(rename);
	rc = real_rename(oldp, newp);
	sv = errno;
	ENTER() {
		char a[PATH_MAX * 2], b[PATH_MAX * 2];
		MKABS(a, AT_FDCWD, oldp);
		MKABS(b, AT_FDCWD, newp);
		if (path_in_scope(a) || path_in_scope(b))
			LOG_OP("RENAME \"%s\" \"%s\"", rc, sv, a, b);
		LEAVE();
	}
	errno = sv;
	return rc;
}

int renameat(int olddfd, const char *oldp, int newdfd, const char *newp)
{
	int rc, sv;
	BIND(renameat);
	rc = real_renameat(olddfd, oldp, newdfd, newp);
	sv = errno;
	ENTER() {
		char a[PATH_MAX * 2], b[PATH_MAX * 2];
		MKABS(a, olddfd, oldp);
		MKABS(b, newdfd, newp);
		if (path_in_scope(a) || path_in_scope(b))
			LOG_OP("RENAME \"%s\" \"%s\"", rc, sv, a, b);
		LEAVE();
	}
	errno = sv;
	return rc;
}

int renameat2(int olddfd, const char *oldp, int newdfd, const char *newp,
	      unsigned flags)
{
	int rc, sv;
	BIND(renameat2);
	rc = real_renameat2(olddfd, oldp, newdfd, newp, flags);
	sv = errno;
	ENTER() {
		char a[PATH_MAX * 2], b[PATH_MAX * 2];
		MKABS(a, olddfd, oldp);
		MKABS(b, newdfd, newp);
		if (path_in_scope(a) || path_in_scope(b))
			LOG_OP("RENAME \"%s\" \"%s\"", rc, sv, a, b);
		(void)flags;
		LEAVE();
	}
	errno = sv;
	return rc;
}

/* ===== unlink ===== */
int unlink(const char *path)
{
	int rc, sv;
	BIND(unlink);
	rc = real_unlink(path);
	sv = errno;
	ENTER() {
		char p[PATH_MAX * 2];
		MKABS(p, AT_FDCWD, path);
		LOG_IF_PATH_IN_SCOPE(p, "UNLINK \"%s\"", p);
		LEAVE();
	}
	errno = sv;
	return rc;
}

int unlinkat(int dirfd, const char *path, int flags)
{
	int rc, sv;
	BIND(unlinkat);
	rc = real_unlinkat(dirfd, path, flags);
	sv = errno;
	ENTER() {
		char p[PATH_MAX * 2];
		MKABS(p, dirfd, path);
		if (flags & AT_REMOVEDIR)
			LOG_IF_PATH_IN_SCOPE(p, "RMDIR \"%s\"", p);
		else
			LOG_IF_PATH_IN_SCOPE(p, "UNLINK \"%s\"", p);
		LEAVE();
	}
	errno = sv;
	return rc;
}

int mkdir(const char *path, mode_t mode)
{
	int rc, sv;
	BIND(mkdir);
	rc = real_mkdir(path, mode);
	sv = errno;
	ENTER() {
		char p[PATH_MAX * 2];
		MKABS(p, AT_FDCWD, path);
		LOG_IF_PATH_IN_SCOPE(p, "MKDIR \"%s\" %u", p, (unsigned)mode);
		LEAVE();
	}
	errno = sv;
	return rc;
}

int mkdirat(int dirfd, const char *path, mode_t mode)
{
	int rc, sv;
	BIND(mkdirat);
	rc = real_mkdirat(dirfd, path, mode);
	sv = errno;
	ENTER() {
		char p[PATH_MAX * 2];
		MKABS(p, dirfd, path);
		LOG_IF_PATH_IN_SCOPE(p, "MKDIR \"%s\" %u", p, (unsigned)mode);
		LEAVE();
	}
	errno = sv;
	return rc;
}

int rmdir(const char *path)
{
	int rc, sv;
	BIND(rmdir);
	rc = real_rmdir(path);
	sv = errno;
	ENTER() {
		char p[PATH_MAX * 2];
		MKABS(p, AT_FDCWD, path);
		LOG_IF_PATH_IN_SCOPE(p, "RMDIR \"%s\"", p);
		LEAVE();
	}
	errno = sv;
	return rc;
}

/* ===== stat family. glibc may dispatch through __xstat or stat. ===== */
int __xstat(int ver, const char *path, struct stat *st)
{
	int rc, sv;
	BIND(__xstat);
	rc = real___xstat(ver, path, st);
	sv = errno;
	ENTER() {
		char p[PATH_MAX * 2];
		MKABS(p, AT_FDCWD, path);
		LOG_IF_PATH_IN_SCOPE(p, "STAT \"%s\"", p);
		LEAVE();
	}
	errno = sv;
	return rc;
}

int __lxstat(int ver, const char *path, struct stat *st)
{
	int rc, sv;
	BIND(__lxstat);
	rc = real___lxstat(ver, path, st);
	sv = errno;
	ENTER() {
		char p[PATH_MAX * 2];
		MKABS(p, AT_FDCWD, path);
		LOG_IF_PATH_IN_SCOPE(p, "LSTAT \"%s\"", p);
		LEAVE();
	}
	errno = sv;
	return rc;
}

int __fxstat(int ver, int fd, struct stat *st)
{
	int rc, sv;
	BIND(__fxstat);
	rc = real___fxstat(ver, fd, st);
	sv = errno;
	ENTER() {
		LOG_IF_FD_IN_SCOPE("FSTAT %d", fd);
		LEAVE();
	}
	errno = sv;
	return rc;
}

int __fxstatat(int ver, int dirfd, const char *path, struct stat *st, int flags)
{
	int rc, sv;
	BIND(__fxstatat);
	rc = real___fxstatat(ver, dirfd, path, st, flags);
	sv = errno;
	ENTER() {
		char p[PATH_MAX * 2];
		MKABS(p, dirfd, path);
		LOG_IF_PATH_IN_SCOPE(p, "FSTATAT \"%s\"", p);
		(void)flags;
		LEAVE();
	}
	errno = sv;
	return rc;
}

/* ===== 64-bit-off variants for _FILE_OFFSET_BITS=64 binaries ===== */
int __xstat64(int ver, const char *path, void *st)
{
	int rc, sv;
	BIND(__xstat64);
	rc = real___xstat64(ver, path, st);
	sv = errno;
	ENTER() {
		char p[PATH_MAX * 2];
		MKABS(p, AT_FDCWD, path);
		LOG_IF_PATH_IN_SCOPE(p, "STAT \"%s\"", p);
		LEAVE();
	}
	errno = sv;
	return rc;
}

int __lxstat64(int ver, const char *path, void *st)
{
	int rc, sv;
	BIND(__lxstat64);
	rc = real___lxstat64(ver, path, st);
	sv = errno;
	ENTER() {
		char p[PATH_MAX * 2];
		MKABS(p, AT_FDCWD, path);
		LOG_IF_PATH_IN_SCOPE(p, "LSTAT \"%s\"", p);
		LEAVE();
	}
	errno = sv;
	return rc;
}

int __fxstat64(int ver, int fd, void *st)
{
	int rc, sv;
	BIND(__fxstat64);
	rc = real___fxstat64(ver, fd, st);
	sv = errno;
	ENTER() {
		LOG_IF_FD_IN_SCOPE("FSTAT %d", fd);
		LEAVE();
	}
	errno = sv;
	return rc;
}

int __fxstatat64(int ver, int dirfd, const char *path, void *st, int flags)
{
	int rc, sv;
	BIND(__fxstatat64);
	rc = real___fxstatat64(ver, dirfd, path, st, flags);
	sv = errno;
	ENTER() {
		char p[PATH_MAX * 2];
		MKABS(p, dirfd, path);
		LOG_IF_PATH_IN_SCOPE(p, "FSTATAT \"%s\"", p);
		(void)flags;
		LEAVE();
	}
	errno = sv;
	return rc;
}

int stat(const char *path, struct stat *st)
{
	int rc, sv;
	BIND(stat);
	if (real_stat) {
		rc = real_stat(path, st);
	} else {
		BIND(__xstat);
		rc = real___xstat(_STAT_VER, path, st);
	}
	sv = errno;
	ENTER() {
		char p[PATH_MAX * 2];
		MKABS(p, AT_FDCWD, path);
		LOG_IF_PATH_IN_SCOPE(p, "STAT \"%s\"", p);
		LEAVE();
	}
	errno = sv;
	return rc;
}

int lstat(const char *path, struct stat *st)
{
	int rc, sv;
	BIND(lstat);
	if (real_lstat) {
		rc = real_lstat(path, st);
	} else {
		BIND(__lxstat);
		rc = real___lxstat(_STAT_VER, path, st);
	}
	sv = errno;
	ENTER() {
		char p[PATH_MAX * 2];
		MKABS(p, AT_FDCWD, path);
		LOG_IF_PATH_IN_SCOPE(p, "LSTAT \"%s\"", p);
		LEAVE();
	}
	errno = sv;
	return rc;
}

int fstat(int fd, struct stat *st)
{
	int rc, sv;
	BIND(fstat);
	if (real_fstat) {
		rc = real_fstat(fd, st);
	} else {
		BIND(__fxstat);
		rc = real___fxstat(_STAT_VER, fd, st);
	}
	sv = errno;
	ENTER() {
		LOG_IF_FD_IN_SCOPE("FSTAT %d", fd);
		LEAVE();
	}
	errno = sv;
	return rc;
}

int fstatat(int dirfd, const char *path, struct stat *st, int flags)
{
	int rc, sv;
	BIND(fstatat);
	if (real_fstatat) {
		rc = real_fstatat(dirfd, path, st, flags);
	} else {
		BIND(__fxstatat);
		rc = real___fxstatat(_STAT_VER, dirfd, path, st, flags);
	}
	sv = errno;
	ENTER() {
		char p[PATH_MAX * 2];
		MKABS(p, dirfd, path);
		LOG_IF_PATH_IN_SCOPE(p, "FSTATAT \"%s\"", p);
		(void)flags;
		LEAVE();
	}
	errno = sv;
	return rc;
}

/* ===== truncate ===== */
int truncate(const char *path, off_t len)
{
	int rc, sv;
	BIND(truncate);
	rc = real_truncate(path, len);
	sv = errno;
	ENTER() {
		char p[PATH_MAX * 2];
		MKABS(p, AT_FDCWD, path);
		LOG_IF_PATH_IN_SCOPE(p, "TRUNCATE \"%s\" %lld",
				     p, (long long)len);
		LEAVE();
	}
	errno = sv;
	return rc;
}

int ftruncate(int fd, off_t len)
{
	int rc, sv;
	BIND(ftruncate);
	rc = real_ftruncate(fd, len);
	sv = errno;
	ENTER() {
		LOG_IF_FD_IN_SCOPE("FTRUNCATE %d %lld", fd, (long long)len);
		LEAVE();
	}
	errno = sv;
	return rc;
}

ssize_t getdents64(int fd, void *buf, size_t n)
{
	ssize_t rc = syscall(SYS_getdents64, fd, buf, n);
	int sv = errno;
	ENTER() {
		LOG_IF_FD_IN_SCOPE("GETDENTS64 %d %zu", fd, n);
		LEAVE();
	}
	errno = sv;
	return rc;
}
