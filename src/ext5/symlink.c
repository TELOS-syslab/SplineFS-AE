// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/ext5/symlink.c
 *
 * Only fast symlinks left here - the rest is done by generic code. AV, 1999
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/symlink.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  ext5 symlink handling code
 */

#include <linux/fs.h>
#include <linux/namei.h>
#include "ext5.h"
#include "xattr.h"

static const char *ext5_encrypted_get_link(struct dentry *dentry,
					   struct inode *inode,
					   struct delayed_call *done)
{
	struct buffer_head *bh = NULL;
	const void *caddr;
	unsigned int max_size;
	const char *paddr;

	if (!dentry)
		return ERR_PTR(-ECHILD);

	if (ext5_inode_is_fast_symlink(inode)) {
		caddr = EXT5_I(inode)->i_data;
		max_size = sizeof(EXT5_I(inode)->i_data);
	} else {
		bh = ext5_bread(NULL, inode, 0, 0);
		if (IS_ERR(bh))
			return ERR_CAST(bh);
		if (!bh) {
			EXT5_ERROR_INODE(inode, "bad symlink.");
			return ERR_PTR(-EFSCORRUPTED);
		}
		caddr = bh->b_data;
		max_size = inode->i_sb->s_blocksize;
	}

	paddr = fscrypt_get_symlink(inode, caddr, max_size, done);
	brelse(bh);
	return paddr;
}

static int ext5_encrypted_symlink_getattr(struct mnt_idmap *idmap,
					  const struct path *path,
					  struct kstat *stat, u32 request_mask,
					  unsigned int query_flags)
{
	ext5_getattr(idmap, path, stat, request_mask, query_flags);

	return fscrypt_symlink_getattr(path, stat);
}

static void ext5_free_link(void *bh)
{
	brelse(bh);
}

static const char *ext5_get_link(struct dentry *dentry, struct inode *inode,
				 struct delayed_call *callback)
{
	struct buffer_head *bh;
	char *inline_link;

	/*
	 * Create a new inlined symlink is not supported, just provide a
	 * method to read the leftovers.
	 */
	if (ext5_has_inline_data(inode)) {
		if (!dentry)
			return ERR_PTR(-ECHILD);

		inline_link = ext5_read_inline_link(inode);
		if (!IS_ERR(inline_link))
			set_delayed_call(callback, kfree_link, inline_link);
		return inline_link;
	}

	if (!dentry) {
		bh = ext5_getblk(NULL, inode, 0, EXT5_GET_BLOCKS_CACHED_NOWAIT);
		if (IS_ERR(bh) || !bh)
			return ERR_PTR(-ECHILD);
		if (!ext5_buffer_uptodate(bh)) {
			brelse(bh);
			return ERR_PTR(-ECHILD);
		}
	} else {
		bh = ext5_bread(NULL, inode, 0, 0);
		if (IS_ERR(bh))
			return ERR_CAST(bh);
		if (!bh) {
			EXT5_ERROR_INODE(inode, "bad symlink.");
			return ERR_PTR(-EFSCORRUPTED);
		}
	}

	set_delayed_call(callback, ext5_free_link, bh);
	nd_terminate_link(bh->b_data, inode->i_size,
			  inode->i_sb->s_blocksize - 1);
	return bh->b_data;
}

const struct inode_operations ext5_encrypted_symlink_inode_operations = {
	.get_link	= ext5_encrypted_get_link,
	.setattr	= ext5_setattr,
	.getattr	= ext5_encrypted_symlink_getattr,
	.listxattr	= ext5_listxattr,
};

const struct inode_operations ext5_symlink_inode_operations = {
	.get_link	= ext5_get_link,
	.setattr	= ext5_setattr,
	.getattr	= ext5_getattr,
	.listxattr	= ext5_listxattr,
};

const struct inode_operations ext5_fast_symlink_inode_operations = {
	.get_link	= simple_get_link,
	.setattr	= ext5_setattr,
	.getattr	= ext5_getattr,
	.listxattr	= ext5_listxattr,
};
