// SPDX-License-Identifier: LGPL-2.1
/*
 * Copyright (c) 2012 Taobao.
 * Written by Tao Ma <boyu.mt@taobao.com>
 */

#include <linux/iomap.h>
#include <linux/fiemap.h>
#include <linux/namei.h>
#include <linux/iversion.h>
#include <linux/sched/mm.h>

#include "ext5_jbd3.h"
#include "ext5.h"
#include "xattr.h"
#include "truncate.h"

#define EXT5_XATTR_SYSTEM_DATA	"data"
#define EXT5_MIN_INLINE_DATA_SIZE	((sizeof(__le32) * EXT5_N_BLOCKS))
#define EXT5_INLINE_DOTDOT_OFFSET	2
#define EXT5_INLINE_DOTDOT_SIZE		4


static int ext5_da_convert_inline_data_to_extent(struct address_space *mapping,
						 struct inode *inode,
						 void **fsdata);

static int ext5_get_inline_size(struct inode *inode)
{
	if (EXT5_I(inode)->i_inline_off)
		return EXT5_I(inode)->i_inline_size;

	return 0;
}

static int get_max_inline_xattr_value_size(struct inode *inode,
					   struct ext5_iloc *iloc)
{
	struct ext5_xattr_ibody_header *header;
	struct ext5_xattr_entry *entry;
	struct ext5_inode *raw_inode;
	void *end;
	int free, min_offs;

	if (!EXT5_INODE_HAS_XATTR_SPACE(inode))
		return 0;

	min_offs = EXT5_SB(inode->i_sb)->s_inode_size -
			EXT5_GOOD_OLD_INODE_SIZE -
			EXT5_I(inode)->i_extra_isize -
			sizeof(struct ext5_xattr_ibody_header);

	/*
	 * We need to subtract another sizeof(__u32) since an in-inode xattr
	 * needs an empty 4 bytes to indicate the gap between the xattr entry
	 * and the name/value pair.
	 */
	if (!ext5_test_inode_state(inode, EXT5_STATE_XATTR))
		return EXT5_XATTR_SIZE(min_offs -
			EXT5_XATTR_LEN(strlen(EXT5_XATTR_SYSTEM_DATA)) -
			EXT5_XATTR_ROUND - sizeof(__u32));

	raw_inode = ext5_raw_inode(iloc);
	header = IHDR(inode, raw_inode);
	entry = IFIRST(header);
	end = (void *)raw_inode + EXT5_SB(inode->i_sb)->s_inode_size;

	/* Compute min_offs. */
	while (!IS_LAST_ENTRY(entry)) {
		void *next = EXT5_XATTR_NEXT(entry);

		if (next >= end) {
			EXT5_ERROR_INODE(inode,
					 "corrupt xattr in inline inode");
			return 0;
		}
		if (!entry->e_value_inum && entry->e_value_size) {
			size_t offs = le16_to_cpu(entry->e_value_offs);
			if (offs < min_offs)
				min_offs = offs;
		}
		entry = next;
	}
	free = min_offs -
		((void *)entry - (void *)IFIRST(header)) - sizeof(__u32);

	if (EXT5_I(inode)->i_inline_off) {
		entry = (struct ext5_xattr_entry *)
			((void *)raw_inode + EXT5_I(inode)->i_inline_off);

		free += EXT5_XATTR_SIZE(le32_to_cpu(entry->e_value_size));
		goto out;
	}

	free -= EXT5_XATTR_LEN(strlen(EXT5_XATTR_SYSTEM_DATA));

	if (free > EXT5_XATTR_ROUND)
		free = EXT5_XATTR_SIZE(free - EXT5_XATTR_ROUND);
	else
		free = 0;

out:
	return free;
}

/*
 * Get the maximum size we now can store in an inode.
 * If we can't find the space for a xattr entry, don't use the space
 * of the extents since we have no space to indicate the inline data.
 */
int ext5_get_max_inline_size(struct inode *inode)
{
	int error, max_inline_size;
	struct ext5_iloc iloc;

	if (EXT5_I(inode)->i_extra_isize == 0)
		return 0;

	error = ext5_get_inode_loc(inode, &iloc);
	if (error) {
		ext5_error_inode_err(inode, __func__, __LINE__, 0, -error,
				     "can't get inode location %lu",
				     inode->i_ino);
		return 0;
	}

	down_read(&EXT5_I(inode)->xattr_sem);
	max_inline_size = get_max_inline_xattr_value_size(inode, &iloc);
	up_read(&EXT5_I(inode)->xattr_sem);

	brelse(iloc.bh);

	if (!max_inline_size)
		return 0;

	return max_inline_size + EXT5_MIN_INLINE_DATA_SIZE;
}

/*
 * this function does not take xattr_sem, which is OK because it is
 * currently only used in a code path coming form ext5_iget, before
 * the new inode has been unlocked
 */
int ext5_find_inline_data_nolock(struct inode *inode)
{
	struct ext5_xattr_ibody_find is = {
		.s = { .not_found = -ENODATA, },
	};
	struct ext5_xattr_info i = {
		.name_index = EXT5_XATTR_INDEX_SYSTEM,
		.name = EXT5_XATTR_SYSTEM_DATA,
	};
	int error;

	if (EXT5_I(inode)->i_extra_isize == 0)
		return 0;

	error = ext5_get_inode_loc(inode, &is.iloc);
	if (error)
		return error;

	error = ext5_xattr_ibody_find(inode, &i, &is);
	if (error)
		goto out;

	if (!is.s.not_found) {
		if (is.s.here->e_value_inum) {
			EXT5_ERROR_INODE(inode, "inline data xattr refers "
					 "to an external xattr inode");
			error = -EFSCORRUPTED;
			goto out;
		}
		EXT5_I(inode)->i_inline_off = (u16)((void *)is.s.here -
					(void *)ext5_raw_inode(&is.iloc));
		EXT5_I(inode)->i_inline_size = EXT5_MIN_INLINE_DATA_SIZE +
				le32_to_cpu(is.s.here->e_value_size);
	}
out:
	brelse(is.iloc.bh);
	return error;
}

static int ext5_read_inline_data(struct inode *inode, void *buffer,
				 unsigned int len,
				 struct ext5_iloc *iloc)
{
	struct ext5_xattr_entry *entry;
	struct ext5_xattr_ibody_header *header;
	int cp_len = 0;
	struct ext5_inode *raw_inode;

	if (!len)
		return 0;

	BUG_ON(len > EXT5_I(inode)->i_inline_size);

	cp_len = min_t(unsigned int, len, EXT5_MIN_INLINE_DATA_SIZE);

	raw_inode = ext5_raw_inode(iloc);
	memcpy(buffer, (void *)(raw_inode->i_block), cp_len);

	len -= cp_len;
	buffer += cp_len;

	if (!len)
		goto out;

	header = IHDR(inode, raw_inode);
	entry = (struct ext5_xattr_entry *)((void *)raw_inode +
					    EXT5_I(inode)->i_inline_off);
	len = min_t(unsigned int, len,
		    (unsigned int)le32_to_cpu(entry->e_value_size));

	memcpy(buffer,
	       (void *)IFIRST(header) + le16_to_cpu(entry->e_value_offs), len);
	cp_len += len;

out:
	return cp_len;
}

/*
 * write the buffer to the inline inode.
 * If 'create' is set, we don't need to do the extra copy in the xattr
 * value since it is already handled by ext5_xattr_ibody_set.
 * That saves us one memcpy.
 */
static void ext5_write_inline_data(struct inode *inode, struct ext5_iloc *iloc,
				   void *buffer, loff_t pos, unsigned int len)
{
	struct ext5_xattr_entry *entry;
	struct ext5_xattr_ibody_header *header;
	struct ext5_inode *raw_inode;
	int cp_len = 0;

	if (unlikely(ext5_emergency_state(inode->i_sb)))
		return;

	BUG_ON(!EXT5_I(inode)->i_inline_off);
	BUG_ON(pos + len > EXT5_I(inode)->i_inline_size);

	raw_inode = ext5_raw_inode(iloc);
	buffer += pos;

	if (pos < EXT5_MIN_INLINE_DATA_SIZE) {
		cp_len = pos + len > EXT5_MIN_INLINE_DATA_SIZE ?
			 EXT5_MIN_INLINE_DATA_SIZE - pos : len;
		memcpy((void *)raw_inode->i_block + pos, buffer, cp_len);

		len -= cp_len;
		buffer += cp_len;
		pos += cp_len;
	}

	if (!len)
		return;

	pos -= EXT5_MIN_INLINE_DATA_SIZE;
	header = IHDR(inode, raw_inode);
	entry = (struct ext5_xattr_entry *)((void *)raw_inode +
					    EXT5_I(inode)->i_inline_off);

	memcpy((void *)IFIRST(header) + le16_to_cpu(entry->e_value_offs) + pos,
	       buffer, len);
}

static int ext5_create_inline_data(handle_t *handle,
				   struct inode *inode, unsigned len)
{
	int error;
	void *value = NULL;
	struct ext5_xattr_ibody_find is = {
		.s = { .not_found = -ENODATA, },
	};
	struct ext5_xattr_info i = {
		.name_index = EXT5_XATTR_INDEX_SYSTEM,
		.name = EXT5_XATTR_SYSTEM_DATA,
	};

	error = ext5_get_inode_loc(inode, &is.iloc);
	if (error)
		return error;

	BUFFER_TRACE(is.iloc.bh, "get_write_access");
	error = ext5_journal_get_write_access(handle, inode->i_sb, is.iloc.bh,
					      EXT5_JTR_NONE);
	if (error)
		goto out;

	if (len > EXT5_MIN_INLINE_DATA_SIZE) {
		value = EXT5_ZERO_XATTR_VALUE;
		len -= EXT5_MIN_INLINE_DATA_SIZE;
	} else {
		value = "";
		len = 0;
	}

	/* Insert the xttr entry. */
	i.value = value;
	i.value_len = len;

	error = ext5_xattr_ibody_find(inode, &i, &is);
	if (error)
		goto out;

	if (!is.s.not_found) {
		EXT5_ERROR_INODE(inode, "unexpected inline data xattr");
		error = -EFSCORRUPTED;
		goto out;
	}

	error = ext5_xattr_ibody_set(handle, inode, &i, &is);
	if (error) {
		if (error == -ENOSPC)
			ext5_clear_inode_state(inode,
					       EXT5_STATE_MAY_INLINE_DATA);
		goto out;
	}

	memset((void *)ext5_raw_inode(&is.iloc)->i_block,
		0, EXT5_MIN_INLINE_DATA_SIZE);

	EXT5_I(inode)->i_inline_off = (u16)((void *)is.s.here -
				      (void *)ext5_raw_inode(&is.iloc));
	EXT5_I(inode)->i_inline_size = len + EXT5_MIN_INLINE_DATA_SIZE;
	ext5_clear_inode_flag(inode, EXT5_INODE_EXTENTS);
	ext5_set_inode_flag(inode, EXT5_INODE_INLINE_DATA);
	get_bh(is.iloc.bh);
	error = ext5_mark_iloc_dirty(handle, inode, &is.iloc);

out:
	brelse(is.iloc.bh);
	return error;
}

static int ext5_update_inline_data(handle_t *handle, struct inode *inode,
				   unsigned int len)
{
	int error;
	void *value = NULL;
	struct ext5_xattr_ibody_find is = {
		.s = { .not_found = -ENODATA, },
	};
	struct ext5_xattr_info i = {
		.name_index = EXT5_XATTR_INDEX_SYSTEM,
		.name = EXT5_XATTR_SYSTEM_DATA,
	};

	/* If the old space is ok, write the data directly. */
	if (len <= EXT5_I(inode)->i_inline_size)
		return 0;

	error = ext5_get_inode_loc(inode, &is.iloc);
	if (error)
		return error;

	error = ext5_xattr_ibody_find(inode, &i, &is);
	if (error)
		goto out;

	if (is.s.not_found) {
		EXT5_ERROR_INODE(inode, "missing inline data xattr");
		error = -EFSCORRUPTED;
		goto out;
	}

	len -= EXT5_MIN_INLINE_DATA_SIZE;
	value = kzalloc(len, GFP_NOFS);
	if (!value) {
		error = -ENOMEM;
		goto out;
	}

	error = ext5_xattr_ibody_get(inode, i.name_index, i.name,
				     value, len);
	if (error < 0)
		goto out;

	BUFFER_TRACE(is.iloc.bh, "get_write_access");
	error = ext5_journal_get_write_access(handle, inode->i_sb, is.iloc.bh,
					      EXT5_JTR_NONE);
	if (error)
		goto out;

	/* Update the xattr entry. */
	i.value = value;
	i.value_len = len;

	error = ext5_xattr_ibody_set(handle, inode, &i, &is);
	if (error)
		goto out;

	EXT5_I(inode)->i_inline_off = (u16)((void *)is.s.here -
				      (void *)ext5_raw_inode(&is.iloc));
	EXT5_I(inode)->i_inline_size = EXT5_MIN_INLINE_DATA_SIZE +
				le32_to_cpu(is.s.here->e_value_size);
	ext5_set_inode_state(inode, EXT5_STATE_MAY_INLINE_DATA);
	get_bh(is.iloc.bh);
	error = ext5_mark_iloc_dirty(handle, inode, &is.iloc);

out:
	kfree(value);
	brelse(is.iloc.bh);
	return error;
}

static int ext5_prepare_inline_data(handle_t *handle, struct inode *inode,
				    loff_t len)
{
	int ret, size, no_expand;
	struct ext5_inode_info *ei = EXT5_I(inode);

	if (!ext5_test_inode_state(inode, EXT5_STATE_MAY_INLINE_DATA))
		return -ENOSPC;

	size = ext5_get_max_inline_size(inode);
	if (size < len)
		return -ENOSPC;

	ext5_write_lock_xattr(inode, &no_expand);

	if (ei->i_inline_off)
		ret = ext5_update_inline_data(handle, inode, len);
	else
		ret = ext5_create_inline_data(handle, inode, len);

	ext5_write_unlock_xattr(inode, &no_expand);
	return ret;
}

static int ext5_destroy_inline_data_nolock(handle_t *handle,
					   struct inode *inode)
{
	struct ext5_inode_info *ei = EXT5_I(inode);
	struct ext5_xattr_ibody_find is = {
		.s = { .not_found = 0, },
	};
	struct ext5_xattr_info i = {
		.name_index = EXT5_XATTR_INDEX_SYSTEM,
		.name = EXT5_XATTR_SYSTEM_DATA,
		.value = NULL,
		.value_len = 0,
	};
	int error;

	if (!ei->i_inline_off)
		return 0;

	error = ext5_get_inode_loc(inode, &is.iloc);
	if (error)
		return error;

	error = ext5_xattr_ibody_find(inode, &i, &is);
	if (error)
		goto out;

	BUFFER_TRACE(is.iloc.bh, "get_write_access");
	error = ext5_journal_get_write_access(handle, inode->i_sb, is.iloc.bh,
					      EXT5_JTR_NONE);
	if (error)
		goto out;

	error = ext5_xattr_ibody_set(handle, inode, &i, &is);
	if (error)
		goto out;

	memset((void *)ext5_raw_inode(&is.iloc)->i_block,
		0, EXT5_MIN_INLINE_DATA_SIZE);
	memset(ei->i_data, 0, EXT5_MIN_INLINE_DATA_SIZE);

	if (ext5_has_feature_extents(inode->i_sb)) {
		if (S_ISDIR(inode->i_mode) ||
		    S_ISREG(inode->i_mode) || S_ISLNK(inode->i_mode)) {
			ext5_set_inode_flag(inode, EXT5_INODE_EXTENTS);
			ext5_ext_tree_init(handle, inode);
		}
	}
	ext5_clear_inode_flag(inode, EXT5_INODE_INLINE_DATA);

	get_bh(is.iloc.bh);
	error = ext5_mark_iloc_dirty(handle, inode, &is.iloc);

	EXT5_I(inode)->i_inline_off = 0;
	EXT5_I(inode)->i_inline_size = 0;
	ext5_clear_inode_state(inode, EXT5_STATE_MAY_INLINE_DATA);
out:
	brelse(is.iloc.bh);
	if (error == -ENODATA)
		error = 0;
	return error;
}

static int ext5_read_inline_folio(struct inode *inode, struct folio *folio)
{
	void *kaddr;
	int ret = 0;
	size_t len;
	struct ext5_iloc iloc;

	BUG_ON(!folio_test_locked(folio));
	BUG_ON(!ext5_has_inline_data(inode));
	BUG_ON(folio->index);

	if (!EXT5_I(inode)->i_inline_off) {
		ext5_warning(inode->i_sb, "inode %lu doesn't have inline data.",
			     inode->i_ino);
		goto out;
	}

	ret = ext5_get_inode_loc(inode, &iloc);
	if (ret)
		goto out;

	len = min_t(size_t, ext5_get_inline_size(inode), i_size_read(inode));
	BUG_ON(len > PAGE_SIZE);
	kaddr = kmap_local_folio(folio, 0);
	ret = ext5_read_inline_data(inode, kaddr, len, &iloc);
	kaddr = folio_zero_tail(folio, len, kaddr + len);
	kunmap_local(kaddr);
	folio_mark_uptodate(folio);
	brelse(iloc.bh);

out:
	return ret;
}

int ext5_readpage_inline(struct inode *inode, struct folio *folio)
{
	int ret = 0;

	down_read(&EXT5_I(inode)->xattr_sem);
	if (!ext5_has_inline_data(inode)) {
		up_read(&EXT5_I(inode)->xattr_sem);
		return -EAGAIN;
	}

	/*
	 * Current inline data can only exist in the 1st page,
	 * So for all the other pages, just set them uptodate.
	 */
	if (!folio->index)
		ret = ext5_read_inline_folio(inode, folio);
	else if (!folio_test_uptodate(folio)) {
		folio_zero_segment(folio, 0, folio_size(folio));
		folio_mark_uptodate(folio);
	}

	up_read(&EXT5_I(inode)->xattr_sem);

	folio_unlock(folio);
	return ret >= 0 ? 0 : ret;
}

static int ext5_convert_inline_data_to_extent(struct address_space *mapping,
					      struct inode *inode)
{
	int ret, needed_blocks, no_expand;
	handle_t *handle = NULL;
	int retries = 0, sem_held = 0;
	struct folio *folio = NULL;
	unsigned from, to;
	struct ext5_iloc iloc;

	if (!ext5_has_inline_data(inode)) {
		/*
		 * clear the flag so that no new write
		 * will trap here again.
		 */
		ext5_clear_inode_state(inode, EXT5_STATE_MAY_INLINE_DATA);
		return 0;
	}

	needed_blocks = ext5_chunk_trans_extent(inode, 1);

	ret = ext5_get_inode_loc(inode, &iloc);
	if (ret)
		return ret;

retry:
	handle = ext5_journal_start(inode, EXT5_HT_WRITE_PAGE, needed_blocks);
	if (IS_ERR(handle)) {
		ret = PTR_ERR(handle);
		handle = NULL;
		goto out;
	}

	/* We cannot recurse into the filesystem as the transaction is already
	 * started */
	folio = __filemap_get_folio(mapping, 0, FGP_WRITEBEGIN | FGP_NOFS,
			mapping_gfp_mask(mapping));
	if (IS_ERR(folio)) {
		ret = PTR_ERR(folio);
		goto out_nofolio;
	}

	ext5_write_lock_xattr(inode, &no_expand);
	sem_held = 1;
	/* If some one has already done this for us, just exit. */
	if (!ext5_has_inline_data(inode)) {
		ret = 0;
		goto out;
	}

	from = 0;
	to = ext5_get_inline_size(inode);
	if (!folio_test_uptodate(folio)) {
		ret = ext5_read_inline_folio(inode, folio);
		if (ret < 0)
			goto out;
	}

	ext5_fc_track_inode(handle, inode);
	ret = ext5_destroy_inline_data_nolock(handle, inode);
	if (ret)
		goto out;

	if (ext5_should_dioread_nolock(inode)) {
		ret = ext5_block_write_begin(handle, folio, from, to,
					     ext5_get_block_unwritten);
	} else
		ret = ext5_block_write_begin(handle, folio, from, to,
					     ext5_get_block);
	clear_buffer_new(folio_buffers(folio));

	if (!ret && ext5_should_journal_data(inode)) {
		ret = ext5_walk_page_buffers(handle, inode,
					     folio_buffers(folio), from, to,
					     NULL, do_journal_get_write_access);
	}

	if (ret) {
		folio_unlock(folio);
		folio_put(folio);
		folio = NULL;
		ext5_orphan_add(handle, inode);
		ext5_write_unlock_xattr(inode, &no_expand);
		sem_held = 0;
		ext5_journal_stop(handle);
		handle = NULL;
		ext5_truncate_failed_write(inode);
		/*
		 * If truncate failed early the inode might
		 * still be on the orphan list; we need to
		 * make sure the inode is removed from the
		 * orphan list in that case.
		 */
		if (inode->i_nlink)
			ext5_orphan_del(NULL, inode);
	}

	if (ret == -ENOSPC && ext5_should_retry_alloc(inode->i_sb, &retries))
		goto retry;

	if (folio)
		block_commit_write(folio, from, to);
out:
	if (folio) {
		folio_unlock(folio);
		folio_put(folio);
	}
out_nofolio:
	if (sem_held)
		ext5_write_unlock_xattr(inode, &no_expand);
	if (handle)
		ext5_journal_stop(handle);
	brelse(iloc.bh);
	return ret;
}

/*
 * Prepare the write for the inline data.
 * If the data can be written into the inode, we just read
 * the page and make it uptodate, and start the journal.
 * Otherwise read the page, makes it dirty so that it can be
 * handle in writepages(the i_disksize update is left to the
 * normal ext5_da_write_end).
 */
int ext5_generic_write_inline_data(struct address_space *mapping,
					  struct inode *inode,
					  loff_t pos, unsigned len,
					  struct folio **foliop,
					  void **fsdata, bool da)
{
	int ret;
	handle_t *handle;
	struct folio *folio;
	struct ext5_iloc iloc;
	int retries = 0;

	ret = ext5_get_inode_loc(inode, &iloc);
	if (ret)
		return ret;

retry_journal:
	handle = ext5_journal_start(inode, EXT5_HT_INODE, 1);
	if (IS_ERR(handle)) {
		ret = PTR_ERR(handle);
		goto out_release_bh;
	}

	ret = ext5_prepare_inline_data(handle, inode, pos + len);
	if (ret && ret != -ENOSPC)
		goto out_stop_journal;

	if (ret == -ENOSPC) {
		ext5_journal_stop(handle);
		if (!da) {
			brelse(iloc.bh);
			/* Retry inside */
			return ext5_convert_inline_data_to_extent(mapping, inode);
		}

		ret = ext5_da_convert_inline_data_to_extent(mapping, inode, fsdata);
		if (ret == -ENOSPC &&
		    ext5_should_retry_alloc(inode->i_sb, &retries))
			goto retry_journal;
		goto out_release_bh;
	}

	folio = __filemap_get_folio(mapping, 0, FGP_WRITEBEGIN | FGP_NOFS,
					mapping_gfp_mask(mapping));
	if (IS_ERR(folio)) {
		ret = PTR_ERR(folio);
		goto out_stop_journal;
	}

	down_read(&EXT5_I(inode)->xattr_sem);
	/* Someone else had converted it to extent */
	if (!ext5_has_inline_data(inode)) {
		ret = 0;
		goto out_release_folio;
	}

	if (!folio_test_uptodate(folio)) {
		ret = ext5_read_inline_folio(inode, folio);
		if (ret < 0)
			goto out_release_folio;
	}

	ret = ext5_journal_get_write_access(handle, inode->i_sb, iloc.bh, EXT5_JTR_NONE);
	if (ret)
		goto out_release_folio;
	*foliop = folio;
	up_read(&EXT5_I(inode)->xattr_sem);
	brelse(iloc.bh);
	return 1;

out_release_folio:
	up_read(&EXT5_I(inode)->xattr_sem);
	folio_unlock(folio);
	folio_put(folio);
out_stop_journal:
	ext5_journal_stop(handle);
out_release_bh:
	brelse(iloc.bh);
	return ret;
}

/*
 * Try to write data in the inode.
 * If the inode has inline data, check whether the new write can be
 * in the inode also. If not, create the page the handle, move the data
 * to the page make it update and let the later codes create extent for it.
 */
int ext5_try_to_write_inline_data(struct address_space *mapping,
				  struct inode *inode,
				  loff_t pos, unsigned len,
				  struct folio **foliop)
{
	if (pos + len > ext5_get_max_inline_size(inode))
		return ext5_convert_inline_data_to_extent(mapping, inode);
	return ext5_generic_write_inline_data(mapping, inode, pos, len,
					      foliop, NULL, false);
}

int ext5_write_inline_data_end(struct inode *inode, loff_t pos, unsigned len,
			       unsigned copied, struct folio *folio)
{
	handle_t *handle = ext5_journal_current_handle();
	int no_expand;
	void *kaddr;
	struct ext5_iloc iloc;
	int ret = 0, ret2;

	if (unlikely(copied < len) && !folio_test_uptodate(folio))
		copied = 0;

	if (likely(copied)) {
		ret = ext5_get_inode_loc(inode, &iloc);
		if (ret) {
			folio_unlock(folio);
			folio_put(folio);
			ext5_std_error(inode->i_sb, ret);
			goto out;
		}
		ext5_write_lock_xattr(inode, &no_expand);
		BUG_ON(!ext5_has_inline_data(inode));

		/*
		 * ei->i_inline_off may have changed since
		 * ext5_write_begin() called
		 * ext5_try_to_write_inline_data()
		 */
		(void) ext5_find_inline_data_nolock(inode);

		kaddr = kmap_local_folio(folio, 0);
		ext5_write_inline_data(inode, &iloc, kaddr, pos, copied);
		kunmap_local(kaddr);
		folio_mark_uptodate(folio);
		/* clear dirty flag so that writepages wouldn't work for us. */
		folio_clear_dirty(folio);

		ext5_write_unlock_xattr(inode, &no_expand);
		brelse(iloc.bh);

		/*
		 * It's important to update i_size while still holding folio
		 * lock: page writeout could otherwise come in and zero
		 * beyond i_size.
		 */
		ext5_update_inode_size(inode, pos + copied);
	}
	folio_unlock(folio);
	folio_put(folio);

	/*
	 * Don't mark the inode dirty under folio lock. First, it unnecessarily
	 * makes the holding time of folio lock longer. Second, it forces lock
	 * ordering of folio lock and transaction start for journaling
	 * filesystems.
	 */
	if (likely(copied))
		mark_inode_dirty(inode);
out:
	/*
	 * If we didn't copy as much data as expected, we need to trim back
	 * size of xattr containing inline data.
	 */
	if (pos + len > inode->i_size && ext5_can_truncate(inode))
		ext5_orphan_add(handle, inode);

	ret2 = ext5_journal_stop(handle);
	if (!ret)
		ret = ret2;
	if (pos + len > inode->i_size) {
		ext5_truncate_failed_write(inode);
		/*
		 * If truncate failed early the inode might still be
		 * on the orphan list; we need to make sure the inode
		 * is removed from the orphan list in that case.
		 */
		if (inode->i_nlink)
			ext5_orphan_del(NULL, inode);
	}
	return ret ? ret : copied;
}

/*
 * Try to make the page cache and handle ready for the inline data case.
 * We can call this function in 2 cases:
 * 1. The inode is created and the first write exceeds inline size. We can
 *    clear the inode state safely.
 * 2. The inode has inline data, then we need to read the data, make it
 *    update and dirty so that ext5_da_writepages can handle it. We don't
 *    need to start the journal since the file's metadata isn't changed now.
 */
static int ext5_da_convert_inline_data_to_extent(struct address_space *mapping,
						 struct inode *inode,
						 void **fsdata)
{
	int ret = 0, inline_size;
	struct folio *folio;

	folio = __filemap_get_folio(mapping, 0, FGP_WRITEBEGIN,
					mapping_gfp_mask(mapping));
	if (IS_ERR(folio))
		return PTR_ERR(folio);

	down_read(&EXT5_I(inode)->xattr_sem);
	if (!ext5_has_inline_data(inode)) {
		ext5_clear_inode_state(inode, EXT5_STATE_MAY_INLINE_DATA);
		goto out;
	}

	inline_size = ext5_get_inline_size(inode);

	if (!folio_test_uptodate(folio)) {
		ret = ext5_read_inline_folio(inode, folio);
		if (ret < 0)
			goto out;
	}

	ret = ext5_block_write_begin(NULL, folio, 0, inline_size,
				     ext5_da_get_block_prep);
	if (ret) {
		up_read(&EXT5_I(inode)->xattr_sem);
		folio_unlock(folio);
		folio_put(folio);
		ext5_truncate_failed_write(inode);
		return ret;
	}

	clear_buffer_new(folio_buffers(folio));
	folio_mark_dirty(folio);
	folio_mark_uptodate(folio);
	ext5_clear_inode_state(inode, EXT5_STATE_MAY_INLINE_DATA);
	*fsdata = (void *)CONVERT_INLINE_DATA;

out:
	up_read(&EXT5_I(inode)->xattr_sem);
	if (folio) {
		folio_unlock(folio);
		folio_put(folio);
	}
	return ret;
}

#ifdef INLINE_DIR_DEBUG
void ext5_show_inline_dir(struct inode *dir, struct buffer_head *bh,
			  void *inline_start, int inline_size)
{
	int offset;
	unsigned short de_len;
	struct ext5_dir_entry_2 *de = inline_start;
	void *dlimit = inline_start + inline_size;

	trace_printk("inode %lu\n", dir->i_ino);
	offset = 0;
	while ((void *)de < dlimit) {
		de_len = ext5_rec_len_from_disk(de->rec_len, inline_size);
		trace_printk("de: off %u rlen %u name %.*s nlen %u ino %u\n",
			     offset, de_len, de->name_len, de->name,
			     de->name_len, le32_to_cpu(de->inode));
		if (ext5_check_dir_entry(dir, NULL, de, bh,
					 inline_start, inline_size, offset))
			BUG();

		offset += de_len;
		de = (struct ext5_dir_entry_2 *) ((char *) de + de_len);
	}
}
#else
#define ext5_show_inline_dir(dir, bh, inline_start, inline_size)
#endif

/*
 * Add a new entry into a inline dir.
 * It will return -ENOSPC if no space is available, and -EIO
 * and -EEXIST if directory entry already exists.
 */
static int ext5_add_dirent_to_inline(handle_t *handle,
				     struct ext5_filename *fname,
				     struct inode *dir,
				     struct inode *inode,
				     struct ext5_iloc *iloc,
				     void *inline_start, int inline_size)
{
	int		err;
	struct ext5_dir_entry_2 *de;

	err = ext5_find_dest_de(dir, iloc->bh, inline_start,
				inline_size, fname, &de);
	if (err)
		return err;

	BUFFER_TRACE(iloc->bh, "get_write_access");
	err = ext5_journal_get_write_access(handle, dir->i_sb, iloc->bh,
					    EXT5_JTR_NONE);
	if (err)
		return err;
	ext5_insert_dentry(dir, inode, de, inline_size, fname);

	ext5_show_inline_dir(dir, iloc->bh, inline_start, inline_size);

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
	return 1;
}

static void *ext5_get_inline_xattr_pos(struct inode *inode,
				       struct ext5_iloc *iloc)
{
	struct ext5_xattr_entry *entry;
	struct ext5_xattr_ibody_header *header;

	BUG_ON(!EXT5_I(inode)->i_inline_off);

	header = IHDR(inode, ext5_raw_inode(iloc));
	entry = (struct ext5_xattr_entry *)((void *)ext5_raw_inode(iloc) +
					    EXT5_I(inode)->i_inline_off);

	return (void *)IFIRST(header) + le16_to_cpu(entry->e_value_offs);
}

/* Set the final de to cover the whole block. */
static void ext5_update_final_de(void *de_buf, int old_size, int new_size)
{
	struct ext5_dir_entry_2 *de, *prev_de;
	void *limit;
	int de_len;

	de = de_buf;
	if (old_size) {
		limit = de_buf + old_size;
		do {
			prev_de = de;
			de_len = ext5_rec_len_from_disk(de->rec_len, old_size);
			de_buf += de_len;
			de = de_buf;
		} while (de_buf < limit);

		prev_de->rec_len = ext5_rec_len_to_disk(de_len + new_size -
							old_size, new_size);
	} else {
		/* this is just created, so create an empty entry. */
		de->inode = 0;
		de->rec_len = ext5_rec_len_to_disk(new_size, new_size);
	}
}

static int ext5_update_inline_dir(handle_t *handle, struct inode *dir,
				  struct ext5_iloc *iloc)
{
	int ret;
	int old_size = EXT5_I(dir)->i_inline_size - EXT5_MIN_INLINE_DATA_SIZE;
	int new_size = get_max_inline_xattr_value_size(dir, iloc);

	if (new_size - old_size <= ext5_dir_rec_len(1, NULL))
		return -ENOSPC;

	ret = ext5_update_inline_data(handle, dir,
				      new_size + EXT5_MIN_INLINE_DATA_SIZE);
	if (ret)
		return ret;

	ext5_update_final_de(ext5_get_inline_xattr_pos(dir, iloc), old_size,
			     EXT5_I(dir)->i_inline_size -
						EXT5_MIN_INLINE_DATA_SIZE);
	dir->i_size = EXT5_I(dir)->i_disksize = EXT5_I(dir)->i_inline_size;
	return 0;
}

static void ext5_restore_inline_data(handle_t *handle, struct inode *inode,
				     struct ext5_iloc *iloc,
				     void *buf, int inline_size)
{
	int ret;

	ret = ext5_create_inline_data(handle, inode, inline_size);
	if (ret) {
		ext5_msg(inode->i_sb, KERN_EMERG,
			"error restoring inline_data for inode -- potential data loss! (inode %lu, error %d)",
			inode->i_ino, ret);
		return;
	}
	ext5_write_inline_data(inode, iloc, buf, 0, inline_size);
	ext5_set_inode_state(inode, EXT5_STATE_MAY_INLINE_DATA);
}

static int ext5_finish_convert_inline_dir(handle_t *handle,
					  struct inode *inode,
					  struct buffer_head *dir_block,
					  void *buf,
					  int inline_size)
{
	int err, csum_size = 0, header_size = 0;
	struct ext5_dir_entry_2 *de;
	void *target = dir_block->b_data;

	/*
	 * First create "." and ".." and then copy the dir information
	 * back to the block.
	 */
	de = target;
	de = ext5_init_dot_dotdot(inode, de,
		inode->i_sb->s_blocksize, csum_size,
		le32_to_cpu(((struct ext5_dir_entry_2 *)buf)->inode), 1);
	header_size = (void *)de - target;

	memcpy((void *)de, buf + EXT5_INLINE_DOTDOT_SIZE,
		inline_size - EXT5_INLINE_DOTDOT_SIZE);

	if (ext5_has_feature_metadata_csum(inode->i_sb))
		csum_size = sizeof(struct ext5_dir_entry_tail);

	inode->i_size = inode->i_sb->s_blocksize;
	i_size_write(inode, inode->i_sb->s_blocksize);
	EXT5_I(inode)->i_disksize = inode->i_sb->s_blocksize;
	ext5_update_final_de(dir_block->b_data,
			inline_size - EXT5_INLINE_DOTDOT_SIZE + header_size,
			inode->i_sb->s_blocksize - csum_size);

	if (csum_size)
		ext5_initialize_dirent_tail(dir_block,
					    inode->i_sb->s_blocksize);
	set_buffer_uptodate(dir_block);
	unlock_buffer(dir_block);
	err = ext5_handle_dirty_dirblock(handle, inode, dir_block);
	if (err)
		return err;
	set_buffer_verified(dir_block);
	return ext5_mark_inode_dirty(handle, inode);
}

static int ext5_convert_inline_data_nolock(handle_t *handle,
					   struct inode *inode,
					   struct ext5_iloc *iloc)
{
	int error;
	void *buf = NULL;
	struct buffer_head *data_bh = NULL;
	struct ext5_map_blocks map;
	int inline_size;

	inline_size = ext5_get_inline_size(inode);
	buf = kmalloc(inline_size, GFP_NOFS);
	if (!buf) {
		error = -ENOMEM;
		goto out;
	}

	error = ext5_read_inline_data(inode, buf, inline_size, iloc);
	if (error < 0)
		goto out;

	/*
	 * Make sure the inline directory entries pass checks before we try to
	 * convert them, so that we avoid touching stuff that needs fsck.
	 */
	if (S_ISDIR(inode->i_mode)) {
		error = ext5_check_all_de(inode, iloc->bh,
					buf + EXT5_INLINE_DOTDOT_SIZE,
					inline_size - EXT5_INLINE_DOTDOT_SIZE);
		if (error)
			goto out;
	}

	error = ext5_destroy_inline_data_nolock(handle, inode);
	if (error)
		goto out;

	map.m_lblk = 0;
	map.m_len = 1;
	map.m_flags = 0;
	error = ext5_map_blocks(handle, inode, &map, EXT5_GET_BLOCKS_CREATE);
	if (error < 0)
		goto out_restore;
	if (!(map.m_flags & EXT5_MAP_MAPPED)) {
		error = -EIO;
		goto out_restore;
	}

	data_bh = sb_getblk(inode->i_sb, map.m_pblk);
	if (!data_bh) {
		error = -ENOMEM;
		goto out_restore;
	}

	lock_buffer(data_bh);
	error = ext5_journal_get_create_access(handle, inode->i_sb, data_bh,
					       EXT5_JTR_NONE);
	if (error) {
		unlock_buffer(data_bh);
		error = -EIO;
		goto out_restore;
	}
	memset(data_bh->b_data, 0, inode->i_sb->s_blocksize);

	if (!S_ISDIR(inode->i_mode)) {
		memcpy(data_bh->b_data, buf, inline_size);
		set_buffer_uptodate(data_bh);
		unlock_buffer(data_bh);
		error = ext5_handle_dirty_metadata(handle,
						   inode, data_bh);
	} else {
		error = ext5_finish_convert_inline_dir(handle, inode, data_bh,
						       buf, inline_size);
	}

out_restore:
	if (error)
		ext5_restore_inline_data(handle, inode, iloc, buf, inline_size);

out:
	brelse(data_bh);
	kfree(buf);
	return error;
}

/*
 * Try to add the new entry to the inline data.
 * If succeeds, return 0. If not, extended the inline dir and copied data to
 * the new created block.
 */
int ext5_try_add_inline_entry(handle_t *handle, struct ext5_filename *fname,
			      struct inode *dir, struct inode *inode)
{
	int ret, ret2, inline_size, no_expand;
	void *inline_start;
	struct ext5_iloc iloc;

	ret = ext5_get_inode_loc(dir, &iloc);
	if (ret)
		return ret;

	ext5_write_lock_xattr(dir, &no_expand);
	if (!ext5_has_inline_data(dir))
		goto out;

	inline_start = (void *)ext5_raw_inode(&iloc)->i_block +
						 EXT5_INLINE_DOTDOT_SIZE;
	inline_size = EXT5_MIN_INLINE_DATA_SIZE - EXT5_INLINE_DOTDOT_SIZE;

	ret = ext5_add_dirent_to_inline(handle, fname, dir, inode, &iloc,
					inline_start, inline_size);
	if (ret != -ENOSPC)
		goto out;

	/* check whether it can be inserted to inline xattr space. */
	inline_size = EXT5_I(dir)->i_inline_size -
			EXT5_MIN_INLINE_DATA_SIZE;
	if (!inline_size) {
		/* Try to use the xattr space.*/
		ret = ext5_update_inline_dir(handle, dir, &iloc);
		if (ret && ret != -ENOSPC)
			goto out;

		inline_size = EXT5_I(dir)->i_inline_size -
				EXT5_MIN_INLINE_DATA_SIZE;
	}

	if (inline_size) {
		inline_start = ext5_get_inline_xattr_pos(dir, &iloc);

		ret = ext5_add_dirent_to_inline(handle, fname, dir,
						inode, &iloc, inline_start,
						inline_size);

		if (ret != -ENOSPC)
			goto out;
	}

	/*
	 * The inline space is filled up, so create a new block for it.
	 * As the extent tree will be created, we have to save the inline
	 * dir first.
	 */
	ret = ext5_convert_inline_data_nolock(handle, dir, &iloc);

out:
	ext5_write_unlock_xattr(dir, &no_expand);
	ret2 = ext5_mark_inode_dirty(handle, dir);
	if (unlikely(ret2 && !ret))
		ret = ret2;
	brelse(iloc.bh);
	return ret;
}

/*
 * This function fills a red-black tree with information from an
 * inlined dir.  It returns the number directory entries loaded
 * into the tree.  If there is an error it is returned in err.
 */
int ext5_inlinedir_to_tree(struct file *dir_file,
			   struct inode *dir, ext5_lblk_t block,
			   struct dx_hash_info *hinfo,
			   __u32 start_hash, __u32 start_minor_hash,
			   int *has_inline_data)
{
	int err = 0, count = 0;
	unsigned int parent_ino;
	int pos;
	struct ext5_dir_entry_2 *de;
	struct inode *inode = file_inode(dir_file);
	int ret, inline_size = 0;
	struct ext5_iloc iloc;
	void *dir_buf = NULL;
	struct ext5_dir_entry_2 fake;
	struct fscrypt_str tmp_str;

	ret = ext5_get_inode_loc(inode, &iloc);
	if (ret)
		return ret;

	down_read(&EXT5_I(inode)->xattr_sem);
	if (!ext5_has_inline_data(inode)) {
		up_read(&EXT5_I(inode)->xattr_sem);
		*has_inline_data = 0;
		goto out;
	}

	inline_size = ext5_get_inline_size(inode);
	dir_buf = kmalloc(inline_size, GFP_NOFS);
	if (!dir_buf) {
		ret = -ENOMEM;
		up_read(&EXT5_I(inode)->xattr_sem);
		goto out;
	}

	ret = ext5_read_inline_data(inode, dir_buf, inline_size, &iloc);
	up_read(&EXT5_I(inode)->xattr_sem);
	if (ret < 0)
		goto out;

	pos = 0;
	parent_ino = le32_to_cpu(((struct ext5_dir_entry_2 *)dir_buf)->inode);
	while (pos < inline_size) {
		/*
		 * As inlined dir doesn't store any information about '.' and
		 * only the inode number of '..' is stored, we have to handle
		 * them differently.
		 */
		if (pos == 0) {
			fake.inode = cpu_to_le32(inode->i_ino);
			fake.name_len = 1;
			strcpy(fake.name, ".");
			fake.rec_len = ext5_rec_len_to_disk(
					  ext5_dir_rec_len(fake.name_len, NULL),
					  inline_size);
			ext5_set_de_type(inode->i_sb, &fake, S_IFDIR);
			de = &fake;
			pos = EXT5_INLINE_DOTDOT_OFFSET;
		} else if (pos == EXT5_INLINE_DOTDOT_OFFSET) {
			fake.inode = cpu_to_le32(parent_ino);
			fake.name_len = 2;
			strcpy(fake.name, "..");
			fake.rec_len = ext5_rec_len_to_disk(
					  ext5_dir_rec_len(fake.name_len, NULL),
					  inline_size);
			ext5_set_de_type(inode->i_sb, &fake, S_IFDIR);
			de = &fake;
			pos = EXT5_INLINE_DOTDOT_SIZE;
		} else {
			de = (struct ext5_dir_entry_2 *)(dir_buf + pos);
			pos += ext5_rec_len_from_disk(de->rec_len, inline_size);
			if (ext5_check_dir_entry(inode, dir_file, de,
					 iloc.bh, dir_buf,
					 inline_size, pos)) {
				ret = count;
				goto out;
			}
		}

		if (ext5_hash_in_dirent(dir)) {
			hinfo->hash = EXT5_DIRENT_HASH(de);
			hinfo->minor_hash = EXT5_DIRENT_MINOR_HASH(de);
		} else {
			err = ext5fs_dirhash(dir, de->name, de->name_len, hinfo);
			if (err) {
				ret = err;
				goto out;
			}
		}
		if ((hinfo->hash < start_hash) ||
		    ((hinfo->hash == start_hash) &&
		     (hinfo->minor_hash < start_minor_hash)))
			continue;
		if (de->inode == 0)
			continue;
		tmp_str.name = de->name;
		tmp_str.len = de->name_len;
		err = ext5_htree_store_dirent(dir_file, hinfo->hash,
					      hinfo->minor_hash, de, &tmp_str);
		if (err) {
			ret = err;
			goto out;
		}
		count++;
	}
	ret = count;
out:
	kfree(dir_buf);
	brelse(iloc.bh);
	return ret;
}

/*
 * So this function is called when the volume is mkfsed with
 * dir_index disabled. In order to keep f_pos persistent
 * after we convert from an inlined dir to a blocked based,
 * we just pretend that we are a normal dir and return the
 * offset as if '.' and '..' really take place.
 *
 */
int ext5_read_inline_dir(struct file *file,
			 struct dir_context *ctx,
			 int *has_inline_data)
{
	unsigned int offset, parent_ino;
	int i;
	struct ext5_dir_entry_2 *de;
	struct super_block *sb;
	struct inode *inode = file_inode(file);
	int ret, inline_size = 0;
	struct ext5_iloc iloc;
	void *dir_buf = NULL;
	int dotdot_offset, dotdot_size, extra_offset, extra_size;
	struct dir_private_info *info = file->private_data;

	ret = ext5_get_inode_loc(inode, &iloc);
	if (ret)
		return ret;

	down_read(&EXT5_I(inode)->xattr_sem);
	if (!ext5_has_inline_data(inode)) {
		up_read(&EXT5_I(inode)->xattr_sem);
		*has_inline_data = 0;
		goto out;
	}

	inline_size = ext5_get_inline_size(inode);
	dir_buf = kmalloc(inline_size, GFP_NOFS);
	if (!dir_buf) {
		ret = -ENOMEM;
		up_read(&EXT5_I(inode)->xattr_sem);
		goto out;
	}

	ret = ext5_read_inline_data(inode, dir_buf, inline_size, &iloc);
	up_read(&EXT5_I(inode)->xattr_sem);
	if (ret < 0)
		goto out;

	ret = 0;
	sb = inode->i_sb;
	parent_ino = le32_to_cpu(((struct ext5_dir_entry_2 *)dir_buf)->inode);
	offset = ctx->pos;

	/*
	 * dotdot_offset and dotdot_size is the real offset and
	 * size for ".." and "." if the dir is block based while
	 * the real size for them are only EXT5_INLINE_DOTDOT_SIZE.
	 * So we will use extra_offset and extra_size to indicate them
	 * during the inline dir iteration.
	 */
	dotdot_offset = ext5_dir_rec_len(1, NULL);
	dotdot_size = dotdot_offset + ext5_dir_rec_len(2, NULL);
	extra_offset = dotdot_size - EXT5_INLINE_DOTDOT_SIZE;
	extra_size = extra_offset + inline_size;

	/*
	 * If the cookie has changed since the last call to
	 * readdir(2), then we might be pointing to an invalid
	 * dirent right now.  Scan from the start of the inline
	 * dir to make sure.
	 */
	if (!inode_eq_iversion(inode, info->cookie)) {
		for (i = 0; i < extra_size && i < offset;) {
			/*
			 * "." is with offset 0 and
			 * ".." is dotdot_offset.
			 */
			if (!i) {
				i = dotdot_offset;
				continue;
			} else if (i == dotdot_offset) {
				i = dotdot_size;
				continue;
			}
			/* for other entry, the real offset in
			 * the buf has to be tuned accordingly.
			 */
			de = (struct ext5_dir_entry_2 *)
				(dir_buf + i - extra_offset);
			/* It's too expensive to do a full
			 * dirent test each time round this
			 * loop, but we do have to test at
			 * least that it is non-zero.  A
			 * failure will be detected in the
			 * dirent test below. */
			if (ext5_rec_len_from_disk(de->rec_len, extra_size)
				< ext5_dir_rec_len(1, NULL))
				break;
			i += ext5_rec_len_from_disk(de->rec_len,
						    extra_size);
		}
		offset = i;
		ctx->pos = offset;
		info->cookie = inode_query_iversion(inode);
	}

	while (ctx->pos < extra_size) {
		if (ctx->pos == 0) {
			if (!dir_emit(ctx, ".", 1, inode->i_ino, DT_DIR))
				goto out;
			ctx->pos = dotdot_offset;
			continue;
		}

		if (ctx->pos == dotdot_offset) {
			if (!dir_emit(ctx, "..", 2, parent_ino, DT_DIR))
				goto out;
			ctx->pos = dotdot_size;
			continue;
		}

		de = (struct ext5_dir_entry_2 *)
			(dir_buf + ctx->pos - extra_offset);
		if (ext5_check_dir_entry(inode, file, de, iloc.bh, dir_buf,
					 extra_size, ctx->pos))
			goto out;
		if (le32_to_cpu(de->inode)) {
			if (!dir_emit(ctx, de->name, de->name_len,
				      le32_to_cpu(de->inode),
				      get_dtype(sb, de->file_type)))
				goto out;
		}
		ctx->pos += ext5_rec_len_from_disk(de->rec_len, extra_size);
	}
out:
	kfree(dir_buf);
	brelse(iloc.bh);
	return ret;
}

void *ext5_read_inline_link(struct inode *inode)
{
	struct ext5_iloc iloc;
	int ret, inline_size;
	void *link;

	ret = ext5_get_inode_loc(inode, &iloc);
	if (ret)
		return ERR_PTR(ret);

	ret = -ENOMEM;
	inline_size = ext5_get_inline_size(inode);
	link = kmalloc(inline_size + 1, GFP_NOFS);
	if (!link)
		goto out;

	ret = ext5_read_inline_data(inode, link, inline_size, &iloc);
	if (ret < 0) {
		kfree(link);
		goto out;
	}
	nd_terminate_link(link, inode->i_size, ret);
out:
	if (ret < 0)
		link = ERR_PTR(ret);
	brelse(iloc.bh);
	return link;
}

struct buffer_head *ext5_get_first_inline_block(struct inode *inode,
					struct ext5_dir_entry_2 **parent_de,
					int *retval)
{
	struct ext5_iloc iloc;

	*retval = ext5_get_inode_loc(inode, &iloc);
	if (*retval)
		return NULL;

	*parent_de = (struct ext5_dir_entry_2 *)ext5_raw_inode(&iloc)->i_block;

	return iloc.bh;
}

/*
 * Try to create the inline data for the new dir.
 * If it succeeds, return 0, otherwise return the error.
 * In case of ENOSPC, the caller should create the normal disk layout dir.
 */
int ext5_try_create_inline_dir(handle_t *handle, struct inode *parent,
			       struct inode *inode)
{
	int ret, inline_size = EXT5_MIN_INLINE_DATA_SIZE;
	struct ext5_iloc iloc;
	struct ext5_dir_entry_2 *de;

	ret = ext5_get_inode_loc(inode, &iloc);
	if (ret)
		return ret;

	ret = ext5_prepare_inline_data(handle, inode, inline_size);
	if (ret)
		goto out;

	/*
	 * For inline dir, we only save the inode information for the ".."
	 * and create a fake dentry to cover the left space.
	 */
	de = (struct ext5_dir_entry_2 *)ext5_raw_inode(&iloc)->i_block;
	de->inode = cpu_to_le32(parent->i_ino);
	de = (struct ext5_dir_entry_2 *)((void *)de + EXT5_INLINE_DOTDOT_SIZE);
	de->inode = 0;
	de->rec_len = ext5_rec_len_to_disk(
				inline_size - EXT5_INLINE_DOTDOT_SIZE,
				inline_size);
	set_nlink(inode, 2);
	inode->i_size = EXT5_I(inode)->i_disksize = inline_size;
out:
	brelse(iloc.bh);
	return ret;
}

struct buffer_head *ext5_find_inline_entry(struct inode *dir,
					struct ext5_filename *fname,
					struct ext5_dir_entry_2 **res_dir,
					int *has_inline_data)
{
	struct ext5_xattr_ibody_find is = {
		.s = { .not_found = -ENODATA, },
	};
	struct ext5_xattr_info i = {
		.name_index = EXT5_XATTR_INDEX_SYSTEM,
		.name = EXT5_XATTR_SYSTEM_DATA,
	};
	int ret;
	void *inline_start;
	int inline_size;

	ret = ext5_get_inode_loc(dir, &is.iloc);
	if (ret)
		return ERR_PTR(ret);

	down_read(&EXT5_I(dir)->xattr_sem);

	ret = ext5_xattr_ibody_find(dir, &i, &is);
	if (ret)
		goto out;

	if (!ext5_has_inline_data(dir)) {
		*has_inline_data = 0;
		goto out;
	}

	inline_start = (void *)ext5_raw_inode(&is.iloc)->i_block +
						EXT5_INLINE_DOTDOT_SIZE;
	inline_size = EXT5_MIN_INLINE_DATA_SIZE - EXT5_INLINE_DOTDOT_SIZE;
	ret = ext5_search_dir(is.iloc.bh, inline_start, inline_size,
			      dir, fname, 0, res_dir);
	if (ret == 1)
		goto out_find;
	if (ret < 0)
		goto out;

	if (ext5_get_inline_size(dir) == EXT5_MIN_INLINE_DATA_SIZE)
		goto out;

	inline_start = ext5_get_inline_xattr_pos(dir, &is.iloc);
	inline_size = ext5_get_inline_size(dir) - EXT5_MIN_INLINE_DATA_SIZE;

	ret = ext5_search_dir(is.iloc.bh, inline_start, inline_size,
			      dir, fname, 0, res_dir);
	if (ret == 1)
		goto out_find;

out:
	brelse(is.iloc.bh);
	if (ret < 0)
		is.iloc.bh = ERR_PTR(ret);
	else
		is.iloc.bh = NULL;
out_find:
	up_read(&EXT5_I(dir)->xattr_sem);
	return is.iloc.bh;
}

int ext5_delete_inline_entry(handle_t *handle,
			     struct inode *dir,
			     struct ext5_dir_entry_2 *de_del,
			     struct buffer_head *bh,
			     int *has_inline_data)
{
	int err, inline_size, no_expand;
	struct ext5_iloc iloc;
	void *inline_start;

	err = ext5_get_inode_loc(dir, &iloc);
	if (err)
		return err;

	ext5_write_lock_xattr(dir, &no_expand);
	if (!ext5_has_inline_data(dir)) {
		*has_inline_data = 0;
		goto out;
	}

	if ((void *)de_del - ((void *)ext5_raw_inode(&iloc)->i_block) <
		EXT5_MIN_INLINE_DATA_SIZE) {
		inline_start = (void *)ext5_raw_inode(&iloc)->i_block +
					EXT5_INLINE_DOTDOT_SIZE;
		inline_size = EXT5_MIN_INLINE_DATA_SIZE -
				EXT5_INLINE_DOTDOT_SIZE;
	} else {
		inline_start = ext5_get_inline_xattr_pos(dir, &iloc);
		inline_size = ext5_get_inline_size(dir) -
				EXT5_MIN_INLINE_DATA_SIZE;
	}

	BUFFER_TRACE(bh, "get_write_access");
	err = ext5_journal_get_write_access(handle, dir->i_sb, bh,
					    EXT5_JTR_NONE);
	if (err)
		goto out;

	err = ext5_generic_delete_entry(dir, de_del, bh,
					inline_start, inline_size, 0);
	if (err)
		goto out;

	ext5_show_inline_dir(dir, iloc.bh, inline_start, inline_size);
out:
	ext5_write_unlock_xattr(dir, &no_expand);
	if (likely(err == 0))
		err = ext5_mark_inode_dirty(handle, dir);
	brelse(iloc.bh);
	if (err != -ENOENT)
		ext5_std_error(dir->i_sb, err);
	return err;
}

/*
 * Get the inline dentry at offset.
 */
static inline struct ext5_dir_entry_2 *
ext5_get_inline_entry(struct inode *inode,
		      struct ext5_iloc *iloc,
		      unsigned int offset,
		      void **inline_start,
		      int *inline_size)
{
	void *inline_pos;

	BUG_ON(offset > ext5_get_inline_size(inode));

	if (offset < EXT5_MIN_INLINE_DATA_SIZE) {
		inline_pos = (void *)ext5_raw_inode(iloc)->i_block;
		*inline_size = EXT5_MIN_INLINE_DATA_SIZE;
	} else {
		inline_pos = ext5_get_inline_xattr_pos(inode, iloc);
		offset -= EXT5_MIN_INLINE_DATA_SIZE;
		*inline_size = ext5_get_inline_size(inode) -
				EXT5_MIN_INLINE_DATA_SIZE;
	}

	if (inline_start)
		*inline_start = inline_pos;
	return (struct ext5_dir_entry_2 *)(inline_pos + offset);
}

bool empty_inline_dir(struct inode *dir, int *has_inline_data)
{
	int err, inline_size;
	struct ext5_iloc iloc;
	size_t inline_len;
	void *inline_pos;
	unsigned int offset;
	struct ext5_dir_entry_2 *de;
	bool ret = false;

	err = ext5_get_inode_loc(dir, &iloc);
	if (err) {
		EXT5_ERROR_INODE_ERR(dir, -err,
				     "error %d getting inode %lu block",
				     err, dir->i_ino);
		return false;
	}

	down_read(&EXT5_I(dir)->xattr_sem);
	if (!ext5_has_inline_data(dir)) {
		*has_inline_data = 0;
		ret = true;
		goto out;
	}

	de = (struct ext5_dir_entry_2 *)ext5_raw_inode(&iloc)->i_block;
	if (!le32_to_cpu(de->inode)) {
		ext5_warning(dir->i_sb,
			     "bad inline directory (dir #%lu) - no `..'",
			     dir->i_ino);
		goto out;
	}

	inline_len = ext5_get_inline_size(dir);
	offset = EXT5_INLINE_DOTDOT_SIZE;
	while (offset < inline_len) {
		de = ext5_get_inline_entry(dir, &iloc, offset,
					   &inline_pos, &inline_size);
		if (ext5_check_dir_entry(dir, NULL, de,
					 iloc.bh, inline_pos,
					 inline_size, offset)) {
			ext5_warning(dir->i_sb,
				     "bad inline directory (dir #%lu) - "
				     "inode %u, rec_len %u, name_len %d"
				     "inline size %d",
				     dir->i_ino, le32_to_cpu(de->inode),
				     le16_to_cpu(de->rec_len), de->name_len,
				     inline_size);
			goto out;
		}
		if (le32_to_cpu(de->inode)) {
			goto out;
		}
		offset += ext5_rec_len_from_disk(de->rec_len, inline_size);
	}

	ret = true;
out:
	up_read(&EXT5_I(dir)->xattr_sem);
	brelse(iloc.bh);
	return ret;
}

int ext5_destroy_inline_data(handle_t *handle, struct inode *inode)
{
	int ret, no_expand;

	ext5_write_lock_xattr(inode, &no_expand);
	ret = ext5_destroy_inline_data_nolock(handle, inode);
	ext5_write_unlock_xattr(inode, &no_expand);

	return ret;
}

int ext5_inline_data_iomap(struct inode *inode, struct iomap *iomap)
{
	__u64 addr;
	int error = -EAGAIN;
	struct ext5_iloc iloc;

	down_read(&EXT5_I(inode)->xattr_sem);
	if (!ext5_has_inline_data(inode))
		goto out;

	error = ext5_get_inode_loc(inode, &iloc);
	if (error)
		goto out;

	addr = (__u64)iloc.bh->b_blocknr << inode->i_sb->s_blocksize_bits;
	addr += (char *)ext5_raw_inode(&iloc) - iloc.bh->b_data;
	addr += offsetof(struct ext5_inode, i_block);

	brelse(iloc.bh);

	iomap->addr = addr;
	iomap->offset = 0;
	iomap->length = min_t(loff_t, ext5_get_inline_size(inode),
			      i_size_read(inode));
	iomap->type = IOMAP_INLINE;
	iomap->flags = 0;

out:
	up_read(&EXT5_I(inode)->xattr_sem);
	return error;
}

int ext5_inline_data_truncate(struct inode *inode, int *has_inline)
{
	handle_t *handle;
	int inline_size, value_len, needed_blocks, no_expand, err = 0;
	size_t i_size;
	void *value = NULL;
	struct ext5_xattr_ibody_find is = {
		.s = { .not_found = -ENODATA, },
	};
	struct ext5_xattr_info i = {
		.name_index = EXT5_XATTR_INDEX_SYSTEM,
		.name = EXT5_XATTR_SYSTEM_DATA,
	};


	needed_blocks = ext5_chunk_trans_extent(inode, 1);
	handle = ext5_journal_start(inode, EXT5_HT_INODE, needed_blocks);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	ext5_write_lock_xattr(inode, &no_expand);
	if (!ext5_has_inline_data(inode)) {
		ext5_write_unlock_xattr(inode, &no_expand);
		*has_inline = 0;
		ext5_journal_stop(handle);
		return 0;
	}

	if ((err = ext5_orphan_add(handle, inode)) != 0)
		goto out;

	if ((err = ext5_get_inode_loc(inode, &is.iloc)) != 0)
		goto out;

	down_write(&EXT5_I(inode)->i_data_sem);
	i_size = inode->i_size;
	inline_size = ext5_get_inline_size(inode);
	EXT5_I(inode)->i_disksize = i_size;

	if (i_size < inline_size) {
		/*
		 * if there's inline data to truncate and this file was
		 * converted to extents after that inline data was written,
		 * the extent status cache must be cleared to avoid leaving
		 * behind stale delayed allocated extent entries
		 */
		if (!ext5_test_inode_state(inode, EXT5_STATE_MAY_INLINE_DATA))
			ext5_es_remove_extent(inode, 0, EXT_MAX_BLOCKS);

		/* Clear the content in the xattr space. */
		if (inline_size > EXT5_MIN_INLINE_DATA_SIZE) {
			if ((err = ext5_xattr_ibody_find(inode, &i, &is)) != 0)
				goto out_error;

			if (is.s.not_found) {
				EXT5_ERROR_INODE(inode,
						 "missing inline data xattr");
				err = -EFSCORRUPTED;
				goto out_error;
			}

			value_len = le32_to_cpu(is.s.here->e_value_size);
			value = kmalloc(value_len, GFP_NOFS);
			if (!value) {
				err = -ENOMEM;
				goto out_error;
			}

			err = ext5_xattr_ibody_get(inode, i.name_index,
						   i.name, value, value_len);
			if (err <= 0)
				goto out_error;

			i.value = value;
			i.value_len = i_size > EXT5_MIN_INLINE_DATA_SIZE ?
					i_size - EXT5_MIN_INLINE_DATA_SIZE : 0;
			err = ext5_xattr_ibody_set(handle, inode, &i, &is);
			if (err)
				goto out_error;
		}

		/* Clear the content within i_blocks. */
		if (i_size < EXT5_MIN_INLINE_DATA_SIZE) {
			void *p = (void *) ext5_raw_inode(&is.iloc)->i_block;
			memset(p + i_size, 0,
			       EXT5_MIN_INLINE_DATA_SIZE - i_size);
		}

		EXT5_I(inode)->i_inline_size = i_size <
					EXT5_MIN_INLINE_DATA_SIZE ?
					EXT5_MIN_INLINE_DATA_SIZE : i_size;
	}

out_error:
	up_write(&EXT5_I(inode)->i_data_sem);
out:
	brelse(is.iloc.bh);
	ext5_write_unlock_xattr(inode, &no_expand);
	kfree(value);
	if (inode->i_nlink)
		ext5_orphan_del(handle, inode);

	if (err == 0) {
		inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
		err = ext5_mark_inode_dirty(handle, inode);
		if (IS_SYNC(inode))
			ext5_handle_sync(handle);
	}
	ext5_journal_stop(handle);
	return err;
}

int ext5_convert_inline_data(struct inode *inode)
{
	int error, needed_blocks, no_expand;
	handle_t *handle;
	struct ext5_iloc iloc;

	if (!ext5_has_inline_data(inode)) {
		ext5_clear_inode_state(inode, EXT5_STATE_MAY_INLINE_DATA);
		return 0;
	} else if (!ext5_test_inode_state(inode, EXT5_STATE_MAY_INLINE_DATA)) {
		/*
		 * Inode has inline data but EXT5_STATE_MAY_INLINE_DATA is
		 * cleared. This means we are in the middle of moving of
		 * inline data to delay allocated block. Just force writeout
		 * here to finish conversion.
		 */
		error = filemap_flush(inode->i_mapping);
		if (error)
			return error;
		if (!ext5_has_inline_data(inode))
			return 0;
	}

	needed_blocks = ext5_chunk_trans_extent(inode, 1);

	iloc.bh = NULL;
	error = ext5_get_inode_loc(inode, &iloc);
	if (error)
		return error;

	handle = ext5_journal_start(inode, EXT5_HT_WRITE_PAGE, needed_blocks);
	if (IS_ERR(handle)) {
		error = PTR_ERR(handle);
		goto out_free;
	}

	ext5_write_lock_xattr(inode, &no_expand);
	if (ext5_has_inline_data(inode))
		error = ext5_convert_inline_data_nolock(handle, inode, &iloc);
	ext5_write_unlock_xattr(inode, &no_expand);
	ext5_journal_stop(handle);
out_free:
	brelse(iloc.bh);
	return error;
}
