// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../src/ext5/radix_spline.h"

static uint64_t rng_state = UINT64_C(0xd1b54a32d192ed03);

static uint64_t next_random(void)
{
	uint64_t x = rng_state;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	rng_state = x;
	return x;
}

static int u64_cmp(const void *left, const void *right)
{
	const uint64_t a = *(const uint64_t *)left;
	const uint64_t b = *(const uint64_t *)right;

	return a < b ? -1 : a > b;
}

static uint32_t lower_bound(const uint64_t *keys, uint32_t count, uint64_t key)
{
	uint32_t lo = 0, hi = count;

	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;

		if (keys[mid] < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static void check_queries(const struct sfs_rs_model *model,
			  const uint64_t *keys, uint32_t count)
{
	uint32_t i;

	for (i = 0; i < 200000; i++) {
		uint64_t query = next_random();
		uint32_t actual;
		struct sfs_rs_bound bound;

		if (query < keys[0] || query > keys[count - 1])
			continue;
		actual = lower_bound(keys, count, query);
		if (actual == count)
			continue;
		bound = sfs_rs_get_bound(model, query);
		if (actual < bound.begin || actual >= bound.end) {
			fprintf(stderr,
				"absent-key bound failure query=%" PRIu64
				" actual=%u bound=[%u,%u)\n",
				query, actual, bound.begin, bound.end);
			exit(1);
		}
	}
}

static void run_uniform(uint32_t count, uint32_t epsilon)
{
	uint64_t *keys = malloc((size_t)count * sizeof(*keys));
	struct sfs_rs_model *model = NULL;
	uint32_t i;
	int err;

	if (!keys) {
		perror("malloc");
		exit(1);
	}
	for (i = 0; i < count; i++)
		keys[i] = next_random();
	qsort(keys, count, sizeof(*keys), u64_cmp);
	err = sfs_rs_build(keys, count, 0, epsilon, 1, &model);
	if (err) {
		fprintf(stderr, "uniform build failed: %d\n", err);
		exit(1);
	}
	if (sfs_rs_validate(model, keys, count)) {
		fprintf(stderr, "uniform present-key validation failed\n");
		exit(1);
	}
	if (count >= 100000 && model->point_count >= count / 4) {
		fprintf(stderr,
			"degenerate model: %u points for %u uniform keys\n",
			model->point_count, count);
		exit(1);
	}
	check_queries(model, keys, count);
	printf("uniform count=%u epsilon=%u radix=%u points=%u table=%u OK\n",
	       count, epsilon, model->radix_bits, model->point_count,
	       model->radix_count);
	sfs_rs_destroy(model);
	free(keys);
}

static void run_composite(uint32_t parents, uint32_t per_parent)
{
	uint32_t count = parents * per_parent;
	uint64_t *keys = malloc((size_t)count * sizeof(*keys));
	struct sfs_rs_model *model = NULL;
	uint32_t p, i;
	int err;

	if (!keys) {
		perror("malloc");
		exit(1);
	}
	for (p = 0; p < parents; p++) {
		uint64_t *slice = keys + p * per_parent;

		for (i = 0; i < per_parent; i++)
			slice[i] = ((uint64_t)p << 32) | (uint32_t)next_random();
		qsort(slice, per_parent, sizeof(*slice), u64_cmp);
	}
	err = sfs_rs_build(keys, count, 0, 32, 1, &model);
	if (err || sfs_rs_validate(model, keys, count)) {
		fprintf(stderr, "composite validation failed: %d\n", err);
		exit(1);
	}
	check_queries(model, keys, count);
	printf("composite parents=%u entries=%u points=%u table=%u OK\n",
	       parents, count, model->point_count, model->radix_count);
	sfs_rs_destroy(model);
	free(keys);
}

static void run_collisions(void)
{
	uint64_t keys[] = { 1, 2, 2, 2, 3, 5, 8, 13, 21, 34 };
	struct sfs_rs_model *model = NULL;
	int err;

	err = sfs_rs_build(keys, sizeof(keys) / sizeof(keys[0]), 8, 8, 3,
			   &model);
	if (err || sfs_rs_validate(model, keys,
				    sizeof(keys) / sizeof(keys[0]))) {
		fprintf(stderr, "collision validation failed: %d\n", err);
		exit(1);
	}
	sfs_rs_destroy(model);
	model = NULL;
	err = sfs_rs_build(keys, sizeof(keys) / sizeof(keys[0]), 8, 2, 3,
			   &model);
	if (err != -ERANGE) {
		fprintf(stderr, "oversized collision run was not rejected: %d\n",
			err);
		exit(1);
	}
	puts("collision slack and rejection OK");
}

int main(void)
{
	run_uniform(1000, 16);
	run_uniform(100000, 32);
	run_composite(2000, 100);
	run_collisions();
	puts("all radix spline tests passed");
	return 0;
}
