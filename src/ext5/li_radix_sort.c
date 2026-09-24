// SPDX-License-Identifier: GPL-2.0
/* Linear-time ordering for promotion and compaction records. */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "li_radix_sort.h"

#define SFS_LI_RADIX_BITS	16U
#define SFS_LI_RADIX_BUCKETS	(1U << SFS_LI_RADIX_BITS)
#define SFS_LI_RADIX_PASSES	6U

struct sfs_li_sort_record {
	struct sfs_li_sort_prefix order;
	u64 tail;
};

static u16 sfs_li_radix_digit(const struct sfs_li_sort_prefix *record,
			      u32 pass)
{
	/* LSD passes over (PLID, hash32, SipHash-high32). */
	if (pass < 2)
		return record->tiebreak64 >>
			(32 + pass * SFS_LI_RADIX_BITS);
	if (pass < 4)
		return record->tiebreak64 >>
			((pass - 2) * SFS_LI_RADIX_BITS);
	/* key.high32 holds the PLID. */
	return (record->key >> 32) >>
		((pass - 4) * SFS_LI_RADIX_BITS);
}

static bool sfs_li_ordered(const struct sfs_li_sort_prefix *left,
			   const struct sfs_li_sort_prefix *right)
{
	if (left->key != right->key)
		return left->key < right->key;
	if (left->tiebreak64 != right->tiebreak64)
		return left->tiebreak64 < right->tiebreak64;
	return true;
}

int sfs_li_radix_sort(void *records, u32 nr)
{
	struct sfs_li_sort_record *tmp, *src = records, *dst;
	u32 *cursor;
	u32 pass, i, bucket;

	if (nr < 2)
		return 0;
	if (!records)
		return -EINVAL;
	BUILD_BUG_ON(sizeof(struct sfs_li_sort_record) != 32);
	/* tiebreak64.low32 == key.low32 == hash32: six passes give the full order. */
	for (i = 0; i < nr; i++)
		if (unlikely((u32)src[i].order.key !=
			     (u32)src[i].order.tiebreak64))
			return -EUCLEAN;
	tmp = kvmalloc_array(nr, sizeof(*tmp), GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;
	cursor = kvcalloc(SFS_LI_RADIX_BUCKETS, sizeof(*cursor), GFP_KERNEL);
	if (!cursor) {
		kvfree(tmp);
		return -ENOMEM;
	}
	dst = tmp;

	for (pass = 0; pass < SFS_LI_RADIX_PASSES; pass++) {
		u32 total = 0;
		u16 first_digit = sfs_li_radix_digit(&src[0].order, pass);
		bool varying = false;

		memset(cursor, 0, SFS_LI_RADIX_BUCKETS * sizeof(*cursor));
		for (i = 0; i < nr; i++) {
			u16 digit = sfs_li_radix_digit(&src[i].order, pass);

			cursor[digit]++;
			varying |= digit != first_digit;
		}
		/* A constant digit cannot change a stable order. */
		if (!varying)
			continue;
		for (bucket = 0; bucket < SFS_LI_RADIX_BUCKETS; bucket++) {
			u32 count = cursor[bucket];

			cursor[bucket] = total;
			total += count;
		}
		for (i = 0; i < nr; i++) {
			u16 digit = sfs_li_radix_digit(&src[i].order, pass);

			dst[cursor[digit]++] = src[i];
		}
		swap(src, dst);
		cond_resched();
	}
	if (src != records)
		memcpy(records, src, (size_t)nr * sizeof(*src));
	for (i = 1; i < nr; i++) {
		const struct sfs_li_sort_prefix *left = &
			((struct sfs_li_sort_record *)records)[i - 1].order;
		const struct sfs_li_sort_prefix *right = &
			((struct sfs_li_sort_record *)records)[i].order;

		if (unlikely(!sfs_li_ordered(left, right))) {
			kvfree(cursor);
			kvfree(tmp);
			return -EUCLEAN;
		}
	}

	kvfree(cursor);
	kvfree(tmp);
	return 0;
}
