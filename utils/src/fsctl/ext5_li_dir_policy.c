// SPDX-License-Identifier: GPL-2.0
/* ext5_li_dir_policy.c -- report why a directory is or is not promoted.
 *
 * ext5_li_walk says what each directory is; this says why.  Use it to find
 * out what keeps a mutable directory mutable, which matters because ext4
 * charges a full block per directory however few entries it holds, so a
 * small unpromoted remainder can dominate a footprint measurement.
 *
 *   ext5_li_dir_policy [--csv] PATH...
 *   find /mnt/tree -type d -print0 | xargs -0 ext5_li_dir_policy --csv
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/ioctl.h>
#include <linux/types.h>

struct ext5_li_dir_policy {
	__u32 mode;
	__u32 blocked_by;
	__u64 subtree_files;
	__u64 subtree_dirs;
	__u64 region_entries;
	__u64 size_floor;
	__u64 rmd;
	__u64 t_star;
	__u64 subtree_root_ino;
	__u64 _pad[3];
};
#define EXT5_IOC_LI_DIR_POLICY _IOR('f', 0x88, struct ext5_li_dir_policy)

static const char *gate_name(unsigned g)
{
	switch (g) {
	case 0: return "none";
	case 1: return "policy-off";
	case 2: return "no-state";
	case 3: return "size-floor";
	case 4: return "quiescence";
	case 5: return "already-stable";
	default: return "?";
	}
}

static const char *mode_name(unsigned m)
{
	switch (m) {
	case 0: return "MUTABLE";
	case 1: return "ROOT";
	case 2: return "INTERIOR";
	default: return "?";
	}
}

int main(int argc, char **argv)
{
	int csv = 0, i, first = 1, rc = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--csv")) { csv = 1; first = i + 1; }
	}
	if (first >= argc) {
		fprintf(stderr, "Usage: %s [--csv] PATH...\n", argv[0]);
		return 2;
	}
	if (csv)
		printf("path,mode,blocked_by,subtree_files,subtree_dirs,"
		       "region_entries,size_floor,rmd,t_star,subtree_root_ino\n");

	for (i = first; i < argc; i++) {
		struct ext5_li_dir_policy p;
		int fd = open(argv[i], O_RDONLY | O_DIRECTORY);

		if (fd < 0) { perror(argv[i]); rc = 1; continue; }
		memset(&p, 0, sizeof(p));
		if (ioctl(fd, EXT5_IOC_LI_DIR_POLICY, &p) != 0) {
			fprintf(stderr, "%s: %s\n", argv[i], strerror(errno));
			close(fd); rc = 1; continue;
		}
		close(fd);
		if (csv)
			printf("%s,%s,%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
			       argv[i], mode_name(p.mode), gate_name(p.blocked_by),
			       (unsigned long long)p.subtree_files,
			       (unsigned long long)p.subtree_dirs,
			       (unsigned long long)p.region_entries,
			       (unsigned long long)p.size_floor,
			       (unsigned long long)p.rmd,
			       (unsigned long long)p.t_star,
			       (unsigned long long)p.subtree_root_ino);
		else
			printf("%s\n  mode=%s blocked_by=%s\n"
			       "  subtree_files=%llu subtree_dirs=%llu\n"
			       "  region_entries=%llu size_floor=%llu\n"
			       "  rmd=%llu t_star=%llu\n",
			       argv[i], mode_name(p.mode), gate_name(p.blocked_by),
			       (unsigned long long)p.subtree_files,
			       (unsigned long long)p.subtree_dirs,
			       (unsigned long long)p.region_entries,
			       (unsigned long long)p.size_floor,
			       (unsigned long long)p.rmd,
			       (unsigned long long)p.t_star);
	}
	return rc;
}
