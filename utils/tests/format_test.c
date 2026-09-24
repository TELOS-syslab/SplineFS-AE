// SPDX-License-Identifier: GPL-2.0
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "../../src/ext5/lidir_format.h"

#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "format check failed: %s\n", #expr); \
		return 1; \
	} \
} while (0)

int main(void)
{
	CHECK(sizeof(struct lidir_disk_descriptor) == LIDIR_DESC_BYTES);
	CHECK(sizeof(struct lidir_disk_spline_point) == 16);
	CHECK(sizeof(struct lidir_disk_slot) == 24);
	CHECK(sizeof(struct lidir_disk_delta_record) == 32);
	CHECK(offsetof(struct lidir_disk_descriptor, header_csum) <
	      offsetof(struct lidir_disk_descriptor, reserved));
	CHECK(LIDIR_DESC_CSUM_LEN ==
	      offsetof(struct lidir_disk_descriptor, header_csum));
	CHECK(LIDIR_DESC_CSUM_LEN + sizeof(uint32_t) <= 1024);
	CHECK(offsetof(struct lidir_disk_slot, dx_hash) == 16);
	CHECK(LIDIR_VERSION == 10);
	puts("SplineFS disk format checks passed");
	return 0;
}
