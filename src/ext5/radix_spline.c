// SPDX-License-Identifier: GPL-2.0
/*
 * Integer RadixSpline: GreedySplineCorridor with exact 128-bit
 * cross-products, since kernel code cannot use floating point.
 */

#include "radix_spline.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/log2.h>
#include <linux/math64.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#define rs_alloc(n, size) kvmalloc_array((n), (size), GFP_KERNEL)
#define rs_zalloc(n, size) kvcalloc((n), (size), GFP_KERNEL)
#define rs_free(ptr) kvfree(ptr)
#else
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#define rs_alloc(n, size) malloc((size_t)(n) * (size))
#define rs_zalloc(n, size) calloc((n), (size))
#define rs_free(ptr) free(ptr)
#endif

typedef unsigned __int128 sfs_u128;

struct sfs_rs_coord {
	u64 x;
	u64 y;
};

struct sfs_rs_builder {
	struct sfs_rs_model *model;
	u32 point_capacity;
	u32 distinct_keys;
	u64 previous_key;
	u32 previous_rank;
	u32 previous_prefix;
	struct sfs_rs_coord upper_limit;
	struct sfs_rs_coord lower_limit;
	struct sfs_rs_coord previous_point;
};

enum sfs_rs_orientation {
	SFS_RS_COLLINEAR = 0,
	SFS_RS_CW = 1,
	SFS_RS_CCW = -1,
};

static u8 sfs_rs_bit_width(u64 value)
{
	u8 width = 0;

	while (value) {
		value >>= 1;
		width++;
	}
	return width;
}

u8 sfs_rs_auto_radix_bits(u32 num_keys)
{
	u8 width = sfs_rs_bit_width(num_keys ? num_keys : 1);
	int bits = (int)width - 8;

	if (bits < 8)
		bits = 8;
	if (bits > 18)
		bits = 18;
	return (u8)bits;
}

static enum sfs_rs_orientation sfs_rs_orientation(u64 dx1, s64 dy1,
						   u64 dx2, s64 dy2)
{
	sfs_u128 left;
	sfs_u128 right;
	bool left_negative = dy1 < 0;
	bool right_negative = dy2 < 0;

	if (left_negative != right_negative)
		return left_negative ? SFS_RS_CCW : SFS_RS_CW;
	left = (sfs_u128)(left_negative ? -dy1 : dy1) * dx2;
	right = (sfs_u128)(right_negative ? -dy2 : dy2) * dx1;
	if (left_negative) {
		sfs_u128 tmp = left;

		left = right;
		right = tmp;
	}

	if (left > right)
		return SFS_RS_CW;
	if (left < right)
		return SFS_RS_CCW;
	return SFS_RS_COLLINEAR;
}

static u32 sfs_rs_prefix(const struct sfs_rs_model *model, u64 key)
{
	u64 delta;
	u64 prefix;

	if (key <= model->min_key)
		return 0;
	delta = key - model->min_key;
	prefix = model->shift_bits ? delta >> model->shift_bits : delta;
	if (prefix + 1 >= model->radix_count)
		prefix = model->radix_count - 2;
	return (u32)prefix;
}

static void sfs_rs_add_radix(struct sfs_rs_builder *builder, u64 key)
{
	struct sfs_rs_model *model = builder->model;
	u32 prefix = sfs_rs_prefix(model, key);
	u32 point_index = model->point_count - 1;
	u32 i;

	if (prefix == builder->previous_prefix)
		return;
	for (i = builder->previous_prefix + 1; i <= prefix; i++)
		model->radix[i] = point_index;
	builder->previous_prefix = prefix;
}

static int sfs_rs_add_point(struct sfs_rs_builder *builder, u64 key, u32 rank)
{
	struct sfs_rs_model *model = builder->model;
	struct sfs_rs_point *point;

	if (model->point_count >= builder->point_capacity)
		return -EOVERFLOW;
	point = &model->points[model->point_count++];
	point->key = key;
	point->rank = rank;
	point->reserved = 0;
	sfs_rs_add_radix(builder, key);
	return 0;
}

static int sfs_rs_add_distinct(struct sfs_rs_builder *builder,
			       u64 key, u32 rank)
{
	struct sfs_rs_model *model = builder->model;
	const struct sfs_rs_point *last;
	u64 upper_y;
	u64 lower_y;
	u64 ul_dx, ll_dx, x_dx;
	s64 ul_dy, ll_dy, y_dy;
	int err;

	if (!builder->distinct_keys) {
		err = sfs_rs_add_point(builder, key, rank);
		if (err)
			return err;
		builder->distinct_keys = 1;
		builder->previous_point.x = key;
		builder->previous_point.y = rank;
		return 0;
	}

	builder->distinct_keys++;
	if (builder->distinct_keys == 2) {
		builder->upper_limit.x = key;
		builder->upper_limit.y = (u64)rank + model->corridor_error;
		builder->lower_limit.x = key;
		builder->lower_limit.y = rank < model->corridor_error ?
			0 : rank - model->corridor_error;
		builder->previous_point.x = key;
		builder->previous_point.y = rank;
		return 0;
	}

	last = &model->points[model->point_count - 1];
	upper_y = (u64)rank + model->corridor_error;
	lower_y = rank < model->corridor_error ?
		0 : rank - model->corridor_error;

	ul_dx = builder->upper_limit.x - last->key;
	ll_dx = builder->lower_limit.x - last->key;
	x_dx = key - last->key;
	ul_dy = (s64)builder->upper_limit.y - (s64)last->rank;
	ll_dy = (s64)builder->lower_limit.y - (s64)last->rank;
	y_dy = (s64)rank - (s64)last->rank;

	if (sfs_rs_orientation(ul_dx, ul_dy, x_dx, y_dy) != SFS_RS_CW ||
	    sfs_rs_orientation(ll_dx, ll_dy, x_dx, y_dy) != SFS_RS_CCW) {
		err = sfs_rs_add_point(builder, builder->previous_point.x,
				       (u32)builder->previous_point.y);
		if (err)
			return err;
		builder->upper_limit.x = key;
		builder->upper_limit.y = upper_y;
		builder->lower_limit.x = key;
		builder->lower_limit.y = lower_y;
	} else {
		s64 upper_dy = (s64)upper_y - (s64)last->rank;
		s64 lower_dy = (s64)lower_y - (s64)last->rank;

		if (sfs_rs_orientation(ul_dx, ul_dy, x_dx, upper_dy) ==
		    SFS_RS_CW) {
			builder->upper_limit.x = key;
			builder->upper_limit.y = upper_y;
		}
		if (sfs_rs_orientation(ll_dx, ll_dy, x_dx, lower_dy) ==
		    SFS_RS_CCW) {
			builder->lower_limit.x = key;
			builder->lower_limit.y = lower_y;
		}
	}

	builder->previous_point.x = key;
	builder->previous_point.y = rank;
	return 0;
}

static void sfs_rs_finish_radix(struct sfs_rs_builder *builder)
{
	struct sfs_rs_model *model = builder->model;
	u32 i = builder->previous_prefix + 1;

	for (; i < model->radix_count; i++)
		model->radix[i] = model->point_count;
}

void sfs_rs_destroy(struct sfs_rs_model *model)
{
	if (!model)
		return;
	rs_free(model->radix);
	rs_free(model->points);
	rs_free(model);
}

int sfs_rs_build(const u64 *keys, u32 num_keys, u8 radix_bits,
		 u32 total_error, u32 collision_run,
		 struct sfs_rs_model **out)
{
	struct sfs_rs_builder builder = { 0 };
	struct sfs_rs_model *model;
	u64 diff;
	u64 max_prefix;
	u32 i;
	int err = 0;

	if (!out || (!keys && num_keys) || !total_error)
		return -EINVAL;
	*out = NULL;
	if (!num_keys)
		return -ENODATA;
	if (!collision_run)
		collision_run = 1;
	if (collision_run > total_error)
		return -ERANGE;
	if (!radix_bits)
		radix_bits = sfs_rs_auto_radix_bits(num_keys);
	if (radix_bits > 24)
		return -EINVAL;

	for (i = 1; i < num_keys; i++)
		if (keys[i] < keys[i - 1])
			return -EINVAL;

	model = rs_zalloc(1, sizeof(*model));
	if (!model)
		return -ENOMEM;
	model->min_key = keys[0];
	model->max_key = keys[num_keys - 1];
	model->num_keys = num_keys;
	model->max_error = total_error;
	model->corridor_error = total_error - (collision_run - 1);
	model->radix_bits = radix_bits;

	diff = model->max_key - model->min_key;
	{
		u8 width = sfs_rs_bit_width(diff);

		model->shift_bits = width > radix_bits ? width - radix_bits : 0;
	}
	max_prefix = model->shift_bits ? diff >> model->shift_bits : diff;
	if (max_prefix > ((1U << 24) - 2)) {
		err = -E2BIG;
		goto fail;
	}
	model->radix_count = (u32)max_prefix + 2;
	model->radix = rs_zalloc(model->radix_count, sizeof(*model->radix));
	model->points = rs_alloc(num_keys, sizeof(*model->points));
	if (!model->radix || !model->points) {
		err = -ENOMEM;
		goto fail;
	}

	builder.model = model;
	builder.point_capacity = num_keys;
	builder.previous_key = keys[0];
	builder.previous_rank = 0;
	builder.previous_prefix = 0;

	for (i = 0; i < num_keys; i++) {
		if (!i || keys[i] != builder.previous_key) {
			err = sfs_rs_add_distinct(&builder, keys[i], i);
			if (err)
				goto fail;
		}
		builder.previous_key = keys[i];
		builder.previous_rank = i;
	}

	if (model->points[model->point_count - 1].key != builder.previous_key ||
	    model->points[model->point_count - 1].rank != builder.previous_rank) {
		err = sfs_rs_add_point(&builder, builder.previous_key,
				       builder.previous_rank);
		if (err)
			goto fail;
	}
	sfs_rs_finish_radix(&builder);

	err = sfs_rs_validate(model, keys, num_keys);
	if (err)
		goto fail;
	*out = model;
	return 0;

fail:
	sfs_rs_destroy(model);
	return err;
}

static u32 sfs_rs_segment(const struct sfs_rs_model *model, u64 key)
{
	u32 prefix;
	u32 begin;
	u32 end;
	u32 lo, hi;

	if (model->point_count <= 1)
		return 0;
	prefix = sfs_rs_prefix(model, key);
	begin = model->radix[prefix];
	end = model->radix[prefix + 1];
	if (begin >= model->point_count)
		begin = model->point_count - 1;
	if (end > model->point_count)
		end = model->point_count;
	if (end <= begin)
		end = begin + 1;

	/* Find the first spline point whose key is >= the lookup key. */
	if (end - begin < 32) {
		u32 pos = begin;

		while (pos < end && model->points[pos].key < key)
			pos++;
		if (pos >= model->point_count)
			pos = model->point_count - 1;
		if (!pos)
			pos = 1;
		return pos;
	}

	lo = begin;
	hi = end;
	while (lo < hi) {
		u32 mid = lo + (hi - lo) / 2;

		if (model->points[mid].key < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo >= model->point_count)
		lo = model->point_count - 1;
	if (!lo)
		lo = 1;
	return lo;
}

u32 sfs_rs_estimate(const struct sfs_rs_model *model, u64 key)
{
	u32 segment;
	const struct sfs_rs_point *down;
	const struct sfs_rs_point *up;
	u64 x_delta;
	u64 x_span;
	u64 y_span;
	u64 offset;
	u64 estimate;

	if (!model || !model->num_keys || !model->point_count)
		return 0;
	if (key <= model->min_key)
		return 0;
	if (key >= model->max_key)
		return model->num_keys - 1;
	if (model->point_count == 1)
		return model->points[0].rank;

	segment = sfs_rs_segment(model, key);
	down = &model->points[segment - 1];
	up = &model->points[segment];
	x_span = up->key - down->key;
	if (!x_span)
		return down->rank;
	x_delta = key - down->key;
	y_span = up->rank - down->rank;
#ifdef __KERNEL__
	offset = mul_u64_u64_div_u64(x_delta, y_span, x_span);
#else
	offset = (u64)(((sfs_u128)x_delta * y_span) / x_span);
#endif
	estimate = down->rank + offset;
	if (estimate >= model->num_keys)
		estimate = model->num_keys - 1;
	return (u32)estimate;
}

struct sfs_rs_bound sfs_rs_get_bound(const struct sfs_rs_model *model,
				      u64 key)
{
	struct sfs_rs_bound bound = { 0, 0 };
	u32 estimate;
	u64 end;

	if (!model || !model->num_keys)
		return bound;
	estimate = sfs_rs_estimate(model, key);
	bound.begin = estimate < model->max_error ?
		0 : estimate - model->max_error;
	end = (u64)estimate + model->max_error + 2;
	bound.end = end > model->num_keys ? model->num_keys : (u32)end;
	return bound;
}

int sfs_rs_validate(const struct sfs_rs_model *model, const u64 *keys,
		    u32 num_keys)
{
	u32 i;

	if (!model || !keys || num_keys != model->num_keys)
		return -EINVAL;
	for (i = 0; i < num_keys; i++) {
		struct sfs_rs_bound bound = sfs_rs_get_bound(model, keys[i]);

		if (i < bound.begin || i >= bound.end)
			return -ERANGE;
	}
	return 0;
}
