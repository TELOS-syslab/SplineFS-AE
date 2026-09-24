// SPDX-License-Identifier: GPL-2.0

#include <linux/quotaops.h>
#include <linux/uuid.h>

#include "ext5.h"
#include "xattr.h"
#include "ext5_jbd3.h"

static void ext5_fname_from_fscrypt_name(struct ext5_filename *dst,
					 const struct fscrypt_name *src)
{
	memset(dst, 0, sizeof(*dst));

	dst->usr_fname = src->usr_fname;
	dst->disk_name = src->disk_name;
	dst->hinfo.hash = src->hash;
	dst->hinfo.minor_hash = src->minor_hash;
	dst->crypto_buf = src->crypto_buf;
}

int ext5_fname_setup_filename(struct inode *dir, const struct qstr *iname,
			      int lookup, struct ext5_filename *fname)
{
	struct fscrypt_name name;
	int err;

	err = fscrypt_setup_filename(dir, iname, lookup, &name);
	if (err)
		return err;

	ext5_fname_from_fscrypt_name(fname, &name);

	err = ext5_fname_setup_ci_filename(dir, iname, fname);
	if (err)
		ext5_fname_free_filename(fname);

	return err;
}

int ext5_fname_prepare_lookup(struct inode *dir, struct dentry *dentry,
			      struct ext5_filename *fname)
{
	struct fscrypt_name name;
	int err;

	err = fscrypt_prepare_lookup(dir, dentry, &name);
	if (err)
		return err;

	ext5_fname_from_fscrypt_name(fname, &name);

	err = ext5_fname_setup_ci_filename(dir, &dentry->d_name, fname);
	if (err)
		ext5_fname_free_filename(fname);
	return err;
}

void ext5_fname_free_filename(struct ext5_filename *fname)
{
	struct fscrypt_name name;

	name.crypto_buf = fname->crypto_buf;
	fscrypt_free_filename(&name);

	fname->crypto_buf.name = NULL;
	fname->usr_fname = NULL;
	fname->disk_name.name = NULL;

	ext5_fname_free_ci_filename(fname);
}

static bool uuid_is_zero(__u8 u[16])
{
	int i;

	for (i = 0; i < 16; i++)
		if (u[i])
			return false;
	return true;
}

int ext5_ioctl_get_encryption_pwsalt(struct file *filp, void __user *arg)
{
	struct super_block *sb = file_inode(filp)->i_sb;
	struct ext5_sb_info *sbi = EXT5_SB(sb);
	int err, err2;
	handle_t *handle;

	if (!ext5_has_feature_encrypt(sb))
		return -EOPNOTSUPP;

	if (uuid_is_zero(sbi->s_es->s_encrypt_pw_salt)) {
		err = mnt_want_write_file(filp);
		if (err)
			return err;
		handle = ext5_journal_start_sb(sb, EXT5_HT_MISC, 1);
		if (IS_ERR(handle)) {
			err = PTR_ERR(handle);
			goto pwsalt_err_exit;
		}
		err = ext5_journal_get_write_access(handle, sb, sbi->s_sbh,
						    EXT5_JTR_NONE);
		if (err)
			goto pwsalt_err_journal;
		lock_buffer(sbi->s_sbh);
		generate_random_uuid(sbi->s_es->s_encrypt_pw_salt);
		ext5_superblock_csum_set(sb);
		unlock_buffer(sbi->s_sbh);
		err = ext5_handle_dirty_metadata(handle, NULL, sbi->s_sbh);
pwsalt_err_journal:
		err2 = ext5_journal_stop(handle);
		if (err2 && !err)
			err = err2;
pwsalt_err_exit:
		mnt_drop_write_file(filp);
		if (err)
			return err;
	}

	if (copy_to_user(arg, sbi->s_es->s_encrypt_pw_salt, 16))
		return -EFAULT;
	return 0;
}

static int ext5_get_context(struct inode *inode, void *ctx, size_t len)
{
	return ext5_xattr_get(inode, EXT5_XATTR_INDEX_ENCRYPTION,
				 EXT5_XATTR_NAME_ENCRYPTION_CONTEXT, ctx, len);
}

static int ext5_set_context(struct inode *inode, const void *ctx, size_t len,
							void *fs_data)
{
	handle_t *handle = fs_data;
	int res, res2, credits, retries = 0;

	/*
	 * Encrypting the root directory is not allowed because e2fsck expects
	 * lost+found to exist and be unencrypted, and encrypting the root
	 * directory would imply encrypting the lost+found directory as well as
	 * the filename "lost+found" itself.
	 */
	if (inode->i_ino == EXT5_ROOT_INO)
		return -EPERM;

	if (WARN_ON_ONCE(IS_DAX(inode) && i_size_read(inode)))
		return -EINVAL;

	if (ext5_test_inode_flag(inode, EXT5_INODE_DAX))
		return -EOPNOTSUPP;

	res = ext5_convert_inline_data(inode);
	if (res)
		return res;

	/*
	 * If a journal handle was specified, then the encryption context is
	 * being set on a new inode via inheritance and is part of a larger
	 * transaction to create the inode.  Otherwise the encryption context is
	 * being set on an existing inode in its own transaction.  Only in the
	 * latter case should the "retry on ENOSPC" logic be used.
	 */

	if (handle) {
		res = ext5_xattr_set_handle(handle, inode,
					    EXT5_XATTR_INDEX_ENCRYPTION,
					    EXT5_XATTR_NAME_ENCRYPTION_CONTEXT,
					    ctx, len, 0);
		if (!res) {
			ext5_set_inode_flag(inode, EXT5_INODE_ENCRYPT);
			ext5_clear_inode_state(inode,
					EXT5_STATE_MAY_INLINE_DATA);
			/*
			 * Update inode->i_flags - S_ENCRYPTED will be enabled,
			 * S_DAX may be disabled
			 */
			ext5_set_inode_flags(inode, false);
		}
		return res;
	}

	res = dquot_initialize(inode);
	if (res)
		return res;
retry:
	res = ext5_xattr_set_credits(inode, len, false /* is_create */,
				     &credits);
	if (res)
		return res;

	handle = ext5_journal_start(inode, EXT5_HT_MISC, credits);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	res = ext5_xattr_set_handle(handle, inode, EXT5_XATTR_INDEX_ENCRYPTION,
				    EXT5_XATTR_NAME_ENCRYPTION_CONTEXT,
				    ctx, len, 0);
	if (!res) {
		ext5_set_inode_flag(inode, EXT5_INODE_ENCRYPT);
		/*
		 * Update inode->i_flags - S_ENCRYPTED will be enabled,
		 * S_DAX may be disabled
		 */
		ext5_set_inode_flags(inode, false);
		res = ext5_mark_inode_dirty(handle, inode);
		if (res)
			EXT5_ERROR_INODE(inode, "Failed to mark inode dirty");
	}
	res2 = ext5_journal_stop(handle);

	if (res == -ENOSPC && ext5_should_retry_alloc(inode->i_sb, &retries))
		goto retry;
	if (!res)
		res = res2;
	return res;
}

static const union fscrypt_policy *ext5_get_dummy_policy(struct super_block *sb)
{
	return EXT5_SB(sb)->s_dummy_enc_policy.policy;
}

static bool ext5_has_stable_inodes(struct super_block *sb)
{
	return ext5_has_feature_stable_inodes(sb);
}

const struct fscrypt_operations ext5_cryptops = {
	.needs_bounce_pages	= 1,
	.has_32bit_inodes	= 1,
	.supports_subblock_data_units = 1,
	.legacy_key_prefix	= "ext5:",
	.get_context		= ext5_get_context,
	.set_context		= ext5_set_context,
	.get_dummy_policy	= ext5_get_dummy_policy,
	.empty_dir		= ext5_empty_dir,
	.has_stable_inodes	= ext5_has_stable_inodes,
};
