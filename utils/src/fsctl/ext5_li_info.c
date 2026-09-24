/* SPDX-License-Identifier: GPL-2.0 */
/* ext5_li_info.c -- print SplineFS LI-dir descriptor stats. */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

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
	uint64_t resident_payload_bytes;
	uint32_t resident_payload_blocks;
	uint32_t payload_extent_runs;
};

#define EXT5_IOC_LI_DESC_INFO _IOR('f', 0x84, struct ext5_li_desc_info)

int main(int argc, char **argv)
{
	int fd;
	struct ext5_li_desc_info info;
	int format_csv = 0;

	if (argc != 2 && !(argc == 3 && strcmp(argv[1], "--csv") == 0)) {
		fprintf(stderr, "Usage: %s [--csv] <directory>\n", argv[0]);
		return 2;
	}
	const char *path = argv[argc - 1];
	if (argc == 3) format_csv = 1;

	fd = open(path, O_RDONLY | O_DIRECTORY);
	if (fd < 0) { fprintf(stderr, "open: %s\n", strerror(errno)); return 1; }
	if (ioctl(fd, EXT5_IOC_LI_DESC_INFO, &info) < 0) {
		fprintf(stderr, "ioctl: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);

	if (format_csv) {
		printf("%s,%" PRIu32 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
		       ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
		       ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
		       ",%" PRIu64 ",%" PRIu64 "\n",
			path, info.entry_count, info.base_bytes,
			info.nav_bytes, info.names_bytes,
			info.delta_bytes, info.delta_used, info.generation,
			info.delta_trigger, info.delta_records,
			info.delta_appended_inserts,
			info.delta_appended_deletes, info.refill_start_op,
			info.refill_span_ops, info.tstar_ops);
		return 0;
	}
	printf("path:           %s\n", path);
	printf("entry_count:    %" PRIu32 "\n", info.entry_count);
	printf("parent_count:   %" PRIu32 "\n", info.parent_count);
	printf("radix_count:    %" PRIu32 "\n", info.radix_count);
	printf("spline_count:   %" PRIu32 "\n", info.spline_count);
	printf("spline_epsilon: %" PRIu32 "\n", info.spline_epsilon);
	printf("radix_bits:     %" PRIu32 "\n", info.radix_bits);
	printf("filter_bits:    %" PRIu32 "\n", info.filter_bits);
	printf("base_bytes:     %" PRIu64 "\n", info.base_bytes);
	printf("  nav_bytes:    %" PRIu64
		" (filter+radix+spline+parent_index+extent_map)\n",
		info.nav_bytes);
	printf("  names_bytes:  %" PRIu64 "\n", info.names_bytes);
	printf("resident_base:  %" PRIu64 " bytes / %" PRIu32
	       " blocks / %" PRIu32 " extent runs\n",
		info.resident_payload_bytes, info.resident_payload_blocks,
		info.payload_extent_runs);
	printf("delta_bytes:    %" PRIu64 " (reserved)\n", info.delta_bytes);
	printf("delta_used:     %" PRIu64 "\n", info.delta_used);
	printf("delta_trigger:  %" PRIu64 " (logical refill)\n",
		info.delta_trigger);
	printf("delta_records:  %" PRIu64 " (live coalesced)\n",
		info.delta_records);
	printf("delta_inserts:  %" PRIu64 " (appended this generation)\n",
		info.delta_appended_inserts);
	printf("delta_deletes:  %" PRIu64 " (appended this generation)\n",
		info.delta_appended_deletes);
	printf("delta_filter:   %" PRIu32 " bits (in-memory presence filter)\n",
		info.delta_filter_bits);
	printf("generation:     %" PRIu64 "\n", info.generation);
	printf("refill_start:   %" PRIu64 " operations\n", info.refill_start_op);
	printf("refill_base:    %" PRIu64 " delta bytes\n",
		info.refill_start_used_bytes);
	printf("refill_span:    %" PRIu64 " operations\n", info.refill_span_ops);
	printf("tstar:          %" PRIu64 " operations\n", info.tstar_ops);
	printf("last_refill:    %" PRIu64 " observed / %" PRIu64
	       " projected / T* %" PRIu64
	       " / class %" PRIu32 " (1 rapid, 2 slow, 3 noninsert)\n",
		info.last_refill_span_ops, info.last_refill_projected_ops,
		info.last_refill_tstar_ops, info.last_refill_class);
	if (info.entry_count) {
		printf("nav_per_entry:  %.2f bytes\n",
			(double)info.nav_bytes / info.entry_count);
	}
	return 0;
}
