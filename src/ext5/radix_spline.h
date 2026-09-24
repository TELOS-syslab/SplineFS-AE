/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SPLINEFS_RADIX_SPLINE_H
#define _SPLINEFS_RADIX_SPLINE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;
#endif

/* One persisted spline point, 16 bytes. */
struct sfs_rs_point {
	u64 key;
	u32 rank;
	u32 reserved;
};

struct sfs_rs_bound {
	u32 begin;
	u32 end;	/* exclusive */
};

/* Host-endian model; the generation encoder converts it for disk. */
struct sfs_rs_model {
	u64 min_key;
	u64 max_key;
	u32 num_keys;
	u32 max_error;		/* total externally visible error bound */
	u32 corridor_error;	/* model allowance after collision slack */
	u8 radix_bits;
	u8 shift_bits;
	u16 reserved;
	u32 radix_count;
	u32 point_count;
	u32 *radix;
	struct sfs_rs_point *points;
};

u8 sfs_rs_auto_radix_bits(u32 num_keys);

/* Keys must be nondecreasing; collision_run is charged to total_error. */
int sfs_rs_build(const u64 *keys, u32 num_keys, u8 radix_bits,
		 u32 total_error, u32 collision_run,
		 struct sfs_rs_model **out);

void sfs_rs_destroy(struct sfs_rs_model *model);

u32 sfs_rs_estimate(const struct sfs_rs_model *model, u64 key);
struct sfs_rs_bound sfs_rs_get_bound(const struct sfs_rs_model *model,
				      u64 key);

/* Exhaustive present-key validation. Returns zero or -ERANGE. */
int sfs_rs_validate(const struct sfs_rs_model *model, const u64 *keys,
		    u32 num_keys);

#endif /* _SPLINEFS_RADIX_SPLINE_H */
