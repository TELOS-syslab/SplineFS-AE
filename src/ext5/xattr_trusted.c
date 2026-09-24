// SPDX-License-Identifier: GPL-2.0
/*
 * linux/fs/ext5/xattr_trusted.c
 * Handler for trusted extended attributes.
 *
 * Copyright (C) 2003 by Andreas Gruenbacher, <a.gruenbacher@computer.org>
 */

#include <linux/string.h>
#include <linux/capability.h>
#include <linux/fs.h>
#include "ext5_jbd3.h"
#include "ext5.h"
#include "xattr.h"

static bool
ext5_xattr_trusted_list(struct dentry *dentry)
{
	return capable(CAP_SYS_ADMIN);
}

static int
ext5_xattr_trusted_get(const struct xattr_handler *handler,
		       struct dentry *unused, struct inode *inode,
		       const char *name, void *buffer, size_t size)
{
	return ext5_xattr_get(inode, EXT5_XATTR_INDEX_TRUSTED,
			      name, buffer, size);
}

static int
ext5_xattr_trusted_set(const struct xattr_handler *handler,
		       struct mnt_idmap *idmap,
		       struct dentry *unused, struct inode *inode,
		       const char *name, const void *value,
		       size_t size, int flags)
{
	return ext5_xattr_set(inode, EXT5_XATTR_INDEX_TRUSTED,
			      name, value, size, flags);
}

const struct xattr_handler ext5_xattr_trusted_handler = {
	.prefix	= XATTR_TRUSTED_PREFIX,
	.list	= ext5_xattr_trusted_list,
	.get	= ext5_xattr_trusted_get,
	.set	= ext5_xattr_trusted_set,
};
