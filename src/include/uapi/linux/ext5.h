/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */

#ifndef _UAPI_LINUX_EXT5_H
#define _UAPI_LINUX_EXT5_H
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * ext5-specific ioctl commands
 */
#define	EXT5_IOC_GETVERSION		_IOR('f', 3, long)
#define	EXT5_IOC_SETVERSION		_IOW('f', 4, long)
#define	EXT5_IOC_GETVERSION_OLD		FS_IOC_GETVERSION
#define	EXT5_IOC_SETVERSION_OLD		FS_IOC_SETVERSION
#define EXT5_IOC_GETRSVSZ		_IOR('f', 5, long)
#define EXT5_IOC_SETRSVSZ		_IOW('f', 6, long)
#define EXT5_IOC_GROUP_EXTEND		_IOW('f', 7, unsigned long)
#define EXT5_IOC_GROUP_ADD		_IOW('f', 8, struct ext5_new_group_input)
#define EXT5_IOC_MIGRATE		_IO('f', 9)
 /* note ioctl 10 reserved for an early version of the FIEMAP ioctl */
 /* note ioctl 11 reserved for filesystem-independent FIEMAP ioctl */
#define EXT5_IOC_ALLOC_DA_BLKS		_IO('f', 12)
#define EXT5_IOC_MOVE_EXT		_IOWR('f', 15, struct move_extent)
#define EXT5_IOC_RESIZE_FS		_IOW('f', 16, __u64)
#define EXT5_IOC_SWAP_BOOT		_IO('f', 17)
#define EXT5_IOC_PRECACHE_EXTENTS	_IO('f', 18)
/* ioctl codes 19--39 are reserved for fscrypt */
#define EXT5_IOC_CLEAR_ES_CACHE		_IO('f', 40)
#define EXT5_IOC_GETSTATE		_IOW('f', 41, __u32)
#define EXT5_IOC_GET_ES_CACHE		_IOWR('f', 42, struct fiemap)
#define EXT5_IOC_CHECKPOINT		_IOW('f', 43, __u32)
#define EXT5_IOC_GETFSUUID		_IOR('f', 44, struct fsuuid)
#define EXT5_IOC_SETFSUUID		_IOW('f', 44, struct fsuuid)

#define EXT5_IOC_SHUTDOWN _IOR('X', 125, __u32)

/* Promote a mutable directory to a stable subtree root. */
#define EXT5_IOC_PROMOTE_DIR		_IO('f', 0x80)

/* Reserve an inode-table range (__u32 count) for creates under this root. */
#define EXT5_IOC_LI_RESERVE_POOL	_IOW('f', 0x82, __u32)

/* Compact a subtree root: merge its delta into a new base. */
#define EXT5_IOC_LI_FORCE_COMPACT	_IO('f', 0x83)

/* Descriptor summary of a subtree root. */
struct ext5_li_desc_info {
	__u64 base_bytes;
	__u64 names_bytes;
	__u64 nav_bytes;	/* base_bytes - names_bytes */
	__u64 delta_bytes;
	__u64 delta_used;
	__u64 generation;
	__u64 delta_trigger;
	__u64 delta_records;
	__u64 delta_appended_inserts;
	__u64 delta_appended_deletes;
	__u64 refill_start_op;
	__u64 refill_start_used_bytes;
	__u64 refill_span_ops;
	__u64 tstar_ops;
	__u64 last_refill_span_ops;
	__u64 last_refill_projected_ops;
	__u64 last_refill_tstar_ops;
	__u32 entry_count;
	__u32 parent_count;
	__u32 filter_bits;
	__u32 radix_count;
	__u32 spline_count;
	__u32 spline_epsilon;
	__u32 radix_bits;
	__u32 delta_filter_bits;
	__u32 last_refill_class;
	__u64 resident_payload_bytes;
	__u32 resident_payload_blocks;
	__u32 payload_extent_runs;
};
#define EXT5_IOC_LI_DESC_INFO		_IOR('f', 0x84, struct ext5_li_desc_info)

/* Demote a stable subtree to conventional directories.  Not crash-safe
 * across a power loss mid-transition. */
#define EXT5_IOC_LI_DEMOTE_DIR		_IO('f', 0x85)

/* LI mode of a directory: 0 mutable, 1 stable root, 2 stable interior. */
struct ext5_li_dir_mode {
	__u32 mode;             /* 0/1/2; see above */
	__u32 _pad;
	__u64 subtree_root_ino; /* meaningful only when mode == 2 */
};
#define EXT5_IOC_LI_DIR_MODE		_IOR('f', 0x86, struct ext5_li_dir_mode)

/* Why a directory is or is not promoted.  Read-only; triggers nothing. */
struct ext5_li_dir_policy {
	__u32 mode;              /* as ext5_li_dir_mode: 0/1/2 */
	__u32 blocked_by;        /* EXT5_LI_GATE_*, 0 when promoted */
	__u64 subtree_files;     /* entries observed under this directory */
	__u64 subtree_dirs;      /* directories observed under it */
	__u64 region_entries;    /* what the policy sizes T* from */
	__u64 size_floor;        /* effective floor for this mount */
	__u64 rmd;               /* operations since this region last mutated */
	__u64 t_star;            /* threshold the RMD must clear */
	__u64 subtree_root_ino;  /* owning root when mode == 2 */
	__u64 _pad[3];
};

/* blocked_by values.  Zero means nothing blocks it. */
#define EXT5_LI_GATE_NONE		0
#define EXT5_LI_GATE_POLICY_OFF		1  /* li_policy_mode == 0 */
#define EXT5_LI_GATE_NO_STATE		2  /* no sidecar: never observed */
#define EXT5_LI_GATE_SIZE_FLOOR		3  /* region_entries < size_floor */
#define EXT5_LI_GATE_QUIESCENCE		4  /* rmd < t_star */
#define EXT5_LI_GATE_ALREADY_STABLE	5  /* already root or interior */

#define EXT5_IOC_LI_DIR_POLICY		_IOR('f', 0x88, struct ext5_li_dir_policy)

/* Wait for queued promote, compact and demote work; triggers none. */
#define EXT5_IOC_LI_WAIT_IDLE		_IO('f', 0x87)

/*
 * ioctl commands in 32 bit emulation
 */
#define EXT5_IOC32_GETVERSION		_IOR('f', 3, int)
#define EXT5_IOC32_SETVERSION		_IOW('f', 4, int)
#define EXT5_IOC32_GETRSVSZ		_IOR('f', 5, int)
#define EXT5_IOC32_SETRSVSZ		_IOW('f', 6, int)
#define EXT5_IOC32_GROUP_EXTEND		_IOW('f', 7, unsigned int)
#define EXT5_IOC32_GROUP_ADD		_IOW('f', 8, struct compat_ext5_new_group_input)
#define EXT5_IOC32_GETVERSION_OLD	FS_IOC32_GETVERSION
#define EXT5_IOC32_SETVERSION_OLD	FS_IOC32_SETVERSION

/*
 * Flags returned by EXT5_IOC_GETSTATE
 *
 * We only expose to userspace a subset of the state flags in
 * i_state_flags
 */
#define EXT5_STATE_FLAG_EXT_PRECACHED	0x00000001
#define EXT5_STATE_FLAG_NEW		0x00000002
#define EXT5_STATE_FLAG_NEWENTRY	0x00000004
#define EXT5_STATE_FLAG_DA_ALLOC_CLOSE	0x00000008

/*
 * Flags for ioctl EXT5_IOC_CHECKPOINT
 */
#define EXT5_IOC_CHECKPOINT_FLAG_DISCARD	0x1
#define EXT5_IOC_CHECKPOINT_FLAG_ZEROOUT	0x2
#define EXT5_IOC_CHECKPOINT_FLAG_DRY_RUN	0x4
#define EXT5_IOC_CHECKPOINT_FLAG_VALID		(EXT5_IOC_CHECKPOINT_FLAG_DISCARD | \
						EXT5_IOC_CHECKPOINT_FLAG_ZEROOUT | \
						EXT5_IOC_CHECKPOINT_FLAG_DRY_RUN)

/*
 * Structure for EXT5_IOC_GETFSUUID/EXT5_IOC_SETFSUUID
 */
struct fsuuid {
	__u32       fsu_len;
	__u32       fsu_flags;
	__u8        fsu_uuid[];
};

/*
 * Structure for EXT5_IOC_MOVE_EXT
 */
struct move_extent {
	__u32 reserved;		/* should be zero */
	__u32 donor_fd;		/* donor file descriptor */
	__u64 orig_start;	/* logical start offset in block for orig */
	__u64 donor_start;	/* logical start offset in block for donor */
	__u64 len;		/* block length to be moved */
	__u64 moved_len;	/* moved block length */
};

/*
 * Flags used by EXT5_IOC_SHUTDOWN
 */
#define EXT5_GOING_FLAGS_DEFAULT		0x0	/* going down */
#define EXT5_GOING_FLAGS_LOGFLUSH		0x1	/* flush log but not data */
#define EXT5_GOING_FLAGS_NOLOGFLUSH		0x2	/* don't flush log nor data */

/* Used to pass group descriptor data when online resize is done */
struct ext5_new_group_input {
	__u32 group;		/* Group number for this data */
	__u64 block_bitmap;	/* Absolute block number of block bitmap */
	__u64 inode_bitmap;	/* Absolute block number of inode bitmap */
	__u64 inode_table;	/* Absolute block number of inode table start */
	__u32 blocks_count;	/* Total number of blocks in this group */
	__u16 reserved_blocks;	/* Number of reserved blocks in this group */
	__u16 unused;
};

/*
 * Returned by EXT5_IOC_GET_ES_CACHE as an additional possible flag.
 * It indicates that the entry in extent status cache is for a hole.
 */
#define EXT5_FIEMAP_EXTENT_HOLE		0x08000000

#endif /* _UAPI_LINUX_EXT5_H */
