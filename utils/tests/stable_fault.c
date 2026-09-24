// SPDX-License-Identifier: GPL-2.0
/*
 * Test-only on-disk fault injector for a stable directory.
 *
 * FIEMAP resolves the directory file's logical learned-stream offsets while
 * it is mounted. The helper then cleanly unmounts the filesystem before
 * changing the underlying block device, so no kernel test hook or live raw
 * write is needed. It is intended only for disposable QEMU images.
 */
#define _GNU_SOURCE
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <unistd.h>

#include "../../src/ext5/lidir_format.h"

#define MAX_EXTENTS 1024U

static void die(const char *what)
{
	perror(what);
	exit(EXIT_FAILURE);
}

static uint32_t crc32_le_bytes(uint32_t crc, const void *data, size_t len)
{
	const uint8_t *p = data;
	size_t i;

	while (len--) {
		crc ^= *p++;
		for (i = 0; i < 8; i++)
			crc = (crc >> 1) ^
				(0xedb88320U & (uint32_t)-(int32_t)(crc & 1));
	}
	return crc;
}

static int descriptor_valid(const struct lidir_disk_descriptor *desc)
{
	return le32toh(desc->magic) == LIDIR_MAGIC &&
	       le32toh(desc->version) == LIDIR_VERSION &&
	       le32toh(desc->header_bytes) == sizeof(*desc) &&
	       crc32_le_bytes(0, desc, LIDIR_DESC_CSUM_LEN) ==
		le32toh(desc->header_csum);
}

static struct fiemap *map_directory(int fd)
{
	size_t bytes = sizeof(struct fiemap) +
		MAX_EXTENTS * sizeof(struct fiemap_extent);
	struct fiemap *map = calloc(1, bytes);

	if (!map)
		die("calloc fiemap");
	map->fm_start = 0;
	map->fm_length = UINT64_MAX;
	map->fm_flags = FIEMAP_FLAG_SYNC;
	map->fm_extent_count = MAX_EXTENTS;
	if (ioctl(fd, FS_IOC_FIEMAP, map) < 0)
		die("FIEMAP");
	if (!map->fm_mapped_extents) {
		errno = ENODATA;
		die("FIEMAP empty");
	}
	if (map->fm_mapped_extents == MAX_EXTENTS &&
	    !(map->fm_extents[MAX_EXTENTS - 1].fe_flags & FIEMAP_EXTENT_LAST)) {
		errno = E2BIG;
		die("FIEMAP extent limit");
	}
	return map;
}

static void device_io(const struct fiemap *map, int fd, uint64_t logical,
			      void *buffer, size_t len, int write_data)
{
	uint8_t *p = buffer;

	while (len) {
		const struct fiemap_extent *extent = NULL;
		uint64_t physical;
		size_t chunk;
		uint32_t i;

		for (i = 0; i < map->fm_mapped_extents; i++) {
			const struct fiemap_extent *candidate = &map->fm_extents[i];

			if (logical >= candidate->fe_logical &&
			    logical - candidate->fe_logical < candidate->fe_length) {
				extent = candidate;
				break;
			}
		}
		if (!extent ||
		    (extent->fe_flags & (FIEMAP_EXTENT_UNKNOWN |
			FIEMAP_EXTENT_DELALLOC | FIEMAP_EXTENT_ENCODED |
			FIEMAP_EXTENT_DATA_ENCRYPTED | FIEMAP_EXTENT_NOT_ALIGNED |
			FIEMAP_EXTENT_UNWRITTEN))) {
			errno = ENXIO;
			die("unmapped learned-stream byte");
		}
		physical = extent->fe_physical + logical - extent->fe_logical;
		chunk = extent->fe_length - (logical - extent->fe_logical);
		if (chunk > len)
			chunk = len;
		while (chunk) {
			ssize_t done = write_data ?
				pwrite(fd, p, chunk, (off_t)physical) :
				pread(fd, p, chunk, (off_t)physical);

			if (done < 0 && errno == EINTR)
				continue;
			if (done <= 0)
				die(write_data ? "pwrite device" : "pread device");
			p += done;
			logical += done;
			physical += done;
			len -= done;
			chunk -= done;
		}
	}
}

static int active_descriptor(const struct lidir_disk_descriptor *descs)
{
	int active = -1;
	unsigned int i;

	for (i = 0; i < LIDIR_DESC_SLOTS; i++) {
		if (!descriptor_valid(&descs[i]))
			continue;
		if (active < 0 ||
		    le64toh(descs[i].generation) >
		    le64toh(descs[active].generation))
			active = i;
	}
	return active;
}

static void fault_descriptor_header(const struct fiemap *map, int devfd,
				    struct lidir_disk_descriptor *descs)
{
	int active = active_descriptor(descs);

	if (active < 0) {
		errno = EBADMSG;
		die("no valid descriptor");
	}
	descs[active].header_csum ^= htole32(1);
	device_io(map, devfd, (uint64_t)active * LIDIR_DESC_BYTES,
		  &descs[active], sizeof(descs[active]), 1);
	printf("FAULT_DESCRIPTOR_HEADER slot=%d\n", active);
}

static void fault_descriptor_base(const struct fiemap *map, int devfd,
				  struct lidir_disk_descriptor *descs)
{
	struct lidir_disk_descriptor candidate;
	uint64_t generation;
	int active = active_descriptor(descs);
	int target;

	if (active < 0) {
		errno = EBADMSG;
		die("no valid descriptor");
	}
	candidate = descs[active];
	generation = le64toh(candidate.generation) + 1;
	target = generation & 1;
	candidate.generation = htole64(generation);
	candidate.base_csum ^= htole32(1);
	candidate.header_csum = htole32(
		crc32_le_bytes(0, &candidate, LIDIR_DESC_CSUM_LEN));
	device_io(map, devfd, (uint64_t)target * LIDIR_DESC_BYTES,
		  &candidate, sizeof(candidate), 1);
	printf("FAULT_DESCRIPTOR_BASE slot=%d generation=%" PRIu64 "\n",
	       target, generation);
}

static void fault_delta_tail(const struct fiemap *map, int devfd,
			     const struct lidir_disk_descriptor *descs,
			     int corrupt_sequence)
{
	const struct lidir_disk_descriptor *desc;
	struct lidir_disk_delta_record header;
	struct lidir_disk_delta_record last_header;
	uint64_t region_off, region_bytes, off = 0, last_off = UINT64_MAX;
	uint64_t last_seq = 0;
	int active = active_descriptor(descs);
	unsigned int records = 0;

	if (active < 0) {
		errno = EBADMSG;
		die("no valid descriptor");
	}
	desc = &descs[active];
	region_off = le64toh(desc->delta_off);
	region_bytes = le64toh(desc->delta_bytes);
	while (off + LIDIR_DELTA_HEADER_BYTES <= region_bytes) {
		uint8_t name[UINT8_MAX];
		uint32_t checksum;
		unsigned int bytes;

		device_io(map, devfd, region_off + off, &header,
			  sizeof(header), 0);
		if (header.op == LIDIR_DELTA_NOP)
			break;
		if ((header.op != LIDIR_DELTA_INSERT &&
		     header.op != LIDIR_DELTA_DELETE) || !header.name_len) {
			errno = EBADMSG;
			die("invalid delta before tail");
		}
		bytes = lidir_delta_record_bytes(header.name_len);
		if (off + bytes > region_bytes) {
			errno = EBADMSG;
			die("truncated delta before tail");
		}
		device_io(map, devfd, region_off + off +
			  LIDIR_DELTA_HEADER_BYTES, name, header.name_len, 0);
		checksum = crc32_le_bytes(0,
			(const uint8_t *)&header +
				offsetof(struct lidir_disk_delta_record, op),
			LIDIR_DELTA_HEADER_BYTES -
				offsetof(struct lidir_disk_delta_record, op));
		checksum = crc32_le_bytes(checksum, name, header.name_len);
		if (checksum != le32toh(header.csum)) {
			errno = EBADMSG;
			die("invalid delta checksum before tail");
		}
		last_off = off;
		last_seq = le64toh(header.seq);
		last_header = header;
		records++;
		off += bytes;
	}
	if (records < 2 || last_off == UINT64_MAX) {
		errno = ENODATA;
		die("need at least two delta records");
	}
	if (corrupt_sequence)
		last_header.seq = htole64(last_seq + 7);
	else
		last_header.csum ^= htole32(1);
	device_io(map, devfd, region_off + last_off, &last_header,
		  sizeof(last_header), 1);
	printf("FAULT_DELTA_%s seq=%" PRIu64 " records=%u\n",
	       corrupt_sequence ? "SEQUENCE" : "TAIL", last_seq, records);
}

int main(int argc, char **argv)
{
	struct lidir_disk_descriptor descs[LIDIR_DESC_SLOTS];
	struct fiemap *map;
	int dirfd, devfd;
	unsigned int i;

	if (argc != 5 && !(argc == 6 && !strcmp(argv[4], "pool-check"))) {
		fprintf(stderr, "usage: %s DIR MOUNT DEVICE "
			"descriptor-header|descriptor-base|delta-tail|delta-sequence|delta-capacity|pool-check [USED]\n",
			argv[0]);
		return EXIT_FAILURE;
	}
	dirfd = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dirfd < 0)
		die("open stable directory");
	if (syncfs(dirfd) < 0)
		die("syncfs");
	map = map_directory(dirfd);
	close(dirfd);
	sync();
	if (umount2(argv[2], 0) < 0)
		die("umount fault image");

	devfd = open(argv[3], O_RDWR | O_CLOEXEC | O_SYNC);
	if (devfd < 0)
		die("open block device");
	for (i = 0; i < LIDIR_DESC_SLOTS; i++)
		device_io(map, devfd, (uint64_t)i * LIDIR_DESC_BYTES,
			  &descs[i], sizeof(descs[i]), 0);

	if (!strcmp(argv[4], "descriptor-header"))
		fault_descriptor_header(map, devfd, descs);
	else if (!strcmp(argv[4], "descriptor-base"))
		fault_descriptor_base(map, devfd, descs);
	else if (!strcmp(argv[4], "delta-tail"))
		fault_delta_tail(map, devfd, descs, 0);
	else if (!strcmp(argv[4], "delta-sequence"))
		fault_delta_tail(map, devfd, descs, 1);
	else if (!strcmp(argv[4], "delta-capacity")) {
		int active = active_descriptor(descs);

		if (active < 0 || le64toh(descs[active].delta_bytes) < 4096) {
			errno = EINVAL;
			die("delta capacity fixture");
		}
		/* Apply a valid small reservation after unmount. No maintenance work
		 * is queued until the tested rename starts on the next mount. */
		descs[active].delta_bytes = htole64(4096);
		descs[active].header_csum = htole32(
			crc32_le_bytes(0, &descs[active], LIDIR_DESC_CSUM_LEN));
		device_io(map, devfd, (uint64_t)active * LIDIR_DESC_BYTES,
			  &descs[active], sizeof(descs[active]), 1);
		puts("DELTA_CAPACITY bytes=4096");
	}
	else if (!strcmp(argv[4], "pool-check")) {
		int active = active_descriptor(descs);
		uint32_t used = argc == 6 ? strtoul(argv[5], NULL, 0) : 16;

		if (active < 0 || !le32toh(descs[active].inode_pool_start) ||
		    le32toh(descs[active].inode_pool_count) != 128 ||
		    le32toh(descs[active].inode_pool_used) != used) {
			if (active >= 0)
				fprintf(stderr, "pool count=%u used=%u expected_used=%u\n",
					le32toh(descs[active].inode_pool_count),
					le32toh(descs[active].inode_pool_used), used);
			errno = EINVAL;
			die("persisted pool state");
		}
		printf("POOL_STATE count=128 used=%u slot=%d\n", used, active);
	}
	else {
		errno = EINVAL;
		die("unknown fault mode");
	}
	if (fsync(devfd) < 0)
		die("fsync block device");
	if (ioctl(devfd, BLKFLSBUF, 0) < 0 && errno != EINVAL && errno != ENOTTY)
		die("BLKFLSBUF");
	close(devfd);
	free(map);
	return EXIT_SUCCESS;
}
