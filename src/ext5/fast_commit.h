/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __FAST_COMMIT_H__
#define __FAST_COMMIT_H__

/*
 * Note this file is present in e2fsprogs/lib/ext2fs/fast_commit.h and
 * linux/fs/ext5/fast_commit.h. These file should always be byte identical.
 */

/* Fast commit tags */
#define EXT5_FC_TAG_ADD_RANGE		0x0001
#define EXT5_FC_TAG_DEL_RANGE		0x0002
#define EXT5_FC_TAG_CREAT		0x0003
#define EXT5_FC_TAG_LINK		0x0004
#define EXT5_FC_TAG_UNLINK		0x0005
#define EXT5_FC_TAG_INODE		0x0006
#define EXT5_FC_TAG_PAD			0x0007
#define EXT5_FC_TAG_TAIL		0x0008
#define EXT5_FC_TAG_HEAD		0x0009

#define EXT5_FC_SUPPORTED_FEATURES	0x0

/* On disk fast commit tlv value structures */

/* Fast commit on disk tag length structure */
struct ext5_fc_tl {
	__le16 fc_tag;
	__le16 fc_len;
};

/* Value structure for tag EXT5_FC_TAG_HEAD. */
struct ext5_fc_head {
	__le32 fc_features;
	__le32 fc_tid;
};

/* Value structure for EXT5_FC_TAG_ADD_RANGE. */
struct ext5_fc_add_range {
	__le32 fc_ino;
	__u8 fc_ex[12];
};

/* Value structure for tag EXT5_FC_TAG_DEL_RANGE. */
struct ext5_fc_del_range {
	__le32 fc_ino;
	__le32 fc_lblk;
	__le32 fc_len;
};

/*
 * This is the value structure for tags EXT5_FC_TAG_CREAT, EXT5_FC_TAG_LINK
 * and EXT5_FC_TAG_UNLINK.
 */
struct ext5_fc_dentry_info {
	__le32 fc_parent_ino;
	__le32 fc_ino;
	__u8 fc_dname[];
};

/* Value structure for EXT5_FC_TAG_INODE. */
struct ext5_fc_inode {
	__le32 fc_ino;
	__u8 fc_raw_inode[];
};

/* Value structure for tag EXT5_FC_TAG_TAIL. */
struct ext5_fc_tail {
	__le32 fc_tid;
	__le32 fc_crc;
};

/* Tag base length */
#define EXT5_FC_TAG_BASE_LEN (sizeof(struct ext5_fc_tl))

/*
 * Fast commit status codes
 */
enum {
	EXT5_FC_STATUS_OK = 0,
	EXT5_FC_STATUS_INELIGIBLE,
	EXT5_FC_STATUS_SKIPPED,
	EXT5_FC_STATUS_FAILED,
};

/*
 * Fast commit ineligiblity reasons:
 */
enum {
	EXT5_FC_REASON_XATTR = 0,
	EXT5_FC_REASON_CROSS_RENAME,
	EXT5_FC_REASON_JOURNAL_FLAG_CHANGE,
	EXT5_FC_REASON_NOMEM,
	EXT5_FC_REASON_SWAP_BOOT,
	EXT5_FC_REASON_RESIZE,
	EXT5_FC_REASON_RENAME_DIR,
	EXT5_FC_REASON_FALLOC_RANGE,
	EXT5_FC_REASON_INODE_JOURNAL_DATA,
	EXT5_FC_REASON_ENCRYPTED_FILENAME,
	EXT5_FC_REASON_MAX
};

#ifdef __KERNEL__
/*
 * In memory list of dentry updates that are performed on the file
 * system used by fast commit code.
 */
struct ext5_fc_dentry_update {
	int fcd_op;		/* Type of update create / unlink / link */
	int fcd_parent;		/* Parent inode number */
	int fcd_ino;		/* Inode number */
	struct name_snapshot fcd_name;	/* Dirent name */
	struct list_head fcd_list;
	struct list_head fcd_dilist;
};

struct ext5_fc_stats {
	unsigned int fc_ineligible_reason_count[EXT5_FC_REASON_MAX];
	unsigned long fc_num_commits;
	unsigned long fc_ineligible_commits;
	unsigned long fc_failed_commits;
	unsigned long fc_skipped_commits;
	unsigned long fc_numblks;
	u64 s_fc_avg_commit_time;
};

#define EXT5_FC_REPLAY_REALLOC_INCREMENT	4

/*
 * Physical block regions added to different inodes due to fast commit
 * recovery. These are set during the SCAN phase. During the replay phase,
 * our allocator excludes these from its allocation. This ensures that
 * we don't accidentally allocating a block that is going to be used by
 * another inode.
 */
struct ext5_fc_alloc_region {
	ext5_lblk_t lblk;
	ext5_fsblk_t pblk;
	int ino, len;
};

/*
 * Fast commit replay state.
 */
struct ext5_fc_replay_state {
	int fc_replay_num_tags;
	int fc_replay_expected_off;
	int fc_current_pass;
	int fc_cur_tag;
	int fc_crc;
	struct ext5_fc_alloc_region *fc_regions;
	int fc_regions_size, fc_regions_used, fc_regions_valid;
	int *fc_modified_inodes;
	int fc_modified_inodes_used, fc_modified_inodes_size;
};

#define region_last(__region) (((__region)->lblk) + ((__region)->len) - 1)
#endif

static inline const char *tag2str(__u16 tag)
{
	switch (tag) {
	case EXT5_FC_TAG_LINK:
		return "ADD_ENTRY";
	case EXT5_FC_TAG_UNLINK:
		return "DEL_ENTRY";
	case EXT5_FC_TAG_ADD_RANGE:
		return "ADD_RANGE";
	case EXT5_FC_TAG_CREAT:
		return "CREAT_DENTRY";
	case EXT5_FC_TAG_DEL_RANGE:
		return "DEL_RANGE";
	case EXT5_FC_TAG_INODE:
		return "INODE";
	case EXT5_FC_TAG_PAD:
		return "PAD";
	case EXT5_FC_TAG_TAIL:
		return "TAIL";
	case EXT5_FC_TAG_HEAD:
		return "HEAD";
	default:
		return "ERROR";
	}
}

#endif /* __FAST_COMMIT_H__ */
