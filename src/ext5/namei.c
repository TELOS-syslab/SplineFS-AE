// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/ext5/namei.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/namei.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  Big-endian to little-endian byte-swapping/bitmaps by
 *        David S. Miller (davem@caip.rutgers.edu), 1995
 *  Directory entry file type support and forward compatibility hooks
 *	for B-tree directories by Theodore Ts'o (tytso@mit.edu), 1998
 *  Hash Tree Directory indexing (c)
 *	Daniel Phillips, 2001
 *  Hash Tree Directory indexing porting
 *	Christopher Li, 2002
 *  Hash Tree Directory indexing cleanup
 *	Theodore Ts'o, 2002
 */

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/time.h>
#include <linux/fcntl.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/quotaops.h>
#include <linux/buffer_head.h>
#include <linux/bio.h>
#include <linux/iversion.h>
#include <linux/unicode.h>
#include "ext5.h"
#include "ext5_jbd3.h"

#include "xattr.h"
#include "acl.h"

#include <trace/events/ext5.h>
/*
 * define how far ahead to read directories while searching them.
 */
#define NAMEI_RA_CHUNKS  2
#define NAMEI_RA_BLOCKS  4
#define NAMEI_RA_SIZE	     (NAMEI_RA_CHUNKS * NAMEI_RA_BLOCKS)

static struct buffer_head *ext5_append(handle_t *handle,
					struct inode *inode,
					ext5_lblk_t *block)
{
	struct ext5_map_blocks map;
	struct buffer_head *bh;
	int err;

	if (unlikely(EXT5_SB(inode->i_sb)->s_max_dir_size_kb &&
		     ((inode->i_size >> 10) >=
		      EXT5_SB(inode->i_sb)->s_max_dir_size_kb)))
		return ERR_PTR(-ENOSPC);

	*block = inode->i_size >> inode->i_sb->s_blocksize_bits;
	map.m_lblk = *block;
	map.m_len = 1;

	/*
	 * We're appending new directory block. Make sure the block is not
	 * allocated yet, otherwise we will end up corrupting the
	 * directory.
	 */
	err = ext5_map_blocks(NULL, inode, &map, 0);
	if (err < 0)
		return ERR_PTR(err);
	if (err) {
		EXT5_ERROR_INODE(inode, "Logical block already allocated");
		return ERR_PTR(-EFSCORRUPTED);
	}

	bh = ext5_bread(handle, inode, *block, EXT5_GET_BLOCKS_CREATE);
	if (IS_ERR(bh))
		return bh;
	inode->i_size += inode->i_sb->s_blocksize;
	EXT5_I(inode)->i_disksize = inode->i_size;
	err = ext5_mark_inode_dirty(handle, inode);
	if (err)
		goto out;
	BUFFER_TRACE(bh, "get_write_access");
	err = ext5_journal_get_write_access(handle, inode->i_sb, bh,
					    EXT5_JTR_NONE);
	if (err)
		goto out;
	return bh;

out:
	brelse(bh);
	ext5_std_error(inode->i_sb, err);
	return ERR_PTR(err);
}

static int ext5_dx_csum_verify(struct inode *inode,
			       struct ext5_dir_entry *dirent);

/*
 * Hints to ext5_read_dirblock regarding whether we expect a directory
 * block being read to be an index block, or a block containing
 * directory entries (and if the latter, whether it was found via a
 * logical block in an htree index block).  This is used to control
 * what sort of sanity checkinig ext5_read_dirblock() will do on the
 * directory block read from the storage device.  EITHER will means
 * the caller doesn't know what kind of directory block will be read,
 * so no specific verification will be done.
 */
typedef enum {
	EITHER, INDEX, DIRENT, DIRENT_HTREE
} dirblock_type_t;

#define ext5_read_dirblock(inode, block, type) \
	__ext5_read_dirblock((inode), (block), (type), __func__, __LINE__)

static struct buffer_head *__ext5_read_dirblock(struct inode *inode,
						ext5_lblk_t block,
						dirblock_type_t type,
						const char *func,
						unsigned int line)
{
	struct buffer_head *bh;
	struct ext5_dir_entry *dirent;
	int is_dx_block = 0;

	if (block >= inode->i_size >> inode->i_blkbits) {
		ext5_error_inode(inode, func, line, block,
		       "Attempting to read directory block (%u) that is past i_size (%llu)",
		       block, inode->i_size);
		return ERR_PTR(-EFSCORRUPTED);
	}

	if (ext5_simulate_fail(inode->i_sb, EXT5_SIM_DIRBLOCK_EIO))
		bh = ERR_PTR(-EIO);
	else
		bh = ext5_bread(NULL, inode, block, 0);
	if (IS_ERR(bh)) {
		__ext5_warning(inode->i_sb, func, line,
			       "inode #%lu: lblock %lu: comm %s: "
			       "error %ld reading directory block",
			       inode->i_ino, (unsigned long)block,
			       current->comm, PTR_ERR(bh));

		return bh;
	}
	/* The first directory block must not be a hole. */
	if (!bh && (type == INDEX || type == DIRENT_HTREE || block == 0)) {
		ext5_error_inode(inode, func, line, block,
				 "Directory hole found for htree %s block %u",
				 (type == INDEX) ? "index" : "leaf", block);
		return ERR_PTR(-EFSCORRUPTED);
	}
	if (!bh)
		return NULL;
	dirent = (struct ext5_dir_entry *) bh->b_data;
	/* Determine whether or not we have an index block */
	if (is_dx(inode)) {
		if (block == 0)
			is_dx_block = 1;
		else if (ext5_rec_len_from_disk(dirent->rec_len,
						inode->i_sb->s_blocksize) ==
			 inode->i_sb->s_blocksize)
			is_dx_block = 1;
	}
	if (!is_dx_block && type == INDEX) {
		ext5_error_inode(inode, func, line, block,
		       "directory leaf block found instead of index block");
		brelse(bh);
		return ERR_PTR(-EFSCORRUPTED);
	}
	if (!ext5_has_feature_metadata_csum(inode->i_sb) ||
	    buffer_verified(bh))
		return bh;

	/*
	 * An empty leaf block can get mistaken for a index block; for
	 * this reason, we can only check the index checksum when the
	 * caller is sure it should be an index block.
	 */
	if (is_dx_block && type == INDEX) {
		if (ext5_dx_csum_verify(inode, dirent) &&
		    !ext5_simulate_fail(inode->i_sb, EXT5_SIM_DIRBLOCK_CRC))
			set_buffer_verified(bh);
		else {
			ext5_error_inode_err(inode, func, line, block,
					     EFSBADCRC,
					     "Directory index failed checksum");
			brelse(bh);
			return ERR_PTR(-EFSBADCRC);
		}
	}
	if (!is_dx_block) {
		if (ext5_dirblock_csum_verify(inode, bh) &&
		    !ext5_simulate_fail(inode->i_sb, EXT5_SIM_DIRBLOCK_CRC))
			set_buffer_verified(bh);
		else {
			ext5_error_inode_err(inode, func, line, block,
					     EFSBADCRC,
					     "Directory block failed checksum");
			brelse(bh);
			return ERR_PTR(-EFSBADCRC);
		}
	}
	return bh;
}

#ifdef DX_DEBUG
#define dxtrace(command) command
#else
#define dxtrace(command)
#endif

struct fake_dirent
{
	__le32 inode;
	__le16 rec_len;
	u8 name_len;
	u8 file_type;
};

struct dx_countlimit
{
	__le16 limit;
	__le16 count;
};

struct dx_entry
{
	__le32 hash;
	__le32 block;
};

/*
 * dx_root_info is laid out so that if it should somehow get overlaid by a
 * dirent the two low bits of the hash version will be zero.  Therefore, the
 * hash version mod 4 should never be 0.  Sincerely, the paranoia department.
 */

struct dx_root
{
	struct fake_dirent dot;
	char dot_name[4];
	struct fake_dirent dotdot;
	char dotdot_name[4];
	struct dx_root_info
	{
		__le32 reserved_zero;
		u8 hash_version;
		u8 info_length; /* 8 */
		u8 indirect_levels;
		u8 unused_flags;
	}
	info;
	struct dx_entry	entries[];
};

struct dx_node
{
	struct fake_dirent fake;
	struct dx_entry	entries[];
};


struct dx_frame
{
	struct buffer_head *bh;
	struct dx_entry *entries;
	struct dx_entry *at;
};

struct dx_map_entry
{
	u32 hash;
	u16 offs;
	u16 size;
};

/*
 * This goes at the end of each htree block.
 */
struct dx_tail {
	u32 dt_reserved;
	__le32 dt_checksum;	/* crc32c(uuid+inum+dirblock) */
};

static struct buffer_head * ext5_dx_find_entry(struct inode *dir,
		struct ext5_filename *fname,
		struct ext5_dir_entry_2 **res_dir);
static int ext5_dx_add_entry(handle_t *handle, struct ext5_filename *fname,
			     struct inode *dir, struct inode *inode);

/* checksumming functions */
void ext5_initialize_dirent_tail(struct buffer_head *bh,
				 unsigned int blocksize)
{
	struct ext5_dir_entry_tail *t = EXT5_DIRENT_TAIL(bh->b_data, blocksize);

	memset(t, 0, sizeof(struct ext5_dir_entry_tail));
	t->det_rec_len = ext5_rec_len_to_disk(
			sizeof(struct ext5_dir_entry_tail), blocksize);
	t->det_reserved_ft = EXT5_FT_DIR_CSUM;
}

/* Walk through a dirent block to find a checksum "dirent" at the tail */
static struct ext5_dir_entry_tail *get_dirent_tail(struct inode *inode,
						   struct buffer_head *bh)
{
	struct ext5_dir_entry_tail *t;
	int blocksize = EXT5_BLOCK_SIZE(inode->i_sb);

#ifdef PARANOID
	struct ext5_dir_entry *d, *top;

	d = (struct ext5_dir_entry *)bh->b_data;
	top = (struct ext5_dir_entry *)(bh->b_data +
		(blocksize - sizeof(struct ext5_dir_entry_tail)));
	while (d < top && ext5_rec_len_from_disk(d->rec_len, blocksize))
		d = (struct ext5_dir_entry *)(((void *)d) +
		    ext5_rec_len_from_disk(d->rec_len, blocksize));

	if (d != top)
		return NULL;

	t = (struct ext5_dir_entry_tail *)d;
#else
	t = EXT5_DIRENT_TAIL(bh->b_data, EXT5_BLOCK_SIZE(inode->i_sb));
#endif

	if (t->det_reserved_zero1 ||
	    (ext5_rec_len_from_disk(t->det_rec_len, blocksize) !=
	     sizeof(struct ext5_dir_entry_tail)) ||
	    t->det_reserved_zero2 ||
	    t->det_reserved_ft != EXT5_FT_DIR_CSUM)
		return NULL;

	return t;
}

static __le32 ext5_dirblock_csum(struct inode *inode, void *dirent, int size)
{
	struct ext5_inode_info *ei = EXT5_I(inode);
	__u32 csum;

	csum = ext5_chksum(ei->i_csum_seed, (__u8 *)dirent, size);
	return cpu_to_le32(csum);
}

#define warn_no_space_for_csum(inode)					\
	__warn_no_space_for_csum((inode), __func__, __LINE__)

static void __warn_no_space_for_csum(struct inode *inode, const char *func,
				     unsigned int line)
{
	__ext5_warning_inode(inode, func, line,
		"No space for directory leaf checksum. Please run e2fsck -D.");
}

int ext5_dirblock_csum_verify(struct inode *inode, struct buffer_head *bh)
{
	struct ext5_dir_entry_tail *t;

	if (!ext5_has_feature_metadata_csum(inode->i_sb))
		return 1;

	t = get_dirent_tail(inode, bh);
	if (!t) {
		warn_no_space_for_csum(inode);
		return 0;
	}

	if (t->det_checksum != ext5_dirblock_csum(inode, bh->b_data,
						  (char *)t - bh->b_data))
		return 0;

	return 1;
}

static void ext5_dirblock_csum_set(struct inode *inode,
				 struct buffer_head *bh)
{
	struct ext5_dir_entry_tail *t;

	if (!ext5_has_feature_metadata_csum(inode->i_sb))
		return;

	t = get_dirent_tail(inode, bh);
	if (!t) {
		warn_no_space_for_csum(inode);
		return;
	}

	t->det_checksum = ext5_dirblock_csum(inode, bh->b_data,
					     (char *)t - bh->b_data);
}

int ext5_handle_dirty_dirblock(handle_t *handle,
			       struct inode *inode,
			       struct buffer_head *bh)
{
	ext5_dirblock_csum_set(inode, bh);
	return ext5_handle_dirty_metadata(handle, inode, bh);
}

static struct dx_countlimit *get_dx_countlimit(struct inode *inode,
					       struct ext5_dir_entry *dirent,
					       int *offset)
{
	struct ext5_dir_entry *dp;
	struct dx_root_info *root;
	int count_offset;
	int blocksize = EXT5_BLOCK_SIZE(inode->i_sb);
	unsigned int rlen = ext5_rec_len_from_disk(dirent->rec_len, blocksize);

	if (rlen == blocksize)
		count_offset = 8;
	else if (rlen == 12) {
		dp = (struct ext5_dir_entry *)(((void *)dirent) + 12);
		if (ext5_rec_len_from_disk(dp->rec_len, blocksize) != blocksize - 12)
			return NULL;
		root = (struct dx_root_info *)(((void *)dp + 12));
		if (root->reserved_zero ||
		    root->info_length != sizeof(struct dx_root_info))
			return NULL;
		count_offset = 32;
	} else
		return NULL;

	if (offset)
		*offset = count_offset;
	return (struct dx_countlimit *)(((void *)dirent) + count_offset);
}

static __le32 ext5_dx_csum(struct inode *inode, struct ext5_dir_entry *dirent,
			   int count_offset, int count, struct dx_tail *t)
{
	struct ext5_inode_info *ei = EXT5_I(inode);
	__u32 csum;
	int size;
	__u32 dummy_csum = 0;
	int offset = offsetof(struct dx_tail, dt_checksum);

	size = count_offset + (count * sizeof(struct dx_entry));
	csum = ext5_chksum(ei->i_csum_seed, (__u8 *)dirent, size);
	csum = ext5_chksum(csum, (__u8 *)t, offset);
	csum = ext5_chksum(csum, (__u8 *)&dummy_csum, sizeof(dummy_csum));

	return cpu_to_le32(csum);
}

static int ext5_dx_csum_verify(struct inode *inode,
			       struct ext5_dir_entry *dirent)
{
	struct dx_countlimit *c;
	struct dx_tail *t;
	int count_offset, limit, count;

	if (!ext5_has_feature_metadata_csum(inode->i_sb))
		return 1;

	c = get_dx_countlimit(inode, dirent, &count_offset);
	if (!c) {
		EXT5_ERROR_INODE(inode, "dir seems corrupt?  Run e2fsck -D.");
		return 0;
	}
	limit = le16_to_cpu(c->limit);
	count = le16_to_cpu(c->count);
	if (count_offset + (limit * sizeof(struct dx_entry)) >
	    EXT5_BLOCK_SIZE(inode->i_sb) - sizeof(struct dx_tail)) {
		warn_no_space_for_csum(inode);
		return 0;
	}
	t = (struct dx_tail *)(((struct dx_entry *)c) + limit);

	if (t->dt_checksum != ext5_dx_csum(inode, dirent, count_offset,
					    count, t))
		return 0;
	return 1;
}

static void ext5_dx_csum_set(struct inode *inode, struct ext5_dir_entry *dirent)
{
	struct dx_countlimit *c;
	struct dx_tail *t;
	int count_offset, limit, count;

	if (!ext5_has_feature_metadata_csum(inode->i_sb))
		return;

	c = get_dx_countlimit(inode, dirent, &count_offset);
	if (!c) {
		EXT5_ERROR_INODE(inode, "dir seems corrupt?  Run e2fsck -D.");
		return;
	}
	limit = le16_to_cpu(c->limit);
	count = le16_to_cpu(c->count);
	if (count_offset + (limit * sizeof(struct dx_entry)) >
	    EXT5_BLOCK_SIZE(inode->i_sb) - sizeof(struct dx_tail)) {
		warn_no_space_for_csum(inode);
		return;
	}
	t = (struct dx_tail *)(((struct dx_entry *)c) + limit);

	t->dt_checksum = ext5_dx_csum(inode, dirent, count_offset, count, t);
}

static inline int ext5_handle_dirty_dx_node(handle_t *handle,
					    struct inode *inode,
					    struct buffer_head *bh)
{
	ext5_dx_csum_set(inode, (struct ext5_dir_entry *)bh->b_data);
	return ext5_handle_dirty_metadata(handle, inode, bh);
}

/*
 * p is at least 6 bytes before the end of page
 */
static inline struct ext5_dir_entry_2 *
ext5_next_entry(struct ext5_dir_entry_2 *p, unsigned long blocksize)
{
	return (struct ext5_dir_entry_2 *)((char *)p +
		ext5_rec_len_from_disk(p->rec_len, blocksize));
}

/*
 * Future: use high four bits of block for coalesce-on-delete flags
 * Mask them off for now.
 */

static inline ext5_lblk_t dx_get_block(struct dx_entry *entry)
{
	return le32_to_cpu(entry->block) & 0x0fffffff;
}

static inline void dx_set_block(struct dx_entry *entry, ext5_lblk_t value)
{
	entry->block = cpu_to_le32(value);
}

static inline unsigned dx_get_hash(struct dx_entry *entry)
{
	return le32_to_cpu(entry->hash);
}

static inline void dx_set_hash(struct dx_entry *entry, unsigned value)
{
	entry->hash = cpu_to_le32(value);
}

static inline unsigned dx_get_count(struct dx_entry *entries)
{
	return le16_to_cpu(((struct dx_countlimit *) entries)->count);
}

static inline unsigned dx_get_limit(struct dx_entry *entries)
{
	return le16_to_cpu(((struct dx_countlimit *) entries)->limit);
}

static inline void dx_set_count(struct dx_entry *entries, unsigned value)
{
	((struct dx_countlimit *) entries)->count = cpu_to_le16(value);
}

static inline void dx_set_limit(struct dx_entry *entries, unsigned value)
{
	((struct dx_countlimit *) entries)->limit = cpu_to_le16(value);
}

static inline unsigned dx_root_limit(struct inode *dir, unsigned infosize)
{
	unsigned int entry_space = dir->i_sb->s_blocksize -
			ext5_dir_rec_len(1, NULL) -
			ext5_dir_rec_len(2, NULL) - infosize;

	if (ext5_has_feature_metadata_csum(dir->i_sb))
		entry_space -= sizeof(struct dx_tail);
	return entry_space / sizeof(struct dx_entry);
}

static inline unsigned dx_node_limit(struct inode *dir)
{
	unsigned int entry_space = dir->i_sb->s_blocksize -
			ext5_dir_rec_len(0, dir);

	if (ext5_has_feature_metadata_csum(dir->i_sb))
		entry_space -= sizeof(struct dx_tail);
	return entry_space / sizeof(struct dx_entry);
}

/*
 * Debug
 */
#ifdef DX_DEBUG
static void dx_show_index(char * label, struct dx_entry *entries)
{
	int i, n = dx_get_count (entries);
	printk(KERN_DEBUG "%s index", label);
	for (i = 0; i < n; i++) {
		printk(KERN_CONT " %x->%lu",
		       i ? dx_get_hash(entries + i) : 0,
		       (unsigned long)dx_get_block(entries + i));
	}
	printk(KERN_CONT "\n");
}

struct stats
{
	unsigned names;
	unsigned space;
	unsigned bcount;
};

static struct stats dx_show_leaf(struct inode *dir,
				struct dx_hash_info *hinfo,
				struct ext5_dir_entry_2 *de,
				int size, int show_names)
{
	unsigned names = 0, space = 0;
	char *base = (char *) de;
	struct dx_hash_info h = *hinfo;

	printk("names: ");
	while ((char *) de < base + size)
	{
		if (de->inode)
		{
			if (show_names)
			{
#ifdef CONFIG_FS_ENCRYPTION
				int len;
				char *name;
				struct fscrypt_str fname_crypto_str =
					FSTR_INIT(NULL, 0);
				int res = 0;

				name  = de->name;
				len = de->name_len;
				if (!IS_ENCRYPTED(dir)) {
					/* Directory is not encrypted */
					(void) ext5fs_dirhash(dir, de->name,
						de->name_len, &h);
					printk("%*.s:(U)%x.%u ", len,
					       name, h.hash,
					       (unsigned) ((char *) de
							   - base));
				} else {
					struct fscrypt_str de_name =
						FSTR_INIT(name, len);

					/* Directory is encrypted */
					res = fscrypt_fname_alloc_buffer(
						len, &fname_crypto_str);
					if (res)
						printk(KERN_WARNING "Error "
							"allocating crypto "
							"buffer--skipping "
							"crypto\n");
					res = fscrypt_fname_disk_to_usr(dir,
						0, 0, &de_name,
						&fname_crypto_str);
					if (res) {
						printk(KERN_WARNING "Error "
							"converting filename "
							"from disk to usr"
							"\n");
						name = "??";
						len = 2;
					} else {
						name = fname_crypto_str.name;
						len = fname_crypto_str.len;
					}
					if (IS_CASEFOLDED(dir))
						h.hash = EXT5_DIRENT_HASH(de);
					else
						(void) ext5fs_dirhash(dir,
							de->name,
							de->name_len, &h);
					printk("%*.s:(E)%x.%u ", len, name,
					       h.hash, (unsigned) ((char *) de
								   - base));
					fscrypt_fname_free_buffer(
							&fname_crypto_str);
				}
#else
				int len = de->name_len;
				char *name = de->name;
				(void) ext5fs_dirhash(dir, de->name,
						      de->name_len, &h);
				printk("%*.s:%x.%u ", len, name, h.hash,
				       (unsigned) ((char *) de - base));
#endif
			}
			space += ext5_dir_rec_len(de->name_len, dir);
			names++;
		}
		de = ext5_next_entry(de, size);
	}
	printk(KERN_CONT "(%i)\n", names);
	return (struct stats) { names, space, 1 };
}

struct stats dx_show_entries(struct dx_hash_info *hinfo, struct inode *dir,
			     struct dx_entry *entries, int levels)
{
	unsigned blocksize = dir->i_sb->s_blocksize;
	unsigned count = dx_get_count(entries), names = 0, space = 0, i;
	unsigned bcount = 0;
	struct buffer_head *bh;
	printk("%i indexed blocks...\n", count);
	for (i = 0; i < count; i++, entries++)
	{
		ext5_lblk_t block = dx_get_block(entries);
		ext5_lblk_t hash  = i ? dx_get_hash(entries): 0;
		u32 range = i < count - 1? (dx_get_hash(entries + 1) - hash): ~hash;
		struct stats stats;
		printk("%s%3u:%03u hash %8x/%8x ",levels?"":"   ", i, block, hash, range);
		bh = ext5_bread(NULL,dir, block, 0);
		if (!bh || IS_ERR(bh))
			continue;
		stats = levels?
		   dx_show_entries(hinfo, dir, ((struct dx_node *) bh->b_data)->entries, levels - 1):
		   dx_show_leaf(dir, hinfo, (struct ext5_dir_entry_2 *)
			bh->b_data, blocksize, 0);
		names += stats.names;
		space += stats.space;
		bcount += stats.bcount;
		brelse(bh);
	}
	if (bcount)
		printk(KERN_DEBUG "%snames %u, fullness %u (%u%%)\n",
		       levels ? "" : "   ", names, space/bcount,
		       (space/bcount)*100/blocksize);
	return (struct stats) { names, space, bcount};
}

/*
 * Linear search cross check
 */
static inline void htree_rep_invariant_check(struct dx_entry *at,
					     struct dx_entry *target,
					     u32 hash, unsigned int n)
{
	while (n--) {
		dxtrace(printk(KERN_CONT ","));
		if (dx_get_hash(++at) > hash) {
			at--;
			break;
		}
	}
	ASSERT(at == target - 1);
}
#else /* DX_DEBUG */
static inline void htree_rep_invariant_check(struct dx_entry *at,
					     struct dx_entry *target,
					     u32 hash, unsigned int n)
{
}
#endif /* DX_DEBUG */

/*
 * Probe for a directory leaf block to search.
 *
 * dx_probe can return ERR_BAD_DX_DIR, which means there was a format
 * error in the directory index, and the caller should fall back to
 * searching the directory normally.  The callers of dx_probe **MUST**
 * check for this error code, and make sure it never gets reflected
 * back to userspace.
 */
static struct dx_frame *
dx_probe(struct ext5_filename *fname, struct inode *dir,
	 struct dx_hash_info *hinfo, struct dx_frame *frame_in)
{
	unsigned count, indirect, level, i;
	struct dx_entry *at, *entries, *p, *q, *m;
	struct dx_root *root;
	struct dx_frame *frame = frame_in;
	struct dx_frame *ret_err = ERR_PTR(ERR_BAD_DX_DIR);
	u32 hash;
	ext5_lblk_t block;
	ext5_lblk_t blocks[EXT5_HTREE_LEVEL];

	memset(frame_in, 0, EXT5_HTREE_LEVEL * sizeof(frame_in[0]));
	frame->bh = ext5_read_dirblock(dir, 0, INDEX);
	if (IS_ERR(frame->bh))
		return (struct dx_frame *) frame->bh;

	root = (struct dx_root *) frame->bh->b_data;
	if (root->info.hash_version != DX_HASH_TEA &&
	    root->info.hash_version != DX_HASH_HALF_MD4 &&
	    root->info.hash_version != DX_HASH_LEGACY &&
	    root->info.hash_version != DX_HASH_SIPHASH) {
		ext5_warning_inode(dir, "Unrecognised inode hash code %u",
				   root->info.hash_version);
		goto fail;
	}
	if (ext5_hash_in_dirent(dir)) {
		if (root->info.hash_version != DX_HASH_SIPHASH) {
			ext5_warning_inode(dir,
				"Hash in dirent, but hash is not SIPHASH");
			goto fail;
		}
	} else {
		if (root->info.hash_version == DX_HASH_SIPHASH) {
			ext5_warning_inode(dir,
				"Hash code is SIPHASH, but hash not in dirent");
			goto fail;
		}
	}
	if (fname)
		hinfo = &fname->hinfo;
	hinfo->hash_version = root->info.hash_version;
	if (hinfo->hash_version <= DX_HASH_TEA)
		hinfo->hash_version += EXT5_SB(dir->i_sb)->s_hash_unsigned;
	hinfo->seed = EXT5_SB(dir->i_sb)->s_hash_seed;
	/* hash is already computed for encrypted casefolded directory */
	if (fname && fname_name(fname) &&
	    !(IS_ENCRYPTED(dir) && IS_CASEFOLDED(dir))) {
		int ret = ext5fs_dirhash(dir, fname_name(fname),
					 fname_len(fname), hinfo);
		if (ret < 0) {
			ret_err = ERR_PTR(ret);
			goto fail;
		}
	}
	hash = hinfo->hash;

	if (root->info.unused_flags & 1) {
		ext5_warning_inode(dir, "Unimplemented hash flags: %#06x",
				   root->info.unused_flags);
		goto fail;
	}

	indirect = root->info.indirect_levels;
	if (indirect >= ext5_dir_htree_level(dir->i_sb)) {
		ext5_warning(dir->i_sb,
			     "Directory (ino: %lu) htree depth %#06x exceed"
			     "supported value", dir->i_ino,
			     ext5_dir_htree_level(dir->i_sb));
		if (ext5_dir_htree_level(dir->i_sb) < EXT5_HTREE_LEVEL) {
			ext5_warning(dir->i_sb, "Enable large directory "
						"feature to access it");
		}
		goto fail;
	}

	entries = (struct dx_entry *)(((char *)&root->info) +
				      root->info.info_length);

	if (dx_get_limit(entries) != dx_root_limit(dir,
						   root->info.info_length)) {
		ext5_warning_inode(dir, "dx entry: limit %u != root limit %u",
				   dx_get_limit(entries),
				   dx_root_limit(dir, root->info.info_length));
		goto fail;
	}

	dxtrace(printk("Look up %x", hash));
	level = 0;
	blocks[0] = 0;
	while (1) {
		count = dx_get_count(entries);
		if (!count || count > dx_get_limit(entries)) {
			ext5_warning_inode(dir,
					   "dx entry: count %u beyond limit %u",
					   count, dx_get_limit(entries));
			goto fail;
		}

		p = entries + 1;
		q = entries + count - 1;
		while (p <= q) {
			m = p + (q - p) / 2;
			dxtrace(printk(KERN_CONT "."));
			if (dx_get_hash(m) > hash)
				q = m - 1;
			else
				p = m + 1;
		}

		htree_rep_invariant_check(entries, p, hash, count - 1);

		at = p - 1;
		dxtrace(printk(KERN_CONT " %x->%u\n",
			       at == entries ? 0 : dx_get_hash(at),
			       dx_get_block(at)));
		frame->entries = entries;
		frame->at = at;

		block = dx_get_block(at);
		for (i = 0; i <= level; i++) {
			if (blocks[i] == block) {
				ext5_warning_inode(dir,
					"dx entry: tree cycle block %u points back to block %u",
					blocks[level], block);
				goto fail;
			}
		}
		if (++level > indirect)
			return frame;
		blocks[level] = block;
		frame++;
		frame->bh = ext5_read_dirblock(dir, block, INDEX);
		if (IS_ERR(frame->bh)) {
			ret_err = (struct dx_frame *) frame->bh;
			frame->bh = NULL;
			goto fail;
		}

		entries = ((struct dx_node *) frame->bh->b_data)->entries;

		if (dx_get_limit(entries) != dx_node_limit(dir)) {
			ext5_warning_inode(dir,
				"dx entry: limit %u != node limit %u",
				dx_get_limit(entries), dx_node_limit(dir));
			goto fail;
		}
	}
fail:
	while (frame >= frame_in) {
		brelse(frame->bh);
		frame--;
	}

	if (ret_err == ERR_PTR(ERR_BAD_DX_DIR))
		ext5_warning_inode(dir,
			"Corrupt directory, running e2fsck is recommended");
	return ret_err;
}

static void dx_release(struct dx_frame *frames)
{
	struct dx_root_info *info;
	int i;
	unsigned int indirect_levels;

	if (frames[0].bh == NULL)
		return;

	info = &((struct dx_root *)frames[0].bh->b_data)->info;
	/* save local copy, "info" may be freed after brelse() */
	indirect_levels = info->indirect_levels;
	for (i = 0; i <= indirect_levels; i++) {
		if (frames[i].bh == NULL)
			break;
		brelse(frames[i].bh);
		frames[i].bh = NULL;
	}
}

/*
 * This function increments the frame pointer to search the next leaf
 * block, and reads in the necessary intervening nodes if the search
 * should be necessary.  Whether or not the search is necessary is
 * controlled by the hash parameter.  If the hash value is even, then
 * the search is only continued if the next block starts with that
 * hash value.  This is used if we are searching for a specific file.
 *
 * If the hash value is HASH_NB_ALWAYS, then always go to the next block.
 *
 * This function returns 1 if the caller should continue to search,
 * or 0 if it should not.  If there is an error reading one of the
 * index blocks, it will a negative error code.
 *
 * If start_hash is non-null, it will be filled in with the starting
 * hash of the next page.
 */
static int ext5_htree_next_block(struct inode *dir, __u32 hash,
				 struct dx_frame *frame,
				 struct dx_frame *frames,
				 __u32 *start_hash)
{
	struct dx_frame *p;
	struct buffer_head *bh;
	int num_frames = 0;
	__u32 bhash;

	p = frame;
	/*
	 * Find the next leaf page by incrementing the frame pointer.
	 * If we run out of entries in the interior node, loop around and
	 * increment pointer in the parent node.  When we break out of
	 * this loop, num_frames indicates the number of interior
	 * nodes need to be read.
	 */
	while (1) {
		if (++(p->at) < p->entries + dx_get_count(p->entries))
			break;
		if (p == frames)
			return 0;
		num_frames++;
		p--;
	}

	/*
	 * If the hash is 1, then continue only if the next page has a
	 * continuation hash of any value.  This is used for readdir
	 * handling.  Otherwise, check to see if the hash matches the
	 * desired continuation hash.  If it doesn't, return since
	 * there's no point to read in the successive index pages.
	 */
	bhash = dx_get_hash(p->at);
	if (start_hash)
		*start_hash = bhash;
	if ((hash & 1) == 0) {
		if ((bhash & ~1) != hash)
			return 0;
	}
	/*
	 * If the hash is HASH_NB_ALWAYS, we always go to the next
	 * block so no check is necessary
	 */
	while (num_frames--) {
		bh = ext5_read_dirblock(dir, dx_get_block(p->at), INDEX);
		if (IS_ERR(bh))
			return PTR_ERR(bh);
		p++;
		brelse(p->bh);
		p->bh = bh;
		p->at = p->entries = ((struct dx_node *) bh->b_data)->entries;
	}
	return 1;
}


/*
 * This function fills a red-black tree with information from a
 * directory block.  It returns the number directory entries loaded
 * into the tree.  If there is an error it is returned in err.
 */
static int htree_dirblock_to_tree(struct file *dir_file,
				  struct inode *dir, ext5_lblk_t block,
				  struct dx_hash_info *hinfo,
				  __u32 start_hash, __u32 start_minor_hash)
{
	struct buffer_head *bh;
	struct ext5_dir_entry_2 *de, *top;
	int err = 0, count = 0;
	struct fscrypt_str fname_crypto_str = FSTR_INIT(NULL, 0), tmp_str;
	int csum = ext5_has_feature_metadata_csum(dir->i_sb);

	dxtrace(printk(KERN_INFO "In htree dirblock_to_tree: block %lu\n",
							(unsigned long)block));
	bh = ext5_read_dirblock(dir, block, DIRENT_HTREE);
	if (IS_ERR(bh))
		return PTR_ERR(bh);

	de = (struct ext5_dir_entry_2 *) bh->b_data;
	/* csum entries are not larger in the casefolded encrypted case */
	top = (struct ext5_dir_entry_2 *) ((char *) de +
					   dir->i_sb->s_blocksize -
					   ext5_dir_rec_len(0,
							   csum ? NULL : dir));
	/* Check if the directory is encrypted */
	if (IS_ENCRYPTED(dir)) {
		err = fscrypt_prepare_readdir(dir);
		if (err < 0) {
			brelse(bh);
			return err;
		}
		err = fscrypt_fname_alloc_buffer(EXT5_NAME_LEN,
						 &fname_crypto_str);
		if (err < 0) {
			brelse(bh);
			return err;
		}
	}

	for (; de < top; de = ext5_next_entry(de, dir->i_sb->s_blocksize)) {
		if (ext5_check_dir_entry(dir, NULL, de, bh,
				bh->b_data, bh->b_size,
				(block<<EXT5_BLOCK_SIZE_BITS(dir->i_sb))
					 + ((char *)de - bh->b_data))) {
			/* silently ignore the rest of the block */
			break;
		}
		if (ext5_hash_in_dirent(dir)) {
			if (de->name_len && de->inode) {
				hinfo->hash = EXT5_DIRENT_HASH(de);
				hinfo->minor_hash = EXT5_DIRENT_MINOR_HASH(de);
			} else {
				hinfo->hash = 0;
				hinfo->minor_hash = 0;
			}
		} else {
			err = ext5fs_dirhash(dir, de->name,
					     de->name_len, hinfo);
			if (err < 0) {
				count = err;
				goto errout;
			}
		}
		if ((hinfo->hash < start_hash) ||
		    ((hinfo->hash == start_hash) &&
		     (hinfo->minor_hash < start_minor_hash)))
			continue;
		if (de->inode == 0)
			continue;
		if (!IS_ENCRYPTED(dir)) {
			tmp_str.name = de->name;
			tmp_str.len = de->name_len;
			err = ext5_htree_store_dirent(dir_file,
				   hinfo->hash, hinfo->minor_hash, de,
				   &tmp_str);
		} else {
			int save_len = fname_crypto_str.len;
			struct fscrypt_str de_name = FSTR_INIT(de->name,
								de->name_len);

			/* Directory is encrypted */
			err = fscrypt_fname_disk_to_usr(dir, hinfo->hash,
					hinfo->minor_hash, &de_name,
					&fname_crypto_str);
			if (err) {
				count = err;
				goto errout;
			}
			err = ext5_htree_store_dirent(dir_file,
				   hinfo->hash, hinfo->minor_hash, de,
					&fname_crypto_str);
			fname_crypto_str.len = save_len;
		}
		if (err != 0) {
			count = err;
			goto errout;
		}
		count++;
	}
errout:
	brelse(bh);
	fscrypt_fname_free_buffer(&fname_crypto_str);
	return count;
}


/*
 * This function fills a red-black tree with information from a
 * directory.  We start scanning the directory in hash order, starting
 * at start_hash and start_minor_hash.
 *
 * This function returns the number of entries inserted into the tree,
 * or a negative error code.
 */
int ext5_htree_fill_tree(struct file *dir_file, __u32 start_hash,
			 __u32 start_minor_hash, __u32 *next_hash)
{
	struct dx_hash_info hinfo;
	struct ext5_dir_entry_2 *de;
	struct dx_frame frames[EXT5_HTREE_LEVEL], *frame;
	struct inode *dir;
	ext5_lblk_t block;
	int count = 0;
	int ret, err;
	__u32 hashval;
	struct fscrypt_str tmp_str;

	dxtrace(printk(KERN_DEBUG "In htree_fill_tree, start hash: %x:%x\n",
		       start_hash, start_minor_hash));
	dir = file_inode(dir_file);
	if (!(ext5_test_inode_flag(dir, EXT5_INODE_INDEX))) {
		if (ext5_hash_in_dirent(dir))
			hinfo.hash_version = DX_HASH_SIPHASH;
		else
			hinfo.hash_version =
					EXT5_SB(dir->i_sb)->s_def_hash_version;
		if (hinfo.hash_version <= DX_HASH_TEA)
			hinfo.hash_version +=
				EXT5_SB(dir->i_sb)->s_hash_unsigned;
		hinfo.seed = EXT5_SB(dir->i_sb)->s_hash_seed;
		if (ext5_has_inline_data(dir)) {
			int has_inline_data = 1;
			count = ext5_inlinedir_to_tree(dir_file, dir, 0,
						       &hinfo, start_hash,
						       start_minor_hash,
						       &has_inline_data);
			if (has_inline_data) {
				*next_hash = ~0;
				return count;
			}
		}
		count = htree_dirblock_to_tree(dir_file, dir, 0, &hinfo,
					       start_hash, start_minor_hash);
		*next_hash = ~0;
		return count;
	}
	hinfo.hash = start_hash;
	hinfo.minor_hash = 0;
	frame = dx_probe(NULL, dir, &hinfo, frames);
	if (IS_ERR(frame))
		return PTR_ERR(frame);

	/* Add '.' and '..' from the htree header */
	if (!start_hash && !start_minor_hash) {
		de = (struct ext5_dir_entry_2 *) frames[0].bh->b_data;
		tmp_str.name = de->name;
		tmp_str.len = de->name_len;
		err = ext5_htree_store_dirent(dir_file, 0, 0,
					      de, &tmp_str);
		if (err != 0)
			goto errout;
		count++;
	}
	if (start_hash < 2 || (start_hash ==2 && start_minor_hash==0)) {
		de = (struct ext5_dir_entry_2 *) frames[0].bh->b_data;
		de = ext5_next_entry(de, dir->i_sb->s_blocksize);
		tmp_str.name = de->name;
		tmp_str.len = de->name_len;
		err = ext5_htree_store_dirent(dir_file, 2, 0,
					      de, &tmp_str);
		if (err != 0)
			goto errout;
		count++;
	}

	while (1) {
		if (fatal_signal_pending(current)) {
			err = -ERESTARTSYS;
			goto errout;
		}
		cond_resched();
		block = dx_get_block(frame->at);
		ret = htree_dirblock_to_tree(dir_file, dir, block, &hinfo,
					     start_hash, start_minor_hash);
		if (ret < 0) {
			err = ret;
			goto errout;
		}
		count += ret;
		hashval = ~0;
		ret = ext5_htree_next_block(dir, HASH_NB_ALWAYS,
					    frame, frames, &hashval);
		*next_hash = hashval;
		if (ret < 0) {
			err = ret;
			goto errout;
		}
		/*
		 * Stop if:  (a) there are no more entries, or
		 * (b) we have inserted at least one entry and the
		 * next hash value is not a continuation
		 */
		if ((ret == 0) ||
		    (count && ((hashval & 1) == 0)))
			break;
	}
	dx_release(frames);
	dxtrace(printk(KERN_DEBUG "Fill tree: returned %d entries, "
		       "next hash: %x\n", count, *next_hash));
	return count;
errout:
	dx_release(frames);
	return (err);
}

/* Mark HTree-reachable leaf blocks so promotion can parse them in
 * O(entries).  Caller holds the directory's shared lock. */
int ext5_htree_mark_live_leaf_blocks(struct inode *inode,
				     unsigned long *live, u32 nr_blocks)
{
	struct dx_hash_info hinfo = { 0 };
	struct dx_frame frames[EXT5_HTREE_LEVEL], *frame;
	int err = 0;

	if (!ext5_test_inode_flag(inode, EXT5_INODE_INDEX))
		return -EINVAL;
	frame = dx_probe(NULL, inode, &hinfo, frames);
	if (IS_ERR(frame))
		return PTR_ERR(frame);
	for (;;) {
		ext5_lblk_t block = dx_get_block(frame->at);
		u32 next_hash = ~0U;
		int next;

		if (!block || block >= nr_blocks) {
			err = -EFSCORRUPTED;
			break;
		}
		__set_bit(block, live);
		next = ext5_htree_next_block(inode, HASH_NB_ALWAYS, frame,
					       frames, &next_hash);
		if (next <= 0) {
			err = next;
			break;
		}
	}
	dx_release(frames);
	return err;
}

static inline int search_dirblock(struct buffer_head *bh,
				  struct inode *dir,
				  struct ext5_filename *fname,
				  unsigned int offset,
				  struct ext5_dir_entry_2 **res_dir)
{
	return ext5_search_dir(bh, bh->b_data, dir->i_sb->s_blocksize, dir,
			       fname, offset, res_dir);
}

/*
 * Directory block splitting, compacting
 */

/*
 * Create map of hash values, offsets, and sizes, stored at end of block.
 * Returns number of entries mapped.
 */
static int dx_make_map(struct inode *dir, struct buffer_head *bh,
		       struct dx_hash_info *hinfo,
		       struct dx_map_entry *map_tail)
{
	int count = 0;
	struct ext5_dir_entry_2 *de = (struct ext5_dir_entry_2 *)bh->b_data;
	unsigned int buflen = bh->b_size;
	char *base = bh->b_data;
	struct dx_hash_info h = *hinfo;
	int blocksize = EXT5_BLOCK_SIZE(dir->i_sb);

	if (ext5_has_feature_metadata_csum(dir->i_sb))
		buflen -= sizeof(struct ext5_dir_entry_tail);

	while ((char *) de < base + buflen) {
		if (ext5_check_dir_entry(dir, NULL, de, bh, base, buflen,
					 ((char *)de) - base))
			return -EFSCORRUPTED;
		if (de->name_len && de->inode) {
			if (ext5_hash_in_dirent(dir))
				h.hash = EXT5_DIRENT_HASH(de);
			else {
				int err = ext5fs_dirhash(dir, de->name,
						     de->name_len, &h);
				if (err < 0)
					return err;
			}
			map_tail--;
			map_tail->hash = h.hash;
			map_tail->offs = ((char *) de - base)>>2;
			map_tail->size = ext5_rec_len_from_disk(de->rec_len,
								blocksize);
			count++;
			cond_resched();
		}
		de = ext5_next_entry(de, blocksize);
	}
	return count;
}

/* Sort map by hash value */
static void dx_sort_map (struct dx_map_entry *map, unsigned count)
{
	struct dx_map_entry *p, *q, *top = map + count - 1;
	int more;
	/* Combsort until bubble sort doesn't suck */
	while (count > 2) {
		count = count*10/13;
		if (count - 9 < 2) /* 9, 10 -> 11 */
			count = 11;
		for (p = top, q = p - count; q >= map; p--, q--)
			if (p->hash < q->hash)
				swap(*p, *q);
	}
	/* Garden variety bubble sort */
	do {
		more = 0;
		q = top;
		while (q-- > map) {
			if (q[1].hash >= q[0].hash)
				continue;
			swap(*(q+1), *q);
			more = 1;
		}
	} while(more);
}

static void dx_insert_block(struct dx_frame *frame, u32 hash, ext5_lblk_t block)
{
	struct dx_entry *entries = frame->entries;
	struct dx_entry *old = frame->at, *new = old + 1;
	int count = dx_get_count(entries);

	ASSERT(count < dx_get_limit(entries));
	ASSERT(old < entries + count);
	memmove(new + 1, new, (char *)(entries + count) - (char *)(new));
	dx_set_hash(new, hash);
	dx_set_block(new, block);
	dx_set_count(entries, count + 1);
}

#if IS_ENABLED(CONFIG_UNICODE)
int ext5_fname_setup_ci_filename(struct inode *dir, const struct qstr *iname,
				  struct ext5_filename *name)
{
	struct qstr *cf_name = &name->cf_name;
	unsigned char *buf;
	struct dx_hash_info *hinfo = &name->hinfo;
	int len;

	if (!IS_CASEFOLDED(dir) ||
	    (IS_ENCRYPTED(dir) && !fscrypt_has_encryption_key(dir))) {
		cf_name->name = NULL;
		return 0;
	}

	buf = kmalloc(EXT5_NAME_LEN, GFP_NOFS);
	if (!buf)
		return -ENOMEM;

	len = utf8_casefold(dir->i_sb->s_encoding, iname, buf, EXT5_NAME_LEN);
	if (len <= 0) {
		kfree(buf);
		buf = NULL;
	}
	cf_name->name = buf;
	cf_name->len = (unsigned) len;

	if (!IS_ENCRYPTED(dir))
		return 0;

	hinfo->hash_version = DX_HASH_SIPHASH;
	hinfo->seed = NULL;
	if (cf_name->name)
		return ext5fs_dirhash(dir, cf_name->name, cf_name->len, hinfo);
	else
		return ext5fs_dirhash(dir, iname->name, iname->len, hinfo);
}
#endif

/*
 * Test whether a directory entry matches the filename being searched for.
 *
 * Return: %true if the directory entry matches, otherwise %false.
 */
static bool ext5_match(struct inode *parent,
			      const struct ext5_filename *fname,
			      struct ext5_dir_entry_2 *de)
{
	struct fscrypt_name f;

	if (!de->inode)
		return false;

	f.usr_fname = fname->usr_fname;
	f.disk_name = fname->disk_name;
#ifdef CONFIG_FS_ENCRYPTION
	f.crypto_buf = fname->crypto_buf;
#endif

#if IS_ENABLED(CONFIG_UNICODE)
	if (IS_CASEFOLDED(parent) &&
	    (!IS_ENCRYPTED(parent) || fscrypt_has_encryption_key(parent))) {
		/*
		 * Just checking IS_ENCRYPTED(parent) below is not
		 * sufficient to decide whether one can use the hash for
		 * skipping the string comparison, because the key might
		 * have been added right after
		 * ext5_fname_setup_ci_filename().  In this case, a hash
		 * mismatch will be a false negative.  Therefore, make
		 * sure cf_name was properly initialized before
		 * considering the calculated hash.
		 */
		if (sb_no_casefold_compat_fallback(parent->i_sb) &&
		    IS_ENCRYPTED(parent) && fname->cf_name.name &&
		    (fname->hinfo.hash != EXT5_DIRENT_HASH(de) ||
		     fname->hinfo.minor_hash != EXT5_DIRENT_MINOR_HASH(de)))
			return false;
		/*
		 * Treat comparison errors as not a match.  The
		 * only case where it happens is on a disk
		 * corruption or ENOMEM.
		 */

		return generic_ci_match(parent, fname->usr_fname,
					&fname->cf_name, de->name,
					de->name_len) > 0;
	}
#endif

	return fscrypt_match_name(&f, de->name, de->name_len);
}

/*
 * Returns 0 if not found, -EFSCORRUPTED on failure, and 1 on success
 */
int ext5_search_dir(struct buffer_head *bh, char *search_buf, int buf_size,
		    struct inode *dir, struct ext5_filename *fname,
		    unsigned int offset, struct ext5_dir_entry_2 **res_dir)
{
	struct ext5_dir_entry_2 * de;
	char * dlimit;
	int de_len;

	de = (struct ext5_dir_entry_2 *)search_buf;
	dlimit = search_buf + buf_size;
	while ((char *) de < dlimit - EXT5_BASE_DIR_LEN) {
		/* this code is executed quadratically often */
		/* do minimal checking `by hand' */
		if (de->name + de->name_len <= dlimit &&
		    ext5_match(dir, fname, de)) {
			/* found a match - just to be sure, do
			 * a full check */
			if (ext5_check_dir_entry(dir, NULL, de, bh, search_buf,
						 buf_size, offset))
				return -EFSCORRUPTED;
			*res_dir = de;
			return 1;
		}
		/* prevent looping on a bad block */
		de_len = ext5_rec_len_from_disk(de->rec_len,
						dir->i_sb->s_blocksize);
		if (de_len <= 0)
			return -EFSCORRUPTED;
		offset += de_len;
		de = (struct ext5_dir_entry_2 *) ((char *) de + de_len);
	}
	return 0;
}

static int is_dx_internal_node(struct inode *dir, ext5_lblk_t block,
			       struct ext5_dir_entry *de)
{
	struct super_block *sb = dir->i_sb;

	if (!is_dx(dir))
		return 0;
	if (block == 0)
		return 1;
	if (de->inode == 0 &&
	    ext5_rec_len_from_disk(de->rec_len, sb->s_blocksize) ==
			sb->s_blocksize)
		return 1;
	return 0;
}

/*
 *	__ext5_find_entry()
 *
 * finds an entry in the specified directory with the wanted name. It
 * returns the cache buffer in which the entry was found, and the entry
 * itself (as a parameter - res_dir). It does NOT read the inode of the
 * entry - you'll have to do that yourself if you want to.
 *
 * The returned buffer_head has ->b_count elevated.  The caller is expected
 * to brelse() it when appropriate.
 */
static struct buffer_head *__ext5_find_entry(struct inode *dir,
					     struct ext5_filename *fname,
					     struct ext5_dir_entry_2 **res_dir,
					     int *inlined)
{
	struct super_block *sb;
	struct buffer_head *bh_use[NAMEI_RA_SIZE];
	struct buffer_head *bh, *ret = NULL;
	ext5_lblk_t start, block;
	const u8 *name = fname->usr_fname->name;
	size_t ra_max = 0;	/* Number of bh's in the readahead
				   buffer, bh_use[] */
	size_t ra_ptr = 0;	/* Current index into readahead
				   buffer */
	ext5_lblk_t  nblocks;
	int i, namelen, retval;

	*res_dir = NULL;
	sb = dir->i_sb;
	namelen = fname->usr_fname->len;
	if (namelen > EXT5_NAME_LEN)
		return NULL;

	if (ext5_has_inline_data(dir)) {
		int has_inline_data = 1;
		ret = ext5_find_inline_entry(dir, fname, res_dir,
					     &has_inline_data);
		if (inlined)
			*inlined = has_inline_data;
		if (has_inline_data || IS_ERR(ret))
			goto cleanup_and_exit;
	}

	if ((namelen <= 2) && (name[0] == '.') &&
	    (name[1] == '.' || name[1] == '\0')) {
		/*
		 * "." or ".." will only be in the first block
		 * NFS may look up ".."; "." should be handled by the VFS
		 */
		block = start = 0;
		nblocks = 1;
		goto restart;
	}
	if (is_dx(dir)) {
		ret = ext5_dx_find_entry(dir, fname, res_dir);
		/*
		 * On success, or if the error was file not found,
		 * return.  Otherwise, fall back to doing a search the
		 * old fashioned way.
		 */
		if (IS_ERR(ret) && PTR_ERR(ret) == ERR_BAD_DX_DIR)
			dxtrace(printk(KERN_DEBUG "ext5_find_entry: dx failed, "
				       "falling back\n"));
		else if (!sb_no_casefold_compat_fallback(dir->i_sb) &&
			 *res_dir == NULL && IS_CASEFOLDED(dir))
			dxtrace(printk(KERN_DEBUG "ext5_find_entry: casefold "
				       "failed, falling back\n"));
		else
			goto cleanup_and_exit;
		ret = NULL;
	}
	nblocks = dir->i_size >> EXT5_BLOCK_SIZE_BITS(sb);
	if (!nblocks) {
		ret = NULL;
		goto cleanup_and_exit;
	}
	start = EXT5_I(dir)->i_dir_start_lookup;
	if (start >= nblocks)
		start = 0;
	block = start;
restart:
	do {
		/*
		 * We deal with the read-ahead logic here.
		 */
		cond_resched();
		if (ra_ptr >= ra_max) {
			/* Refill the readahead buffer */
			ra_ptr = 0;
			if (block < start)
				ra_max = start - block;
			else
				ra_max = nblocks - block;
			ra_max = min(ra_max, ARRAY_SIZE(bh_use));
			retval = ext5_bread_batch(dir, block, ra_max,
						  false /* wait */, bh_use);
			if (retval) {
				ret = ERR_PTR(retval);
				ra_max = 0;
				goto cleanup_and_exit;
			}
		}
		if ((bh = bh_use[ra_ptr++]) == NULL)
			goto next;
		wait_on_buffer(bh);
		if (!buffer_uptodate(bh)) {
			EXT5_ERROR_INODE_ERR(dir, EIO,
					     "reading directory lblock %lu",
					     (unsigned long) block);
			brelse(bh);
			ret = ERR_PTR(-EIO);
			goto cleanup_and_exit;
		}
		if (!buffer_verified(bh) &&
		    !is_dx_internal_node(dir, block,
					 (struct ext5_dir_entry *)bh->b_data) &&
		    !ext5_dirblock_csum_verify(dir, bh)) {
			EXT5_ERROR_INODE_ERR(dir, EFSBADCRC,
					     "checksumming directory "
					     "block %lu", (unsigned long)block);
			brelse(bh);
			ret = ERR_PTR(-EFSBADCRC);
			goto cleanup_and_exit;
		}
		set_buffer_verified(bh);
		i = search_dirblock(bh, dir, fname,
			    block << EXT5_BLOCK_SIZE_BITS(sb), res_dir);
		if (i == 1) {
			EXT5_I(dir)->i_dir_start_lookup = block;
			ret = bh;
			goto cleanup_and_exit;
		} else {
			brelse(bh);
			if (i < 0) {
				ret = ERR_PTR(i);
				goto cleanup_and_exit;
			}
		}
	next:
		if (++block >= nblocks)
			block = 0;
	} while (block != start);

	/*
	 * If the directory has grown while we were searching, then
	 * search the last part of the directory before giving up.
	 */
	block = nblocks;
	nblocks = dir->i_size >> EXT5_BLOCK_SIZE_BITS(sb);
	if (block < nblocks) {
		start = 0;
		goto restart;
	}

cleanup_and_exit:
	/* Clean up the read-ahead blocks */
	for (; ra_ptr < ra_max; ra_ptr++)
		brelse(bh_use[ra_ptr]);
	return ret;
}

static struct buffer_head *ext5_find_entry(struct inode *dir,
					   const struct qstr *d_name,
					   struct ext5_dir_entry_2 **res_dir,
					   int *inlined)
{
	int err;
	struct ext5_filename fname;
	struct buffer_head *bh;

	err = ext5_fname_setup_filename(dir, d_name, 1, &fname);
	if (err == -ENOENT)
		return NULL;
	if (err)
		return ERR_PTR(err);

	bh = __ext5_find_entry(dir, &fname, res_dir, inlined);

	ext5_fname_free_filename(&fname);
	return bh;
}

static struct buffer_head *ext5_lookup_entry(struct inode *dir,
					     struct dentry *dentry,
					     struct ext5_dir_entry_2 **res_dir)
{
	int err;
	struct ext5_filename fname;
	struct buffer_head *bh;

	err = ext5_fname_prepare_lookup(dir, dentry, &fname);
	if (err == -ENOENT)
		return NULL;
	if (err)
		return ERR_PTR(err);

	bh = __ext5_find_entry(dir, &fname, res_dir, NULL);

	ext5_fname_free_filename(&fname);
	return bh;
}

static struct buffer_head * ext5_dx_find_entry(struct inode *dir,
			struct ext5_filename *fname,
			struct ext5_dir_entry_2 **res_dir)
{
	struct super_block * sb = dir->i_sb;
	struct dx_frame frames[EXT5_HTREE_LEVEL], *frame;
	struct buffer_head *bh;
	ext5_lblk_t block;
	int retval;

#ifdef CONFIG_FS_ENCRYPTION
	*res_dir = NULL;
#endif
	frame = dx_probe(fname, dir, NULL, frames);
	if (IS_ERR(frame))
		return ERR_CAST(frame);
	do {
		block = dx_get_block(frame->at);
		bh = ext5_read_dirblock(dir, block, DIRENT_HTREE);
		if (IS_ERR(bh))
			goto errout;

		retval = search_dirblock(bh, dir, fname,
					 block << EXT5_BLOCK_SIZE_BITS(sb),
					 res_dir);
		if (retval == 1)
			goto success;
		brelse(bh);
		if (retval < 0) {
			bh = ERR_PTR(ERR_BAD_DX_DIR);
			goto errout;
		}

		/* Check to see if we should continue to search */
		retval = ext5_htree_next_block(dir, fname->hinfo.hash, frame,
					       frames, NULL);
		if (retval < 0) {
			ext5_warning_inode(dir,
				"error %d reading directory index block",
				retval);
			bh = ERR_PTR(retval);
			goto errout;
		}
	} while (retval == 1);

	bh = NULL;
errout:
	dxtrace(printk(KERN_DEBUG "%s not found\n", fname->usr_fname->name));
success:
	dx_release(frames);
	return bh;
}

static struct dentry *ext5_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	struct inode *inode = NULL;
	struct ext5_dir_entry_2 *de;
	struct buffer_head *bh;
	__u32 ino = 0;

	if (dentry->d_name.len > EXT5_NAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	/* Every dcache miss advances the policy clock. */
	ext5_li_op_tick(dir->i_sb);
	/* A quiet mutable ancestor may absorb an already stable descendant. */
	if (!ext5_dir_is_stable(dir) && EXT5_I(dir)->i_li_state &&
	    READ_ONCE(EXT5_I(dir)->i_li_armed) == 2) {
		ext5_li_note_stable_access(dir);
	} else if (!ext5_dir_is_stable(dir) ||
		   READ_ONCE(dentry->d_parent->d_parent) !=
		   READ_ONCE(dentry->d_parent->d_parent->d_parent)) {
		ext5_li_maybe_promote_on_lookup(dentry);
	}

	if (ext5_dir_is_stable(dir)) {
		ino_t found_ino = 0;
		int err;

		err = ext5_stable_inode_by_name(dir, &dentry->d_name,
						&found_ino);
		if (err && err != -ENOENT)
			return ERR_PTR(err);
		ino = (err == 0) ? (__u32)found_ino : 0;
		goto resolve_inode;
	}

	bh = ext5_lookup_entry(dir, dentry, &de);
	if (IS_ERR(bh))
		return ERR_CAST(bh);
	if (bh) {
		ino = le32_to_cpu(de->inode);
		brelse(bh);
	}

resolve_inode:
	if (ino) {
		if (!ext5_valid_inum(dir->i_sb, ino)) {
			EXT5_ERROR_INODE(dir, "bad inode number: %u", ino);
			return ERR_PTR(-EFSCORRUPTED);
		}
		if (unlikely(ino == dir->i_ino)) {
			EXT5_ERROR_INODE(dir, "'%pd' linked to parent dir",
					 dentry);
			return ERR_PTR(-EFSCORRUPTED);
		}
		inode = ext5_iget(dir->i_sb, ino, EXT5_IGET_NORMAL);
		if (inode == ERR_PTR(-ESTALE)) {
			EXT5_ERROR_INODE(dir,
					 "deleted inode referenced: %u",
					 ino);
			return ERR_PTR(-EFSCORRUPTED);
		}
		if (!IS_ERR(inode) && IS_ENCRYPTED(dir) &&
		    (S_ISDIR(inode->i_mode) || S_ISLNK(inode->i_mode)) &&
		    !fscrypt_has_permitted_context(dir, inode)) {
			ext5_warning(inode->i_sb,
				     "Inconsistent encryption contexts: %lu/%lu",
				     dir->i_ino, inode->i_ino);
			iput(inode);
			return ERR_PTR(-EPERM);
		}
	}

	if (IS_ENABLED(CONFIG_UNICODE) && !inode && IS_CASEFOLDED(dir)) {
		/* Eventually we want to call d_add_ci(dentry, NULL)
		 * for negative dentries in the encoding case as
		 * well.  For now, prevent the negative dentry
		 * from being cached.
		 */
		return NULL;
	}

	return d_splice_alias(inode, dentry);
}


struct dentry *ext5_get_parent(struct dentry *child)
{
	__u32 ino;
	struct ext5_dir_entry_2 * de;
	struct buffer_head *bh;

	/* Stable directories have no dotdot block; NFS export of them is unsupported. */
	if (ext5_dir_is_stable(d_inode(child)))
		return ERR_PTR(-EOPNOTSUPP);

	bh = ext5_find_entry(d_inode(child), &dotdot_name, &de, NULL);
	if (IS_ERR(bh))
		return ERR_CAST(bh);
	if (!bh)
		return ERR_PTR(-ENOENT);
	ino = le32_to_cpu(de->inode);
	brelse(bh);

	if (!ext5_valid_inum(child->d_sb, ino)) {
		EXT5_ERROR_INODE(d_inode(child),
				 "bad parent inode number: %u", ino);
		return ERR_PTR(-EFSCORRUPTED);
	}

	return d_obtain_alias(ext5_iget(child->d_sb, ino, EXT5_IGET_NORMAL));
}

/*
 * Move count entries from end of map between two memory locations.
 * Returns pointer to last entry moved.
 */
static struct ext5_dir_entry_2 *
dx_move_dirents(struct inode *dir, char *from, char *to,
		struct dx_map_entry *map, int count,
		unsigned blocksize)
{
	unsigned rec_len = 0;

	while (count--) {
		struct ext5_dir_entry_2 *de = (struct ext5_dir_entry_2 *)
						(from + (map->offs<<2));
		rec_len = ext5_dir_rec_len(de->name_len, dir);

		memcpy (to, de, rec_len);
		((struct ext5_dir_entry_2 *) to)->rec_len =
				ext5_rec_len_to_disk(rec_len, blocksize);

		/* wipe dir_entry excluding the rec_len field */
		de->inode = 0;
		memset(&de->name_len, 0, ext5_rec_len_from_disk(de->rec_len,
								blocksize) -
					 offsetof(struct ext5_dir_entry_2,
								name_len));

		map++;
		to += rec_len;
	}
	return (struct ext5_dir_entry_2 *) (to - rec_len);
}

/*
 * Compact each dir entry in the range to the minimal rec_len.
 * Returns pointer to last entry in range.
 */
static struct ext5_dir_entry_2 *dx_pack_dirents(struct inode *dir, char *base,
							unsigned int blocksize)
{
	struct ext5_dir_entry_2 *next, *to, *prev, *de = (struct ext5_dir_entry_2 *) base;
	unsigned rec_len = 0;

	prev = to = de;
	while ((char*)de < base + blocksize) {
		next = ext5_next_entry(de, blocksize);
		if (de->inode && de->name_len) {
			rec_len = ext5_dir_rec_len(de->name_len, dir);
			if (de > to)
				memmove(to, de, rec_len);
			to->rec_len = ext5_rec_len_to_disk(rec_len, blocksize);
			prev = to;
			to = (struct ext5_dir_entry_2 *) (((char *) to) + rec_len);
		}
		de = next;
	}
	return prev;
}

/*
 * Split a full leaf block to make room for a new dir entry.
 * Allocate a new block, and move entries so that they are approx. equally full.
 * Returns pointer to de in block into which the new entry will be inserted.
 */
static struct ext5_dir_entry_2 *do_split(handle_t *handle, struct inode *dir,
			struct buffer_head **bh,struct dx_frame *frame,
			struct dx_hash_info *hinfo)
{
	unsigned blocksize = dir->i_sb->s_blocksize;
	unsigned continued;
	int count;
	struct buffer_head *bh2;
	ext5_lblk_t newblock;
	u32 hash2;
	struct dx_map_entry *map;
	char *data1 = (*bh)->b_data, *data2;
	unsigned split, move, size;
	struct ext5_dir_entry_2 *de = NULL, *de2;
	int	csum_size = 0;
	int	err = 0, i;

	if (ext5_has_feature_metadata_csum(dir->i_sb))
		csum_size = sizeof(struct ext5_dir_entry_tail);

	bh2 = ext5_append(handle, dir, &newblock);
	if (IS_ERR(bh2)) {
		brelse(*bh);
		*bh = NULL;
		return ERR_CAST(bh2);
	}

	BUFFER_TRACE(*bh, "get_write_access");
	err = ext5_journal_get_write_access(handle, dir->i_sb, *bh,
					    EXT5_JTR_NONE);
	if (err)
		goto journal_error;

	BUFFER_TRACE(frame->bh, "get_write_access");
	err = ext5_journal_get_write_access(handle, dir->i_sb, frame->bh,
					    EXT5_JTR_NONE);
	if (err)
		goto journal_error;

	data2 = bh2->b_data;

	/* create map in the end of data2 block */
	map = (struct dx_map_entry *) (data2 + blocksize);
	count = dx_make_map(dir, *bh, hinfo, map);
	if (count < 0) {
		err = count;
		goto journal_error;
	}
	map -= count;
	dx_sort_map(map, count);
	/* Ensure that neither split block is over half full */
	size = 0;
	move = 0;
	for (i = count-1; i >= 0; i--) {
		/* is more than half of this entry in 2nd half of the block? */
		if (size + map[i].size/2 > blocksize/2)
			break;
		size += map[i].size;
		move++;
	}
	/*
	 * map index at which we will split
	 *
	 * If the sum of active entries didn't exceed half the block size, just
	 * split it in half by count; each resulting block will have at least
	 * half the space free.
	 */
	if (i >= 0)
		split = count - move;
	else
		split = count/2;

	if (WARN_ON_ONCE(split == 0)) {
		/* Should never happen, but avoid out-of-bounds access below */
		ext5_error_inode_block(dir, (*bh)->b_blocknr, 0,
			"bad indexed directory? hash=%08x:%08x count=%d move=%u",
			hinfo->hash, hinfo->minor_hash, count, move);
		err = -EFSCORRUPTED;
		goto out;
	}

	hash2 = map[split].hash;
	continued = hash2 == map[split - 1].hash;
	dxtrace(printk(KERN_INFO "Split block %lu at %x, %i/%i\n",
			(unsigned long)dx_get_block(frame->at),
					hash2, split, count-split));

	/* Fancy dance to stay within two buffers */
	de2 = dx_move_dirents(dir, data1, data2, map + split, count - split,
			      blocksize);
	de = dx_pack_dirents(dir, data1, blocksize);
	de->rec_len = ext5_rec_len_to_disk(data1 + (blocksize - csum_size) -
					   (char *) de,
					   blocksize);
	de2->rec_len = ext5_rec_len_to_disk(data2 + (blocksize - csum_size) -
					    (char *) de2,
					    blocksize);
	if (csum_size) {
		ext5_initialize_dirent_tail(*bh, blocksize);
		ext5_initialize_dirent_tail(bh2, blocksize);
	}

	dxtrace(dx_show_leaf(dir, hinfo, (struct ext5_dir_entry_2 *) data1,
			blocksize, 1));
	dxtrace(dx_show_leaf(dir, hinfo, (struct ext5_dir_entry_2 *) data2,
			blocksize, 1));

	/* Which block gets the new entry? */
	if (hinfo->hash >= hash2) {
		swap(*bh, bh2);
		de = de2;
	}
	dx_insert_block(frame, hash2 + continued, newblock);
	err = ext5_handle_dirty_dirblock(handle, dir, bh2);
	if (err)
		goto journal_error;
	err = ext5_handle_dirty_dx_node(handle, dir, frame->bh);
	if (err)
		goto journal_error;
	brelse(bh2);
	dxtrace(dx_show_index("frame", frame->entries));
	return de;

journal_error:
	ext5_std_error(dir->i_sb, err);
out:
	brelse(*bh);
	brelse(bh2);
	*bh = NULL;
	return ERR_PTR(err);
}

int ext5_find_dest_de(struct inode *dir, struct buffer_head *bh,
		      void *buf, int buf_size,
		      struct ext5_filename *fname,
		      struct ext5_dir_entry_2 **dest_de)
{
	struct ext5_dir_entry_2 *de;
	unsigned short reclen = ext5_dir_rec_len(fname_len(fname), dir);
	int nlen, rlen;
	unsigned int offset = 0;
	char *top;

	de = buf;
	top = buf + buf_size - reclen;
	while ((char *) de <= top) {
		if (ext5_check_dir_entry(dir, NULL, de, bh,
					 buf, buf_size, offset))
			return -EFSCORRUPTED;
		if (ext5_match(dir, fname, de))
			return -EEXIST;
		nlen = ext5_dir_rec_len(de->name_len, dir);
		rlen = ext5_rec_len_from_disk(de->rec_len, buf_size);
		if ((de->inode ? rlen - nlen : rlen) >= reclen)
			break;
		de = (struct ext5_dir_entry_2 *)((char *)de + rlen);
		offset += rlen;
	}
	if ((char *) de > top)
		return -ENOSPC;

	*dest_de = de;
	return 0;
}

void ext5_insert_dentry(struct inode *dir,
			struct inode *inode,
			struct ext5_dir_entry_2 *de,
			int buf_size,
			struct ext5_filename *fname)
{

	int nlen, rlen;

	nlen = ext5_dir_rec_len(de->name_len, dir);
	rlen = ext5_rec_len_from_disk(de->rec_len, buf_size);
	if (de->inode) {
		struct ext5_dir_entry_2 *de1 =
			(struct ext5_dir_entry_2 *)((char *)de + nlen);
		de1->rec_len = ext5_rec_len_to_disk(rlen - nlen, buf_size);
		de->rec_len = ext5_rec_len_to_disk(nlen, buf_size);
		de = de1;
	}
	de->file_type = EXT5_FT_UNKNOWN;
	de->inode = cpu_to_le32(inode->i_ino);
	ext5_set_de_type(inode->i_sb, de, inode->i_mode);
	de->name_len = fname_len(fname);
	memcpy(de->name, fname_name(fname), fname_len(fname));
	if (ext5_hash_in_dirent(dir)) {
		struct dx_hash_info *hinfo = &fname->hinfo;

		EXT5_DIRENT_HASHES(de)->hash = cpu_to_le32(hinfo->hash);
		EXT5_DIRENT_HASHES(de)->minor_hash =
						cpu_to_le32(hinfo->minor_hash);
	}
}

/*
 * Add a new entry into a directory (leaf) block.  If de is non-NULL,
 * it points to a directory entry which is guaranteed to be large
 * enough for new directory entry.  If de is NULL, then
 * add_dirent_to_buf will attempt search the directory block for
 * space.  It will return -ENOSPC if no space is available, and -EIO
 * and -EEXIST if directory entry already exists.
 */
static int add_dirent_to_buf(handle_t *handle, struct ext5_filename *fname,
			     struct inode *dir,
			     struct inode *inode, struct ext5_dir_entry_2 *de,
			     struct buffer_head *bh)
{
	unsigned int	blocksize = dir->i_sb->s_blocksize;
	int		csum_size = 0;
	int		err, err2;

	if (ext5_has_feature_metadata_csum(inode->i_sb))
		csum_size = sizeof(struct ext5_dir_entry_tail);

	if (!de) {
		err = ext5_find_dest_de(dir, bh, bh->b_data,
					blocksize - csum_size, fname, &de);
		if (err)
			return err;
	}
	BUFFER_TRACE(bh, "get_write_access");
	err = ext5_journal_get_write_access(handle, dir->i_sb, bh,
					    EXT5_JTR_NONE);
	if (err) {
		ext5_std_error(dir->i_sb, err);
		return err;
	}

	/* By now the buffer is marked for journaling */
	ext5_insert_dentry(dir, inode, de, blocksize, fname);

	/*
	 * XXX shouldn't update any times until successful
	 * completion of syscall, but too many callers depend
	 * on this.
	 *
	 * XXX similarly, too many callers depend on
	 * ext5_new_inode() setting the times, but error
	 * recovery deletes the inode, so the worst that can
	 * happen is that the times are slightly out of date
	 * and/or different from the directory change time.
	 */
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	ext5_update_dx_flag(dir);
	inode_inc_iversion(dir);
	err2 = ext5_mark_inode_dirty(handle, dir);
	BUFFER_TRACE(bh, "call ext5_handle_dirty_metadata");
	err = ext5_handle_dirty_dirblock(handle, dir, bh);
	if (err)
		ext5_std_error(dir->i_sb, err);
	return err ? err : err2;
}

static bool ext5_check_dx_root(struct inode *dir, struct dx_root *root)
{
	struct fake_dirent *fde;
	const char *error_msg;
	unsigned int rlen;
	unsigned int blocksize = dir->i_sb->s_blocksize;
	char *blockend = (char *)root + dir->i_sb->s_blocksize;

	fde = &root->dot;
	if (unlikely(fde->name_len != 1)) {
		error_msg = "invalid name_len for '.'";
		goto corrupted;
	}
	if (unlikely(strncmp(root->dot_name, ".", fde->name_len))) {
		error_msg = "invalid name for '.'";
		goto corrupted;
	}
	rlen = ext5_rec_len_from_disk(fde->rec_len, blocksize);
	if (unlikely((char *)fde + rlen >= blockend)) {
		error_msg = "invalid rec_len for '.'";
		goto corrupted;
	}

	fde = &root->dotdot;
	if (unlikely(fde->name_len != 2)) {
		error_msg = "invalid name_len for '..'";
		goto corrupted;
	}
	if (unlikely(strncmp(root->dotdot_name, "..", fde->name_len))) {
		error_msg = "invalid name for '..'";
		goto corrupted;
	}
	rlen = ext5_rec_len_from_disk(fde->rec_len, blocksize);
	if (unlikely((char *)fde + rlen >= blockend)) {
		error_msg = "invalid rec_len for '..'";
		goto corrupted;
	}

	return true;

corrupted:
	EXT5_ERROR_INODE(dir, "Corrupt dir, %s, running e2fsck is recommended",
			 error_msg);
	return false;
}

/*
 * This converts a one block unindexed directory to a 3 block indexed
 * directory, and adds the dentry to the indexed directory.
 */
static int make_indexed_dir(handle_t *handle, struct ext5_filename *fname,
			    struct inode *dir,
			    struct inode *inode, struct buffer_head *bh)
{
	struct buffer_head *bh2;
	struct dx_root	*root;
	struct dx_frame	frames[EXT5_HTREE_LEVEL], *frame;
	struct dx_entry *entries;
	struct ext5_dir_entry_2	*de, *de2;
	char		*data2, *top;
	unsigned	len;
	int		retval;
	unsigned	blocksize;
	ext5_lblk_t  block;
	struct fake_dirent *fde;
	int csum_size = 0;

	if (ext5_has_feature_metadata_csum(inode->i_sb))
		csum_size = sizeof(struct ext5_dir_entry_tail);

	blocksize =  dir->i_sb->s_blocksize;
	dxtrace(printk(KERN_DEBUG "Creating index: inode %lu\n", dir->i_ino));
	BUFFER_TRACE(bh, "get_write_access");
	retval = ext5_journal_get_write_access(handle, dir->i_sb, bh,
					       EXT5_JTR_NONE);
	if (retval) {
		ext5_std_error(dir->i_sb, retval);
		brelse(bh);
		return retval;
	}

	root = (struct dx_root *) bh->b_data;
	if (!ext5_check_dx_root(dir, root)) {
		brelse(bh);
		return -EFSCORRUPTED;
	}

	/* The 0th block becomes the root, move the dirents out */
	fde = &root->dotdot;
	de = (struct ext5_dir_entry_2 *)((char *)fde +
		ext5_rec_len_from_disk(fde->rec_len, blocksize));
	len = ((char *) root) + (blocksize - csum_size) - (char *) de;

	/* Allocate new block for the 0th block's dirents */
	bh2 = ext5_append(handle, dir, &block);
	if (IS_ERR(bh2)) {
		brelse(bh);
		return PTR_ERR(bh2);
	}
	ext5_set_inode_flag(dir, EXT5_INODE_INDEX);
	data2 = bh2->b_data;

	memcpy(data2, de, len);
	memset(de, 0, len); /* wipe old data */
	de = (struct ext5_dir_entry_2 *) data2;
	top = data2 + len;
	while ((char *)(de2 = ext5_next_entry(de, blocksize)) < top) {
		if (ext5_check_dir_entry(dir, NULL, de, bh2, data2, len,
					(char *)de - data2)) {
			brelse(bh2);
			brelse(bh);
			return -EFSCORRUPTED;
		}
		de = de2;
	}
	de->rec_len = ext5_rec_len_to_disk(data2 + (blocksize - csum_size) -
					   (char *) de, blocksize);

	if (csum_size)
		ext5_initialize_dirent_tail(bh2, blocksize);

	/* Initialize the root; the dot dirents already exist */
	de = (struct ext5_dir_entry_2 *) (&root->dotdot);
	de->rec_len = ext5_rec_len_to_disk(
			blocksize - ext5_dir_rec_len(2, NULL), blocksize);
	memset (&root->info, 0, sizeof(root->info));
	root->info.info_length = sizeof(root->info);
	if (ext5_hash_in_dirent(dir))
		root->info.hash_version = DX_HASH_SIPHASH;
	else
		root->info.hash_version =
				EXT5_SB(dir->i_sb)->s_def_hash_version;

	entries = root->entries;
	dx_set_block(entries, 1);
	dx_set_count(entries, 1);
	dx_set_limit(entries, dx_root_limit(dir, sizeof(root->info)));

	/* Initialize as for dx_probe */
	fname->hinfo.hash_version = root->info.hash_version;
	if (fname->hinfo.hash_version <= DX_HASH_TEA)
		fname->hinfo.hash_version += EXT5_SB(dir->i_sb)->s_hash_unsigned;
	fname->hinfo.seed = EXT5_SB(dir->i_sb)->s_hash_seed;

	/* casefolded encrypted hashes are computed on fname setup */
	if (!ext5_hash_in_dirent(dir)) {
		int err = ext5fs_dirhash(dir, fname_name(fname),
					 fname_len(fname), &fname->hinfo);
		if (err < 0) {
			brelse(bh2);
			brelse(bh);
			return err;
		}
	}
	memset(frames, 0, sizeof(frames));
	frame = frames;
	frame->entries = entries;
	frame->at = entries;
	frame->bh = bh;

	retval = ext5_handle_dirty_dx_node(handle, dir, frame->bh);
	if (retval)
		goto out_frames;
	retval = ext5_handle_dirty_dirblock(handle, dir, bh2);
	if (retval)
		goto out_frames;

	de = do_split(handle,dir, &bh2, frame, &fname->hinfo);
	if (IS_ERR(de)) {
		retval = PTR_ERR(de);
		goto out_frames;
	}

	retval = add_dirent_to_buf(handle, fname, dir, inode, de, bh2);
out_frames:
	/*
	 * Even if the block split failed, we have to properly write
	 * out all the changes we did so far. Otherwise we can end up
	 * with corrupted filesystem.
	 */
	if (retval)
		ext5_mark_inode_dirty(handle, dir);
	dx_release(frames);
	brelse(bh2);
	return retval;
}

/*
 *	ext5_add_entry()
 *
 * adds a file entry to the specified directory, using the same
 * semantics as ext5_find_entry(). It returns NULL if it failed.
 *
 * NOTE!! The inode part of 'de' is left at 0 - which means you
 * may not sleep between calling this and putting something into
 * the entry, as someone else might have used it while you slept.
 */
/* Add a (qstr, inode) entry; demotion uses it without a dentry. */
int ext5_add_entry_qstr(handle_t *handle, struct inode *dir,
			const struct qstr *qname, struct inode *inode)
{
	struct buffer_head *bh = NULL;
	struct ext5_dir_entry_2 *de;
	struct super_block *sb;
	struct ext5_filename fname;
	int	retval;
	int	dx_fallback = 0;
	unsigned blocksize;
	ext5_lblk_t block, blocks;
	int	csum_size = 0;

	if (ext5_dir_is_stable(dir)) {
		/* The destination is known negative, so an unlink can cancel this INSERT. */
		retval = ext5_stable_add_link(handle, dir, qname, inode, true);
		if (retval)
			return retval;
		inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
		inode_inc_iversion(dir);
		return ext5_mark_inode_dirty(handle, dir);
	}

	if (ext5_has_feature_metadata_csum(inode->i_sb))
		csum_size = sizeof(struct ext5_dir_entry_tail);

	sb = dir->i_sb;
	blocksize = sb->s_blocksize;

	if (!generic_ci_validate_strict_name(dir, (struct qstr *)qname))
		return -EINVAL;

	retval = ext5_fname_setup_filename(dir, qname, 0, &fname);
	if (retval)
		return retval;

	if (ext5_has_inline_data(dir)) {
		retval = ext5_try_add_inline_entry(handle, &fname, dir, inode);
		if (retval < 0)
			goto out;
		if (retval == 1) {
			retval = 0;
			goto out;
		}
	}

	if (is_dx(dir)) {
		retval = ext5_dx_add_entry(handle, &fname, dir, inode);
		if (!retval || (retval != ERR_BAD_DX_DIR))
			goto out;
		/* Can we just ignore htree data? */
		if (ext5_has_feature_metadata_csum(sb)) {
			EXT5_ERROR_INODE(dir,
				"Directory has corrupted htree index.");
			retval = -EFSCORRUPTED;
			goto out;
		}
		ext5_clear_inode_flag(dir, EXT5_INODE_INDEX);
		dx_fallback++;
		retval = ext5_mark_inode_dirty(handle, dir);
		if (unlikely(retval))
			goto out;
	}
	blocks = dir->i_size >> sb->s_blocksize_bits;
	for (block = 0; block < blocks; block++) {
		bh = ext5_read_dirblock(dir, block, DIRENT);
		if (bh == NULL) {
			bh = ext5_bread(handle, dir, block,
					EXT5_GET_BLOCKS_CREATE);
			goto add_to_new_block;
		}
		if (IS_ERR(bh)) {
			retval = PTR_ERR(bh);
			bh = NULL;
			goto out;
		}
		retval = add_dirent_to_buf(handle, &fname, dir, inode,
					   NULL, bh);
		if (retval != -ENOSPC)
			goto out;

		if (blocks == 1 && !dx_fallback &&
		    ext5_has_feature_dir_index(sb)) {
			retval = make_indexed_dir(handle, &fname, dir,
						  inode, bh);
			bh = NULL; /* make_indexed_dir releases bh */
			goto out;
		}
		brelse(bh);
	}
	bh = ext5_append(handle, dir, &block);
add_to_new_block:
	if (IS_ERR(bh)) {
		retval = PTR_ERR(bh);
		bh = NULL;
		goto out;
	}
	de = (struct ext5_dir_entry_2 *) bh->b_data;
	de->inode = 0;
	de->rec_len = ext5_rec_len_to_disk(blocksize - csum_size, blocksize);

	if (csum_size)
		ext5_initialize_dirent_tail(bh, blocksize);

	retval = add_dirent_to_buf(handle, &fname, dir, inode, de, bh);
out:
	ext5_fname_free_filename(&fname);
	brelse(bh);
	if (retval == 0)
		ext5_set_inode_state(inode, EXT5_STATE_NEWENTRY);
	return retval;
}

/*
 * Build a conventional directory bottom-up from an immutable, de-duplicated
 * snapshot: hash and sort once, pack each leaf once, then publish the htree
 * root.  The target stays an unreachable orphan until the caller publishes it.
 */
struct ext5_bulk_work_entry {
	u32 ino;
	u32 name_off;
	u32 hash;
	u8 name_len;
	u8 file_type;	/* VFS DT_* */
};

struct ext5_bulk_leaf {
	u32 first;
	u32 nr;
	u32 boundary_hash;
	ext5_lblk_t block;
};

struct ext5_bulk_node {
	u32 first_leaf;
	u32 nr_leaves;
	u32 boundary_hash;
	ext5_lblk_t block;
};

/* Pack leaves to 3/4, the steady-state htree load, leaving insert headroom. */
static unsigned int ext5_bulk_leaf_target(unsigned int capacity)
{
	return capacity - capacity / 4;
}

/* HTree routing uses only the 32-bit major hash. */
static int ext5_bulk_radix_sort(struct ext5_bulk_work_entry *work, u32 nr)
{
	struct ext5_bulk_work_entry *tmp;
	u32 *cursor;
	u32 i, bucket, total;

	if (nr < 2)
		return 0;
	tmp = kvmalloc_array(nr, sizeof(*tmp), GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;
	cursor = kvcalloc(1U << 16, sizeof(*cursor), GFP_KERNEL);
	if (!cursor) {
		kvfree(tmp);
		return -ENOMEM;
	}

	for (i = 0; i < nr; i++)
		cursor[work[i].hash & 0xffff]++;
	total = 0;
	for (bucket = 0; bucket < (1U << 16); bucket++) {
		u32 count = cursor[bucket];

		cursor[bucket] = total;
		total += count;
	}
	for (i = 0; i < nr; i++)
		tmp[cursor[work[i].hash & 0xffff]++] = work[i];

	memset(cursor, 0, (1U << 16) * sizeof(*cursor));
	for (i = 0; i < nr; i++)
		cursor[tmp[i].hash >> 16]++;
	total = 0;
	for (bucket = 0; bucket < (1U << 16); bucket++) {
		u32 count = cursor[bucket];

		cursor[bucket] = total;
		total += count;
	}
	for (i = 0; i < nr; i++)
		work[cursor[tmp[i].hash >> 16]++] = tmp[i];

	kvfree(cursor);
	kvfree(tmp);
	return 0;
}

static u8 ext5_bulk_dtype_to_ftype(u8 dtype)
{
	switch (dtype) {
	case DT_REG:
		return EXT5_FT_REG_FILE;
	case DT_DIR:
		return EXT5_FT_DIR;
	case DT_CHR:
		return EXT5_FT_CHRDEV;
	case DT_BLK:
		return EXT5_FT_BLKDEV;
	case DT_FIFO:
		return EXT5_FT_FIFO;
	case DT_SOCK:
		return EXT5_FT_SOCK;
	case DT_LNK:
		return EXT5_FT_SYMLINK;
	default:
		return EXT5_FT_UNKNOWN;
	}
}

static int ext5_bulk_refill_handle(handle_t *handle, struct inode *dir)
{
	int block_credits = EXT5_DATA_TRANS_BLOCKS(dir->i_sb) +
		EXT5_INDEX_EXTRA_TRANS_BLOCKS + 3;
	int refill_credits = max_t(int, block_credits, EXT5_MAX_TRANS_DATA);
	int revoke_credits = ext5_trans_default_revoke_credits(dir->i_sb);
	int err;

	err = __ext5_journal_ensure_credits(handle, block_credits,
					    refill_credits, revoke_credits);
	if (err > 0)
		err = ext5_journal_restart(handle, refill_credits,
					   revoke_credits);
	return err;
}

static void ext5_bulk_set_dirent(struct inode *dir,
				 struct ext5_dir_entry_2 *de,
				 const struct ext5_bulk_work_entry *entry,
				 const char *names, unsigned int rec_len)
{
	de->inode = cpu_to_le32(entry->ino);
	de->rec_len = ext5_rec_len_to_disk(rec_len, dir->i_sb->s_blocksize);
	de->name_len = entry->name_len;
	de->file_type = ext5_has_feature_filetype(dir->i_sb) ?
		ext5_bulk_dtype_to_ftype(entry->file_type) : EXT5_FT_UNKNOWN;
	memcpy(de->name, names + entry->name_off, entry->name_len);
}

static int ext5_bulk_write_leaf(handle_t *handle, struct inode *dir,
				const struct ext5_bulk_work_entry *work,
				const char *names, u32 first, u32 nr,
				ext5_lblk_t *block_out)
{
	unsigned int blocksize = dir->i_sb->s_blocksize;
	unsigned int csum_size = ext5_has_feature_metadata_csum(dir->i_sb) ?
		sizeof(struct ext5_dir_entry_tail) : 0;
	unsigned int capacity = blocksize - csum_size;
	struct ext5_dir_entry_2 *last = NULL;
	struct buffer_head *bh;
	ext5_lblk_t block;
	unsigned int offset = 0;
	u32 i;
	int err;

	err = ext5_bulk_refill_handle(handle, dir);
	if (err)
		return err;
	bh = ext5_append(handle, dir, &block);
	if (IS_ERR(bh))
		return PTR_ERR(bh);
	memset(bh->b_data, 0, blocksize);
	for (i = 0; i < nr; i++) {
		const struct ext5_bulk_work_entry *entry = &work[first + i];
		unsigned int rec_len = ext5_dir_rec_len(entry->name_len, dir);
		struct ext5_dir_entry_2 *de;

		if (rec_len > capacity - offset) {
			err = -EFSCORRUPTED;
			goto out;
		}
		de = (struct ext5_dir_entry_2 *)(bh->b_data + offset);
		ext5_bulk_set_dirent(dir, de, entry, names, rec_len);
		last = de;
		offset += rec_len;
	}
	if (WARN_ON_ONCE(!last)) {
		err = -EFSCORRUPTED;
		goto out;
	}
	last->rec_len = ext5_rec_len_to_disk(capacity -
					      ((char *)last - bh->b_data),
					      blocksize);
	if (csum_size)
		ext5_initialize_dirent_tail(bh, blocksize);
	err = ext5_handle_dirty_dirblock(handle, dir, bh);
	if (!err)
		set_buffer_verified(bh);
out:
	brelse(bh);
	if (!err)
		*block_out = block;
	return err;
}

static int ext5_bulk_write_node(handle_t *handle, struct inode *dir,
				const struct ext5_bulk_leaf *leaves,
				u32 first, u32 nr, ext5_lblk_t *block_out)
{
	unsigned int blocksize = dir->i_sb->s_blocksize;
	struct buffer_head *bh;
	struct dx_node *node;
	struct dx_entry *entries;
	ext5_lblk_t block;
	u32 i;
	int err;

	err = ext5_bulk_refill_handle(handle, dir);
	if (err)
		return err;
	bh = ext5_append(handle, dir, &block);
	if (IS_ERR(bh))
		return PTR_ERR(bh);
	memset(bh->b_data, 0, blocksize);
	node = (struct dx_node *)bh->b_data;
	node->fake.rec_len = ext5_rec_len_to_disk(blocksize, blocksize);
	entries = node->entries;
	dx_set_limit(entries, dx_node_limit(dir));
	dx_set_count(entries, nr);
	for (i = 0; i < nr; i++) {
		if (i)
			dx_set_hash(entries + i, leaves[first + i].boundary_hash);
		dx_set_block(entries + i, leaves[first + i].block);
	}
	err = ext5_handle_dirty_dx_node(handle, dir, bh);
	if (!err)
		set_buffer_verified(bh);
	brelse(bh);
	if (!err)
		*block_out = block;
	return err;
}

static int ext5_bulk_write_root(handle_t *handle, struct inode *dir,
				const struct ext5_bulk_leaf *leaves,
				const struct ext5_bulk_node *nodes,
				u32 nr_children, u8 depth)
{
	unsigned int blocksize = dir->i_sb->s_blocksize;
	struct buffer_head *bh;
	struct dx_root *root;
	struct dx_entry *entries;
	u32 i;
	int err;

	err = ext5_bulk_refill_handle(handle, dir);
	if (err)
		return err;
	bh = ext5_read_dirblock(dir, 0, DIRENT);
	if (IS_ERR(bh))
		return PTR_ERR(bh);
	root = (struct dx_root *)bh->b_data;
	if (!ext5_check_dx_root(dir, root)) {
		err = -EFSCORRUPTED;
		goto out;
	}
	err = ext5_journal_get_write_access(handle, dir->i_sb, bh,
					    EXT5_JTR_NONE);
	if (err)
		goto out;
	/* An htree root owns a dx tail, so dotdot spans the whole block, as in
	 * make_indexed_dir(). */
	root->dotdot.rec_len = ext5_rec_len_to_disk(blocksize -
						ext5_dir_rec_len(1, NULL),
						blocksize);
	memset(&root->info, 0, blocksize -
		((char *)&root->info - (char *)root));
	root->info.info_length = sizeof(root->info);
	root->info.indirect_levels = depth;
	root->info.hash_version = EXT5_SB(dir->i_sb)->s_def_hash_version;
	entries = root->entries;
	dx_set_limit(entries, dx_root_limit(dir, sizeof(root->info)));
	dx_set_count(entries, nr_children);
	for (i = 0; i < nr_children; i++) {
		u32 boundary = depth ? nodes[i].boundary_hash :
			leaves[i].boundary_hash;
		ext5_lblk_t block = depth ? nodes[i].block : leaves[i].block;

		if (i)
			dx_set_hash(entries + i, boundary);
		dx_set_block(entries + i, block);
	}
	ext5_set_inode_flag(dir, EXT5_INODE_INDEX);
	err = ext5_handle_dirty_dx_node(handle, dir, bh);
	if (!err)
		err = ext5_mark_inode_dirty(handle, dir);
	if (!err)
		set_buffer_verified(bh);
out:
	brelse(bh);
	return err;
}

static int ext5_bulk_build_linear(handle_t *handle, struct inode *dir,
				  const struct ext5_bulk_work_entry *work,
				  u32 nr_entries, const char *names)
{
	unsigned int blocksize = dir->i_sb->s_blocksize;
	unsigned int csum_size = ext5_has_feature_metadata_csum(dir->i_sb) ?
		sizeof(struct ext5_dir_entry_tail) : 0;
	unsigned int capacity = blocksize - csum_size;
	u32 next = 0;
	bool first = true;

	while (next < nr_entries) {
		struct ext5_dir_entry_2 *last = NULL;
		struct buffer_head *bh;
		unsigned int offset;
		ext5_lblk_t block;
		int err;

		err = ext5_bulk_refill_handle(handle, dir);
		if (err)
			return err;
		if (first) {
			struct ext5_dir_entry_2 *dot;

			bh = ext5_read_dirblock(dir, 0, DIRENT);
			if (IS_ERR(bh))
				return PTR_ERR(bh);
			err = ext5_journal_get_write_access(handle, dir->i_sb, bh,
							    EXT5_JTR_NONE);
			if (err) {
				brelse(bh);
				return err;
			}
			dot = (struct ext5_dir_entry_2 *)bh->b_data;
			last = ext5_next_entry(dot, blocksize);
			offset = ext5_dir_rec_len(1, NULL) +
				ext5_dir_rec_len(2, NULL);
			last->rec_len = ext5_rec_len_to_disk(
				ext5_dir_rec_len(2, NULL), blocksize);
			first = false;
		} else {
			bh = ext5_append(handle, dir, &block);
			if (IS_ERR(bh))
				return PTR_ERR(bh);
			memset(bh->b_data, 0, blocksize);
			offset = 0;
		}

		while (next < nr_entries) {
			const struct ext5_bulk_work_entry *entry = &work[next];
			unsigned int rec_len = ext5_dir_rec_len(entry->name_len,
								      dir);
			struct ext5_dir_entry_2 *de;

			if (rec_len > capacity - offset)
				break;
			de = (struct ext5_dir_entry_2 *)(bh->b_data + offset);
			ext5_bulk_set_dirent(dir, de, entry, names, rec_len);
			last = de;
			offset += rec_len;
			next++;
		}
		if (WARN_ON_ONCE(!last || (!offset && next < nr_entries))) {
			brelse(bh);
			return -EFSCORRUPTED;
		}
		last->rec_len = ext5_rec_len_to_disk(capacity -
					      ((char *)last - bh->b_data),
					      blocksize);
		if (csum_size)
			ext5_initialize_dirent_tail(bh, blocksize);
		err = ext5_handle_dirty_dirblock(handle, dir, bh);
		if (!err)
			set_buffer_verified(bh);
		brelse(bh);
		if (err)
			return err;
		cond_resched();
	}
	return 0;
}

int ext5_dir_rebuild_bulk(struct inode *dir,
			  const struct ext5_dir_bulk_entry *source,
			  u32 nr_entries, const char *names,
			  u32 names_bytes)
{
	struct ext5_bulk_work_entry *work = NULL;
	struct ext5_bulk_leaf *leaves = NULL;
	struct ext5_bulk_node *nodes = NULL;
	struct super_block *sb = dir->i_sb;
	unsigned int blocksize = sb->s_blocksize;
	unsigned int csum_size = ext5_has_feature_metadata_csum(sb) ?
		sizeof(struct ext5_dir_entry_tail) : 0;
	unsigned int capacity = blocksize - csum_size;
	unsigned int first_block_capacity = capacity -
		ext5_dir_rec_len(1, dir) - ext5_dir_rec_len(2, dir);
	unsigned int leaf_target = ext5_bulk_leaf_target(capacity);
	int block_credits = EXT5_DATA_TRANS_BLOCKS(sb) +
		EXT5_INDEX_EXTRA_TRANS_BLOCKS + 3;
	int start_credits = max_t(int, block_credits, EXT5_MAX_TRANS_DATA);
	handle_t *handle;
	u32 nr_leaves = 0, nr_nodes = 0;
	u64 linear_bytes = 0;
	u32 i, first, leaf;
	int err = 0, stop_err;

	lockdep_assert_held(&dir->i_rwsem);
	if (!nr_entries)
		return 0;
	if (!source || !names || ext5_has_inline_data(dir) ||
	    i_size_read(dir) != blocksize)
		return -EINVAL;

	work = kvmalloc_array(nr_entries, sizeof(*work), GFP_KERNEL);
	if (!work)
		return -ENOMEM;
	for (i = 0; i < nr_entries; i++) {
		if (!source[i].name_len ||
		    source[i].name_off > names_bytes ||
		    source[i].name_len > names_bytes - source[i].name_off ||
		    !ext5_valid_inum(sb, source[i].ino)) {
			err = -EFSCORRUPTED;
			goto out;
		}
		work[i].ino = source[i].ino;
		work[i].name_off = source[i].name_off;
		work[i].name_len = source[i].name_len;
		work[i].file_type = source[i].file_type;
		work[i].hash = source[i].dx_hash;
		linear_bytes += ext5_dir_rec_len(source[i].name_len, dir);
		if (!(i & 4095))
			cond_resched();
	}

	/* A one-block directory stays linear, without an htree. */
	if (!ext5_has_feature_dir_index(sb) ||
	    linear_bytes <= first_block_capacity) {
		handle = ext5_journal_start(dir, EXT5_HT_DIR, start_credits);
		if (IS_ERR(handle)) {
			err = PTR_ERR(handle);
			goto out;
		}
		err = ext5_bulk_build_linear(handle, dir, work, nr_entries,
					    names);
		stop_err = ext5_journal_stop(handle);
		if (!err)
			err = stop_err;
		goto out;
	}

	err = ext5_bulk_radix_sort(work, nr_entries);
	if (err)
		goto out;
	/* The merged generation must be a set: reject duplicate names. */
	for (i = 1; i < nr_entries; i++) {
		const struct ext5_bulk_work_entry *left = &work[i - 1];
		const struct ext5_bulk_work_entry *right = &work[i];

		if (left->hash == right->hash &&
		    left->name_len == right->name_len &&
		    !memcmp(names + left->name_off, names + right->name_off,
			    left->name_len)) {
			err = -EFSCORRUPTED;
			goto out;
		}
	}
	for (i = 0; i < nr_entries; ) {
		unsigned int used = 0;

		do {
			unsigned int rec_len = ext5_dir_rec_len(work[i].name_len,
								 dir);

			if (rec_len > capacity) {
				err = -ENAMETOOLONG;
				goto out;
			}
			if (used && (used >= leaf_target ||
				     rec_len > leaf_target - used))
				break;
			used += rec_len;
			i++;
		} while (i < nr_entries);
		if (nr_leaves == UINT_MAX) {
			err = -EOVERFLOW;
			goto out;
		}
		nr_leaves++;
	}
	leaves = kvmalloc_array(nr_leaves, sizeof(*leaves), GFP_KERNEL);
	if (!leaves) {
		err = -ENOMEM;
		goto out;
	}
	if (nr_leaves > dx_root_limit(dir, sizeof(struct dx_root_info))) {
		u32 node_limit = dx_node_limit(dir);

		if (!node_limit) {
			err = -EFSCORRUPTED;
			goto out;
		}
		nr_nodes = DIV_ROUND_UP(nr_leaves, node_limit);
		if (nr_nodes > dx_root_limit(dir,
						 sizeof(struct dx_root_info)) ||
		    ext5_dir_htree_level(sb) < 2) {
			err = -ENOSPC;
			goto out;
		}
		nodes = kvmalloc_array(nr_nodes, sizeof(*nodes), GFP_KERNEL);
		if (!nodes) {
			err = -ENOMEM;
			goto out;
		}
	}

	handle = ext5_journal_start(dir, EXT5_HT_DIR, start_credits);
	if (IS_ERR(handle)) {
		err = PTR_ERR(handle);
		goto out;
	}
	first = 0;
	for (leaf = 0; leaf < nr_leaves; leaf++) {
		unsigned int used = 0;
		u32 end = first;

		while (end < nr_entries) {
			unsigned int rec_len = ext5_dir_rec_len(work[end].name_len,
								      dir);

			if (used && (used >= leaf_target ||
				     rec_len > leaf_target - used))
				break;
			used += rec_len;
			end++;
		}
		leaves[leaf].first = first;
		leaves[leaf].nr = end - first;
		leaves[leaf].boundary_hash = work[first].hash;
		if (leaf && work[first - 1].hash == work[first].hash)
			leaves[leaf].boundary_hash |= 1;
		err = ext5_bulk_write_leaf(handle, dir, work, names, first,
					   end - first, &leaves[leaf].block);
		if (err)
			break;
		first = end;
		cond_resched();
	}

	if (!err && nr_nodes) {
		u32 node_limit = dx_node_limit(dir);

		first = 0;
		for (i = 0; i < nr_nodes; i++) {
			u32 count = min_t(u32, node_limit, nr_leaves - first);

			nodes[i].first_leaf = first;
			nodes[i].nr_leaves = count;
			nodes[i].boundary_hash = leaves[first].boundary_hash;
			err = ext5_bulk_write_node(handle, dir, leaves, first,
						   count, &nodes[i].block);
			if (err)
				break;
			first += count;
		}
	}
	if (!err)
		err = ext5_bulk_write_root(handle, dir, leaves, nodes,
					   nr_nodes ? nr_nodes : nr_leaves,
					   nr_nodes ? 1 : 0);
	stop_err = ext5_journal_stop(handle);
	if (!err)
		err = stop_err;
out:
	kvfree(nodes);
	kvfree(leaves);
	kvfree(work);
	return err;
}

static int ext5_add_entry(handle_t *handle, struct dentry *dentry,
			  struct inode *inode)
{
	struct inode *dir = d_inode(dentry->d_parent);

	if (fscrypt_is_nokey_name(dentry))
		return -ENOKEY;
	return ext5_add_entry_qstr(handle, dir, &dentry->d_name, inode);
}

/*
 * Returns 0 for success, or a negative error value
 */
static int ext5_dx_add_entry(handle_t *handle, struct ext5_filename *fname,
			     struct inode *dir, struct inode *inode)
{
	struct dx_frame frames[EXT5_HTREE_LEVEL], *frame;
	struct dx_entry *entries, *at;
	struct buffer_head *bh;
	struct super_block *sb = dir->i_sb;
	struct ext5_dir_entry_2 *de;
	int restart;
	int err;

again:
	restart = 0;
	frame = dx_probe(fname, dir, NULL, frames);
	if (IS_ERR(frame))
		return PTR_ERR(frame);
	entries = frame->entries;
	at = frame->at;
	bh = ext5_read_dirblock(dir, dx_get_block(frame->at), DIRENT_HTREE);
	if (IS_ERR(bh)) {
		err = PTR_ERR(bh);
		bh = NULL;
		goto cleanup;
	}

	BUFFER_TRACE(bh, "get_write_access");
	err = ext5_journal_get_write_access(handle, sb, bh, EXT5_JTR_NONE);
	if (err)
		goto journal_error;

	err = add_dirent_to_buf(handle, fname, dir, inode, NULL, bh);
	if (err != -ENOSPC)
		goto cleanup;

	err = 0;
	/* Block full, should compress but for now just split */
	dxtrace(printk(KERN_DEBUG "using %u of %u node entries\n",
		       dx_get_count(entries), dx_get_limit(entries)));
	/* Need to split index? */
	if (dx_get_count(entries) == dx_get_limit(entries)) {
		ext5_lblk_t newblock;
		int levels = frame - frames + 1;
		unsigned int icount;
		int add_level = 1;
		struct dx_entry *entries2;
		struct dx_node *node2;
		struct buffer_head *bh2;

		while (frame > frames) {
			if (dx_get_count((frame - 1)->entries) <
			    dx_get_limit((frame - 1)->entries)) {
				add_level = 0;
				break;
			}
			frame--; /* split higher index block */
			at = frame->at;
			entries = frame->entries;
			restart = 1;
		}
		if (add_level && levels == ext5_dir_htree_level(sb)) {
			ext5_warning(sb, "Directory (ino: %lu) index full, "
					 "reach max htree level :%d",
					 dir->i_ino, levels);
			if (ext5_dir_htree_level(sb) < EXT5_HTREE_LEVEL) {
				ext5_warning(sb, "Large directory feature is "
						 "not enabled on this "
						 "filesystem");
			}
			err = -ENOSPC;
			goto cleanup;
		}
		icount = dx_get_count(entries);
		bh2 = ext5_append(handle, dir, &newblock);
		if (IS_ERR(bh2)) {
			err = PTR_ERR(bh2);
			goto cleanup;
		}
		node2 = (struct dx_node *)(bh2->b_data);
		entries2 = node2->entries;
		memset(&node2->fake, 0, sizeof(struct fake_dirent));
		node2->fake.rec_len = ext5_rec_len_to_disk(sb->s_blocksize,
							   sb->s_blocksize);
		BUFFER_TRACE(frame->bh, "get_write_access");
		err = ext5_journal_get_write_access(handle, sb, frame->bh,
						    EXT5_JTR_NONE);
		if (err) {
			brelse(bh2);
			goto journal_error;
		}
		if (!add_level) {
			unsigned icount1 = icount/2, icount2 = icount - icount1;
			unsigned hash2 = dx_get_hash(entries + icount1);
			dxtrace(printk(KERN_DEBUG "Split index %i/%i\n",
				       icount1, icount2));

			BUFFER_TRACE(frame->bh, "get_write_access"); /* index root */
			err = ext5_journal_get_write_access(handle, sb,
							    (frame - 1)->bh,
							    EXT5_JTR_NONE);
			if (err) {
				brelse(bh2);
				goto journal_error;
			}

			memcpy((char *) entries2, (char *) (entries + icount1),
			       icount2 * sizeof(struct dx_entry));
			dx_set_count(entries, icount1);
			dx_set_count(entries2, icount2);
			dx_set_limit(entries2, dx_node_limit(dir));

			/* Which index block gets the new entry? */
			if (at - entries >= icount1) {
				frame->at = at - entries - icount1 + entries2;
				frame->entries = entries = entries2;
				swap(frame->bh, bh2);
			}
			dx_insert_block((frame - 1), hash2, newblock);
			dxtrace(dx_show_index("node", frame->entries));
			dxtrace(dx_show_index("node",
			       ((struct dx_node *) bh2->b_data)->entries));
			err = ext5_handle_dirty_dx_node(handle, dir, bh2);
			if (err) {
				brelse(bh2);
				goto journal_error;
			}
			brelse (bh2);
			err = ext5_handle_dirty_dx_node(handle, dir,
						   (frame - 1)->bh);
			if (err)
				goto journal_error;
			err = ext5_handle_dirty_dx_node(handle, dir,
							frame->bh);
			if (restart || err)
				goto journal_error;
		} else {
			struct dx_root *dxroot;
			memcpy((char *) entries2, (char *) entries,
			       icount * sizeof(struct dx_entry));
			dx_set_limit(entries2, dx_node_limit(dir));

			/* Set up root */
			dx_set_count(entries, 1);
			dx_set_block(entries + 0, newblock);
			dxroot = (struct dx_root *)frames[0].bh->b_data;
			dxroot->info.indirect_levels += 1;
			dxtrace(printk(KERN_DEBUG
				       "Creating %d level index...\n",
				       dxroot->info.indirect_levels));
			err = ext5_handle_dirty_dx_node(handle, dir, frame->bh);
			if (err) {
				brelse(bh2);
				goto journal_error;
			}
			err = ext5_handle_dirty_dx_node(handle, dir, bh2);
			brelse(bh2);
			restart = 1;
			goto journal_error;
		}
	}
	de = do_split(handle, dir, &bh, frame, &fname->hinfo);
	if (IS_ERR(de)) {
		err = PTR_ERR(de);
		goto cleanup;
	}
	err = add_dirent_to_buf(handle, fname, dir, inode, de, bh);
	goto cleanup;

journal_error:
	ext5_std_error(dir->i_sb, err); /* this is a no-op if err == 0 */
cleanup:
	brelse(bh);
	dx_release(frames);
	/* @restart is true means htree-path has been changed, we need to
	 * repeat dx_probe() to find out valid htree-path
	 */
	if (restart && err == 0)
		goto again;
	return err;
}

/*
 * ext5_generic_delete_entry deletes a directory entry by merging it
 * with the previous entry
 */
int ext5_generic_delete_entry(struct inode *dir,
			      struct ext5_dir_entry_2 *de_del,
			      struct buffer_head *bh,
			      void *entry_buf,
			      int buf_size,
			      int csum_size)
{
	struct ext5_dir_entry_2 *de, *pde;
	unsigned int blocksize = dir->i_sb->s_blocksize;
	int i;

	i = 0;
	pde = NULL;
	de = entry_buf;
	while (i < buf_size - csum_size) {
		if (ext5_check_dir_entry(dir, NULL, de, bh,
					 entry_buf, buf_size, i))
			return -EFSCORRUPTED;
		if (de == de_del)  {
			if (pde) {
				pde->rec_len = ext5_rec_len_to_disk(
					ext5_rec_len_from_disk(pde->rec_len,
							       blocksize) +
					ext5_rec_len_from_disk(de->rec_len,
							       blocksize),
					blocksize);

				/* wipe entire dir_entry */
				memset(de, 0, ext5_rec_len_from_disk(de->rec_len,
								blocksize));
			} else {
				/* wipe dir_entry excluding the rec_len field */
				de->inode = 0;
				memset(&de->name_len, 0,
					ext5_rec_len_from_disk(de->rec_len,
								blocksize) -
					offsetof(struct ext5_dir_entry_2,
								name_len));
			}

			inode_inc_iversion(dir);
			return 0;
		}
		i += ext5_rec_len_from_disk(de->rec_len, blocksize);
		pde = de;
		de = ext5_next_entry(de, blocksize);
	}
	return -ENOENT;
}

static int ext5_delete_entry(handle_t *handle,
			     struct inode *dir,
			     struct ext5_dir_entry_2 *de_del,
			     struct buffer_head *bh)
{
	int err, csum_size = 0;

	/* Stable directories have no dirent pages. */
	if (ext5_dir_is_stable(dir))
		return -EROFS;

	if (ext5_has_inline_data(dir)) {
		int has_inline_data = 1;
		err = ext5_delete_inline_entry(handle, dir, de_del, bh,
					       &has_inline_data);
		if (has_inline_data)
			return err;
	}

	if (ext5_has_feature_metadata_csum(dir->i_sb))
		csum_size = sizeof(struct ext5_dir_entry_tail);

	BUFFER_TRACE(bh, "get_write_access");
	err = ext5_journal_get_write_access(handle, dir->i_sb, bh,
					    EXT5_JTR_NONE);
	if (unlikely(err))
		goto out;

	err = ext5_generic_delete_entry(dir, de_del, bh, bh->b_data,
					dir->i_sb->s_blocksize, csum_size);
	if (err)
		goto out;

	BUFFER_TRACE(bh, "call ext5_handle_dirty_metadata");
	err = ext5_handle_dirty_dirblock(handle, dir, bh);
	if (unlikely(err))
		goto out;

	return 0;
out:
	if (err != -ENOENT)
		ext5_std_error(dir->i_sb, err);
	return err;
}

/*
 * Set directory link count to 1 if nlinks > EXT5_LINK_MAX, or if nlinks == 2
 * since this indicates that nlinks count was previously 1 to avoid overflowing
 * the 16-bit i_links_count field on disk.  Directories with i_nlink == 1 mean
 * that subdirectory link counts are not being maintained accurately.
 *
 * The caller has already checked for i_nlink overflow in case the DIR_LINK
 * feature is not enabled and returned -EMLINK.  The is_dx() check is a proxy
 * for checking S_ISDIR(inode) (since the INODE_INDEX feature will not be set
 * on regular files) and to avoid creating huge/slow non-HTREE directories.
 */
void ext5_inc_count(struct inode *inode)
{
	inc_nlink(inode);
	if (is_dx(inode) &&
	    (inode->i_nlink > EXT5_LINK_MAX || inode->i_nlink == 2))
		set_nlink(inode, 1);
}

/*
 * If a directory had nlink == 1, then we should let it be 1. This indicates
 * directory has >EXT5_LINK_MAX subdirs.
 */
static void ext5_dec_count(struct inode *inode)
{
	if (!S_ISDIR(inode->i_mode) || inode->i_nlink > 2)
		drop_nlink(inode);
}


/*
 * Add non-directory inode to a directory. On success, the inode reference is
 * consumed by dentry is instantiation. This is also indicated by clearing of
 * *inodep pointer. On failure, the caller is responsible for dropping the
 * inode reference in the safe context.
 */
static int ext5_add_nondir(handle_t *handle,
		struct dentry *dentry, struct inode **inodep)
{
	struct inode *dir = d_inode(dentry->d_parent);
	struct inode *inode = *inodep;
	int err = ext5_add_entry(handle, dentry, inode);
	if (!err) {
		err = ext5_mark_inode_dirty(handle, inode);
		if (IS_DIRSYNC(dir))
			ext5_handle_sync(handle);
		d_instantiate_new(dentry, inode);
		*inodep = NULL;
		return err;
	}
	drop_nlink(inode);
	ext5_mark_inode_dirty(handle, inode);
	ext5_orphan_add(handle, inode);
	unlock_new_inode(inode);
	return err;
}

/*
 * By the time this is called, we already have created
 * the directory cache entry for the new file, but it
 * is so far negative - it has no inode.
 *
 * If the create succeeds, we fill in the inode information
 * with d_instantiate().
 */
static int ext5_create(struct mnt_idmap *idmap, struct inode *dir,
		       struct dentry *dentry, umode_t mode, bool excl)
{
	handle_t *handle;
	struct inode *inode;
	int err, credits, retries = 0;

	err = dquot_initialize(dir);
	if (err)
		return err;

	credits = (EXT5_DATA_TRANS_BLOCKS(dir->i_sb) +
		   EXT5_INDEX_EXTRA_TRANS_BLOCKS + 3);
retry:
	inode = ext5_new_inode_start_handle(idmap, dir, mode, &dentry->d_name,
					    0, NULL, EXT5_HT_DIR, credits);
	handle = ext5_journal_current_handle();
	err = PTR_ERR(inode);
	if (!IS_ERR(inode)) {
		inode->i_op = &ext5_file_inode_operations;
		inode->i_fop = &ext5_file_operations;
		ext5_set_aops(inode);
		err = ext5_add_nondir(handle, dentry, &inode);
		if (!err)
			ext5_fc_track_create(handle, dentry);
	}
	if (handle)
		ext5_journal_stop(handle);
	if (!IS_ERR_OR_NULL(inode))
		iput(inode);
	if (err == -ENOSPC && ext5_should_retry_alloc(dir->i_sb, &retries))
		goto retry;
	if (!err) {
		ext5_subtree_count_inc(dentry, false);
	}
	return err;
}

static int ext5_mknod(struct mnt_idmap *idmap, struct inode *dir,
		      struct dentry *dentry, umode_t mode, dev_t rdev)
{
	handle_t *handle;
	struct inode *inode;
	int err, credits, retries = 0;

	err = dquot_initialize(dir);
	if (err)
		return err;

	credits = (EXT5_DATA_TRANS_BLOCKS(dir->i_sb) +
		   EXT5_INDEX_EXTRA_TRANS_BLOCKS + 3);
retry:
	inode = ext5_new_inode_start_handle(idmap, dir, mode, &dentry->d_name,
					    0, NULL, EXT5_HT_DIR, credits);
	handle = ext5_journal_current_handle();
	err = PTR_ERR(inode);
	if (!IS_ERR(inode)) {
		init_special_inode(inode, inode->i_mode, rdev);
		inode->i_op = &ext5_special_inode_operations;
		err = ext5_add_nondir(handle, dentry, &inode);
		if (!err)
			ext5_fc_track_create(handle, dentry);
	}
	if (handle)
		ext5_journal_stop(handle);
	if (!IS_ERR_OR_NULL(inode))
		iput(inode);
	if (err == -ENOSPC && ext5_should_retry_alloc(dir->i_sb, &retries))
		goto retry;
	if (!err) {
		ext5_subtree_count_inc(dentry, false);
	}
	return err;
}

static int ext5_tmpfile(struct mnt_idmap *idmap, struct inode *dir,
			struct file *file, umode_t mode)
{
	handle_t *handle;
	struct inode *inode;
	int err, retries = 0;

	err = dquot_initialize(dir);
	if (err)
		return err;

retry:
	inode = ext5_new_inode_start_handle(idmap, dir, mode,
					    NULL, 0, NULL,
					    EXT5_HT_DIR,
			EXT5_MAXQUOTAS_TRANS_BLOCKS(dir->i_sb) +
			  4 + EXT5_XATTR_TRANS_BLOCKS);
	handle = ext5_journal_current_handle();
	err = PTR_ERR(inode);
	if (!IS_ERR(inode)) {
		inode->i_op = &ext5_file_inode_operations;
		inode->i_fop = &ext5_file_operations;
		ext5_set_aops(inode);
		d_tmpfile(file, inode);
		err = ext5_orphan_add(handle, inode);
		if (err)
			goto err_unlock_inode;
		mark_inode_dirty(inode);
		unlock_new_inode(inode);
	}
	if (handle)
		ext5_journal_stop(handle);
	if (err == -ENOSPC && ext5_should_retry_alloc(dir->i_sb, &retries))
		goto retry;
	return finish_open_simple(file, err);
err_unlock_inode:
	ext5_journal_stop(handle);
	unlock_new_inode(inode);
	return err;
}

struct ext5_dir_entry_2 *ext5_init_dot_dotdot(struct inode *inode,
			  struct ext5_dir_entry_2 *de,
			  int blocksize, int csum_size,
			  unsigned int parent_ino, int dotdot_real_len)
{
	de->inode = cpu_to_le32(inode->i_ino);
	de->name_len = 1;
	de->rec_len = ext5_rec_len_to_disk(ext5_dir_rec_len(de->name_len, NULL),
					   blocksize);
	strcpy(de->name, ".");
	ext5_set_de_type(inode->i_sb, de, S_IFDIR);

	de = ext5_next_entry(de, blocksize);
	de->inode = cpu_to_le32(parent_ino);
	de->name_len = 2;
	if (!dotdot_real_len)
		de->rec_len = ext5_rec_len_to_disk(blocksize -
					(csum_size + ext5_dir_rec_len(1, NULL)),
					blocksize);
	else
		de->rec_len = ext5_rec_len_to_disk(
					ext5_dir_rec_len(de->name_len, NULL),
					blocksize);
	strcpy(de->name, "..");
	ext5_set_de_type(inode->i_sb, de, S_IFDIR);

	return ext5_next_entry(de, blocksize);
}

int ext5_init_new_dir(handle_t *handle, struct inode *dir,
			     struct inode *inode)
{
	struct buffer_head *dir_block = NULL;
	struct ext5_dir_entry_2 *de;
	ext5_lblk_t block = 0;
	unsigned int blocksize = dir->i_sb->s_blocksize;
	int csum_size = 0;
	int err;

	if (ext5_has_feature_metadata_csum(dir->i_sb))
		csum_size = sizeof(struct ext5_dir_entry_tail);

	if (ext5_test_inode_state(inode, EXT5_STATE_MAY_INLINE_DATA)) {
		err = ext5_try_create_inline_dir(handle, dir, inode);
		if (err < 0 && err != -ENOSPC)
			goto out;
		if (!err)
			goto out;
	}

	inode->i_size = 0;
	dir_block = ext5_append(handle, inode, &block);
	if (IS_ERR(dir_block))
		return PTR_ERR(dir_block);
	de = (struct ext5_dir_entry_2 *)dir_block->b_data;
	ext5_init_dot_dotdot(inode, de, blocksize, csum_size, dir->i_ino, 0);
	set_nlink(inode, 2);
	if (csum_size)
		ext5_initialize_dirent_tail(dir_block, blocksize);

	BUFFER_TRACE(dir_block, "call ext5_handle_dirty_metadata");
	err = ext5_handle_dirty_dirblock(handle, inode, dir_block);
	if (err)
		goto out;
	set_buffer_verified(dir_block);
out:
	brelse(dir_block);
	return err;
}

static struct dentry *ext5_mkdir(struct mnt_idmap *idmap, struct inode *dir,
				 struct dentry *dentry, umode_t mode)
{
	handle_t *handle;
	struct inode *inode;
	int err, err2 = 0, credits, retries = 0;

	if (EXT5_DIR_LINK_MAX(dir))
		return ERR_PTR(-EMLINK);

	err = dquot_initialize(dir);
	if (err)
		return ERR_PTR(err);

	credits = (EXT5_DATA_TRANS_BLOCKS(dir->i_sb) +
		   EXT5_INDEX_EXTRA_TRANS_BLOCKS + 3);
retry:
	inode = ext5_new_inode_start_handle(idmap, dir, S_IFDIR | mode,
					    &dentry->d_name,
					    0, NULL, EXT5_HT_DIR, credits);
	handle = ext5_journal_current_handle();
	err = PTR_ERR(inode);
	if (IS_ERR(inode))
		goto out_stop;

	inode->i_op = &ext5_dir_inode_operations;
	inode->i_fop = &ext5_dir_operations;
	err = ext5_init_new_dir(handle, dir, inode);
	if (err)
		goto out_clear_inode;
	err = ext5_mark_inode_dirty(handle, inode);
	if (!err)
		err = ext5_add_entry(handle, dentry, inode);
	if (err) {
out_clear_inode:
		clear_nlink(inode);
		ext5_orphan_add(handle, inode);
		unlock_new_inode(inode);
		err2 = ext5_mark_inode_dirty(handle, inode);
		if (unlikely(err2))
			err = err2;
		ext5_journal_stop(handle);
		iput(inode);
		goto out_retry;
	}
	ext5_inc_count(dir);

	ext5_update_dx_flag(dir);
	err = ext5_mark_inode_dirty(handle, dir);
	if (err)
		goto out_clear_inode;
	d_instantiate_new(dentry, inode);
	ext5_fc_track_create(handle, dentry);
	if (IS_DIRSYNC(dir))
		ext5_handle_sync(handle);

out_stop:
	if (handle)
		ext5_journal_stop(handle);
out_retry:
	if (err == -ENOSPC && ext5_should_retry_alloc(dir->i_sb, &retries))
		goto retry;
	if (!err) {
		ext5_subtree_count_inc(dentry, true);
	}
	return ERR_PTR(err);
}

/*
 * routine to check that the specified directory is empty (for rmdir)
 */
bool ext5_empty_dir(struct inode *inode)
{
	unsigned int offset;
	struct buffer_head *bh;
	struct ext5_dir_entry_2 *de;
	struct super_block *sb;

	/* On a parse or read failure report non-empty, so rmdir refuses. */
	if (ext5_dir_is_stable(inode))
		return ext5_stable_empty_dir(inode) ? true : false;

	if (ext5_has_inline_data(inode)) {
		int has_inline_data = 1;
		int ret;

		ret = empty_inline_dir(inode, &has_inline_data);
		if (has_inline_data)
			return ret;
	}

	sb = inode->i_sb;
	if (inode->i_size < ext5_dir_rec_len(1, NULL) +
					ext5_dir_rec_len(2, NULL)) {
		EXT5_ERROR_INODE(inode, "invalid size");
		return false;
	}
	bh = ext5_read_dirblock(inode, 0, EITHER);
	if (IS_ERR(bh))
		return false;

	de = (struct ext5_dir_entry_2 *) bh->b_data;
	if (ext5_check_dir_entry(inode, NULL, de, bh, bh->b_data, bh->b_size,
				 0) ||
	    le32_to_cpu(de->inode) != inode->i_ino || strcmp(".", de->name)) {
		ext5_warning_inode(inode, "directory missing '.'");
		brelse(bh);
		return false;
	}
	offset = ext5_rec_len_from_disk(de->rec_len, sb->s_blocksize);
	de = ext5_next_entry(de, sb->s_blocksize);
	if (ext5_check_dir_entry(inode, NULL, de, bh, bh->b_data, bh->b_size,
				 offset) ||
	    le32_to_cpu(de->inode) == 0 || strcmp("..", de->name)) {
		ext5_warning_inode(inode, "directory missing '..'");
		brelse(bh);
		return false;
	}
	offset += ext5_rec_len_from_disk(de->rec_len, sb->s_blocksize);
	while (offset < inode->i_size) {
		if (!(offset & (sb->s_blocksize - 1))) {
			unsigned int lblock;
			brelse(bh);
			lblock = offset >> EXT5_BLOCK_SIZE_BITS(sb);
			bh = ext5_read_dirblock(inode, lblock, EITHER);
			if (bh == NULL) {
				offset += sb->s_blocksize;
				continue;
			}
			if (IS_ERR(bh))
				return false;
		}
		de = (struct ext5_dir_entry_2 *) (bh->b_data +
					(offset & (sb->s_blocksize - 1)));
		if (ext5_check_dir_entry(inode, NULL, de, bh,
					 bh->b_data, bh->b_size, offset) ||
		    le32_to_cpu(de->inode)) {
			brelse(bh);
			return false;
		}
		offset += ext5_rec_len_from_disk(de->rec_len, sb->s_blocksize);
	}
	brelse(bh);
	return true;
}

static int ext5_rmdir(struct inode *dir, struct dentry *dentry)
{
	int retval;
	struct inode *inode;
	struct buffer_head *bh;
	struct ext5_dir_entry_2 *de;
	handle_t *handle = NULL;

	retval = ext5_emergency_state(dir->i_sb);
	if (unlikely(retval))
		return retval;

	/* Initialize quotas before so that eventual writes go in
	 * separate transaction */
	retval = dquot_initialize(dir);
	if (retval)
		return retval;
	retval = dquot_initialize(d_inode(dentry));
	if (retval)
		return retval;

	if (ext5_dir_is_stable(dir)) {
		inode = d_inode(dentry);
		if (EXT5_I(inode)->i_li_state &&
		    atomic_read(&EXT5_I(inode)->i_li_rehome_pending))
			return -EAGAIN;
		if (!ext5_empty_dir(inode))
			return -ENOTEMPTY;
		handle = ext5_journal_start(dir, EXT5_HT_DIR,
					    EXT5_DATA_TRANS_BLOCKS(dir->i_sb));
		if (IS_ERR(handle))
			return PTR_ERR(handle);
		retval = ext5_stable_delete_name(handle, dir, &dentry->d_name);
		if (retval)
			goto out_stable_rmdir;
		if (IS_DIRSYNC(dir))
			ext5_handle_sync(handle);
		inode_inc_iversion(dir);
		inode_inc_iversion(inode);
		clear_nlink(inode);
		inode->i_size = 0;
		ext5_orphan_add(handle, inode);
		inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
		inode_set_ctime_current(inode);
		retval = ext5_mark_inode_dirty(handle, inode);
		if (retval)
			goto out_stable_rmdir;
		ext5_dec_count(dir);
		retval = ext5_mark_inode_dirty(handle, dir);
out_stable_rmdir:
		{
			int stop_err = ext5_journal_stop(handle);

			if (!retval)
				retval = stop_err;
		}
		if (!retval)
			ext5_subtree_count_dec(dentry, true);
		return retval;
	}

	retval = -ENOENT;
	bh = ext5_find_entry(dir, &dentry->d_name, &de, NULL);
	if (IS_ERR(bh))
		return PTR_ERR(bh);
	if (!bh)
		goto end_rmdir;

	inode = d_inode(dentry);

	retval = -EFSCORRUPTED;
	if (le32_to_cpu(de->inode) != inode->i_ino)
		goto end_rmdir;

	retval = -ENOTEMPTY;
	if (!ext5_empty_dir(inode))
		goto end_rmdir;

	handle = ext5_journal_start(dir, EXT5_HT_DIR,
				    EXT5_DATA_TRANS_BLOCKS(dir->i_sb));
	if (IS_ERR(handle)) {
		retval = PTR_ERR(handle);
		handle = NULL;
		goto end_rmdir;
	}

	if (IS_DIRSYNC(dir))
		ext5_handle_sync(handle);

	retval = ext5_delete_entry(handle, dir, de, bh);
	if (retval)
		goto end_rmdir;
	if (!EXT5_DIR_LINK_EMPTY(inode))
		ext5_warning_inode(inode,
			     "empty directory '%.*s' has too many links (%u)",
			     dentry->d_name.len, dentry->d_name.name,
			     inode->i_nlink);
	inode_inc_iversion(inode);
	clear_nlink(inode);
	/* There's no need to set i_disksize: the fact that i_nlink is
	 * zero will ensure that the right thing happens during any
	 * recovery. */
	inode->i_size = 0;
	ext5_orphan_add(handle, inode);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	inode_set_ctime_current(inode);
	retval = ext5_mark_inode_dirty(handle, inode);
	if (retval)
		goto end_rmdir;
	ext5_dec_count(dir);
	ext5_update_dx_flag(dir);
	ext5_fc_track_unlink(handle, dentry);
	retval = ext5_mark_inode_dirty(handle, dir);

	/* VFS negative dentries are incompatible with Encoding and
	 * Case-insensitiveness. Eventually we'll want avoid
	 * invalidating the dentries here, alongside with returning the
	 * negative dentries at ext5_lookup(), when it is better
	 * supported by the VFS for the CI case.
	 */
	if (IS_ENABLED(CONFIG_UNICODE) && IS_CASEFOLDED(dir))
		d_invalidate(dentry);

end_rmdir:
	brelse(bh);
	if (handle)
		ext5_journal_stop(handle);
	if (!retval)
		ext5_subtree_count_dec(dentry, true);
	return retval;
}

int __ext5_unlink(struct inode *dir, const struct qstr *d_name,
		  struct inode *inode,
		  struct dentry *dentry /* NULL during fast_commit recovery */)
{
	int retval = -ENOENT;
	struct buffer_head *bh;
	struct ext5_dir_entry_2 *de;
	handle_t *handle;
	int skip_remove_dentry = 0;

	if (ext5_dir_is_stable(dir)) {
		handle = ext5_journal_start(dir, EXT5_HT_DIR,
					    EXT5_DATA_TRANS_BLOCKS(dir->i_sb));
		if (IS_ERR(handle))
			return PTR_ERR(handle);
		retval = ext5_stable_delete_name(handle, dir, d_name);
		if (retval) {
			ext5_journal_stop(handle);
			return retval;
		}
		if (IS_DIRSYNC(dir))
			ext5_handle_sync(handle);
		inode_inc_iversion(dir);
		inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
		retval = ext5_mark_inode_dirty(handle, dir);
		if (retval) {
			ext5_journal_stop(handle);
			return retval;
		}
		if (inode->i_nlink == 0)
			ext5_warning_inode(inode,
				"Deleting file '%.*s' with no links",
				d_name->len, d_name->name);
		else
			drop_nlink(inode);
		if (!inode->i_nlink)
			ext5_orphan_add(handle, inode);
		inode_set_ctime_current(inode);
		retval = ext5_mark_inode_dirty(handle, inode);
		ext5_journal_stop(handle);
		return retval;
	}

	/*
	 * Keep this outside the transaction; it may have to set up the
	 * directory's encryption key, which isn't GFP_NOFS-safe.
	 */
	bh = ext5_find_entry(dir, d_name, &de, NULL);
	if (IS_ERR(bh))
		return PTR_ERR(bh);

	if (!bh)
		return -ENOENT;

	if (le32_to_cpu(de->inode) != inode->i_ino) {
		/*
		 * It's okay if we find dont find dentry which matches
		 * the inode. That's because it might have gotten
		 * renamed to a different inode number
		 */
		if (EXT5_SB(inode->i_sb)->s_mount_state & EXT5_FC_REPLAY)
			skip_remove_dentry = 1;
		else
			goto out_bh;
	}

	handle = ext5_journal_start(dir, EXT5_HT_DIR,
				    EXT5_DATA_TRANS_BLOCKS(dir->i_sb));
	if (IS_ERR(handle)) {
		retval = PTR_ERR(handle);
		goto out_bh;
	}

	if (IS_DIRSYNC(dir))
		ext5_handle_sync(handle);

	if (!skip_remove_dentry) {
		retval = ext5_delete_entry(handle, dir, de, bh);
		if (retval)
			goto out_handle;
		inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
		ext5_update_dx_flag(dir);
		retval = ext5_mark_inode_dirty(handle, dir);
		if (retval)
			goto out_handle;
	} else {
		retval = 0;
	}
	if (inode->i_nlink == 0)
		ext5_warning_inode(inode, "Deleting file '%.*s' with no links",
				   d_name->len, d_name->name);
	else
		drop_nlink(inode);
	if (!inode->i_nlink)
		ext5_orphan_add(handle, inode);
	inode_set_ctime_current(inode);
	retval = ext5_mark_inode_dirty(handle, inode);
	if (dentry && !retval)
		ext5_fc_track_unlink(handle, dentry);
out_handle:
	ext5_journal_stop(handle);
out_bh:
	brelse(bh);
	return retval;
}

static int ext5_unlink(struct inode *dir, struct dentry *dentry)
{
	int retval;

	retval = ext5_emergency_state(dir->i_sb);
	if (unlikely(retval))
		return retval;

	trace_ext5_unlink_enter(dir, dentry);
	/*
	 * Initialize quotas before so that eventual writes go
	 * in separate transaction
	 */
	retval = dquot_initialize(dir);
	if (retval)
		goto out_trace;
	retval = dquot_initialize(d_inode(dentry));
	if (retval)
		goto out_trace;

	retval = __ext5_unlink(dir, &dentry->d_name, d_inode(dentry), dentry);

	if (!retval)
		ext5_subtree_count_dec(dentry, false);

	/* VFS negative dentries are incompatible with Encoding and
	 * Case-insensitiveness. Eventually we'll want avoid
	 * invalidating the dentries here, alongside with returning the
	 * negative dentries at ext5_lookup(), when it is  better
	 * supported by the VFS for the CI case.
	 */
	if (IS_ENABLED(CONFIG_UNICODE) && IS_CASEFOLDED(dir))
		d_invalidate(dentry);

out_trace:
	trace_ext5_unlink_exit(dentry, retval);
	return retval;
}

static int ext5_init_symlink_block(handle_t *handle, struct inode *inode,
				   struct fscrypt_str *disk_link)
{
	struct buffer_head *bh;
	char *kaddr;
	int err = 0;

	bh = ext5_bread(handle, inode, 0, EXT5_GET_BLOCKS_CREATE);
	if (IS_ERR(bh))
		return PTR_ERR(bh);

	BUFFER_TRACE(bh, "get_write_access");
	err = ext5_journal_get_write_access(handle, inode->i_sb, bh, EXT5_JTR_NONE);
	if (err)
		goto out;

	kaddr = (char *)bh->b_data;
	memcpy(kaddr, disk_link->name, disk_link->len);
	inode->i_size = disk_link->len - 1;
	EXT5_I(inode)->i_disksize = inode->i_size;
	err = ext5_handle_dirty_metadata(handle, inode, bh);
out:
	brelse(bh);
	return err;
}

static int ext5_symlink(struct mnt_idmap *idmap, struct inode *dir,
			struct dentry *dentry, const char *symname)
{
	handle_t *handle;
	struct inode *inode;
	int err, len = strlen(symname);
	int credits;
	struct fscrypt_str disk_link;
	int retries = 0;

	err = ext5_emergency_state(dir->i_sb);
	if (unlikely(err))
		return err;

	err = fscrypt_prepare_symlink(dir, symname, len, dir->i_sb->s_blocksize,
				      &disk_link);
	if (err)
		return err;

	err = dquot_initialize(dir);
	if (err)
		return err;

	/*
	 * EXT5_INDEX_EXTRA_TRANS_BLOCKS for addition of entry into the
	 * directory. +3 for inode, inode bitmap, group descriptor allocation.
	 * EXT5_DATA_TRANS_BLOCKS for the data block allocation and
	 * modification.
	 */
	credits = EXT5_DATA_TRANS_BLOCKS(dir->i_sb) +
		  EXT5_INDEX_EXTRA_TRANS_BLOCKS + 3;
retry:
	inode = ext5_new_inode_start_handle(idmap, dir, S_IFLNK|S_IRWXUGO,
					    &dentry->d_name, 0, NULL,
					    EXT5_HT_DIR, credits);
	handle = ext5_journal_current_handle();
	if (IS_ERR(inode)) {
		if (handle)
			ext5_journal_stop(handle);
		err = PTR_ERR(inode);
		goto out_retry;
	}

	if (IS_ENCRYPTED(inode)) {
		err = fscrypt_encrypt_symlink(inode, symname, len, &disk_link);
		if (err)
			goto err_drop_inode;
		inode->i_op = &ext5_encrypted_symlink_inode_operations;
	} else {
		if ((disk_link.len > EXT5_N_BLOCKS * 4)) {
			inode->i_op = &ext5_symlink_inode_operations;
		} else {
			inode->i_op = &ext5_fast_symlink_inode_operations;
		}
	}

	if ((disk_link.len > EXT5_N_BLOCKS * 4)) {
		/* alloc symlink block and fill it */
		err = ext5_init_symlink_block(handle, inode, &disk_link);
		if (err)
			goto err_drop_inode;
	} else {
		/* clear the extent format for fast symlink */
		ext5_clear_inode_flag(inode, EXT5_INODE_EXTENTS);
		memcpy((char *)&EXT5_I(inode)->i_data, disk_link.name,
		       disk_link.len);
		inode->i_size = disk_link.len - 1;
		EXT5_I(inode)->i_disksize = inode->i_size;
		if (!IS_ENCRYPTED(inode))
			inode_set_cached_link(inode, (char *)&EXT5_I(inode)->i_data,
					      inode->i_size);
	}
	err = ext5_add_nondir(handle, dentry, &inode);
	if (handle)
		ext5_journal_stop(handle);
	iput(inode);
	goto out_retry;

err_drop_inode:
	clear_nlink(inode);
	ext5_mark_inode_dirty(handle, inode);
	ext5_orphan_add(handle, inode);
	unlock_new_inode(inode);
	if (handle)
		ext5_journal_stop(handle);
	iput(inode);
out_retry:
	if (err == -ENOSPC && ext5_should_retry_alloc(dir->i_sb, &retries))
		goto retry;
	if (!err) {
		ext5_subtree_count_inc(dentry, false);
	}
	if (disk_link.name != (unsigned char *)symname)
		kfree(disk_link.name);
	return err;
}

int __ext5_link(struct inode *dir, struct inode *inode, struct dentry *dentry)
{
	handle_t *handle;
	int err, retries = 0;
retry:
	handle = ext5_journal_start(dir, EXT5_HT_DIR,
		(EXT5_DATA_TRANS_BLOCKS(dir->i_sb) +
		 EXT5_INDEX_EXTRA_TRANS_BLOCKS) + 1);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	if (IS_DIRSYNC(dir))
		ext5_handle_sync(handle);

	inode_set_ctime_current(inode);
	ext5_inc_count(inode);
	ihold(inode);

	err = ext5_add_entry(handle, dentry, inode);
	if (!err) {
		err = ext5_mark_inode_dirty(handle, inode);
		/* this can happen only for tmpfile being
		 * linked the first time
		 */
		if (inode->i_nlink == 1)
			ext5_orphan_del(handle, inode);
		d_instantiate(dentry, inode);
		ext5_fc_track_link(handle, dentry);
	} else {
		drop_nlink(inode);
		iput(inode);
	}
	ext5_journal_stop(handle);
	if (err == -ENOSPC && ext5_should_retry_alloc(dir->i_sb, &retries))
		goto retry;
	if (!err) {
		ext5_subtree_count_inc(dentry, false);
	}
	return err;
}

static int ext5_link(struct dentry *old_dentry,
		     struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(old_dentry);
	int err;

	if (inode->i_nlink >= EXT5_LINK_MAX)
		return -EMLINK;

	err = fscrypt_prepare_link(old_dentry, dir, dentry);
	if (err)
		return err;

	if ((ext5_test_inode_flag(dir, EXT5_INODE_PROJINHERIT)) &&
	    (!projid_eq(EXT5_I(dir)->i_projid,
			EXT5_I(old_dentry->d_inode)->i_projid)))
		return -EXDEV;

	err = dquot_initialize(dir);
	if (err)
		return err;
	return __ext5_link(dir, inode, dentry);
}

/*
 * Try to find buffer head where contains the parent block.
 * It should be the inode block if it is inlined or the 1st block
 * if it is a normal dir.
 */
static struct buffer_head *ext5_get_first_dir_block(handle_t *handle,
					struct inode *inode,
					int *retval,
					struct ext5_dir_entry_2 **parent_de,
					int *inlined)
{
	struct buffer_head *bh;

	if (!ext5_has_inline_data(inode)) {
		struct ext5_dir_entry_2 *de;
		unsigned int offset;

		bh = ext5_read_dirblock(inode, 0, EITHER);
		if (IS_ERR(bh)) {
			*retval = PTR_ERR(bh);
			return NULL;
		}

		de = (struct ext5_dir_entry_2 *) bh->b_data;
		if (ext5_check_dir_entry(inode, NULL, de, bh, bh->b_data,
					 bh->b_size, 0) ||
		    le32_to_cpu(de->inode) != inode->i_ino ||
		    strcmp(".", de->name)) {
			EXT5_ERROR_INODE(inode, "directory missing '.'");
			brelse(bh);
			*retval = -EFSCORRUPTED;
			return NULL;
		}
		offset = ext5_rec_len_from_disk(de->rec_len,
						inode->i_sb->s_blocksize);
		de = ext5_next_entry(de, inode->i_sb->s_blocksize);
		if (ext5_check_dir_entry(inode, NULL, de, bh, bh->b_data,
					 bh->b_size, offset) ||
		    le32_to_cpu(de->inode) == 0 || strcmp("..", de->name)) {
			EXT5_ERROR_INODE(inode, "directory missing '..'");
			brelse(bh);
			*retval = -EFSCORRUPTED;
			return NULL;
		}
		*parent_de = de;

		return bh;
	}

	*inlined = 1;
	return ext5_get_first_inline_block(inode, parent_de, retval);
}

struct ext5_renament {
	struct inode *dir;
	struct dentry *dentry;
	struct inode *inode;
	bool is_dir;
	int dir_nlink_delta;

	/* entry for "dentry" */
	struct buffer_head *bh;
	struct ext5_dir_entry_2 *de;
	int inlined;

	/* entry for ".." in inode if it's a directory */
	struct buffer_head *dir_bh;
	struct ext5_dir_entry_2 *parent_de;
	int dir_inlined;
};

static int ext5_rename_dir_prepare(handle_t *handle, struct ext5_renament *ent, bool is_cross)
{
	int retval;

	ent->is_dir = true;
	if (!is_cross)
		return 0;
	/* A learned root has a descriptor where a conventional directory keeps
	 * dot entries. Its parent is supplied by the VFS dentry (also in readdir),
	 * so moving the root only changes the two parents' directory entries.
	 */
	if (ext5_dir_mode(ent->inode) == DIR_STABLE_ROOT)
		return 0;

	ent->dir_bh = ext5_get_first_dir_block(handle, ent->inode,
					      &retval, &ent->parent_de,
					      &ent->dir_inlined);
	if (!ent->dir_bh)
		return retval;
	if (le32_to_cpu(ent->parent_de->inode) != ent->dir->i_ino)
		return -EFSCORRUPTED;
	BUFFER_TRACE(ent->dir_bh, "get_write_access");
	return ext5_journal_get_write_access(handle, ent->dir->i_sb,
					     ent->dir_bh, EXT5_JTR_NONE);
}

static int ext5_rename_dir_finish(handle_t *handle, struct ext5_renament *ent,
				  unsigned dir_ino)
{
	int retval;

	if (!ent->dir_bh)
		return 0;

	ent->parent_de->inode = cpu_to_le32(dir_ino);
	BUFFER_TRACE(ent->dir_bh, "call ext5_handle_dirty_metadata");
	if (!ent->dir_inlined) {
		if (is_dx(ent->inode)) {
			retval = ext5_handle_dirty_dx_node(handle,
							   ent->inode,
							   ent->dir_bh);
		} else {
			retval = ext5_handle_dirty_dirblock(handle, ent->inode,
							    ent->dir_bh);
		}
	} else {
		retval = ext5_mark_inode_dirty(handle, ent->inode);
	}
	if (retval) {
		ext5_std_error(ent->dir->i_sb, retval);
		return retval;
	}
	return 0;
}

static int ext5_setent(handle_t *handle, struct ext5_renament *ent,
		       unsigned ino, unsigned file_type)
{
	int retval, retval2;

	BUFFER_TRACE(ent->bh, "get write access");
	retval = ext5_journal_get_write_access(handle, ent->dir->i_sb, ent->bh,
					       EXT5_JTR_NONE);
	if (retval)
		return retval;
	ent->de->inode = cpu_to_le32(ino);
	if (ext5_has_feature_filetype(ent->dir->i_sb))
		ent->de->file_type = file_type;
	inode_inc_iversion(ent->dir);
	inode_set_mtime_to_ts(ent->dir, inode_set_ctime_current(ent->dir));
	retval = ext5_mark_inode_dirty(handle, ent->dir);
	BUFFER_TRACE(ent->bh, "call ext5_handle_dirty_metadata");
	if (!ent->inlined) {
		retval2 = ext5_handle_dirty_dirblock(handle, ent->dir, ent->bh);
		if (unlikely(retval2)) {
			ext5_std_error(ent->dir->i_sb, retval2);
			return retval2;
		}
	}
	return retval;
}

static void ext5_resetent(handle_t *handle, struct ext5_renament *ent,
			  unsigned ino, unsigned file_type)
{
	struct ext5_renament old = *ent;
	int retval = 0;

	/*
	 * old->de could have moved from under us during make indexed dir,
	 * so the old->de may no longer valid and need to find it again
	 * before reset old inode info.
	 */
	old.bh = ext5_find_entry(old.dir, &old.dentry->d_name, &old.de,
				 &old.inlined);
	if (IS_ERR(old.bh))
		retval = PTR_ERR(old.bh);
	if (!old.bh)
		retval = -ENOENT;
	if (retval) {
		ext5_std_error(old.dir->i_sb, retval);
		return;
	}

	ext5_setent(handle, &old, ino, file_type);
	brelse(old.bh);
}

static int ext5_find_delete_entry(handle_t *handle, struct inode *dir,
				  const struct qstr *d_name)
{
	int retval = -ENOENT;
	struct buffer_head *bh;
	struct ext5_dir_entry_2 *de;

	bh = ext5_find_entry(dir, d_name, &de, NULL);
	if (IS_ERR(bh))
		return PTR_ERR(bh);
	if (bh) {
		retval = ext5_delete_entry(handle, dir, de, bh);
		brelse(bh);
	}
	return retval;
}

static void ext5_rename_delete(handle_t *handle, struct ext5_renament *ent,
			       int force_reread)
{
	int retval;
	/*
	 * ent->de could have moved from under us during htree split, so make
	 * sure that we are deleting the right entry.  We might also be pointing
	 * to a stale entry in the unused part of ent->bh so just checking inum
	 * and the name isn't enough.
	 */
	if (le32_to_cpu(ent->de->inode) != ent->inode->i_ino ||
	    ent->de->name_len != ent->dentry->d_name.len ||
	    strncmp(ent->de->name, ent->dentry->d_name.name,
		    ent->de->name_len) ||
	    force_reread) {
		retval = ext5_find_delete_entry(handle, ent->dir,
						&ent->dentry->d_name);
	} else {
		retval = ext5_delete_entry(handle, ent->dir, ent->de, ent->bh);
		if (retval == -ENOENT) {
			retval = ext5_find_delete_entry(handle, ent->dir,
							&ent->dentry->d_name);
		}
	}

	if (retval) {
		ext5_warning_inode(ent->dir,
				   "Deleting old file: nlink %d, error=%d",
				   ent->dir->i_nlink, retval);
	}
}

static void ext5_update_dir_count(handle_t *handle, struct ext5_renament *ent)
{
	if (ent->dir_nlink_delta) {
		if (ent->dir_nlink_delta == -1)
			ext5_dec_count(ent->dir);
		else
			ext5_inc_count(ent->dir);
		ext5_mark_inode_dirty(handle, ent->dir);
	}
}

static struct inode *ext5_whiteout_for_rename(struct mnt_idmap *idmap,
					      struct ext5_renament *ent,
					      int credits, handle_t **h)
{
	struct inode *wh;
	handle_t *handle;
	int retries = 0;

	/*
	 * for inode block, sb block, group summaries,
	 * and inode bitmap
	 */
	credits += (EXT5_MAXQUOTAS_TRANS_BLOCKS(ent->dir->i_sb) +
		    EXT5_XATTR_TRANS_BLOCKS + 4);
retry:
	wh = ext5_new_inode_start_handle(idmap, ent->dir,
					 S_IFCHR | WHITEOUT_MODE,
					 &ent->dentry->d_name, 0, NULL,
					 EXT5_HT_DIR, credits);

	handle = ext5_journal_current_handle();
	if (IS_ERR(wh)) {
		if (handle)
			ext5_journal_stop(handle);
		if (PTR_ERR(wh) == -ENOSPC &&
		    ext5_should_retry_alloc(ent->dir->i_sb, &retries))
			goto retry;
	} else {
		*h = handle;
		init_special_inode(wh, wh->i_mode, WHITEOUT_DEV);
		wh->i_op = &ext5_special_inode_operations;
	}
	return wh;
}

/*
 * Anybody can rename anything with this: the permission checks are left to the
 * higher-level routines.
 *
 * n.b.  old_{dentry,inode) refers to the source dentry/inode
 * while new_{dentry,inode) refers to the destination dentry/inode
 * This comes from rename(const char *oldpath, const char *newpath)
 */
static int ext5_rename(struct mnt_idmap *idmap, struct inode *old_dir,
		       struct dentry *old_dentry, struct inode *new_dir,
		       struct dentry *new_dentry, unsigned int flags)
{
	handle_t *handle = NULL;
	struct ext5_renament old = {
		.dir = old_dir,
		.dentry = old_dentry,
		.inode = d_inode(old_dentry),
	};
	struct ext5_renament new = {
		.dir = new_dir,
		.dentry = new_dentry,
		.inode = d_inode(new_dentry),
	};
	int force_reread;
	int retval;
	struct inode *whiteout = NULL;
	int credits;
	u8 old_file_type;
	bool async_rehome = false;
	u32 rehome_plid = 0;

	if (S_ISDIR(old.inode->i_mode) && EXT5_I(old.inode)->i_li_state &&
	    atomic_read(&EXT5_I(old.inode)->i_li_rehome_pending))
		return -EAGAIN;
	if (new.inode && S_ISDIR(new.inode->i_mode) &&
	    EXT5_I(new.inode)->i_li_state &&
	    atomic_read(&EXT5_I(new.inode)->i_li_rehome_pending))
		return -EAGAIN;

	if (new.inode && new.inode->i_nlink == 0) {
		EXT5_ERROR_INODE(new.inode,
				 "target of rename is already freed");
		return -EFSCORRUPTED;
	}

	if ((ext5_test_inode_flag(new_dir, EXT5_INODE_PROJINHERIT)) &&
	    (!projid_eq(EXT5_I(new_dir)->i_projid,
			EXT5_I(old_dentry->d_inode)->i_projid)))
		return -EXDEV;

	/*
	 * Stable parents have no dirent pages.  Same subtree: INSERT and DELETE
	 * deltas.  A file across roots: the same, per root.  A directory across
	 * roots: update both deltas now and convert the moved branch in a worker.
	 * Any other stable/mutable boundary: demote the roots and retry.
	 */
	if (ext5_dir_is_stable(old_dir) || ext5_dir_is_stable(new_dir)) {
		struct inode *old_root = NULL, *new_root = NULL;
		struct inode *rehome_root = NULL;
		bool same_subtree = false;
		bool delta_rename;
		bool boundary_dir = false;
		bool rehome_reserved = false;

		if (ext5_dir_is_stable(old_dir)) {
			old_root = ext5_stable_get_subtree_root(old_dir);
			if (IS_ERR(old_root))
				return PTR_ERR(old_root);
		}
		if (ext5_dir_is_stable(new_dir)) {
			new_root = ext5_stable_get_subtree_root(new_dir);
			if (IS_ERR(new_root)) {
				if (old_root)
					iput(old_root);
				return PTR_ERR(new_root);
			}
		}
		same_subtree = (old_root && new_root && old_root == new_root);
		delta_rename = same_subtree;

		if (same_subtree && S_ISDIR(old.inode->i_mode) &&
		    old.dir != new.dir && !ext5_dir_is_stable(old.inode)) {
			delta_rename = true;
			boundary_dir = true;
		}
		if (!same_subtree && old_root && new_root &&
		    S_ISDIR(old.inode->i_mode) &&
		    !ext5_dir_is_stable(old.inode)) {
			delta_rename = true;
			boundary_dir = old.dir != new.dir;
		}
		if (!same_subtree && old_root && new_root &&
		    S_ISDIR(old.inode->i_mode) &&
		    ext5_dir_mode(old.inode) == DIR_STABLE_INTERIOR) {
			delta_rename = true;
			async_rehome = true;
			rehome_plid = ext5_stable_dir_plid(old.inode);
		}

		if (!S_ISDIR(old.inode->i_mode) && old_root && new_root)
			delta_rename = true;

		if (!delta_rename) {
			int demote_err = 0;
			bool branch_move = S_ISDIR(old.inode->i_mode) &&
				old_root && new_root;

			if (branch_move &&
			    ext5_dir_mode(old.inode) == DIR_STABLE_INTERIOR)
				demote_err = ext5_schedule_demote_plid_rename(old_root,
					ext5_stable_dir_plid(old.inode),
					old.inode->i_ino, old.dir->i_ino,
					inode_query_iversion(old.dir));
			else if (old_root)
				demote_err = ext5_schedule_demote(old_root);
			if (!demote_err && branch_move && new.inode &&
			    ext5_dir_mode(new.inode) == DIR_STABLE_INTERIOR)
				demote_err = ext5_schedule_demote_plid_rename(new_root,
					ext5_stable_dir_plid(new.inode),
					new.inode->i_ino, new.dir->i_ino,
					inode_query_iversion(new.dir));
			else if (!demote_err && !branch_move && new_root &&
				 new_root != old_root)
				demote_err = ext5_schedule_demote(new_root);
			if (old_root)
				iput(old_root);
			if (new_root)
				iput(new_root);
			return demote_err ? demote_err : -EAGAIN;
		}
		/* VFS holds both parent locks, so lookups never see the midpoint. */
		if (flags & (RENAME_EXCHANGE | RENAME_WHITEOUT)) {
			retval = -EINVAL;
			goto out_stable_roots;
		}
		if (new.inode && S_ISDIR(old.inode->i_mode) &&
		    !ext5_empty_dir(new.inode)) {
			retval = -ENOTEMPTY;
			goto out_stable_roots;
		}

		retval = dquot_initialize(old.dir);
		if (retval)
			goto out_stable_roots;
		retval = dquot_initialize(new.dir);
		if (retval)
			goto out_stable_roots;
		handle = ext5_journal_start(old.dir, EXT5_HT_DIR,
			2 * EXT5_DATA_TRANS_BLOCKS(old.dir->i_sb) +
			2 * EXT5_INDEX_EXTRA_TRANS_BLOCKS + 16);
		if (IS_ERR(handle)) {
			retval = PTR_ERR(handle);
			handle = NULL;
			goto out_stable_roots;
		}
		if (async_rehome) {
			/* Reserve the source transition first: it blocks compaction and holds
			 * the root's single work item.  The worker starts after commit. */
			retval = ext5_reserve_demote_plid_rename(old_root,
				rehome_plid, old.inode, new.dir->i_ino);
			if (retval) {
				(void)ext5_journal_stop(handle);
				handle = NULL;
				goto out_stable_roots;
			}
			/* Recheck after reserving: a prior branch worker may have published. */
			if (ext5_dir_mode(old.inode) != DIR_STABLE_INTERIOR ||
			    ext5_stable_dir_plid(old.inode) != rehome_plid ||
			    EXT5_I(old.inode)->i_subtree_root_ino != old_root->i_ino) {
				ext5_cancel_reserved_demote_plid_rename(old_root);
				retval = -EAGAIN;
				(void)ext5_journal_stop(handle);
				handle = NULL;
				goto out_stable_roots;
			}
			atomic_set(&EXT5_I(old.inode)->i_li_rehome_pending, 1);
			rehome_reserved = true;
			rehome_root = old_root;
			old_root = NULL;
		}
		if (old_root)
			iput(old_root);
		old_root = NULL;
		if (new_root)
			iput(new_root);
		new_root = NULL;
		if (boundary_dir) {
			retval = ext5_rename_dir_prepare(handle, &old, true);
			if (retval)
				goto out_stable_rename;
		}

		retval = ext5_stable_rename_links(handle, old.dir,
			&old_dentry->d_name, new.dir, &new_dentry->d_name, old.inode);
		if (retval)
			goto out_stable_rename;
		if (boundary_dir) {
			retval = ext5_rename_dir_finish(handle, &old,
						new.dir->i_ino);
			if (retval)
				goto out_stable_rename;
			ext5_fc_mark_ineligible(old.inode->i_sb,
				EXT5_FC_REASON_RENAME_DIR, handle);
		}
		if (new.inode && new.inode != old.inode) {
			if (S_ISDIR(new.inode->i_mode))
				clear_nlink(new.inode);
			else if (new.inode->i_nlink)
				drop_nlink(new.inode);
			inode_set_ctime_current(new.inode);
			if (!new.inode->i_nlink)
				ext5_orphan_add(handle, new.inode);
			retval = ext5_mark_inode_dirty(handle, new.inode);
			if (retval)
				goto out_stable_rename;
		}
		if (S_ISDIR(old.inode->i_mode)) {
			if (old.dir != new.dir) {
				ext5_dec_count(old.dir);
				/* Replacing a directory consumes its link: no net change. */
				if (!new.inode)
					ext5_inc_count(new.dir);
			} else if (new.inode) {
				/* Two same-parent subdirectories become one. */
				ext5_dec_count(old.dir);
			}
		}
		inode_inc_iversion(old.dir);
		if (old.dir != new.dir)
			inode_inc_iversion(new.dir);
		inode_set_mtime_to_ts(old.dir, inode_set_ctime_current(old.dir));
		if (old.dir != new.dir)
			inode_set_mtime_to_ts(new.dir,
				inode_set_ctime_current(new.dir));
		retval = ext5_mark_inode_dirty(handle, old.dir);
		if (retval)
			goto out_stable_rename;
		if (old.dir != new.dir) {
			retval = ext5_mark_inode_dirty(handle, new.dir);
			if (retval)
				goto out_stable_rename;
		}
		inode_set_ctime_current(old.inode);
		retval = ext5_mark_inode_dirty(handle, old.inode);
		if (retval)
			goto out_stable_rename;
		retval = 0;
out_stable_rename:
		{
			int stop_err = ext5_journal_stop(handle);

			if (!retval)
				retval = stop_err;
		}
		if (!retval)
			ext5_subtree_rename_update(old.dentry, new.dentry,
					   old.inode, new.inode, false);
		if (rehome_reserved) {
			if (!retval) {
				ext5_start_reserved_demote_plid_rename(
					rehome_root,
					inode_query_iversion(new.dir));
			} else {
				ext5_cancel_reserved_demote_plid_rename(rehome_root);
			}
			rehome_reserved = false;
			iput(rehome_root);
			rehome_root = NULL;
		}
		brelse(old.dir_bh);
		return retval;

out_stable_roots:
		if (rehome_reserved) {
			ext5_cancel_reserved_demote_plid_rename(rehome_root);
		}
		if (rehome_root)
			iput(rehome_root);
		if (old_root)
			iput(old_root);
		if (new_root)
			iput(new_root);
		return retval;
	}

	retval = dquot_initialize(old.dir);
	if (retval)
		return retval;
	retval = dquot_initialize(old.inode);
	if (retval)
		return retval;
	retval = dquot_initialize(new.dir);
	if (retval)
		return retval;

	/* Initialize quotas before so that eventual writes go
	 * in separate transaction */
	if (new.inode) {
		retval = dquot_initialize(new.inode);
		if (retval)
			return retval;
	}

	old.bh = ext5_find_entry(old.dir, &old.dentry->d_name, &old.de,
				 &old.inlined);
	if (IS_ERR(old.bh))
		return PTR_ERR(old.bh);

	/*
	 *  Check for inode number is _not_ due to possible IO errors.
	 *  We might rmdir the source, keep it as pwd of some process
	 *  and merrily kill the link to whatever was created under the
	 *  same name. Goodbye sticky bit ;-<
	 */
	retval = -ENOENT;
	if (!old.bh || le32_to_cpu(old.de->inode) != old.inode->i_ino)
		goto release_bh;

	new.bh = ext5_find_entry(new.dir, &new.dentry->d_name,
				 &new.de, &new.inlined);
	if (IS_ERR(new.bh)) {
		retval = PTR_ERR(new.bh);
		new.bh = NULL;
		goto release_bh;
	}
	if (new.bh) {
		if (!new.inode) {
			brelse(new.bh);
			new.bh = NULL;
		}
	}
	if (new.inode && !test_opt(new.dir->i_sb, NO_AUTO_DA_ALLOC))
		ext5_alloc_da_blocks(old.inode);

	credits = (2 * EXT5_DATA_TRANS_BLOCKS(old.dir->i_sb) +
		   EXT5_INDEX_EXTRA_TRANS_BLOCKS + 2);
	if (!(flags & RENAME_WHITEOUT)) {
		handle = ext5_journal_start(old.dir, EXT5_HT_DIR, credits);
		if (IS_ERR(handle)) {
			retval = PTR_ERR(handle);
			goto release_bh;
		}
	} else {
		whiteout = ext5_whiteout_for_rename(idmap, &old, credits, &handle);
		if (IS_ERR(whiteout)) {
			retval = PTR_ERR(whiteout);
			goto release_bh;
		}
	}

	old_file_type = old.de->file_type;
	if (IS_DIRSYNC(old.dir) || IS_DIRSYNC(new.dir))
		ext5_handle_sync(handle);

	if (S_ISDIR(old.inode->i_mode)) {
		if (new.inode) {
			retval = -ENOTEMPTY;
			if (!ext5_empty_dir(new.inode))
				goto end_rename;
		} else {
			retval = -EMLINK;
			if (new.dir != old.dir && EXT5_DIR_LINK_MAX(new.dir))
				goto end_rename;
		}
		retval = ext5_rename_dir_prepare(handle, &old, new.dir != old.dir);
		if (retval)
			goto end_rename;
	}
	/*
	 * If we're renaming a file within an inline_data dir and adding or
	 * setting the new dirent causes a conversion from inline_data to
	 * extents/blockmap, we need to force the dirent delete code to
	 * re-read the directory, or else we end up trying to delete a dirent
	 * from what is now the extent tree root (or a block map).
	 */
	force_reread = (new.dir->i_ino == old.dir->i_ino &&
			ext5_test_inode_flag(new.dir, EXT5_INODE_INLINE_DATA));

	if (whiteout) {
		/*
		 * Do this before adding a new entry, so the old entry is sure
		 * to be still pointing to the valid old entry.
		 */
		retval = ext5_setent(handle, &old, whiteout->i_ino,
				     EXT5_FT_CHRDEV);
		if (retval)
			goto end_rename;
		retval = ext5_mark_inode_dirty(handle, whiteout);
		if (unlikely(retval))
			goto end_rename;

	}
	if (!new.bh) {
		retval = ext5_add_entry(handle, new.dentry, old.inode);
		if (retval)
			goto end_rename;
	} else {
		retval = ext5_setent(handle, &new,
				     old.inode->i_ino, old_file_type);
		if (retval)
			goto end_rename;
	}
	if (force_reread)
		force_reread = !ext5_test_inode_flag(new.dir,
						     EXT5_INODE_INLINE_DATA);

	/*
	 * Like most other Unix systems, set the ctime for inodes on a
	 * rename.
	 */
	inode_set_ctime_current(old.inode);
	retval = ext5_mark_inode_dirty(handle, old.inode);
	if (unlikely(retval))
		goto end_rename;

	if (!whiteout) {
		/*
		 * ok, that's it
		 */
		ext5_rename_delete(handle, &old, force_reread);
	}

	if (new.inode) {
		ext5_dec_count(new.inode);
		inode_set_ctime_current(new.inode);
	}
	inode_set_mtime_to_ts(old.dir, inode_set_ctime_current(old.dir));
	ext5_update_dx_flag(old.dir);
	if (old.is_dir) {
		retval = ext5_rename_dir_finish(handle, &old, new.dir->i_ino);
		if (retval)
			goto end_rename;

		ext5_dec_count(old.dir);
		if (new.inode) {
			/* checked ext5_empty_dir above, can't have another
			 * parent, ext5_dec_count() won't work for many-linked
			 * dirs */
			clear_nlink(new.inode);
		} else {
			ext5_inc_count(new.dir);
			ext5_update_dx_flag(new.dir);
			retval = ext5_mark_inode_dirty(handle, new.dir);
			if (unlikely(retval))
				goto end_rename;
		}
	}
	retval = ext5_mark_inode_dirty(handle, old.dir);
	if (unlikely(retval))
		goto end_rename;

	if (old.is_dir) {
		/*
		 * We disable fast commits here that's because the
		 * replay code is not yet capable of changing dot dot
		 * dirents in directories.
		 */
		ext5_fc_mark_ineligible(old.inode->i_sb,
			EXT5_FC_REASON_RENAME_DIR, handle);
	} else {
		struct super_block *sb = old.inode->i_sb;

		if (new.inode)
			ext5_fc_track_unlink(handle, new.dentry);
		if (test_opt2(sb, JOURNAL_FAST_COMMIT) &&
		    !(EXT5_SB(sb)->s_mount_state & EXT5_FC_REPLAY) &&
		    !(ext5_test_mount_flag(sb, EXT5_MF_FC_INELIGIBLE))) {
			__ext5_fc_track_link(handle, old.inode, new.dentry);
			__ext5_fc_track_unlink(handle, old.inode, old.dentry);
			if (whiteout)
				__ext5_fc_track_create(handle, whiteout,
						       old.dentry);
		}
	}

	if (new.inode) {
		retval = ext5_mark_inode_dirty(handle, new.inode);
		if (unlikely(retval))
			goto end_rename;
		if (!new.inode->i_nlink)
			ext5_orphan_add(handle, new.inode);
	}
	retval = 0;

end_rename:
	if (whiteout) {
		if (retval) {
			ext5_resetent(handle, &old,
				      old.inode->i_ino, old_file_type);
			drop_nlink(whiteout);
			ext5_mark_inode_dirty(handle, whiteout);
			ext5_orphan_add(handle, whiteout);
		}
		unlock_new_inode(whiteout);
		ext5_journal_stop(handle);
		iput(whiteout);
	} else {
		ext5_journal_stop(handle);
	}
release_bh:
	brelse(old.dir_bh);
	brelse(old.bh);
	brelse(new.bh);

	if (!retval)
		ext5_subtree_rename_update(old.dentry, new.dentry, old.inode,
					   new.inode, false);
	return retval;
}

static int ext5_cross_rename(struct inode *old_dir, struct dentry *old_dentry,
			     struct inode *new_dir, struct dentry *new_dentry)
{
	handle_t *handle = NULL;
	struct ext5_renament old = {
		.dir = old_dir,
		.dentry = old_dentry,
		.inode = d_inode(old_dentry),
	};
	struct ext5_renament new = {
		.dir = new_dir,
		.dentry = new_dentry,
		.inode = d_inode(new_dentry),
	};
	u8 new_file_type;
	int retval;

	if ((ext5_test_inode_flag(new_dir, EXT5_INODE_PROJINHERIT) &&
	     !projid_eq(EXT5_I(new_dir)->i_projid,
			EXT5_I(old_dentry->d_inode)->i_projid)) ||
	    (ext5_test_inode_flag(old_dir, EXT5_INODE_PROJINHERIT) &&
	     !projid_eq(EXT5_I(old_dir)->i_projid,
			EXT5_I(new_dentry->d_inode)->i_projid)))
		return -EXDEV;

	retval = dquot_initialize(old.dir);
	if (retval)
		return retval;
	retval = dquot_initialize(new.dir);
	if (retval)
		return retval;

	old.bh = ext5_find_entry(old.dir, &old.dentry->d_name,
				 &old.de, &old.inlined);
	if (IS_ERR(old.bh))
		return PTR_ERR(old.bh);
	/*
	 *  Check for inode number is _not_ due to possible IO errors.
	 *  We might rmdir the source, keep it as pwd of some process
	 *  and merrily kill the link to whatever was created under the
	 *  same name. Goodbye sticky bit ;-<
	 */
	retval = -ENOENT;
	if (!old.bh || le32_to_cpu(old.de->inode) != old.inode->i_ino)
		goto end_rename;

	new.bh = ext5_find_entry(new.dir, &new.dentry->d_name,
				 &new.de, &new.inlined);
	if (IS_ERR(new.bh)) {
		retval = PTR_ERR(new.bh);
		new.bh = NULL;
		goto end_rename;
	}

	/* RENAME_EXCHANGE case: old *and* new must both exist */
	if (!new.bh || le32_to_cpu(new.de->inode) != new.inode->i_ino)
		goto end_rename;

	handle = ext5_journal_start(old.dir, EXT5_HT_DIR,
		(2 * EXT5_DATA_TRANS_BLOCKS(old.dir->i_sb) +
		 2 * EXT5_INDEX_EXTRA_TRANS_BLOCKS + 2));
	if (IS_ERR(handle)) {
		retval = PTR_ERR(handle);
		handle = NULL;
		goto end_rename;
	}

	if (IS_DIRSYNC(old.dir) || IS_DIRSYNC(new.dir))
		ext5_handle_sync(handle);

	if (S_ISDIR(old.inode->i_mode)) {
		retval = ext5_rename_dir_prepare(handle, &old, new.dir != old.dir);
		if (retval)
			goto end_rename;
	}
	if (S_ISDIR(new.inode->i_mode)) {
		retval = ext5_rename_dir_prepare(handle, &new, new.dir != old.dir);
		if (retval)
			goto end_rename;
	}

	/*
	 * Other than the special case of overwriting a directory, parents'
	 * nlink only needs to be modified if this is a cross directory rename.
	 */
	if (old.dir != new.dir && old.is_dir != new.is_dir) {
		old.dir_nlink_delta = old.is_dir ? -1 : 1;
		new.dir_nlink_delta = -old.dir_nlink_delta;
		retval = -EMLINK;
		if ((old.dir_nlink_delta > 0 && EXT5_DIR_LINK_MAX(old.dir)) ||
		    (new.dir_nlink_delta > 0 && EXT5_DIR_LINK_MAX(new.dir)))
			goto end_rename;
	}

	new_file_type = new.de->file_type;
	retval = ext5_setent(handle, &new, old.inode->i_ino, old.de->file_type);
	if (retval)
		goto end_rename;

	retval = ext5_setent(handle, &old, new.inode->i_ino, new_file_type);
	if (retval)
		goto end_rename;

	/*
	 * Like most other Unix systems, set the ctime for inodes on a
	 * rename.
	 */
	inode_set_ctime_current(old.inode);
	inode_set_ctime_current(new.inode);
	retval = ext5_mark_inode_dirty(handle, old.inode);
	if (unlikely(retval))
		goto end_rename;
	retval = ext5_mark_inode_dirty(handle, new.inode);
	if (unlikely(retval))
		goto end_rename;
	ext5_fc_mark_ineligible(new.inode->i_sb,
				EXT5_FC_REASON_CROSS_RENAME, handle);
	if (old.dir_bh) {
		retval = ext5_rename_dir_finish(handle, &old, new.dir->i_ino);
		if (retval)
			goto end_rename;
	}
	if (new.dir_bh) {
		retval = ext5_rename_dir_finish(handle, &new, old.dir->i_ino);
		if (retval)
			goto end_rename;
	}
	ext5_update_dir_count(handle, &old);
	ext5_update_dir_count(handle, &new);
	retval = 0;

end_rename:
	brelse(old.dir_bh);
	brelse(new.dir_bh);
	brelse(old.bh);
	brelse(new.bh);
	if (handle)
		ext5_journal_stop(handle);
	if (!retval)
		ext5_subtree_rename_update(old.dentry, new.dentry, old.inode,
					   new.inode, true);
	return retval;
}

static int ext5_rename2(struct mnt_idmap *idmap,
			struct inode *old_dir, struct dentry *old_dentry,
			struct inode *new_dir, struct dentry *new_dentry,
			unsigned int flags)
{
	int err;

	err = ext5_emergency_state(old_dir->i_sb);
	if (unlikely(err))
		return err;

	if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE | RENAME_WHITEOUT))
		return -EINVAL;

	err = fscrypt_prepare_rename(old_dir, old_dentry, new_dir, new_dentry,
				     flags);
	if (err)
		return err;

	if (flags & RENAME_EXCHANGE) {
		/* The stable path supports plain rename only, not exchange. */
		if (ext5_dir_is_stable(old_dir) || ext5_dir_is_stable(new_dir))
			return -EOPNOTSUPP;
		return ext5_cross_rename(old_dir, old_dentry,
					 new_dir, new_dentry);
	}

	return ext5_rename(idmap, old_dir, old_dentry, new_dir, new_dentry, flags);
}

/*
 * directories can handle most operations...
 */
const struct inode_operations ext5_dir_inode_operations = {
	.create		= ext5_create,
	.lookup		= ext5_lookup,
	.link		= ext5_link,
	.unlink		= ext5_unlink,
	.symlink	= ext5_symlink,
	.mkdir		= ext5_mkdir,
	.rmdir		= ext5_rmdir,
	.mknod		= ext5_mknod,
	.tmpfile	= ext5_tmpfile,
	.rename		= ext5_rename2,
	.setattr	= ext5_setattr,
	.getattr	= ext5_getattr,
	.listxattr	= ext5_listxattr,
	.get_inode_acl	= ext5_get_acl,
	.set_acl	= ext5_set_acl,
	.fiemap         = ext5_fiemap,
	.fileattr_get	= ext5_fileattr_get,
	.fileattr_set	= ext5_fileattr_set,
};

const struct inode_operations ext5_special_inode_operations = {
	.setattr	= ext5_setattr,
	.getattr	= ext5_getattr,
	.listxattr	= ext5_listxattr,
	.get_inode_acl	= ext5_get_acl,
	.set_acl	= ext5_set_acl,
};
