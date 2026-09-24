// SPDX-License-Identifier: GPL-2.0+
/*
 * ext5_jbd3.h
 *
 * Written by Stephen C. Tweedie <sct@redhat.com>, 1999
 *
 * Copyright 1998--1999 Red Hat corp --- All Rights Reserved
 *
 * Ext4-specific journaling extensions.
 */

#ifndef _EXT5_JBD3_H
#define _EXT5_JBD3_H

#include <linux/fs.h>
#include <linux/jbd3.h>
#include "ext5.h"

#define EXT5_JOURNAL(inode)	(EXT5_SB((inode)->i_sb)->s_journal)

/* Define the number of blocks we need to account to a transaction to
 * modify one block of data.
 *
 * We may have to touch one inode, one bitmap buffer, up to three
 * indirection blocks, the group and superblock summaries, and the data
 * block to complete the transaction.
 *
 * For extents-enabled fs we may have to allocate and modify up to
 * 5 levels of tree, data block (for each of these we need bitmap + group
 * summaries), root which is stored in the inode, sb
 */

#define EXT5_SINGLEDATA_TRANS_BLOCKS(sb)				\
	(ext5_has_feature_extents(sb) ? 20U : 8U)

/* Extended attribute operations touch at most two data buffers,
 * two bitmap buffers, and two group summaries, in addition to the inode
 * and the superblock, which are already accounted for. */

#define EXT5_XATTR_TRANS_BLOCKS		6U

/* Define the minimum size for a transaction which modifies data.  This
 * needs to take into account the fact that we may end up modifying two
 * quota files too (one for the group, one for the user quota).  The
 * superblock only gets updated once, of course, so don't bother
 * counting that again for the quota updates. */

#define EXT5_DATA_TRANS_BLOCKS(sb)	(EXT5_SINGLEDATA_TRANS_BLOCKS(sb) + \
					 EXT5_XATTR_TRANS_BLOCKS - 2 + \
					 EXT5_MAXQUOTAS_TRANS_BLOCKS(sb))

/*
 * Define the number of metadata blocks we need to account to modify data.
 *
 * This include super block, inode block, quota blocks and xattr blocks
 */
#define EXT5_META_TRANS_BLOCKS(sb)	(EXT5_XATTR_TRANS_BLOCKS + \
					EXT5_MAXQUOTAS_TRANS_BLOCKS(sb))

/* Define an arbitrary limit for the amount of data we will anticipate
 * writing to any given transaction.  For unbounded transactions such as
 * write(2) and truncate(2) we can write more than this, but we always
 * start off at the maximum transaction size and grow the transaction
 * optimistically as we go. */

#define EXT5_MAX_TRANS_DATA		64U

/* We break up a large truncate or write transaction once the handle's
 * buffer credits gets this low, we need either to extend the
 * transaction or to start a new one.  Reserve enough space here for
 * inode, bitmap, superblock, group and indirection updates for at least
 * one block, plus two quota updates.  Quota allocations are not
 * needed. */

#define EXT5_RESERVE_TRANS_BLOCKS	12U

/*
 * Number of credits needed if we need to insert an entry into a
 * directory.  For each new index block, we need 4 blocks (old index
 * block, new index block, bitmap block, bg summary).  For normal
 * htree directories there are 2 levels; if the largedir feature
 * enabled it's 3 levels.
 */
#define EXT5_INDEX_EXTRA_TRANS_BLOCKS	12U

#ifdef CONFIG_QUOTA
/* Amount of blocks needed for quota update - we know that the structure was
 * allocated so we need to update only data block */
#define EXT5_QUOTA_TRANS_BLOCKS(sb) ((ext5_quota_capable(sb)) ? 1 : 0)
/* Amount of blocks needed for quota insert/delete - we do some block writes
 * but inode, sb and group updates are done only once */
#define EXT5_QUOTA_INIT_BLOCKS(sb) ((ext5_quota_capable(sb)) ?\
		(DQUOT_INIT_ALLOC*(EXT5_SINGLEDATA_TRANS_BLOCKS(sb)-3)\
		 +3+DQUOT_INIT_REWRITE) : 0)

#define EXT5_QUOTA_DEL_BLOCKS(sb) ((ext5_quota_capable(sb)) ?\
		(DQUOT_DEL_ALLOC*(EXT5_SINGLEDATA_TRANS_BLOCKS(sb)-3)\
		 +3+DQUOT_DEL_REWRITE) : 0)
#else
#define EXT5_QUOTA_TRANS_BLOCKS(sb) 0
#define EXT5_QUOTA_INIT_BLOCKS(sb) 0
#define EXT5_QUOTA_DEL_BLOCKS(sb) 0
#endif
#define EXT5_MAXQUOTAS_TRANS_BLOCKS(sb) (EXT5_MAXQUOTAS*EXT5_QUOTA_TRANS_BLOCKS(sb))
#define EXT5_MAXQUOTAS_INIT_BLOCKS(sb) (EXT5_MAXQUOTAS*EXT5_QUOTA_INIT_BLOCKS(sb))
#define EXT5_MAXQUOTAS_DEL_BLOCKS(sb) (EXT5_MAXQUOTAS*EXT5_QUOTA_DEL_BLOCKS(sb))

/*
 * Ext4 handle operation types -- for logging purposes
 */
#define EXT5_HT_MISC             0
#define EXT5_HT_INODE            1
#define EXT5_HT_WRITE_PAGE       2
#define EXT5_HT_MAP_BLOCKS       3
#define EXT5_HT_DIR              4
#define EXT5_HT_TRUNCATE         5
#define EXT5_HT_QUOTA            6
#define EXT5_HT_RESIZE           7
#define EXT5_HT_MIGRATE          8
#define EXT5_HT_MOVE_EXTENTS     9
#define EXT5_HT_XATTR           10
#define EXT5_HT_EXT_CONVERT     11
#define EXT5_HT_MAX             12

int
ext5_mark_iloc_dirty(handle_t *handle,
		     struct inode *inode,
		     struct ext5_iloc *iloc);

/*
 * On success, We end up with an outstanding reference count against
 * iloc->bh.  This _must_ be cleaned up later.
 */

int ext5_reserve_inode_write(handle_t *handle, struct inode *inode,
			struct ext5_iloc *iloc);

#define ext5_mark_inode_dirty(__h, __i)					\
		__ext5_mark_inode_dirty((__h), (__i), __func__, __LINE__)
int __ext5_mark_inode_dirty(handle_t *handle, struct inode *inode,
				const char *func, unsigned int line);

int ext5_expand_extra_isize(struct inode *inode,
			    unsigned int new_extra_isize,
			    struct ext5_iloc *iloc);
/*
 * Wrapper functions with which ext5 calls into JBD.
 */
int __ext5_journal_get_write_access(const char *where, unsigned int line,
				    handle_t *handle, struct super_block *sb,
				    struct buffer_head *bh,
				    enum ext5_journal_trigger_type trigger_type);

int __ext5_forget(const char *where, unsigned int line, handle_t *handle,
		  int is_metadata, struct inode *inode,
		  struct buffer_head *bh, ext5_fsblk_t blocknr);

int __ext5_journal_get_create_access(const char *where, unsigned int line,
				handle_t *handle, struct super_block *sb,
				struct buffer_head *bh,
				enum ext5_journal_trigger_type trigger_type);

int __ext5_handle_dirty_metadata(const char *where, unsigned int line,
				 handle_t *handle, struct inode *inode,
				 struct buffer_head *bh);

#define ext5_journal_get_write_access(handle, sb, bh, trigger_type) \
	__ext5_journal_get_write_access(__func__, __LINE__, (handle), (sb), \
					(bh), (trigger_type))
#define ext5_forget(handle, is_metadata, inode, bh, block_nr) \
	__ext5_forget(__func__, __LINE__, (handle), (is_metadata), (inode), \
		      (bh), (block_nr))
#define ext5_journal_get_create_access(handle, sb, bh, trigger_type) \
	__ext5_journal_get_create_access(__func__, __LINE__, (handle), (sb), \
					 (bh), (trigger_type))
#define ext5_handle_dirty_metadata(handle, inode, bh) \
	__ext5_handle_dirty_metadata(__func__, __LINE__, (handle), (inode), \
				     (bh))

handle_t *__ext5_journal_start_sb(struct inode *inode, struct super_block *sb,
				  unsigned int line, int type, int blocks,
				  int rsv_blocks, int revoke_creds);
int __ext5_journal_stop(const char *where, unsigned int line, handle_t *handle);

#define EXT5_NOJOURNAL_MAX_REF_COUNT ((unsigned long) 4096)

/* Note:  Do not use this for NULL handles.  This is only to determine if
 * a properly allocated handle is using a journal or not. */
static inline int ext5_handle_valid(handle_t *handle)
{
	if ((unsigned long)handle < EXT5_NOJOURNAL_MAX_REF_COUNT)
		return 0;
	return 1;
}

static inline void ext5_handle_sync(handle_t *handle)
{
	if (ext5_handle_valid(handle))
		handle->h_sync = 1;
}

static inline int ext5_handle_is_aborted(handle_t *handle)
{
	if (ext5_handle_valid(handle))
		return is_handle_aborted(handle);
	return 0;
}

static inline int ext5_free_metadata_revoke_credits(struct super_block *sb,
						    int blocks)
{
	/* Freeing each metadata block can result in freeing one cluster */
	return blocks * EXT5_SB(sb)->s_cluster_ratio;
}

static inline int ext5_trans_default_revoke_credits(struct super_block *sb)
{
	return ext5_free_metadata_revoke_credits(sb, 8);
}

#define ext5_journal_start_sb(sb, type, nblocks)			\
	__ext5_journal_start_sb(NULL, (sb), __LINE__, (type), (nblocks), 0,\
				ext5_trans_default_revoke_credits(sb))

#define ext5_journal_start(inode, type, nblocks)			\
	__ext5_journal_start((inode), __LINE__, (type), (nblocks), 0,	\
			     ext5_trans_default_revoke_credits((inode)->i_sb))

#define ext5_journal_start_with_reserve(inode, type, blocks, rsv_blocks)\
	__ext5_journal_start((inode), __LINE__, (type), (blocks), (rsv_blocks),\
			     ext5_trans_default_revoke_credits((inode)->i_sb))

#define ext5_journal_start_with_revoke(inode, type, blocks, revoke_creds) \
	__ext5_journal_start((inode), __LINE__, (type), (blocks), 0,	\
			     (revoke_creds))

static inline handle_t *__ext5_journal_start(struct inode *inode,
					     unsigned int line, int type,
					     int blocks, int rsv_blocks,
					     int revoke_creds)
{
	return __ext5_journal_start_sb(inode, inode->i_sb, line, type, blocks,
				       rsv_blocks, revoke_creds);
}

#define ext5_journal_stop(handle) \
	__ext5_journal_stop(__func__, __LINE__, (handle))

#define ext5_journal_start_reserved(handle, type) \
	__ext5_journal_start_reserved((handle), __LINE__, (type))

handle_t *__ext5_journal_start_reserved(handle_t *handle, unsigned int line,
					int type);

static inline handle_t *ext5_journal_current_handle(void)
{
	return journal_current_handle();
}

static inline int ext5_journal_extend(handle_t *handle, int nblocks, int revoke)
{
	if (ext5_handle_valid(handle))
		return jbd3_journal_extend(handle, nblocks, revoke);
	return 0;
}

static inline int ext5_journal_restart(handle_t *handle, int nblocks,
				       int revoke)
{
	if (ext5_handle_valid(handle))
		return jbd3__journal_restart(handle, nblocks, revoke, GFP_NOFS);
	return 0;
}

int __ext5_journal_ensure_credits(handle_t *handle, int check_cred,
				  int extend_cred, int revoke_cred);


/*
 * Ensure @handle has at least @check_creds credits available. If not,
 * transaction will be extended or restarted to contain at least @extend_cred
 * credits. Before restarting transaction @fn is executed to allow for cleanup
 * before the transaction is restarted.
 *
 * The return value is < 0 in case of error, 0 in case the handle has enough
 * credits or transaction extension succeeded, 1 in case transaction had to be
 * restarted.
 */
#define ext5_journal_ensure_credits_fn(handle, check_cred, extend_cred,	\
				       revoke_cred, fn) \
({									\
	__label__ __ensure_end;						\
	int err = __ext5_journal_ensure_credits((handle), (check_cred),	\
					(extend_cred), (revoke_cred));	\
									\
	if (err <= 0)							\
		goto __ensure_end;					\
	err = (fn);							\
	if (err < 0)							\
		goto __ensure_end;					\
	err = ext5_journal_restart((handle), (extend_cred), (revoke_cred)); \
	if (err == 0)							\
		err = 1;						\
__ensure_end:								\
	err;								\
})

/*
 * Ensure given handle has at least requested amount of credits available,
 * possibly restarting transaction if needed. We also make sure the transaction
 * has space for at least ext5_trans_default_revoke_credits(sb) revoke records
 * as freeing one or two blocks is very common pattern and requesting this is
 * very cheap.
 */
static inline int ext5_journal_ensure_credits(handle_t *handle, int credits,
					      int revoke_creds)
{
	return ext5_journal_ensure_credits_fn(handle, credits, credits,
				revoke_creds, 0);
}

static inline int ext5_journal_blocks_per_folio(struct inode *inode)
{
	if (EXT5_JOURNAL(inode) != NULL)
		return jbd3_journal_blocks_per_folio(inode);
	return 0;
}

static inline int ext5_journal_force_commit(journal_t *journal)
{
	if (journal)
		return jbd3_journal_force_commit(journal);
	return 0;
}

static inline int ext5_jbd3_inode_add_write(handle_t *handle,
		struct inode *inode, loff_t start_byte, loff_t length)
{
	if (ext5_handle_valid(handle))
		return jbd3_journal_inode_ranged_write(handle,
				EXT5_I(inode)->jinode, start_byte, length);
	return 0;
}

static inline int ext5_jbd3_inode_add_wait(handle_t *handle,
		struct inode *inode, loff_t start_byte, loff_t length)
{
	if (ext5_handle_valid(handle))
		return jbd3_journal_inode_ranged_wait(handle,
				EXT5_I(inode)->jinode, start_byte, length);
	return 0;
}

static inline void ext5_update_inode_fsync_trans(handle_t *handle,
						 struct inode *inode,
						 int datasync)
{
	struct ext5_inode_info *ei = EXT5_I(inode);

	if (ext5_handle_valid(handle) && !is_handle_aborted(handle)) {
		ei->i_sync_tid = handle->h_transaction->t_tid;
		if (datasync)
			ei->i_datasync_tid = handle->h_transaction->t_tid;
	}
}

/* super.c */
int ext5_force_commit(struct super_block *sb);

/*
 * Ext4 inode journal modes
 */
#define EXT5_INODE_JOURNAL_DATA_MODE	0x01 /* journal data mode */
#define EXT5_INODE_ORDERED_DATA_MODE	0x02 /* ordered data mode */
#define EXT5_INODE_WRITEBACK_DATA_MODE	0x04 /* writeback data mode */

int ext5_inode_journal_mode(struct inode *inode);

static inline int ext5_should_journal_data(struct inode *inode)
{
	return ext5_inode_journal_mode(inode) & EXT5_INODE_JOURNAL_DATA_MODE;
}

static inline int ext5_should_order_data(struct inode *inode)
{
	return ext5_inode_journal_mode(inode) & EXT5_INODE_ORDERED_DATA_MODE;
}

static inline int ext5_should_writeback_data(struct inode *inode)
{
	return ext5_inode_journal_mode(inode) & EXT5_INODE_WRITEBACK_DATA_MODE;
}

static inline int ext5_free_data_revoke_credits(struct inode *inode, int blocks)
{
	if (test_opt(inode->i_sb, DATA_FLAGS) == EXT5_MOUNT_JOURNAL_DATA)
		return 0;
	if (!ext5_should_journal_data(inode))
		return 0;
	/*
	 * Data blocks in one extent are contiguous, just account for partial
	 * clusters at extent boundaries
	 */
	return blocks + 2*(EXT5_SB(inode->i_sb)->s_cluster_ratio - 1);
}

/*
 * This function controls whether or not we should try to go down the
 * dioread_nolock code paths, which makes it safe to avoid taking
 * i_rwsem for direct I/O reads.  This only works for extent-based
 * files, and it doesn't work if data journaling is enabled, since the
 * dioread_nolock code uses b_private to pass information back to the
 * I/O completion handler, and this conflicts with the jbd's use of
 * b_private.
 */
static inline int ext5_should_dioread_nolock(struct inode *inode)
{
	if (!test_opt(inode->i_sb, DIOREAD_NOLOCK))
		return 0;
	if (!S_ISREG(inode->i_mode))
		return 0;
	if (!(ext5_test_inode_flag(inode, EXT5_INODE_EXTENTS)))
		return 0;
	if (ext5_should_journal_data(inode))
		return 0;
	/* temporary fix to prevent generic/422 test failures */
	if (!test_opt(inode->i_sb, DELALLOC))
		return 0;
	return 1;
}

/*
 * Pass journal explicitly as it may not be cached in the sbi->s_journal in some
 * cases
 */
static inline int ext5_journal_destroy(struct ext5_sb_info *sbi, journal_t *journal)
{
	int err = 0;

	/*
	 * At this point only two things can be operating on the journal.
	 * JBD3 thread performing transaction commit and s_sb_upd_work
	 * issuing sb update through the journal. Once we set
	 * EXT5_JOURNAL_DESTROY, new ext5_handle_error() calls will not
	 * queue s_sb_upd_work and ext5_force_commit() makes sure any
	 * ext5_handle_error() calls from the running transaction commit are
	 * finished. Hence no new s_sb_upd_work can be queued after we
	 * flush it here.
	 */
	ext5_set_mount_flag(sbi->s_sb, EXT5_MF_JOURNAL_DESTROY);

	ext5_force_commit(sbi->s_sb);
	flush_work(&sbi->s_sb_upd_work);

	err = jbd3_journal_destroy(journal);
	sbi->s_journal = NULL;

	return err;
}

#endif	/* _EXT5_JBD3_H */
