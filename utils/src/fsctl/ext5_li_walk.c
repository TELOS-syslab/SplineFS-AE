/* SPDX-License-Identifier: GPL-2.0 */
/* ext5_li_walk.c -- recursively classify every directory under a path.
 *
 * For each directory below <root>, asks the kernel via
 * EXT5_IOC_LI_DIR_MODE whether it is MUTABLE / STABLE_ROOT /
 * STABLE_INTERIOR.  Reports totals and (for ROOTs) the entry_count
 * and nav_bytes from EXT5_IOC_LI_DESC_INFO.  Used to diagnose why a
 * workload didn't promote what was expected (which subtrees are
 * actually under SplineFS coverage vs. still on ext4-htree).
 *
 * Usage:
 *   ext5_li_walk <path>                 # summary + top unpromoted dirs
 *   ext5_li_walk --csv <path>           # one CSV row per directory
 *   ext5_li_walk --top=N <path>         # show top-N unpromoted by size
 *
 * Output (summary mode):
 *   total dirs:       1234567
 *     ROOT:               12
 *     INTERIOR:        99421
 *     MUTABLE:       1135134
 *   roots: ino=... entries=... nav=...
 *   top unpromoted (by i_size, fanout~=size/16):
 *     /mnt/test/foo  size=4M  ~262144 children
 *     ...
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

struct ext5_li_dir_mode {
    uint32_t mode;
    uint32_t _pad;
    uint64_t subtree_root_ino;
};
#define EXT5_IOC_LI_DIR_MODE _IOR('f', 0x86, struct ext5_li_dir_mode)

struct ext5_li_desc_info {
    uint64_t base_bytes;
    uint64_t names_bytes;
    uint64_t nav_bytes;
    uint64_t delta_bytes;
    uint64_t delta_used;
    uint64_t generation;
    uint64_t delta_trigger;
    uint64_t delta_records;
    uint64_t delta_appended_inserts;
    uint64_t delta_appended_deletes;
    uint64_t refill_start_op;
    uint64_t refill_start_used_bytes;
    uint64_t refill_span_ops;
    uint64_t tstar_ops;
    uint64_t last_refill_span_ops;
    uint64_t last_refill_projected_ops;
    uint64_t last_refill_tstar_ops;
    uint32_t entry_count;
    uint32_t parent_count;
    uint32_t filter_bits;
    uint32_t radix_count;
    uint32_t spline_count;
    uint32_t spline_epsilon;
    uint32_t radix_bits;
    uint32_t delta_filter_bits;
    uint32_t last_refill_class;
};
#define EXT5_IOC_LI_DESC_INFO _IOR('f', 0x84, struct ext5_li_desc_info)

static struct {
    uint64_t total;
    uint64_t mutable_dirs;
    uint64_t interior_dirs;
    uint64_t root_dirs;
    int csv;
    int top_n;
} G = { .top_n = 20 };

struct unpromoted_entry {
    char path[PATH_MAX];
    off_t isize;
};
static struct unpromoted_entry *G_top;
static int G_top_count;
static int G_top_cap;

struct root_entry {
    char path[PATH_MAX];
    ino_t ino;
    uint32_t entry_count;
    uint32_t parent_count;
    uint64_t nav_bytes;
};
static struct root_entry *G_roots;
static int G_roots_count;
static int G_roots_cap;

static void top_insert(const char *path, off_t isize)
{
    if (G_top_count < G.top_n) {
        if (G_top_count == G_top_cap) {
            G_top_cap = G_top_cap ? G_top_cap * 2 : 64;
            G_top = realloc(G_top, G_top_cap * sizeof(*G_top));
        }
        snprintf(G_top[G_top_count].path, PATH_MAX, "%s", path);
        G_top[G_top_count].isize = isize;
        G_top_count++;
        return;
    }
    /* find smallest and replace if larger */
    int min_i = 0;
    for (int i = 1; i < G_top_count; i++) {
        if (G_top[i].isize < G_top[min_i].isize)
            min_i = i;
    }
    if (isize > G_top[min_i].isize) {
        snprintf(G_top[min_i].path, PATH_MAX, "%s", path);
        G_top[min_i].isize = isize;
    }
}

static void roots_append(const char *path, ino_t ino,
                         const struct ext5_li_desc_info *info)
{
    if (G_roots_count == G_roots_cap) {
        G_roots_cap = G_roots_cap ? G_roots_cap * 2 : 64;
        G_roots = realloc(G_roots, G_roots_cap * sizeof(*G_roots));
    }
    snprintf(G_roots[G_roots_count].path, PATH_MAX, "%s", path);
    G_roots[G_roots_count].ino = ino;
    G_roots[G_roots_count].entry_count = info->entry_count;
    G_roots[G_roots_count].parent_count = info->parent_count;
    G_roots[G_roots_count].nav_bytes = info->nav_bytes;
    G_roots_count++;
}

static int visit(const char *path, const struct stat *st,
                 int typeflag, struct FTW *ftw)
{
    (void)ftw;
    if (typeflag != FTW_D && typeflag != FTW_DP)
        return 0;
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0)
        return 0;
    struct ext5_li_dir_mode mode = {0};
    if (ioctl(fd, EXT5_IOC_LI_DIR_MODE, &mode) < 0) {
        close(fd);
        return 0;
    }
    G.total++;
    if (mode.mode == 1) {
        G.root_dirs++;
        struct ext5_li_desc_info info = {0};
        if (ioctl(fd, EXT5_IOC_LI_DESC_INFO, &info) == 0) {
            roots_append(path, st->st_ino, &info);
            if (G.csv) {
                printf("%s,ROOT,%" PRIuMAX ",%" PRIu32 ",%" PRIu32 ",%" PRIu64 "\n",
                       path, (uintmax_t)st->st_ino, info.entry_count,
                       info.parent_count, info.nav_bytes);
            }
        } else if (G.csv) {
            printf("%s,ROOT,%" PRIuMAX ",,,\n", path,
                   (uintmax_t)st->st_ino);
        }
    } else if (mode.mode == 2) {
        G.interior_dirs++;
        if (G.csv) {
            printf("%s,INTERIOR,%" PRIuMAX ",,,%" PRIu64 "\n",
                   path, (uintmax_t)st->st_ino, mode.subtree_root_ino);
        }
    } else {
        G.mutable_dirs++;
        top_insert(path, st->st_size);
        if (G.csv) {
            printf("%s,MUTABLE,%" PRIuMAX ",,,\n",
                   path, (uintmax_t)st->st_ino);
        }
    }
    close(fd);
    return 0;
}

static int cmp_roots(const void *a, const void *b)
{
    const struct root_entry *ra = a;
    const struct root_entry *rb = b;
    if (ra->entry_count > rb->entry_count) return -1;
    if (ra->entry_count < rb->entry_count) return  1;
    return 0;
}

static int cmp_unpromoted(const void *a, const void *b)
{
    const struct unpromoted_entry *ua = a;
    const struct unpromoted_entry *ub = b;
    if (ua->isize > ub->isize) return -1;
    if (ua->isize < ub->isize) return  1;
    return 0;
}

int main(int argc, char **argv)
{
    const char *root = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--csv"))      G.csv = 1;
        else if (!strncmp(argv[i], "--top=", 6)) G.top_n = atoi(argv[i] + 6);
        else                                root = argv[i];
    }
    if (!root) {
        fprintf(stderr,
                "Usage: %s [--csv] [--top=N] <path>\n", argv[0]);
        return 2;
    }
    if (G.csv) {
        printf("path,mode,inode,entry_count,parent_count,nav_bytes_or_subtree_root\n");
    }
    int rc = nftw(root, visit, 64, FTW_PHYS | FTW_MOUNT);
    if (rc != 0) {
        fprintf(stderr, "nftw: %s\n", strerror(errno));
    }
    if (G.csv)
        return 0;

    printf("total dirs:       %" PRIu64 "\n", G.total);
    printf("  ROOT:           %" PRIu64 "\n", G.root_dirs);
    printf("  INTERIOR:       %" PRIu64 "\n", G.interior_dirs);
    printf("  MUTABLE:        %" PRIu64 "\n", G.mutable_dirs);

    if (G_roots_count) {
        qsort(G_roots, G_roots_count, sizeof(*G_roots), cmp_roots);
        printf("\nroots (top %d by entry_count):\n",
               G_roots_count < 20 ? G_roots_count : 20);
        for (int i = 0; i < G_roots_count && i < 20; i++) {
            printf("  ino=%" PRIuMAX " entries=%" PRIu32
                   " parents=%" PRIu32 " nav=%" PRIu64 "B %s\n",
                   (uintmax_t)G_roots[i].ino,
                   G_roots[i].entry_count,
                   G_roots[i].parent_count,
                   G_roots[i].nav_bytes,
                   G_roots[i].path);
        }
    }

    if (G_top_count) {
        qsort(G_top, G_top_count, sizeof(*G_top), cmp_unpromoted);
        printf("\ntop unpromoted dirs by i_size (fanout~=size/16):\n");
        for (int i = 0; i < G_top_count; i++) {
            printf("  size=%lldB ~%lld children  %s\n",
                   (long long)G_top[i].isize,
                   (long long)(G_top[i].isize / 16),
                   G_top[i].path);
        }
    }
    return 0;
}
