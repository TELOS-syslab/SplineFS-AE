/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _EXT5_LI_RADIX_SORT_H
#define _EXT5_LI_RADIX_SORT_H

#include <linux/types.h>

/* Common leading layout for transition records. */
struct sfs_li_sort_prefix {
	u64 key;
	u64 tiebreak64;
	u32 stable_name_off;
	u8 name_len;
};

/* Stable LSD radix sort on (PLID, hash32, SipHash-high32). */
int sfs_li_radix_sort(void *records, u32 nr);

#endif /* _EXT5_LI_RADIX_SORT_H */
