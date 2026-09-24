// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2017 Oracle.  All Rights Reserved.
 *
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef __EXT5_FSMAP_H__
#define	__EXT5_FSMAP_H__

struct fsmap;

/* internal fsmap representation */
struct ext5_fsmap {
	struct list_head	fmr_list;
	dev_t		fmr_device;	/* device id */
	uint32_t	fmr_flags;	/* mapping flags */
	uint64_t	fmr_physical;	/* device offset of segment */
	uint64_t	fmr_owner;	/* owner id */
	uint64_t	fmr_length;	/* length of segment, blocks */
};

struct ext5_fsmap_head {
	uint32_t	fmh_iflags;	/* control flags */
	uint32_t	fmh_oflags;	/* output flags */
	unsigned int	fmh_count;	/* # of entries in array incl. input */
	unsigned int	fmh_entries;	/* # of entries filled in (output). */

	struct ext5_fsmap fmh_keys[2];	/* low and high keys */
};

void ext5_fsmap_from_internal(struct super_block *sb, struct fsmap *dest,
		struct ext5_fsmap *src);
void ext5_fsmap_to_internal(struct super_block *sb, struct ext5_fsmap *dest,
		struct fsmap *src);

/* fsmap to userspace formatter - copy to user & advance pointer */
typedef int (*ext5_fsmap_format_t)(struct ext5_fsmap *, void *);

int ext5_getfsmap(struct super_block *sb, struct ext5_fsmap_head *head,
		ext5_fsmap_format_t formatter, void *arg);

#define EXT5_QUERY_RANGE_ABORT		1
#define EXT5_QUERY_RANGE_CONTINUE	0

/*	fmr_owner special values for FS_IOC_GETFSMAP; some share w/ XFS */
#define EXT5_FMR_OWN_FREE	FMR_OWN_FREE      /* free space */
#define EXT5_FMR_OWN_UNKNOWN	FMR_OWN_UNKNOWN   /* unknown owner */
#define EXT5_FMR_OWN_FS		FMR_OWNER('X', 1) /* static fs metadata */
#define EXT5_FMR_OWN_LOG	FMR_OWNER('X', 2) /* journalling log */
#define EXT5_FMR_OWN_INODES	FMR_OWNER('X', 5) /* inodes */
#define EXT5_FMR_OWN_GDT	FMR_OWNER('f', 1) /* group descriptors */
#define EXT5_FMR_OWN_RESV_GDT	FMR_OWNER('f', 2) /* reserved gdt blocks */
#define EXT5_FMR_OWN_BLKBM	FMR_OWNER('f', 3) /* block bitmap */
#define EXT5_FMR_OWN_INOBM	FMR_OWNER('f', 4) /* inode bitmap */

#endif /* __EXT5_FSMAP_H__ */
