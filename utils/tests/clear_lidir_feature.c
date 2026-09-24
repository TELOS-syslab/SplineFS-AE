// SPDX-License-Identifier: GPL-2.0
/* Clear SplineFS's incompat bit on a fully demoted disposable test image. */
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define EXT_SUPER_OFFSET 1024
#define EXT_SUPER_BYTES 1024
#define EXT_MAGIC_OFFSET 0x38
#define EXT_INCOMPAT_OFFSET 0x60
#define EXT_CHECKSUM_OFFSET 0x3fc
#define EXT_MAGIC 0xef53
#define EXT_INCOMPAT_LIDIR 0x00100000U

static uint32_t get_le32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
		(uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t get_le16(const unsigned char *p)
{
	return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static void put_le32(unsigned char *p, uint32_t value)
{
	p[0] = value;
	p[1] = value >> 8;
	p[2] = value >> 16;
	p[3] = value >> 24;
}

/* Reflected CRC32C, matching ext5_chksum(~0, super, 0x3fc). */
static uint32_t crc32c(uint32_t crc, const unsigned char *buf, size_t len)
{
	while (len--) {
		unsigned int bit;

		crc ^= *buf++;
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0x82f63b78U & (0U - (crc & 1)));
	}
	return crc;
}

int main(int argc, char **argv)
{
	unsigned char super[EXT_SUPER_BYTES];
	uint32_t incompat, expected, actual;
	int fd;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s IMAGE\n", argv[0]);
		return 2;
	}
	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	if (pread(fd, super, sizeof(super), EXT_SUPER_OFFSET) !=
	    (ssize_t)sizeof(super)) {
		perror("pread superblock");
		close(fd);
		return 1;
	}
	if (get_le16(super + EXT_MAGIC_OFFSET) != EXT_MAGIC) {
		fprintf(stderr, "not an ext filesystem image\n");
		close(fd);
		return 1;
	}
	expected = get_le32(super + EXT_CHECKSUM_OFFSET);
	actual = crc32c(~0U, super, EXT_CHECKSUM_OFFSET);
	if (actual != expected) {
		fprintf(stderr, "invalid input superblock checksum: %08x != %08x\n",
			actual, expected);
		close(fd);
		return 1;
	}
	incompat = get_le32(super + EXT_INCOMPAT_OFFSET);
	if (!(incompat & EXT_INCOMPAT_LIDIR)) {
		fprintf(stderr, "LIDIR incompat bit is not set\n");
		close(fd);
		return 1;
	}
	put_le32(super + EXT_INCOMPAT_OFFSET,
		 incompat & ~EXT_INCOMPAT_LIDIR);
	put_le32(super + EXT_CHECKSUM_OFFSET,
		 crc32c(~0U, super, EXT_CHECKSUM_OFFSET));
	if (pwrite(fd, super, sizeof(super), EXT_SUPER_OFFSET) !=
	    (ssize_t)sizeof(super) || fsync(fd)) {
		perror("write superblock");
		close(fd);
		return 1;
	}
	if (close(fd)) {
		perror("close");
		return 1;
	}
	return 0;
}
