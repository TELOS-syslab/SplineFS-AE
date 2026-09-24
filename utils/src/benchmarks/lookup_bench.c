#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/perf_event.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

enum bench_mode {
    MODE_POSITIVE,
    MODE_NEGATIVE,
    MODE_MIXED,
};

struct run_config {
    const char *dir;
    enum bench_mode mode;
    bool unique;
    uint64_t lookups;
    uint64_t files;
    uint32_t miss_percent;
    uint64_t seed;
    uint64_t miss_base;
};

struct perf_counters {
    int cycles_fd;
    int instructions_fd;
    uint64_t cycles;
    uint64_t instructions;
    bool requested;
};

static void usage(FILE *stream, const char *prog)
{
    fprintf(stream,
        "Usage:\n"
        "  %s prepare DIR FILES\n"
        "  %s run DIR MODE LOOKUPS FILES [MISS_PERCENT] [SEED]\n"
        "\n"
        "MODE:\n"
        "  positive  Existing names only\n"
        "  positive-unique  Existing names, each used at most once\n"
        "  negative  Missing names only\n"
        "  negative-unique  Missing names, each used at most once\n"
        "  mixed     Existing and missing names\n"
        "  mixed-unique  Existing and missing names, exact mix, unique hits\n"
        "\n"
        "Examples:\n"
        "  %s prepare /mnt/test/lookup 100000\n"
        "  %s run /mnt/test/lookup positive 5000000 100000\n"
        "  %s run /mnt/test/lookup positive-unique 100000 100000 1\n"
        "  %s run /mnt/test/lookup negative 5000000 100000\n"
        "  %s run /mnt/test/lookup mixed 5000000 100000 10 1\n"
        "\n"
        "Optional environment:\n"
        "  LOOKUP_BENCH_MISS_BASE offsets generated miss names\n"
        "  LOOKUP_BENCH_PERF=1 measures hardware cycles/instructions\n",
        prog, prog, prog, prog, prog, prog, prog);
}

static int perf_event_open(struct perf_event_attr *attr, int group_fd)
{
    return (int)syscall(SYS_perf_event_open, attr, 0, -1, group_fd, 0);
}

static void perf_counters_open(struct perf_counters *counters)
{
    struct perf_event_attr attr = {0};
    const char *requested = getenv("LOOKUP_BENCH_PERF");

    counters->cycles_fd = -1;
    counters->instructions_fd = -1;
    counters->requested = requested && requested[0] &&
        strcmp(requested, "0") != 0 && strcmp(requested, "N") != 0 &&
        strcmp(requested, "n") != 0;
    if (!counters->requested) {
        return;
    }

    attr.type = PERF_TYPE_HARDWARE;
    attr.size = sizeof(attr);
    attr.config = PERF_COUNT_HW_CPU_CYCLES;
    attr.disabled = 1;
    attr.exclude_hv = 1;
    counters->cycles_fd = perf_event_open(&attr, -1);
    if (counters->cycles_fd < 0) {
        perror("perf_event_open cycles");
        exit(1);
    }

    attr.config = PERF_COUNT_HW_INSTRUCTIONS;
    attr.disabled = 0;
    counters->instructions_fd = perf_event_open(&attr, counters->cycles_fd);
    if (counters->instructions_fd < 0) {
        perror("perf_event_open instructions");
        close(counters->cycles_fd);
        exit(1);
    }
}

static void perf_counters_start(struct perf_counters *counters)
{
    if (!counters->requested) {
        return;
    }
    if (ioctl(counters->cycles_fd, PERF_EVENT_IOC_RESET,
              PERF_IOC_FLAG_GROUP) < 0 ||
        ioctl(counters->cycles_fd, PERF_EVENT_IOC_ENABLE,
              PERF_IOC_FLAG_GROUP) < 0) {
        perror("perf counter start");
        exit(1);
    }
}

static void perf_read_exact(int fd, uint64_t *value, const char *name)
{
    ssize_t bytes;

    do {
        bytes = read(fd, value, sizeof(*value));
    } while (bytes < 0 && errno == EINTR);
    if (bytes != (ssize_t)sizeof(*value)) {
        if (bytes >= 0) {
            errno = EIO;
        }
        perror(name);
        exit(1);
    }
}

static void perf_counters_stop(struct perf_counters *counters)
{
    if (!counters->requested) {
        return;
    }
    if (ioctl(counters->cycles_fd, PERF_EVENT_IOC_DISABLE,
              PERF_IOC_FLAG_GROUP) < 0) {
        perror("perf counter stop");
        exit(1);
    }
    perf_read_exact(counters->cycles_fd, &counters->cycles,
                    "read cycles");
    perf_read_exact(counters->instructions_fd, &counters->instructions,
                    "read instructions");
    close(counters->instructions_fd);
    close(counters->cycles_fd);
    counters->instructions_fd = -1;
    counters->cycles_fd = -1;
}

static uint64_t parse_u64(const char *value, const char *name)
{
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno || end == value || *end != '\0') {
        fprintf(stderr, "invalid %s: %s\n", name, value);
        exit(1);
    }
    return (uint64_t)parsed;
}

static uint32_t parse_u32(const char *value, const char *name)
{
    uint64_t parsed = parse_u64(value, name);

    if (parsed > UINT32_MAX) {
        fprintf(stderr, "%s too large: %s\n", name, value);
        exit(1);
    }
    return (uint32_t)parsed;
}

static enum bench_mode parse_mode(const char *value, bool *unique)
{
    *unique = false;

    if (strcmp(value, "positive") == 0) {
        return MODE_POSITIVE;
    }
    if (strcmp(value, "positive-unique") == 0 ||
        strcmp(value, "positive_unique") == 0) {
        *unique = true;
        return MODE_POSITIVE;
    }
    if (strcmp(value, "negative") == 0) {
        return MODE_NEGATIVE;
    }
    if (strcmp(value, "negative-unique") == 0 ||
        strcmp(value, "negative_unique") == 0) {
        *unique = true;
        return MODE_NEGATIVE;
    }
    if (strcmp(value, "mixed") == 0) {
        return MODE_MIXED;
    }
    if (strcmp(value, "mixed-unique") == 0 ||
        strcmp(value, "mixed_unique") == 0) {
        *unique = true;
        return MODE_MIXED;
    }

    fprintf(stderr, "invalid mode: %s\n", value);
    exit(1);
}

static uint64_t next_rand(uint64_t *state)
{
    uint64_t x = *state;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * UINT64_C(2685821657736338717);
}

static double elapsed_seconds(const struct timespec *start,
                              const struct timespec *end)
{
    time_t sec = end->tv_sec - start->tv_sec;
    long nsec = end->tv_nsec - start->tv_nsec;

    if (nsec < 0) {
        sec--;
        nsec += 1000000000L;
    }
    return (double)sec + (double)nsec / 1000000000.0;
}

static void format_existing_name(char *buf, size_t len, uint64_t index)
{
    snprintf(buf, len, "item.%020" PRIu64, index);
}

static void format_missing_name(char *buf, size_t len, uint64_t index)
{
    snprintf(buf, len, "miss.%020" PRIu64, index);
}

static void open_dirfd_or_die(const char *dir, int *dirfd_out)
{
    int dirfd = open(dir, O_RDONLY | O_DIRECTORY);

    if (dirfd < 0) {
        perror("open directory");
        exit(1);
    }
    *dirfd_out = dirfd;
}

static void mkdir_if_needed(const char *dir)
{
    if (mkdir(dir, 0755) == 0) {
        return;
    }
    if (errno == EEXIST) {
        return;
    }
    perror("mkdir");
    exit(1);
}

static void prepare_directory(const char *dir, uint64_t files)
{
    int dirfd;
    uint64_t i;
    char name[64];

    mkdir_if_needed(dir);
    open_dirfd_or_die(dir, &dirfd);

    for (i = 0; i < files; i++) {
        int fd;

        format_existing_name(name, sizeof(name), i);
        fd = openat(dirfd, name, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (fd < 0) {
            perror("openat create");
            close(dirfd);
            exit(1);
        }
        close(fd);
    }

    close(dirfd);
}

static void validate_run_config(const struct run_config *cfg)
{
    uint64_t unique_hits;

    if (cfg->lookups == 0) {
        fprintf(stderr, "lookups must be greater than zero\n");
        exit(1);
    }

    if ((cfg->mode == MODE_POSITIVE || cfg->mode == MODE_MIXED) &&
        cfg->files == 0) {
        fprintf(stderr, "files must be greater than zero for positive or mixed mode\n");
        exit(1);
    }

    if (cfg->mode == MODE_MIXED && cfg->miss_percent > 100) {
        fprintf(stderr, "miss percent must be between 0 and 100\n");
        exit(1);
    }

    if (!cfg->unique) {
        return;
    }

    if (cfg->files > UINT32_MAX) {
        fprintf(stderr, "files too large for unique mode: %" PRIu64 "\n",
            cfg->files);
        exit(1);
    }

    switch (cfg->mode) {
    case MODE_POSITIVE:
        unique_hits = cfg->lookups;
        break;
    case MODE_NEGATIVE:
        unique_hits = 0;
        break;
    case MODE_MIXED:
        unique_hits = cfg->lookups -
            ((cfg->lookups * (uint64_t)cfg->miss_percent) / 100);
        break;
    default:
        exit(1);
    }

    if (unique_hits > cfg->files) {
        fprintf(stderr,
            "unique mode needs at least %" PRIu64 " files, only have %" PRIu64 "\n",
            unique_hits, cfg->files);
        exit(1);
    }
}

static uint64_t env_u64_or_default(const char *name, uint64_t default_value)
{
    const char *value = getenv(name);

    if (!value || !*value) {
        return default_value;
    }
    return parse_u64(value, name);
}

static uint32_t *build_unique_positive_order(uint64_t files, uint64_t *state)
{
    uint32_t *order;
    uint32_t i;

    order = calloc((size_t)files, sizeof(*order));
    if (!order) {
        perror("calloc positive order");
        exit(1);
    }

    for (i = 0; i < files; i++) {
        order[i] = i;
    }

    for (i = (uint32_t)files; i > 1; i--) {
        uint32_t j = (uint32_t)(next_rand(state) % i);
        uint32_t tmp = order[i - 1];

        order[i - 1] = order[j];
        order[j] = tmp;
    }

    return order;
}

static unsigned char *build_unique_mixed_schedule(uint64_t lookups,
                                                  uint32_t miss_percent,
                                                  uint64_t *state)
{
    unsigned char *schedule;
    uint64_t misses = (lookups * (uint64_t)miss_percent) / 100;
    uint64_t i;

    schedule = calloc((size_t)lookups, sizeof(*schedule));
    if (!schedule) {
        perror("calloc mixed schedule");
        exit(1);
    }

    for (i = misses; i < lookups; i++) {
        schedule[i] = 1;
    }

    for (i = lookups; i > 1; i--) {
        uint64_t j = next_rand(state) % i;
        unsigned char tmp = schedule[i - 1];

        schedule[i - 1] = schedule[j];
        schedule[j] = tmp;
    }

    return schedule;
}

static void run_benchmark(const struct run_config *cfg)
{
    int dirfd;
    struct stat st;
    struct timespec start;
    struct timespec end;
    uint64_t state = cfg->seed ? cfg->seed : 1;
    uint64_t positive_ops = 0;
    uint64_t negative_ops = 0;
    uint64_t misses_seen = 0;
    uint64_t positive_seen = 0;
    uint64_t i;
    char name[64];
    double seconds;
    double ops_per_sec;
    double ns_per_op;
    uint32_t *positive_order = NULL;
    unsigned char *mixed_schedule = NULL;
    struct perf_counters counters = {0};

    validate_run_config(cfg);
    open_dirfd_or_die(cfg->dir, &dirfd);

    if (cfg->unique &&
        (cfg->mode == MODE_POSITIVE || cfg->mode == MODE_MIXED)) {
        positive_order = build_unique_positive_order(cfg->files, &state);
    }
    if (cfg->unique && cfg->mode == MODE_MIXED) {
        mixed_schedule = build_unique_mixed_schedule(cfg->lookups,
            cfg->miss_percent, &state);
    }

    perf_counters_open(&counters);

    if (clock_gettime(CLOCK_MONOTONIC_RAW, &start) != 0) {
        perror("clock_gettime start");
        free(mixed_schedule);
        free(positive_order);
        close(dirfd);
        exit(1);
    }

    perf_counters_start(&counters);

    for (i = 0; i < cfg->lookups; i++) {
        bool expect_hit;
        uint64_t index;
        int ret;

        switch (cfg->mode) {
        case MODE_POSITIVE:
            expect_hit = true;
            break;
        case MODE_NEGATIVE:
            expect_hit = false;
            break;
        case MODE_MIXED:
            if (cfg->unique) {
                expect_hit = mixed_schedule[i] != 0;
            } else {
                expect_hit = (next_rand(&state) % 100) >= cfg->miss_percent;
            }
            break;
        default:
            free(mixed_schedule);
            free(positive_order);
            close(dirfd);
            exit(1);
        }

        if (expect_hit) {
            if (cfg->unique) {
                index = positive_order[positive_seen++];
            } else {
                index = next_rand(&state) % cfg->files;
            }
            format_existing_name(name, sizeof(name), index);
            positive_ops++;
        } else {
            index = cfg->files + cfg->miss_base + misses_seen;
            format_missing_name(name, sizeof(name), index);
            negative_ops++;
            misses_seen++;
        }

        ret = fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW);
        if (expect_hit) {
            if (ret != 0) {
                fprintf(stderr, "expected hit for %s: %s\n", name, strerror(errno));
                free(mixed_schedule);
                free(positive_order);
                close(dirfd);
                exit(1);
            }
        } else {
            if (ret == 0) {
                fprintf(stderr, "expected miss for %s but lookup succeeded\n", name);
                free(mixed_schedule);
                free(positive_order);
                close(dirfd);
                exit(1);
            }
            if (errno != ENOENT) {
                fprintf(stderr, "unexpected miss error for %s: %s\n",
                    name, strerror(errno));
                free(mixed_schedule);
                free(positive_order);
                close(dirfd);
                exit(1);
            }
        }
    }

    perf_counters_stop(&counters);

    if (clock_gettime(CLOCK_MONOTONIC_RAW, &end) != 0) {
        perror("clock_gettime end");
        free(mixed_schedule);
        free(positive_order);
        close(dirfd);
        exit(1);
    }

    close(dirfd);
    free(mixed_schedule);
    free(positive_order);

    seconds = elapsed_seconds(&start, &end);
    ops_per_sec = (double)cfg->lookups / seconds;
    ns_per_op = seconds * 1000000000.0 / (double)cfg->lookups;

    printf("lookup_bench\n");
    printf("dir=%s\n", cfg->dir);
    printf("mode=%s\n",
        cfg->mode == MODE_POSITIVE ?
            (cfg->unique ? "positive-unique" : "positive") :
        cfg->mode == MODE_NEGATIVE ?
            (cfg->unique ? "negative-unique" : "negative") :
            (cfg->unique ? "mixed-unique" : "mixed"));
    printf("files=%" PRIu64 "\n", cfg->files);
    printf("lookups=%" PRIu64 "\n", cfg->lookups);
    if (cfg->mode == MODE_MIXED) {
        printf("miss_percent=%u\n", cfg->miss_percent);
    }
    printf("seed=%" PRIu64 "\n", cfg->seed ? cfg->seed : 1);
    printf("miss_base=%" PRIu64 "\n", cfg->miss_base);
    printf("positive_ops=%" PRIu64 "\n", positive_ops);
    printf("negative_ops=%" PRIu64 "\n", negative_ops);
    printf("elapsed_sec=%.6f\n", seconds);
    printf("ops_per_sec=%.3f\n", ops_per_sec);
    printf("ns_per_op=%.3f\n", ns_per_op);
    if (counters.requested) {
        printf("cycles=%" PRIu64 "\n", counters.cycles);
        printf("instructions=%" PRIu64 "\n", counters.instructions);
        printf("cycles_per_op=%.3f\n",
            (double)counters.cycles / (double)cfg->lookups);
        printf("instructions_per_op=%.3f\n",
            (double)counters.instructions / (double)cfg->lookups);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr, argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "prepare") == 0) {
        if (argc != 4) {
            usage(stderr, argv[0]);
            return 1;
        }
        prepare_directory(argv[2], parse_u64(argv[3], "files"));
        return 0;
    }

    if (strcmp(argv[1], "run") == 0) {
        struct run_config cfg;

        if (argc < 6 || argc > 8) {
            usage(stderr, argv[0]);
            return 1;
        }

        cfg.dir = argv[2];
        cfg.mode = parse_mode(argv[3], &cfg.unique);
        cfg.lookups = parse_u64(argv[4], "lookups");
        cfg.files = parse_u64(argv[5], "files");
        cfg.miss_percent = 10;
        cfg.seed = 1;
        cfg.miss_base = env_u64_or_default("LOOKUP_BENCH_MISS_BASE", 0);

        if (cfg.mode == MODE_MIXED) {
            if (argc >= 7) {
                cfg.miss_percent = parse_u32(argv[6], "miss_percent");
            }
            if (argc >= 8) {
                cfg.seed = parse_u64(argv[7], "seed");
            }
        } else if (argc >= 7) {
            cfg.seed = parse_u64(argv[6], "seed");
        }

        run_benchmark(&cfg);
        return 0;
    }

    usage(stderr, argv[0]);
    return 1;
}
