// SPDX-License-Identifier: GPL-2.0
/*
 * SplineFS backend for stable directories.  A subtree root's blob (see
 * lidir_format.h) is parsed into an RCU-cached struct ext5_stable_blob.
 * Lookup: key = (PLID << 32) | hash32(name); the delta, then the Bloom filter,
 * radix and spline prediction, a bounded lower-bound search and an exact
 * name check.
 */

#include "ext5.h"
#include "ext5_jbd3.h"
#include "lidir_format.h"
#include "radix_spline.h"
#include <linux/bitmap.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/crc32.h>
#include <linux/highmem.h>
#include <linux/hash.h>
#include <linux/iversion.h>
#include <linux/math64.h>
#include <linux/moduleparam.h>
#include <linux/pagemap.h>
#include <linux/rbtree.h>
#include <linux/rcupdate.h>
#include <linux/refcount.h>
#include <linux/siphash.h>
#include <linux/slab.h>
#include <linux/shrinker.h>
#include <linux/srcu.h>
#include <linux/vmalloc.h>
#include <linux/writeback.h>

static unsigned int ext5_li_delta_trigger_bytes;
module_param_named(li_delta_trigger_bytes, ext5_li_delta_trigger_bytes,
		   uint, 0644);
MODULE_PARM_DESC(li_delta_trigger_bytes,
		 "Override logical delta refill bytes; 0 derives from entry count");

unsigned int ext5_li_compact_idle_ms = 50U;
module_param_named(li_compact_idle_ms, ext5_li_compact_idle_ms, uint, 0644);
MODULE_PARM_DESC(li_compact_idle_ms,
		 "Mutation-idle delay before soft-watermark compaction; default 50 ms");

static LIST_HEAD(ext5_li_blob_list);
static DEFINE_SPINLOCK(ext5_li_blob_list_lock);
static struct shrinker *ext5_li_model_shrinker;
DEFINE_STATIC_SRCU(ext5_li_lookup_srcu);

static void ext5_stable_queue_compact(struct inode *root, u64 generation,
				      bool urgent);

/*
 * In-memory delta entry, one per live INSERT or DELETE, with its name in the
 * same allocation.  Ordered by (plid, hash32, tiebreak64, name), which matches
 * the base slot order, so readdir merges the two in one pass.
 */
struct ext5_stable_delta_entry {
	struct rb_node rb;	/* in delta.by_key */
	u32 plid;
	u32 hash32;
	u64 tiebreak64;		/* SipHash-high32 | HalfSipHash search key */
	u32 ino;		/* 0 for DELETE tombstone */
	u8  op;
	u8  file_type;		/* DT_* */
	u8  name_len;
	u8  _pad;
	char *name;		/* points just past this allocation */
	u64 record_off;		/* latest record offset within delta region */
	u64 record_seq;
	u16 record_bytes;
	bool tail_cancellable;	/* latest INSERT created an absent name */
};

/* Delta state of one generation, mutated under `lock`.  Compaction
 * publishes a new blob with an empty delta. */
struct ext5_stable_delta {
	spinlock_t lock;
	struct rb_root_cached by_key;	/* ordered by full sort key */
	unsigned long *presence_filter;	/* stale bits are safe; no false negatives */
	u32 presence_filter_bits;	/* power of two, reset per generation */
	u32 nr_inserts;
	u32 nr_deletes;
	u32 nr_records;		/* total live entries (inserts + deletes) */
	u64 appended_inserts;	/* cumulative log records in this generation */
	u64 appended_deletes;

	/* Disk write position within the inode's data file. */
	u64 region_off;		/* start of delta region */
	u64 region_bytes;	/* total reserved bytes */
	u64 used_bytes;		/* bytes used (advances on append) */
	u64 seq_base;		/* sequence immediately before first record */
	u64 next_seq;		/* next seq we'll write */
	u64 trigger_bytes;	/* logical refill threshold, below reservation */
	u64 snapshot_floor_bytes; /* records below this active cut are immutable */
};

struct ext5_stable_extent_run {
	ext5_lblk_t logical;
	u32 length;
	ext5_fsblk_t physical;
};

struct ext5_stable_payload_block {
	struct buffer_head *bh;
	const char *data;
};

/* Parsed generation.  Navigation arrays are charged memory; slots and
 * names stay in the buffer cache. */
struct ext5_stable_blob {
	struct rcu_head rcu;
	struct rcu_head srcu;
	struct list_head cache_list;
	bool cache_listed;

	/*
	 * Lifetime: RCU guards pointer acquisition, sleeping lookups hold SRCU,
	 * readdir and lifecycle jobs take references.  The installed pointer's
	 * reference is dropped only after both grace periods.
	 */
	refcount_t refs;

	u64 generation;
	u32 entry_count;
	u32 parent_count;
	u32 filter_bits;
	u32 filter_mask;
	u32 radix_count;
	u32 spline_count;
	u32 spline_epsilon;
	u32 spline_corridor_error;
	u32 radix_bits;
	u32 radix_shift;
	u64 model_min_key;
	u64 model_max_key;

	/*
	 * The filter, model and parent_index stay in vmalloc; slots and names are
	 * read on demand from `host`'s page cache, charged to the faulting cgroup
	 * like ext4 dirent blocks.  `host` owns and outlives the blob.
	 */
	struct inode *host;
	void *model_buf;		/* charged cache: [filter_off, slots_off) */
	void *pindex_buf;		/* charged cache: parent index only */
	struct ext5_stable_extent_run *payload_extents;
	u32 nr_payload_extents;
	/* Optional pins of the payload's buffer heads, released by the shrinker;
	 * without them lookup reads the physical runs. */
	struct ext5_stable_payload_block *payload_blocks;
	ext5_lblk_t payload_first_block;
	u32 nr_payload_blocks;
	size_t payload_cache_bytes;
	size_t nav_cache_bytes;
	size_t base_size;
	u64 base_off;			/* file offset of the base region */
	u64 filter_off;
	u64 radix_off;
	u64 spline_off;
	u64 slots_off;			/* base-relative offset of slot array (on demand) */
	u64 parent_index_off;
	u64 names_off;			/* base-relative offset of names blob (on demand) */

	u32 names_bytes;

	/* siphash key copied from the descriptor. */
	siphash_key_t hash_key;

	/* Mutable delta state. */
	struct ext5_stable_delta delta;
};

static void ext5_stable_free_blob(struct ext5_stable_blob *blob);
static void ext5_stable_unpin_payload_blocks(struct ext5_stable_blob *blob);
static void ext5_stable_free_blob_rcu_cb(struct rcu_head *head);
static void ext5_stable_free_blob_srcu_cb(struct rcu_head *head);
static int ext5_stable_read_range(struct inode *dir, loff_t pos, size_t len,
				   void *dst);
static int ext5_stable_read_payload_range(
	const struct ext5_stable_blob *blob, loff_t pos, size_t len, void *dst);

/* Read one slot or name from the owner's page cache; `out` holds at least
 * name_len bytes. */
static inline int ext5_stable_read_slot(const struct ext5_stable_blob *blob,
					u32 idx, struct lidir_disk_slot *out)
{
	return ext5_stable_read_payload_range(blob,
		(loff_t)(blob->base_off + blob->slots_off) +
			(loff_t)idx * sizeof(*out),
		sizeof(*out), out);
}

static inline int ext5_stable_read_name(const struct ext5_stable_blob *blob,
					u32 name_off, u8 name_len, char *out)
{
	if (!name_len)
		return 0;
	return ext5_stable_read_payload_range(blob,
		(loff_t)(blob->base_off + blob->names_off) + (loff_t)name_off,
		name_len, out);
}

static inline int ext5_stable_read_radix(const struct ext5_stable_blob *blob,
					  u32 index, u32 *value)
{
	__le32 disk;
	int err;

	if (blob->model_buf) {
		memcpy(&disk, (char *)blob->model_buf +
		       (blob->radix_off - blob->filter_off) +
		       (u64)index * sizeof(disk), sizeof(disk));
		err = 0;
	} else {
		err = ext5_stable_read_range(blob->host,
			blob->base_off + blob->radix_off +
				(u64)index * sizeof(disk), sizeof(disk), &disk);
	}

	if (!err)
		*value = le32_to_cpu(disk);
	return err;
}

static inline int ext5_stable_read_spline(
	const struct ext5_stable_blob *blob, u32 index,
	struct lidir_disk_spline_point *point)
{
	if (blob->model_buf) {
		memcpy(point, (char *)blob->model_buf +
		       (blob->spline_off - blob->filter_off) +
		       (u64)index * sizeof(*point), sizeof(*point));
		return 0;
	}
	return ext5_stable_read_range(blob->host,
		blob->base_off + blob->spline_off +
			(u64)index * sizeof(*point), sizeof(*point), point);
}

static inline int ext5_stable_read_parent_index(
	const struct ext5_stable_blob *blob, u32 index, u32 *value)
{
	__le32 disk;
	int err;

	if (blob->pindex_buf) {
		memcpy(&disk, (char *)blob->pindex_buf +
		       (u64)index * sizeof(disk), sizeof(disk));
		err = 0;
	} else {
		err = ext5_stable_read_range(blob->host,
			blob->base_off + blob->parent_index_off +
				(u64)index * sizeof(disk), sizeof(disk), &disk);
	}

	if (!err)
		*value = le32_to_cpu(disk);
	return err;
}

static inline int ext5_stable_parent_range(const struct ext5_stable_blob *blob,
					    u32 plid, u32 *lo, u32 *hi)
{
	int err;

	if (plid >= blob->parent_count)
		return -EINVAL;
	err = ext5_stable_read_parent_index(blob, plid, lo);
	if (err)
		return err;
	return ext5_stable_read_parent_index(blob, plid + 1, hi);
}

static struct buffer_head *
ext5_stable_payload_bread(const struct ext5_stable_blob *blob,
			  ext5_lblk_t logical, bool *borrowed)
{
	u32 i;

	*borrowed = false;
	if (blob->payload_blocks && logical >= blob->payload_first_block &&
	    logical - blob->payload_first_block < blob->nr_payload_blocks) {
		struct buffer_head *bh = READ_ONCE(blob->payload_blocks[
			logical - blob->payload_first_block].bh);

		if (likely(bh)) {
			*borrowed = true;
			return bh;
		}
	}

	for (i = 0; i < blob->nr_payload_extents; i++) {
		const struct ext5_stable_extent_run *run =
			&blob->payload_extents[i];

		if (logical >= run->logical &&
		    logical - run->logical < run->length)
			return sb_bread(blob->host->i_sb,
				run->physical + (logical - run->logical));
	}
	return ext5_bread(NULL, blob->host, logical, 0);
}

static __always_inline int
ext5_stable_cached_map(const struct ext5_stable_blob *blob, u64 file_off,
			 size_t len, const void **mapped)
{
	u32 blocksize = blob->host->i_sb->s_blocksize;
	ext5_lblk_t block = file_off >> blob->host->i_sb->s_blocksize_bits;
	u32 offset = file_off & (blocksize - 1);
	u32 index;
	struct buffer_head *bh;

	if (offset + len > blocksize)
		return -ERANGE;
	if (unlikely(!blob->payload_blocks ||
		     block < blob->payload_first_block))
		return -ENOENT;
	index = block - blob->payload_first_block;
	if (unlikely(index >= blob->nr_payload_blocks))
		return -ENOENT;
	bh = READ_ONCE(blob->payload_blocks[index].bh);
	if (unlikely(!bh))
		return -EIO;
	*mapped = READ_ONCE(blob->payload_blocks[index].data) + offset;
	return 0;
}

/* Read the payload through the generation's physical extent map, which
 * stays valid after a transition moves the old extent tree. */
static int ext5_stable_read_payload_range(
	const struct ext5_stable_blob *blob, loff_t pos, size_t len, void *dst)
{
	unsigned int bs = blob->host->i_sb->s_blocksize;
	size_t copied = 0;

	while (copied < len) {
		ext5_lblk_t block = (pos + copied) >>
			blob->host->i_sb->s_blocksize_bits;
		size_t off = (pos + copied) & (bs - 1);
		size_t this = min_t(size_t, bs - off, len - copied);
		struct buffer_head *bh;
		bool borrowed;

		bh = ext5_stable_payload_bread(blob, block, &borrowed);
		if (IS_ERR(bh))
			return PTR_ERR(bh);
		if (!bh)
			return -EIO;
		memcpy((char *)dst + copied, bh->b_data + off, this);
		if (!borrowed)
			brelse(bh);
		copied += this;
	}
	return 0;
}

/* Per-lookup cache of the one or two blocks a search window spans. */
struct ext5_stable_reader {
	const struct ext5_stable_blob *blob;
	struct buffer_head *bh[4];
	bool borrowed[4];
	ext5_lblk_t block[4];
	u32 count;
	/* A 24-byte slot can rarely straddle a 4 KiB block boundary. */
	struct lidir_disk_slot crossing_slot;
};

static noinline void ext5_stable_reader_release(
	struct ext5_stable_reader *reader)
{
	u32 i;

	for (i = 0; i < reader->count; i++)
		if (!reader->borrowed[i])
			brelse(reader->bh[i]);
	reader->count = 0;
}

/* Pointer into a block pinned by `reader`, valid until reader_release or
 * until a fifth block evicts it.  -ERANGE: the object crosses a block. */
static int ext5_stable_reader_map(struct ext5_stable_reader *reader,
				  u64 file_off, size_t len,
				  const void **mapped)
{
	struct inode *host = reader->blob->host;
	u32 blocksize = host->i_sb->s_blocksize;
	ext5_lblk_t block = file_off >> host->i_sb->s_blocksize_bits;
	u32 offset = file_off & (blocksize - 1);
	struct buffer_head *bh = NULL;
	u32 i;

	if (offset + len > blocksize)
		return -ERANGE;
	for (i = 0; i < reader->count; i++) {
		if (reader->block[i] == block) {
			bh = reader->bh[i];
			break;
		}
	}
	if (!bh) {
		bool borrowed;

		bh = ext5_stable_payload_bread(reader->blob, block, &borrowed);
		if (IS_ERR(bh))
			return PTR_ERR(bh);
		if (!bh)
			return -EIO;
		if (reader->count == ARRAY_SIZE(reader->bh)) {
			if (!reader->borrowed[0])
				brelse(reader->bh[0]);
			memmove(&reader->bh[0], &reader->bh[1],
				(sizeof(reader->bh[0]) * (ARRAY_SIZE(reader->bh) - 1)));
			memmove(&reader->block[0], &reader->block[1],
				(sizeof(reader->block[0]) *
					 (ARRAY_SIZE(reader->block) - 1)));
			memmove(&reader->borrowed[0], &reader->borrowed[1],
				(sizeof(reader->borrowed[0]) *
				 (ARRAY_SIZE(reader->borrowed) - 1)));
			reader->count--;
		}
		reader->bh[reader->count] = bh;
		reader->block[reader->count] = block;
		reader->borrowed[reader->count] = borrowed;
		reader->count++;
	}
	*mapped = bh->b_data + offset;
	return 0;
}

static __always_inline int
ext5_stable_lookup_map(struct ext5_stable_reader *reader, u64 file_off,
			 size_t len, const void **mapped)
{
	if (likely(reader->blob->payload_blocks))
		return ext5_stable_cached_map(reader->blob, file_off, len, mapped);
	return ext5_stable_reader_map(reader, file_off, len, mapped);
}

static noinline int ext5_stable_reader_slot_slow(
	struct ext5_stable_reader *reader, u64 file_off,
	const struct lidir_disk_slot **slot)
{
	const void *mapped;
	int err;

	err = ext5_stable_lookup_map(reader, file_off, sizeof(**slot), &mapped);
	if (!err) {
		*slot = mapped;
		return 0;
	}
	if (err != -ERANGE)
		return err;
	err = ext5_stable_read_payload_range(reader->blob, file_off,
						 sizeof(**slot),
						 &reader->crossing_slot);
	if (!err)
		*slot = &reader->crossing_slot;
	return err;
}

static __always_inline int
ext5_stable_reader_slot(struct ext5_stable_reader *reader, u32 index,
				const struct lidir_disk_slot **slot)
{
	const struct ext5_stable_blob *blob = reader->blob;
	u64 file_off = blob->base_off + blob->slots_off +
			(u64)index * sizeof(**slot);

	if (likely(blob->payload_blocks)) {
		u32 bits = blob->host->i_sb->s_blocksize_bits;
		u32 blocksize = blob->host->i_sb->s_blocksize;
		ext5_lblk_t block = file_off >> bits;
		u32 offset = file_off & (blocksize - 1);

		if (likely(offset + sizeof(**slot) <= blocksize)) {
			const char *data = blob->payload_blocks[
				block - blob->payload_first_block].data;

			*slot = (const void *)(data + offset);
			return 0;
		}
	}
	return ext5_stable_reader_slot_slow(reader, file_off, slot);
}

static noinline int ext5_stable_reader_verify_slow(
	struct ext5_stable_reader *reader, const struct lidir_disk_slot *slot,
	const struct qstr *child, ino_t *ino, u64 file_off, u8 name_len)
{
	const struct ext5_stable_blob *blob = reader->blob;
	const void *mapped;
	int err;

	err = ext5_stable_lookup_map(reader, file_off, name_len, &mapped);
	if (!err) {
		if (memcmp(child->name, mapped, name_len))
			return -ENOENT;
	} else if (err == -ERANGE) {
		u32 blocksize = blob->host->i_sb->s_blocksize;
		u32 first = blocksize - (file_off & (blocksize - 1));

		err = ext5_stable_lookup_map(reader, file_off, first, &mapped);
		if (err)
			return err;
		if (memcmp(child->name, mapped, first))
			return -ENOENT;
		err = ext5_stable_lookup_map(reader, file_off + first,
					     name_len - first, &mapped);
		if (err)
			return err;
		if (memcmp(child->name + first, mapped, name_len - first))
			return -ENOENT;
	} else {
		return err;
	}
	*ino = le32_to_cpu(slot->ino);
	return 0;
}

static __always_inline int ext5_stable_reader_verify(
	struct ext5_stable_reader *reader, const struct lidir_disk_slot *slot,
	const struct qstr *child, ino_t *ino)
{
	const struct ext5_stable_blob *blob = reader->blob;
	u32 name_off = le32_to_cpu(slot->name_off);
	u8 name_len = slot->name_len;
	u64 file_off;

	if (name_off > blob->names_bytes ||
	    name_len > blob->names_bytes - name_off)
		return -EIO;
	if (child->len != name_len)
		return -ENOENT;
	file_off = blob->base_off + blob->names_off + name_off;
	if (likely(blob->payload_blocks)) {
		u32 bits = blob->host->i_sb->s_blocksize_bits;
		u32 blocksize = blob->host->i_sb->s_blocksize;
		ext5_lblk_t block = file_off >> bits;
		u32 offset = file_off & (blocksize - 1);

		if (likely(offset + name_len <= blocksize)) {
			const char *data = blob->payload_blocks[
				block - blob->payload_first_block].data;

			if (memcmp(child->name, data + offset, name_len))
				return -ENOENT;
			*ino = le32_to_cpu(slot->ino);
			return 0;
		}
	}
	return ext5_stable_reader_verify_slow(reader, slot, child, ino,
					      file_off, name_len);
}

/* Drop a blob reference taken with refcount_inc_not_zero(); frees on 0. */
static inline void ext5_stable_blob_put(struct ext5_stable_blob *blob)
{
	if (blob && refcount_dec_and_test(&blob->refs))
		ext5_stable_free_blob(blob);
}

static struct ext5_stable_delta_entry *
ext5_stable_delta_find(const struct ext5_stable_delta *d, u32 plid,
		       u32 hash32, u64 tiebreak64,
		       const char *name, u8 name_len);
static struct ext5_stable_delta_entry *
ext5_stable_delta_lower_bound(struct ext5_stable_delta *d,
			      u32 plid, u32 hash32, u64 tiebreak64);

/* Tiebreak = SipHash high 32 bits | HalfSipHash key: content-defined and
 * recoverable from the readdir cookie. */
static u64 ext5_stable_tiebreak64(const struct ext5_stable_blob *blob,
				  const char *name, unsigned int len,
				  u32 hash32)
{
	return (siphash(name, len, &blob->hash_key) & 0xffffffff00000000ULL) |
		hash32;
}

static u32 ext5_stable_hash32(const struct ext5_stable_blob *blob,
			       const char *name, unsigned int len)
{
	return hsiphash(name, len, (const hsiphash_key_t *)&blob->hash_key);
}

static int ext5_stable_dx_hash(const struct ext5_stable_blob *blob,
			       const char *name, unsigned int len, u32 *hash)
{
	struct super_block *sb = blob->host->i_sb;
	struct dx_hash_info hinfo = {
		.hash_version = EXT5_SB(sb)->s_def_hash_version,
		.seed = EXT5_SB(sb)->s_hash_seed,
	};
	int err;

	if (!ext5_has_feature_dir_index(sb)) {
		*hash = 0;
		return 0;
	}
	if (hinfo.hash_version <= DX_HASH_TEA)
		hinfo.hash_version += EXT5_SB(sb)->s_hash_unsigned;
	err = ext5fs_dirhash(blob->host, name, len, &hinfo);
	if (err < 0)
		return err;
	*hash = hinfo.hash;
	return 0;
}

static u64 ext5_stable_make_key(u32 parent_local_id, u32 name_hash32)
{
	return ((u64)parent_local_id << 32) | name_hash32;
}

static u64 mix64(u64 hash)
{
	hash ^= hash >> 33;
	hash *= 0xff51afd7ed558ccdULL;
	hash ^= hash >> 33;
	hash *= 0xc4ceb9fe1a85ec53ULL;
	hash ^= hash >> 33;
	return hash;
}

#define EXT5_STABLE_FILTER_PROBES	5U

static int ext5_stable_filter_maybe(const struct ext5_stable_blob *blob,
				    u64 key, bool *maybe)
{
	u64 h1 = mix64(key);
	u64 h2 = ((h1 << 32) | (h1 >> 32)) ^ 0x9e3779b97f4a7c15ULL;
	u32 mask = blob->filter_mask;
	u32 block_mask, block_base;
	u32 i, bit;
	__le64 fallback_block[8];
	const __le64 *block;
	int err;

	if (!mask) {
		*maybe = true;
		return 0;
	}
	block_mask = (blob->filter_bits / 512) - 1;
	block_base = ((u32)h1 & block_mask) * 512;
	h2 |= 1ULL;
	if (likely(blob->model_buf)) {
		block = (const __le64 *)((const char *)blob->model_buf +
					 block_base / 8);
		err = 0;
	} else {
		err = ext5_stable_read_range(blob->host,
			blob->base_off + blob->filter_off + block_base / 8,
			sizeof(fallback_block), fallback_block);
		block = fallback_block;
	}
	if (err)
		return err;
	for (i = 0; i < EXT5_STABLE_FILTER_PROBES; i++) {
		u64 word;
		u32 local_bit;

		bit = block_base + ((u32)(h2 + i * h1) & 511);
		local_bit = bit - block_base;
		word = le64_to_cpu(block[local_bit >> 6]);
		if (!(word & (1ULL << (local_bit & 63)))) {
			*maybe = false;
			return 0;
		}
	}
	*maybe = true;
	return 0;
}

/* Check a slot's name: 0 with *ino set, -ENOENT on mismatch, -EIO on
 * corruption. */
static int ext5_stable_verify_slot(const struct ext5_stable_blob *blob,
				    const struct lidir_disk_slot *slot,
				    const struct qstr *child, ino_t *ino)
{
	u32 name_off = le32_to_cpu(slot->name_off);
	u8 name_len = slot->name_len;
	char namebuf[EXT5_NAME_LEN];
	int err;

	if (name_off > blob->names_bytes ||
	    name_len > blob->names_bytes - name_off)
		return -EIO;
	if (child->len != name_len)
		return -ENOENT;
	err = ext5_stable_read_name(blob, name_off, name_len, namebuf);
	if (err)
		return err;
	if (memcmp(child->name, namebuf, name_len))
		return -ENOENT;
	*ino = le32_to_cpu(slot->ino);
	return 0;
}

static u32 ext5_stable_rs_prefix(const struct ext5_stable_blob *blob, u64 key)
{
	u64 delta, prefix;

	if (key <= blob->model_min_key)
		return 0;
	delta = key - blob->model_min_key;
	prefix = blob->radix_shift ? delta >> blob->radix_shift : delta;
	if (prefix + 1 >= blob->radix_count)
		prefix = blob->radix_count - 2;
	return (u32)prefix;
}

static int ext5_stable_rs_segment(const struct ext5_stable_blob *blob,
				  u64 key, u32 *segment)
{
	u32 prefix = ext5_stable_rs_prefix(blob, key);
	u32 begin, end;
	u32 lo, hi;
	int err;

	err = ext5_stable_read_radix(blob, prefix, &begin);
	if (err)
		return err;
	err = ext5_stable_read_radix(blob, prefix + 1, &end);
	if (err)
		return err;

	if (begin >= blob->spline_count)
		begin = blob->spline_count - 1;
	if (end > blob->spline_count)
		end = blob->spline_count;
	if (end <= begin)
		end = begin + 1;
	if (end - begin < 32) {
		u32 pos = begin;

		while (pos < end) {
			struct lidir_disk_spline_point point;

			err = ext5_stable_read_spline(blob, pos, &point);
			if (err)
				return err;
			if (le64_to_cpu(point.key) >= key)
				break;
			pos++;
		}
		if (pos >= blob->spline_count)
			pos = blob->spline_count - 1;
		*segment = pos ? pos : 1;
		return 0;
	}
	lo = begin;
	hi = end;
	while (lo < hi) {
		u32 mid = lo + (hi - lo) / 2;
		struct lidir_disk_spline_point point;

		err = ext5_stable_read_spline(blob, mid, &point);
		if (err)
			return err;
		if (le64_to_cpu(point.key) < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo >= blob->spline_count)
		lo = blob->spline_count - 1;
	*segment = lo ? lo : 1;
	return 0;
}

static int ext5_stable_rs_estimate(const struct ext5_stable_blob *blob,
				   u64 key, u32 *result)
{
	u32 segment;
	struct lidir_disk_spline_point down, up;
	u64 x0, x1, xdelta, xspan, yspan, estimate;
	u32 y0, y1;
	int err;

	if (key <= blob->model_min_key) {
		*result = 0;
		return 0;
	}
	if (key >= blob->model_max_key) {
		*result = blob->entry_count - 1;
		return 0;
	}
	if (blob->spline_count == 1) {
		err = ext5_stable_read_spline(blob, 0, &down);
		if (!err)
			*result = le32_to_cpu(down.rank);
		return err;
	}
	err = ext5_stable_rs_segment(blob, key, &segment);
	if (err)
		return err;
	err = ext5_stable_read_spline(blob, segment - 1, &down);
	if (err)
		return err;
	err = ext5_stable_read_spline(blob, segment, &up);
	if (err)
		return err;
	x0 = le64_to_cpu(down.key);
	x1 = le64_to_cpu(up.key);
	y0 = le32_to_cpu(down.rank);
	y1 = le32_to_cpu(up.rank);
	if (x1 <= x0) {
		*result = y0;
		return 0;
	}
	xdelta = key - x0;
	xspan = x1 - x0;
	yspan = y1 - y0;
	estimate = y0 + mul_u64_u64_div_u64(xdelta, yspan, xspan);
	if (estimate >= blob->entry_count)
		estimate = blob->entry_count - 1;
	*result = (u32)estimate;
	return 0;
}

struct ext5_stable_lookup_range {
	u32 begin;
	u32 end;
	u32 estimate;
};

static int ext5_stable_lookup_range(const struct ext5_stable_blob *blob,
				    u32 plid, u64 key,
				    struct ext5_stable_lookup_range *range)
{
	u32 parent_lo, parent_hi, begin, end, estimate;
	u64 end64;

	if (!blob->entry_count || plid >= blob->parent_count)
		return -ENOENT;
	if (ext5_stable_parent_range(blob, plid, &parent_lo, &parent_hi))
		return -EIO;
	if (parent_lo > parent_hi || parent_hi > blob->entry_count)
		return -EIO;
	if (parent_lo == parent_hi)
		return -ENOENT;
	if (key < blob->model_min_key || key > blob->model_max_key)
		return -ENOENT;
	if (ext5_stable_rs_estimate(blob, key, &estimate))
		return -EIO;
	ext5_stable_stats_rs_prediction();
	begin = estimate < blob->spline_epsilon ?
		0 : estimate - blob->spline_epsilon;
	end64 = (u64)estimate + blob->spline_epsilon + 2;
	end = end64 > blob->entry_count ? blob->entry_count : (u32)end64;
	begin = max(begin, parent_lo);
	end = min(end, parent_hi);
	if (begin >= end)
		return -ENOENT;
	range->begin = begin;
	range->end = end;
	range->estimate = estimate;
	return 0;
}

/* Fast path for a fully pinned generation, without reader scratch state. */
static __always_inline int ext5_stable_cached_slot(
	const struct ext5_stable_blob *blob, u32 index,
	const struct lidir_disk_slot **slot)
{
	u64 file_off = blob->base_off + blob->slots_off +
		(u64)index * sizeof(**slot);
	const void *mapped;
	int err;

	err = ext5_stable_cached_map(blob, file_off, sizeof(**slot), &mapped);
	if (!err)
		*slot = mapped;
	else if (err == -ENOENT)
		err = -EIO;
	return err;
}

static __always_inline int ext5_stable_cached_verify(
	const struct ext5_stable_blob *blob,
	const struct lidir_disk_slot *slot, const struct qstr *child, ino_t *ino)
{
	u32 name_off = le32_to_cpu(slot->name_off);
	u8 name_len = slot->name_len;
	u64 file_off;
	const void *mapped;
	int err;

	if (name_off > blob->names_bytes ||
	    name_len > blob->names_bytes - name_off)
		return -EIO;
	if (child->len != name_len)
		return -ENOENT;
	file_off = blob->base_off + blob->names_off + name_off;
	err = ext5_stable_cached_map(blob, file_off, name_len, &mapped);
	if (err == -ENOENT)
		err = -EIO;
	if (err)
		return err;
	if (memcmp(child->name, mapped, name_len))
		return -ENOENT;
	*ino = le32_to_cpu(slot->ino);
	return 0;
}

/* -ERANGE: an object crosses a block; redo the search via the reader. */
static int ext5_stable_lookup_cached(
	const struct ext5_stable_blob *blob, u64 key, const struct qstr *child,
	ino_t *ino, const struct ext5_stable_lookup_range *range)
{
	u32 low = range->begin;
	u32 high = range->end;
	u32 i;

	/* Lower-bound only inside the persisted, verified error window. */
	while (low < high) {
		u32 mid = low + (high - low) / 2;
		const struct lidir_disk_slot *slot;
		int err;

		err = ext5_stable_cached_slot(blob, mid, &slot);
		if (err)
			return err;
		if (le64_to_cpu(slot->key) < key)
			low = mid + 1;
		else
			high = mid;
	}
	for (i = low; i < range->end; i++) {
		const struct lidir_disk_slot *slot;
		int err;

		err = ext5_stable_cached_slot(blob, i, &slot);
		if (err)
			return err;
		if (le64_to_cpu(slot->key) != key)
			break;
		err = ext5_stable_cached_verify(blob, slot, child, ino);
		if (!err) {
			ext5_stable_stats_rs_error(i > range->estimate ?
				i - range->estimate : range->estimate - i);
			return 0;
		}
		if (err != -ENOENT)
			return err;
	}
	return -ENOENT;
}

static noinline int ext5_stable_lookup_uncached(
	const struct ext5_stable_blob *blob, u64 key, const struct qstr *child,
	ino_t *ino, const struct ext5_stable_lookup_range *range)
{
	struct ext5_stable_reader reader = { .blob = blob };
	u32 low = range->begin;
	u32 high = range->end;
	u32 i;
	int result = -ENOENT;

	while (low < high) {
		u32 mid = low + (high - low) / 2;
		const struct lidir_disk_slot *slot;

		if (ext5_stable_reader_slot(&reader, mid, &slot)) {
			result = -EIO;
			goto out;
		}
		if (le64_to_cpu(slot->key) < key)
			low = mid + 1;
		else
			high = mid;
	}
	for (i = low; i < range->end; i++) {
		const struct lidir_disk_slot *slot;
		int err;

		if (ext5_stable_reader_slot(&reader, i, &slot)) {
			result = -EIO;
			goto out;
		}
		if (le64_to_cpu(slot->key) != key)
			break;
		err = ext5_stable_reader_verify(&reader, slot, child, ino);
		if (!err) {
			ext5_stable_stats_rs_error(i > range->estimate ?
				i - range->estimate : range->estimate - i);
			result = 0;
			goto out;
		}
		if (err != -ENOENT) {
			result = err;
			goto out;
		}
	}
out:
	if (reader.count)
		ext5_stable_reader_release(&reader);
	return result;
}

static noinline int ext5_stable_lookup_rs(const struct ext5_stable_blob *blob,
					  u32 plid, u64 key,
					  const struct qstr *child, ino_t *ino)
{
	struct ext5_stable_lookup_range range;
	int err;

	err = ext5_stable_lookup_range(blob, plid, key, &range);
	if (err)
		return err;
	if (likely(blob->payload_blocks)) {
		err = ext5_stable_lookup_cached(blob, key, child, ino, &range);
		if (likely(err != -ERANGE))
			return err;
	}
	return ext5_stable_lookup_uncached(blob, key, child, ino, &range);
}

/* Owning subtree root, referenced; the caller must iput() it. */
struct inode *ext5_stable_get_subtree_root(struct inode *dir)
{
	struct ext5_inode_info *ei = EXT5_I(dir);
	struct inode *root, *cached;

	if (ei->i_flags & EXT5_LIDIR_ROOT_FL) {
		ihold(dir);
		return dir;
	}
	if (!(ei->i_flags & EXT5_LIDIR_INTERIOR_FL))
		return ERR_PTR(-EINVAL);

	/* igrab() fails on an inode being evicted, and inodes are freed via RCU. */
	rcu_read_lock();
	cached = rcu_dereference(ei->i_subtree_root_inode);
	if (cached && igrab(cached)) {
		rcu_read_unlock();
		return cached;
	}
	rcu_read_unlock();

	if (!ei->i_subtree_root_ino)
		return ERR_PTR(-EIO);

	/* Install under i_li_lock; recheck, another thread may have won. */
	mutex_lock(&ei->i_li_lock);
	cached = rcu_dereference_protected(ei->i_subtree_root_inode,
					   lockdep_is_held(&ei->i_li_lock));
	if (cached && igrab(cached)) {
		mutex_unlock(&ei->i_li_lock);
		return cached;
	}

	root = ext5_iget(dir->i_sb, ei->i_subtree_root_ino,
			 EXT5_IGET_NORMAL);
	if (IS_ERR(root)) {
		mutex_unlock(&ei->i_li_lock);
		return root;
	}
	if (!(EXT5_I(root)->i_flags & EXT5_LIDIR_ROOT_FL)) {
		mutex_unlock(&ei->i_li_lock);
		iput(root);
		return ERR_PTR(-EIO);
	}

	/* The cache keeps the iget reference; ihold one for the caller. */
	ihold(root);
	rcu_assign_pointer(ei->i_subtree_root_inode, root);
	mutex_unlock(&ei->i_li_lock);
	return root;
}

/* VFS holds the parent across lookup, and demotion needs it exclusively
 * to clear the cached root, so the cache's reference can be borrowed. */
static struct inode *ext5_stable_lookup_root(struct inode *dir)
{
	struct ext5_inode_info *ei = EXT5_I(dir);
	struct inode *root;

	if (ei->i_flags & EXT5_LIDIR_ROOT_FL)
		return dir;
	rcu_read_lock();
	root = rcu_dereference(ei->i_subtree_root_inode);
	rcu_read_unlock();
	if (likely(root))
		return root;

	root = ext5_stable_get_subtree_root(dir);
	if (IS_ERR(root))
		return root;
	iput(root);
	rcu_read_lock();
	root = rcu_dereference(ei->i_subtree_root_inode);
	rcu_read_unlock();
	return root ? root : ERR_PTR(-EIO);
}

int ext5_stable_inode_by_name(struct inode *dir, const struct qstr *child,
			       ino_t *ino)
{
	struct inode *root;
	struct ext5_stable_blob *blob;
	u32 parent_local_id;
	u32 hash32;
	u64 key;
	int err = -ENOENT;
	int srcu_idx;
	bool rcu_lookup = false;
	u32 reparse_attempts = 0;

	if (child->len > EXT5_NAME_LEN)
		return -ENOENT;

	root = ext5_stable_lookup_root(dir);
	if (IS_ERR(root))
		return PTR_ERR(root);
	ext5_li_note_stable_access(root);

	parent_local_id = ext5_stable_dir_plid(dir);

retry_blob:
	/* A fully pinned payload never sleeps, so plain RCU suffices; otherwise
	 * take the SRCU path. */
	rcu_read_lock();
	blob = rcu_dereference(EXT5_I(root)->i_stable_blob);
	if (unlikely(!blob)) {
		rcu_read_unlock();
		err = ext5_stable_ensure_blob(root);
		if (err)
			goto out_root;
		if (reparse_attempts++ < 2)
			goto retry_blob;
		err = -EIO;
		goto out_root;
	}
	if (likely(blob->payload_blocks)) {
		rcu_lookup = true;
	} else {
		rcu_read_unlock();
		srcu_idx = srcu_read_lock(&ext5_li_lookup_srcu);
		blob = srcu_dereference(EXT5_I(root)->i_stable_blob,
					&ext5_li_lookup_srcu);
	}
	if (!blob) {
		srcu_read_unlock(&ext5_li_lookup_srcu, srcu_idx);
		if (reparse_attempts++ < 2) {
			err = ext5_stable_ensure_blob(root);
			if (err)
				goto out_root;
			goto retry_blob;
		}
		err = -EIO;
		goto out_root;
	}
	{
		hash32 = ext5_stable_hash32(blob, child->name, child->len);
		key = ext5_stable_make_key(parent_local_id, hash32);

		ext5_stable_stats_lookup();

		/* The delta overrides the base: INSERT gives the inode, DELETE -ENOENT. */
		if (blob->delta.nr_records) {
			struct ext5_stable_delta_entry *de;
			u64 tb = ext5_stable_tiebreak64(blob, child->name,
							 child->len, hash32);

			spin_lock(&blob->delta.lock);
			de = ext5_stable_delta_find(&blob->delta,
				parent_local_id, hash32, tb,
				child->name, child->len);
			if (de) {
				if (de->op == LIDIR_DELTA_INSERT) {
					*ino = de->ino;
					spin_unlock(&blob->delta.lock);
					err = 0;
					goto out_blob;
				}
				/* DELETE tombstone */
				spin_unlock(&blob->delta.lock);
				err = -ENOENT;
				goto out_blob;
			}
			spin_unlock(&blob->delta.lock);
		}
	}

	{
		bool filter_maybe;

		if (READ_ONCE(ext5_li_bloom_enabled))
			err = ext5_stable_filter_maybe(blob, key, &filter_maybe);
		else {
			err = 0;
			filter_maybe = true;
		}
		if (err) {
			goto out_blob;
		}
		if (!filter_maybe) {
			ext5_stable_stats_filter_negative();
			err = -ENOENT;
			goto out_blob;
		}
	}
	err = ext5_stable_lookup_rs(blob, parent_local_id, key, child, ino);
out_blob:
	if (rcu_lookup)
		rcu_read_unlock();
	else
		srcu_read_unlock(&ext5_li_lookup_srcu, srcu_idx);
	if (!err)
		ext5_stable_stats_hit();
	else if (err == -ENOENT)
		ext5_stable_stats_miss();
out_root:
	return err;
}

int ext5_stable_empty_dir(struct inode *inode)
{
	struct inode *root;
	struct ext5_stable_blob *blob;
	struct ext5_stable_delta_entry *de;
	u32 plid;
	int empty = 0;
	u32 lo, hi;
	u32 deletes = 0;
	u32 i;

	root = ext5_stable_get_subtree_root(inode);
	if (IS_ERR(root))
		return 0;	/* conservative: not empty -> rmdir refuses */

	plid = ext5_stable_dir_plid(inode);
	if (ext5_stable_ensure_blob(root)) {
		iput(root);
		return 0;
	}

	rcu_read_lock();
	blob = rcu_dereference(EXT5_I(root)->i_stable_blob);
	if (!blob || !refcount_inc_not_zero(&blob->refs)) {
		rcu_read_unlock();
		iput(root);
		return 0;
	}
	rcu_read_unlock();
	if (ext5_stable_parent_range(blob, plid, &lo, &hi) ||
	    lo > hi || hi > blob->entry_count)
		goto out;

	/* Count DELETEs under the spinlock; drop it before payload reads sleep. */
	spin_lock(&blob->delta.lock);
	de = ext5_stable_delta_lower_bound(&blob->delta, plid, 0, 0);
	while (de && de->plid != plid)
		de = rb_entry_safe(rb_next(&de->rb),
				   struct ext5_stable_delta_entry, rb);
	while (de && de->plid == plid) {
		if (de->op == LIDIR_DELTA_INSERT) {
			spin_unlock(&blob->delta.lock);
			goto out;
		}
		if (de->op == LIDIR_DELTA_DELETE)
			deletes++;
		de = rb_entry_safe(rb_next(&de->rb),
				   struct ext5_stable_delta_entry, rb);
	}
	spin_unlock(&blob->delta.lock);

	if (lo == hi) {
		empty = 1;
		goto out;
	}
	if (deletes < hi - lo)
		goto out;

	/* Every base name needs its own exact tombstone: a count alone cannot
	 * prove emptiness. */
	for (i = lo; i < hi; i++) {
		struct lidir_disk_slot slot;
		char name[EXT5_NAME_LEN];
		u32 name_off;
		u8 name_len;
		u32 hash32;
		u64 tiebreak64;
		bool deleted;

		if (ext5_stable_read_slot(blob, i, &slot))
			goto out;
		if ((u32)(le64_to_cpu(slot.key) >> 32) != plid)
			goto out;
		name_off = le32_to_cpu(slot.name_off);
		name_len = slot.name_len;
		if (!name_len || name_off > blob->names_bytes ||
		    name_len > blob->names_bytes - name_off)
			goto out;
		if (ext5_stable_read_name(blob, name_off, name_len, name))
			goto out;
		hash32 = (u32)le64_to_cpu(slot.key);
		tiebreak64 = ext5_stable_tiebreak64(blob, name, name_len,
						 hash32);
		spin_lock(&blob->delta.lock);
		de = ext5_stable_delta_find(&blob->delta, plid, hash32,
					    tiebreak64, name, name_len);
		deleted = de && de->op == LIDIR_DELTA_DELETE;
		spin_unlock(&blob->delta.lock);
		if (!deleted)
			goto out;
	}
	empty = 1;
out:
	ext5_stable_blob_put(blob);
	iput(root);
	return empty;
}

/*
 * Readdir cookie: 0 is ".", 1 "..", 2 before the first entry; otherwise
 * bit 63 = 0, bits 62..31 = hash32, bits 30..0 = high32(tiebreak64) >> 1.
 * Cookie order equals slot order within a PLID and depends only on the name
 * and the blob's hash key, so a saved cookie survives compaction.  Slots that
 * differ only in the dropped bit share a cursor; the merge tolerates the
 * re-emit.
 */
#define EXT5_COOKIE_DOT		((loff_t)0)
#define EXT5_COOKIE_DOTDOT	((loff_t)1)
#define EXT5_COOKIE_FIRST_REAL	((loff_t)2)

static loff_t ext5_cookie_encode(u32 hash32, u64 tiebreak64)
{
	u64 v;

	v = ((u64)hash32 << 31) |
	    ((tiebreak64 >> 33) & 0x7FFFFFFFULL);
	if (unlikely(v > S64_MAX - EXT5_COOKIE_FIRST_REAL - 1))
		v = S64_MAX - EXT5_COOKIE_FIRST_REAL - 1;
	v += EXT5_COOKIE_FIRST_REAL;
	return (loff_t)v;
}

/* Inverse of encode; the dropped low bit comes back as 0. */
static void ext5_cookie_decode(loff_t cookie, u32 *hash32_out,
			       u64 *tiebreak64_out)
{
	u64 c = (u64)cookie;
	u64 v, tb_hi;
	u32 h;

	if (c < EXT5_COOKIE_FIRST_REAL) {
		*hash32_out = 0;
		*tiebreak64_out = 0;
		return;
	}
	v = c - EXT5_COOKIE_FIRST_REAL;
	h = (u32)(v >> 31);
	tb_hi = (v & 0x7FFFFFFFULL) << 1;
	*hash32_out = h;
	/* Reconstruct tb: low32 == hash32 (by construction), high32 == tb_hi */
	*tiebreak64_out = (tb_hi << 32) | (u64)h;
}

/* Lower bound on a (hash32, tiebreak64) cursor within one PLID's slots. */
static u32 ext5_stable_base_lower_bound(const struct ext5_stable_blob *blob,
					u32 slot_lo, u32 slot_hi,
					u32 cur_hash, u64 cur_tb)
{
	u32 lo = slot_lo, hi = slot_hi;

	while (lo < hi) {
		u32 mid = lo + (hi - lo) / 2;
		struct lidir_disk_slot s;
		u32 sh;
		u64 stb;
		u32 name_off;
		u8 name_len;
		char namebuf[EXT5_NAME_LEN];

		if (ext5_stable_read_slot(blob, mid, &s))
			return slot_hi;	/* I/O error: the caller returns -EIO */
		sh = (u32)le64_to_cpu(s.key);
		name_off = le32_to_cpu(s.name_off);
		name_len = s.name_len;

		if (name_off > blob->names_bytes ||
		    name_len > blob->names_bytes - name_off)
			return slot_hi;	/* corruption: the caller returns -EIO */

		if (sh < cur_hash) {
			lo = mid + 1;
			continue;
		}
		if (sh > cur_hash) {
			hi = mid;
			continue;
		}
		if (ext5_stable_read_name(blob, name_off, name_len, namebuf))
			return slot_hi;
		stb = ext5_stable_tiebreak64(blob, namebuf, name_len, sh);
		if (stb < cur_tb)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* Delta entry copied out for readdir: base reads sleep, so the merge
 * cannot run under the delta spinlock. */
struct ext5_rd_snap {
	u32 hash32;
	u64 tiebreak64;
	u32 ino;
	u8 op;
	u8 file_type;
	u8 name_len;
	char name[EXT5_NAME_LEN];
};

int ext5_stable_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	struct inode *root;
	struct ext5_stable_blob *blob;
	const bool has_filetype = true;
	u32 plid, slot_lo, slot_hi;
	u32 cur_hash;
	u64 cur_tb;
	struct ext5_stable_delta_entry *d;
	u32 b;
	int ret = 0;
	struct ext5_rd_snap *snap = NULL;
	u32 nsnap = 0, si = 0;

	ext5_stable_stats_readdir();

	root = ext5_stable_get_subtree_root(inode);
	if (IS_ERR(root))
		return PTR_ERR(root);
	ext5_li_note_stable_access(root);

	plid = ext5_stable_dir_plid(inode);

	ret = ext5_stable_ensure_blob(root);
	if (ret) {
		iput(root);
		return ret;
	}

	/* Take a reference and leave RCU: dir_emit() can fault and sleep. */
	rcu_read_lock();
	blob = rcu_dereference(EXT5_I(root)->i_stable_blob);
	if (!blob || !refcount_inc_not_zero(&blob->refs)) {
		rcu_read_unlock();
		iput(root);
		return -EIO;
	}
	rcu_read_unlock();

	if (plid >= blob->parent_count) {
		ret = -EIO;
		goto out;
	}
	ret = ext5_stable_parent_range(blob, plid, &slot_lo, &slot_hi);
	if (ret)
		goto out;
	if (slot_hi < slot_lo || slot_hi > blob->entry_count) {
		ret = -EIO;
		goto out;
	}

	if (ctx->pos == EXT5_COOKIE_DOT) {
		if (!dir_emit_dot(file, ctx))
			goto out;
		ctx->pos = EXT5_COOKIE_DOTDOT;
	}
	if (ctx->pos == EXT5_COOKIE_DOTDOT) {
		if (!dir_emit_dotdot(file, ctx))
			goto out;
		ctx->pos = EXT5_COOKIE_FIRST_REAL;
	}

	ext5_cookie_decode(ctx->pos, &cur_hash, &cur_tb);

	/* Lower-bound both sides on the cursor. */
	b = ext5_stable_base_lower_bound(blob, slot_lo, slot_hi,
					 cur_hash, cur_tb);

	/* Snapshot this PLID's delta under the root lifecycle mutex, which append
	 * and compaction also take, so no concurrent create is missed. */
	mutex_lock(&EXT5_I(root)->i_li_lock);
	spin_lock(&blob->delta.lock);
	{
		struct ext5_stable_delta_entry *c;
		u32 cap = 0;

		c = ext5_stable_delta_lower_bound(&blob->delta, plid,
						  cur_hash, cur_tb);
		while (c && c->plid != plid)
			c = rb_entry_safe(rb_next(&c->rb),
					  struct ext5_stable_delta_entry, rb);
		while (c && c->plid == plid) {
			cap++;
			c = rb_entry_safe(rb_next(&c->rb),
					  struct ext5_stable_delta_entry, rb);
		}
		if (cap) {
			spin_unlock(&blob->delta.lock);
			snap = kvmalloc_array(cap, sizeof(*snap), GFP_KERNEL);
			if (!snap) {
				ret = -ENOMEM;
				mutex_unlock(&EXT5_I(root)->i_li_lock);
				goto out;
			}
			spin_lock(&blob->delta.lock);
			d = ext5_stable_delta_lower_bound(&blob->delta, plid,
							  cur_hash, cur_tb);
			while (d && d->plid != plid)
				d = rb_entry_safe(rb_next(&d->rb),
					struct ext5_stable_delta_entry, rb);
			while (d && d->plid == plid && nsnap < cap) {
				struct ext5_rd_snap *e = &snap[nsnap++];

				e->hash32 = d->hash32;
				e->tiebreak64 = d->tiebreak64;
				e->ino = d->ino;
				e->op = d->op;
				e->file_type = d->file_type;
				e->name_len = d->name_len;
				memcpy(e->name, d->name, d->name_len);
				d = rb_entry_safe(rb_next(&d->rb),
					struct ext5_stable_delta_entry, rb);
			}
		}
	}
	spin_unlock(&blob->delta.lock);
	mutex_unlock(&EXT5_I(root)->i_li_lock);

	/* Two-pointer merge: base [b, slot_hi) x snapshot [si, nsnap). */
	while (b < slot_hi || si < nsnap) {
		struct lidir_disk_slot bs;
		char b_namebuf[EXT5_NAME_LEN];
		u32 b_hash = 0;
		u64 b_tb = 0;
		u8 b_name_len = 0;
		bool have_base = false;
		struct ext5_rd_snap *de = (si < nsnap) ? &snap[si] : NULL;
		int cmp;

		if (b < slot_hi) {
			u32 b_name_off;

			if (ext5_stable_read_slot(blob, b, &bs)) {
				ret = -EIO;
				goto out;
			}
			b_name_off = le32_to_cpu(bs.name_off);
			b_name_len = bs.name_len;
			if (b_name_off > blob->names_bytes ||
			    b_name_len > blob->names_bytes - b_name_off) {
				ret = -EIO;
				goto out;
			}
			if (ext5_stable_read_name(blob, b_name_off, b_name_len,
						  b_namebuf)) {
				ret = -EIO;
				goto out;
			}
			b_hash = (u32)le64_to_cpu(bs.key);
			b_tb = ext5_stable_tiebreak64(blob, b_namebuf, b_name_len,
						    b_hash);
			have_base = true;
		}

		if (have_base && de) {
			if (b_hash != de->hash32)
				cmp = b_hash < de->hash32 ? -1 : 1;
			else if (b_tb != de->tiebreak64)
				cmp = b_tb < de->tiebreak64 ? -1 : 1;
			else if (b_name_len != de->name_len) {
				int c = memcmp(b_namebuf, de->name,
					min_t(u8, b_name_len, de->name_len));
				cmp = c ? c :
				      (b_name_len < de->name_len ? -1 : 1);
			} else {
				cmp = memcmp(b_namebuf, de->name, b_name_len);
			}
		} else if (have_base) {
			cmp = -1;
		} else {
			cmp = 1;
		}

		if (cmp == 0) {
			/* Same name in base and delta: delta wins. */
			if (de->op == LIDIR_DELTA_INSERT) {
				unsigned char dt = has_filetype ?
					de->file_type : DT_UNKNOWN;
				loff_t cookie = ext5_cookie_encode(de->hash32,
								 de->tiebreak64);

				ctx->pos = cookie;
				if (!dir_emit(ctx, de->name, de->name_len,
					      de->ino, dt))
					goto out;
				ctx->pos = cookie + 1;
			}
			si++;
			b++;
		} else if (cmp < 0) {
			/* Base entry comes first. */
			unsigned char dt = has_filetype ?
				bs.file_type : DT_UNKNOWN;
			loff_t cookie = ext5_cookie_encode(b_hash, b_tb);

			ctx->pos = cookie;
			if (!dir_emit(ctx, b_namebuf, b_name_len,
				      le32_to_cpu(bs.ino), dt))
				goto out;
			ctx->pos = cookie + 1;
			b++;
		} else {
			/* Delta entry comes first. */
			if (de->op == LIDIR_DELTA_INSERT) {
				unsigned char dt = has_filetype ?
					de->file_type : DT_UNKNOWN;
				loff_t cookie = ext5_cookie_encode(de->hash32,
								 de->tiebreak64);

				ctx->pos = cookie;
				if (!dir_emit(ctx, de->name, de->name_len,
					      de->ino, dt))
					goto out;
				ctx->pos = cookie + 1;
			}
			si++;
		}
	}
out:
	kvfree(snap);
	ext5_stable_blob_put(blob);
	iput(root);
	return ret;
}

/* memcmp-style order on (plid, hash32, tiebreak64, name). */
static int delta_entry_cmp(const struct ext5_stable_delta_entry *a,
			   const struct ext5_stable_delta_entry *b)
{
	if (a->plid != b->plid)
		return a->plid < b->plid ? -1 : 1;
	if (a->hash32 != b->hash32)
		return a->hash32 < b->hash32 ? -1 : 1;
	if (a->tiebreak64 != b->tiebreak64)
		return a->tiebreak64 < b->tiebreak64 ? -1 : 1;
	if (a->name_len != b->name_len) {
		int c = memcmp(a->name, b->name,
			       min_t(u8, a->name_len, b->name_len));
		if (c)
			return c;
		return a->name_len < b->name_len ? -1 : 1;
	}
	return memcmp(a->name, b->name, a->name_len);
}

/* The same order, against a probe key. */
static int delta_probe_cmp(const struct ext5_stable_delta_entry *a,
			   u32 plid, u32 hash32, u64 tiebreak64,
			   const char *name, u8 name_len)
{
	if (a->plid != plid)
		return a->plid < plid ? -1 : 1;
	if (a->hash32 != hash32)
		return a->hash32 < hash32 ? -1 : 1;
	if (a->tiebreak64 != tiebreak64)
		return a->tiebreak64 < tiebreak64 ? -1 : 1;
	if (a->name_len != name_len) {
		int c = memcmp(a->name, name,
			       min_t(u8, a->name_len, name_len));
		if (c)
			return c;
		return a->name_len < name_len ? -1 : 1;
	}
	return memcmp(a->name, name, name_len);
}

static void ext5_stable_delta_init(struct ext5_stable_delta *d,
				   u64 region_off, u64 region_bytes)
{
	spin_lock_init(&d->lock);
	d->by_key = RB_ROOT_CACHED;
	d->presence_filter = NULL;
	d->presence_filter_bits = 0;
	d->nr_inserts = 0;
	d->nr_deletes = 0;
	d->nr_records = 0;
	d->appended_inserts = 0;
	d->appended_deletes = 0;
	d->region_off = region_off;
	d->region_bytes = region_bytes;
	d->used_bytes = 0;
	d->seq_base = 0;
	d->next_seq = 1;
	d->snapshot_floor_bytes = 0;
}

static u64 ext5_stable_delta_trigger(struct super_block *sb, u32 entries,
				    u32 names_bytes)
{
	u64 override = READ_ONCE(ext5_li_delta_trigger_bytes);
	u64 average_name = entries ? names_bytes / entries : 16;
	u64 record_bytes;
	u64 records, max_records;
	u64 bytes, rate_records, rate_bytes;

	if (override)
		return clamp_t(u64, override, 64U * 1024U,
			       8U * 1024U * 1024U);
	average_name = min_t(u64, average_name, EXT5_NAME_LEN);
	record_bytes = ALIGN(LIDIR_DELTA_HEADER_BYTES + average_name, 8);
	max_records = (8U * 1024U * 1024U) / record_bytes;
	/* Refill at N/8 records: a rebuild scans the whole base, so a smaller
	 * threshold charges each mutation too much. */
	records = clamp_t(u64, max_t(u64, entries / 8, 1),
			      1024, max_records);
	bytes = records * record_bytes;
	bytes = clamp_t(u64, bytes, 256U * 1024U, 8U * 1024U * 1024U);
	/* Cap the refill at half of T* records, so one burst can still reach it. */
	rate_records = ext5_li_t_star_for_entries(sb, entries) / 2;
	rate_records = clamp_t(u64, rate_records, 1024,
			       (8U * 1024U * 1024U) / record_bytes);
	rate_bytes = rate_records * record_bytes;
	rate_bytes = max_t(u64, rate_bytes, 64U * 1024U);
	return min(bytes, rate_bytes);
}

static void ext5_stable_delta_filter_alloc(struct ext5_stable_delta *d)
{
	u64 expected_records;
	unsigned long bits;

	if (!d->trigger_bytes || d->presence_filter)
		return;
	/* Delta Bloom filter: 8 bits per expected record, 3 probes.  On allocation
	 * failure the RB-tree alone answers. */
	expected_records = clamp_t(u64, d->trigger_bytes / 64, 512, 524288);
	bits = roundup_pow_of_two(expected_records * 8);
	bits = clamp_t(unsigned long, bits, 4096, 4194304);
	d->presence_filter = bitmap_zalloc(bits,
					    GFP_KERNEL_ACCOUNT | __GFP_NOWARN);
	if (d->presence_filter)
		d->presence_filter_bits = bits;
}

static void ext5_stable_delta_filter_hashes(const struct ext5_stable_delta *d,
					    u32 plid, u32 hash32,
					    u64 tiebreak64, u32 indexes[3])
{
	u64 first = tiebreak64 ^ ((u64)plid << 32) ^ hash32;
	u64 step = rol64(tiebreak64, 23) ^ ((u64)hash32 << 1) ^
		0x9e3779b97f4a7c15ULL;
	unsigned int order = ilog2(d->presence_filter_bits);

	indexes[0] = hash_64(first, order);
	indexes[1] = hash_64(first + step, order);
	indexes[2] = hash_64(first + step * 2, order);
}

static void ext5_stable_delta_filter_add(struct ext5_stable_delta *d,
					 u32 plid, u32 hash32,
					 u64 tiebreak64)
{
	u32 indexes[3];

	if (!d->presence_filter)
		return;
	ext5_stable_delta_filter_hashes(d, plid, hash32, tiebreak64, indexes);
	__set_bit(indexes[0], d->presence_filter);
	__set_bit(indexes[1], d->presence_filter);
	__set_bit(indexes[2], d->presence_filter);
}

static bool ext5_stable_delta_filter_maybe(const struct ext5_stable_delta *d,
					   u32 plid, u32 hash32,
					   u64 tiebreak64)
{
	u32 indexes[3];

	if (!d->presence_filter)
		return true;
	ext5_stable_delta_filter_hashes(d, plid, hash32, tiebreak64, indexes);
	return test_bit(indexes[0], d->presence_filter) &&
	       test_bit(indexes[1], d->presence_filter) &&
	       test_bit(indexes[2], d->presence_filter);
}

static void ext5_stable_delta_free(struct ext5_stable_delta *d)
{
	struct rb_node *node;

	while ((node = rb_first_cached(&d->by_key))) {
		struct ext5_stable_delta_entry *e =
			rb_entry(node, struct ext5_stable_delta_entry, rb);

		rb_erase_cached(&e->rb, &d->by_key);
		kfree(e);
	}
	bitmap_free(d->presence_filter);
	d->presence_filter = NULL;
	d->presence_filter_bits = 0;
}

/* Live delta entry for the key, or NULL.  Caller holds d->lock. */
static struct ext5_stable_delta_entry *
ext5_stable_delta_find(const struct ext5_stable_delta *d, u32 plid,
		       u32 hash32, u64 tiebreak64,
		       const char *name, u8 name_len)
{
	struct rb_node *node = d->by_key.rb_root.rb_node;

	if (!ext5_stable_delta_filter_maybe(d, plid, hash32, tiebreak64))
		return NULL;

	while (node) {
		struct ext5_stable_delta_entry *e =
			rb_entry(node, struct ext5_stable_delta_entry, rb);
		int c = delta_probe_cmp(e, plid, hash32, tiebreak64,
					name, name_len);

		if (c < 0)
			node = node->rb_right;
		else if (c > 0)
			node = node->rb_left;
		else
			return e;
	}
	return NULL;
}

/* Leftmost entry >= (plid, hash32, tiebreak64); a NULL name sorts first.
 * Caller holds d->lock. */
static struct ext5_stable_delta_entry *
ext5_stable_delta_lower_bound(struct ext5_stable_delta *d,
			      u32 plid, u32 hash32, u64 tiebreak64)
{
	struct rb_node *node = d->by_key.rb_root.rb_node;
	struct ext5_stable_delta_entry *best = NULL;

	while (node) {
		struct ext5_stable_delta_entry *e =
			rb_entry(node, struct ext5_stable_delta_entry, rb);
		int c;

		if (e->plid != plid)
			c = e->plid < plid ? -1 : 1;
		else if (e->hash32 != hash32)
			c = e->hash32 < hash32 ? -1 : 1;
		else if (e->tiebreak64 != tiebreak64)
			c = e->tiebreak64 < tiebreak64 ? -1 : 1;
		else
			c = 0;	/* exact key match -- treat as >= */

		if (c < 0) {
			node = node->rb_right;
		} else {
			best = e;
			node = node->rb_left;
		}
	}
	return best;
}

/*
 * Apply one record to the delta tree, for mutation and mount-time replay.
 * Caller holds d->lock.  A mutation passes a node reserved before journaling,
 * so a durable record never lacks its in-memory state.  INSERT replaces any
 * entry for the name; DELETE turns it into a tombstone or adds one.
 */
static int ext5_stable_delta_apply(struct ext5_stable_delta *d,
				   u8 op, u32 plid, u32 hash32,
				   u64 tiebreak64,
				   const char *name, u8 name_len,
				   u32 ino, u8 file_type,
				   struct ext5_stable_delta_entry **reserved)
{
	struct ext5_stable_delta_entry *e, *parent_entry;
	struct rb_node **link, *parent_node;
	bool leftmost = true;
	u8 prev_op;
	int c;

	link = &d->by_key.rb_root.rb_node;
	parent_node = NULL;
	while (*link) {
		parent_node = *link;
		parent_entry = rb_entry(parent_node,
				struct ext5_stable_delta_entry, rb);
		c = delta_probe_cmp(parent_entry, plid, hash32, tiebreak64,
				    name, name_len);
		if (c < 0) {
			link = &parent_node->rb_right;
			leftmost = false;
		} else if (c > 0) {
			link = &parent_node->rb_left;
		} else {
				/* Existing entry: replace in place. */
				if (reserved && *reserved) {
					kfree(*reserved);
					*reserved = NULL;
				}
			prev_op = parent_entry->op;
			parent_entry->op = op;
			parent_entry->ino = (op == LIDIR_DELTA_INSERT) ?
					    ino : 0;
			parent_entry->file_type = file_type;
			/* Replay and private copies never get tail-cancellation authority. */
			parent_entry->record_off = 0;
			parent_entry->record_seq = 0;
			parent_entry->record_bytes = 0;
			parent_entry->tail_cancellable = false;
			if (prev_op == LIDIR_DELTA_INSERT &&
			    op == LIDIR_DELTA_DELETE) {
				d->nr_inserts--;
				d->nr_deletes++;
			} else if (prev_op == LIDIR_DELTA_DELETE &&
				   op == LIDIR_DELTA_INSERT) {
				d->nr_deletes--;
				d->nr_inserts++;
			}
			if (op == LIDIR_DELTA_INSERT)
				d->appended_inserts++;
			else
				d->appended_deletes++;
			ext5_stable_delta_filter_add(d, plid, hash32, tiebreak64);
			return 0;
		}
	}

	if (reserved) {
		e = *reserved;
		*reserved = NULL;
	} else {
		e = kmalloc(sizeof(*e) + name_len, GFP_KERNEL);
	}
	if (!e)
		return -ENOMEM;
	e->name = (char *)(e + 1);
	memcpy(e->name, name, name_len);
	e->plid = plid;
	e->hash32 = hash32;
	e->tiebreak64 = tiebreak64;
	e->ino = (op == LIDIR_DELTA_INSERT) ? ino : 0;
	e->op = op;
	e->file_type = file_type;
	e->name_len = name_len;
	e->record_off = 0;
	e->record_seq = 0;
	e->record_bytes = 0;
	e->tail_cancellable = false;

	rb_link_node(&e->rb, parent_node, link);
	rb_insert_color_cached(&e->rb, &d->by_key, leftmost);
	ext5_stable_delta_filter_add(d, plid, hash32, tiebreak64);

	d->nr_records++;
	if (op == LIDIR_DELTA_INSERT)
		d->nr_inserts++;
	else
		d->nr_deletes++;
	if (op == LIDIR_DELTA_INSERT)
		d->appended_inserts++;
	else
		d->appended_deletes++;
	return 0;
}

/* Prepare every buffer before publishing a bounded metadata operation.
 * Allocation or journal-access failure therefore cannot expose a prefix.
 */
struct ext5_stable_write {
	struct buffer_head *bh[LIDIR_DESC_BYTES / 1024];
	u32 nr;
	u32 offset;
	u32 bytes;
};

static void ext5_stable_write_release(struct ext5_stable_write *w)
{
	u32 i;

	for (i = 0; i < w->nr; i++)
		brelse(w->bh[i]);
	w->nr = 0;
}

static int ext5_stable_write_prepare(handle_t *handle, struct inode *root,
				    u64 pos, u32 bytes,
				    struct ext5_stable_write *w)
{
	struct super_block *sb = root->i_sb;
	ext5_lblk_t first = pos >> sb->s_blocksize_bits;
	u32 count, i;
	int err;

	w->offset = pos & (sb->s_blocksize - 1);
	w->bytes = bytes;
	count = DIV_ROUND_UP(w->offset + bytes, sb->s_blocksize);
	if (WARN_ON_ONCE(count > ARRAY_SIZE(w->bh)))
		return -E2BIG;
	for (i = 0; i < count; i++) {
		struct buffer_head *bh = ext5_bread(handle, root, first + i,
						   EXT5_GET_BLOCKS_CREATE);

		if (IS_ERR(bh))
			return PTR_ERR(bh);
		if (!bh)
			return -EIO;
		w->bh[w->nr++] = bh;
		err = ext5_journal_get_write_access(handle, sb, bh, EXT5_JTR_NONE);
		if (err)
			return err;
	}
	return 0;
}

static void ext5_stable_write_copy(struct ext5_stable_write *w, const void *data)
{
	u32 i, remaining = w->bytes, off = w->offset;
	const u8 *src = data;

	for (i = 0; i < w->nr; i++) {
		u32 bytes = min_t(u32, remaining, w->bh[i]->b_size - off);

		memcpy(w->bh[i]->b_data + off, src, bytes);
		set_buffer_uptodate(w->bh[i]);
		src += bytes;
		remaining -= bytes;
		off = 0;
	}
}

static int ext5_stable_write_dirty(handle_t *handle, struct inode *root,
				  struct ext5_stable_write *w)
{
	u32 i;
	int err = 0;

	for (i = 0; i < w->nr; i++) {
		int ret = ext5_handle_dirty_metadata(handle, root, w->bh[i]);

		if (!err)
			err = ret;
	}
	return err;
}

/* Write one record (rec_bytes, 8-aligned, name included) at `pos`,
 * dirtying its blocks in the caller's transaction. */
static int ext5_stable_write_delta_bytes(handle_t *handle,
					 struct inode *root, u64 pos,
					 const u8 *src, u32 src_remaining)
{
	struct super_block *sb = root->i_sb;
	unsigned int bs = sb->s_blocksize;
	int err;
	u32 written = 0;

	while (src_remaining) {
		ext5_lblk_t blk = (pos + written) >> sb->s_blocksize_bits;
		u32 off = (u32)((pos + written) & (bs - 1));
		u32 this = min_t(u32, bs - off, src_remaining);
		struct buffer_head *bh;

		bh = ext5_bread(handle, root, blk, EXT5_GET_BLOCKS_CREATE);
		if (IS_ERR(bh)) {
			err = PTR_ERR(bh);
			return err;
		}
		if (!bh) {
			return -EIO;
		}
		err = ext5_journal_get_write_access(handle, sb, bh,
						    EXT5_JTR_NONE);
		if (err) {
			brelse(bh);
			return err;
		}
		memcpy(bh->b_data + off, src, this);
		set_buffer_uptodate(bh);
		err = ext5_handle_dirty_metadata(handle, root, bh);
		brelse(bh);
		if (err)
			return err;

		/* Grow root i_size if necessary. */
		if ((loff_t)((blk + 1) << sb->s_blocksize_bits) >
		    i_size_read(root)) {
			i_size_write(root,
				(loff_t)(blk + 1) << sb->s_blocksize_bits);
			EXT5_I(root)->i_disksize = i_size_read(root);
			ext5_mark_inode_dirty(handle, root);
		}

		src += this;
		written += this;
		src_remaining -= this;
	}
	return 0;
}

#define LIDIR_DELTA_MAX_RECORD_BYTES \
	ALIGN(LIDIR_DELTA_HEADER_BYTES + EXT5_NAME_LEN, 8)
#define LIDIR_DELTA_END_BYTES \
	(offsetof(struct lidir_disk_delta_record, op) + 1)

static int ext5_stable_write_delta_record(handle_t *handle,
					  struct inode *root, u64 pos,
					  const struct lidir_disk_delta_record *hdr,
					  const char *name, u8 name_len,
					  unsigned int rec_bytes,
					  bool write_end_marker)
{
	u8 staged[LIDIR_DELTA_MAX_RECORD_BYTES + LIDIR_DELTA_END_BYTES] = { 0 };
	unsigned int write_bytes = rec_bytes;

	if (write_end_marker)
		write_bytes += LIDIR_DELTA_END_BYTES;
	if (WARN_ON_ONCE(write_bytes > sizeof(staged)))
		return -E2BIG;
	memcpy(staged, hdr, LIDIR_DELTA_HEADER_BYTES);
	memcpy(staged + LIDIR_DELTA_HEADER_BYTES, name, name_len);
	return ext5_stable_write_delta_bytes(handle, root, pos, staged,
					      write_bytes);
}

/* Cancel the newest record by journaling a NOP op byte over it. */
static int ext5_stable_write_delta_nop(handle_t *handle, struct inode *root,
				       u64 record_pos)
{
	struct super_block *sb = root->i_sb;
	u64 pos = record_pos + offsetof(struct lidir_disk_delta_record, op);
	ext5_lblk_t blk = pos >> sb->s_blocksize_bits;
	u32 off = (u32)(pos & (sb->s_blocksize - 1));
	struct buffer_head *bh;
	int err;

	bh = ext5_bread(handle, root, blk, 0);
	if (IS_ERR(bh))
		return PTR_ERR(bh);
	if (!bh)
		return -EIO;
	err = ext5_journal_get_write_access(handle, sb, bh, EXT5_JTR_NONE);
	if (!err) {
		bh->b_data[off] = LIDIR_DELTA_NOP;
		set_buffer_uptodate(bh);
		err = ext5_handle_dirty_metadata(handle, root, bh);
	}
	brelse(bh);
	return err;
}

/*
 * Append one delta record.  Caller holds the VFS lock on the directory;
 * appends to one root serialize on its i_li_lock.  An adjacent inverse
 * mutation may rewind the tail.
 */
static int ext5_stable_append_delta(handle_t *handle, struct inode *root, u8 op,
				    u32 parent_local_id, u32 hash32,
				    u64 tiebreak64,
				    const char *name, u8 name_len,
				    u32 ino, u8 file_type,
				    bool destination_was_absent)
{
	struct ext5_inode_info *root_ei = EXT5_I(root);
	struct ext5_stable_blob *blob;
	struct lidir_disk_delta_record hdr = {0};
	unsigned int rec_bytes = lidir_delta_record_bytes(name_len);
	u64 pos;
	u64 seq;
	u32 csum;
	int err;
	u32 reparse_attempts = 0;
	bool had_prior = false;
	struct ext5_stable_delta_entry *reserved = NULL;

	if (WARN_ON_ONCE(!handle))
		return -EINVAL;

retry:
	mutex_lock(&root_ei->i_li_lock);
	blob = rcu_dereference_protected(root_ei->i_stable_blob,
					 lockdep_is_held(&root_ei->i_li_lock));
	if (!blob) {
		mutex_unlock(&root_ei->i_li_lock);
		if (reparse_attempts++ >= 8)
			return -EIO;
		err = ext5_stable_ensure_blob(root);
		if (err)
			return err;
		goto retry;
	}

	/* An inverse DELETE may cancel the adjacent INSERT of an absent name: the
	 * root mutex excludes other appends and compaction, and the NOP commits
	 * with the link-count change. */
	if (op == LIDIR_DELTA_DELETE) {
		struct ext5_stable_delta_entry *entry;
		u64 cancel_off = 0, cancel_seq = 0;
		u16 cancel_bytes = 0;

		spin_lock(&blob->delta.lock);
		entry = ext5_stable_delta_find(&blob->delta, parent_local_id,
			hash32, tiebreak64, name, name_len);
		if (entry && entry->op == LIDIR_DELTA_INSERT &&
		    entry->tail_cancellable && entry->record_bytes &&
		    entry->record_off >= blob->delta.snapshot_floor_bytes &&
		    entry->record_off + entry->record_bytes ==
			blob->delta.used_bytes &&
		    entry->record_seq + 1 == blob->delta.next_seq) {
			cancel_off = entry->record_off;
			cancel_seq = entry->record_seq;
			cancel_bytes = entry->record_bytes;
		}
		spin_unlock(&blob->delta.lock);

		if (cancel_bytes) {
			err = ext5_stable_write_delta_nop(handle, root,
				blob->delta.region_off + cancel_off);
			if (err) {
				mutex_unlock(&root_ei->i_li_lock);
				return err;
			}

			spin_lock(&blob->delta.lock);
			entry = ext5_stable_delta_find(&blob->delta,
				parent_local_id, hash32, tiebreak64, name, name_len);
			if (WARN_ON_ONCE(!entry ||
			    entry->record_off != cancel_off ||
			    entry->record_seq != cancel_seq ||
			    entry->record_bytes != cancel_bytes)) {
				spin_unlock(&blob->delta.lock);
				mutex_unlock(&root_ei->i_li_lock);
				return -EIO;
			}
			rb_erase_cached(&entry->rb, &blob->delta.by_key);
			blob->delta.nr_records--;
			blob->delta.nr_inserts--;
			blob->delta.appended_inserts--;
			blob->delta.used_bytes = cancel_off;
			blob->delta.next_seq = cancel_seq;
			spin_unlock(&blob->delta.lock);
			kfree(entry);
			WRITE_ONCE(root_ei->i_li_last_delta_jiffies, jiffies);
			mutex_unlock(&root_ei->i_li_lock);
			return 0;
		}
	}

	if (!blob->delta.region_bytes ||
	    blob->delta.used_bytes + rec_bytes > blob->delta.region_bytes) {
		mutex_unlock(&root_ei->i_li_lock);
		return -ENOSPC;
	}

	pos = blob->delta.region_off + blob->delta.used_bytes;
	seq = blob->delta.next_seq;
	{
		struct ext5_stable_delta_entry *entry;
		bool need_new;

		spin_lock(&blob->delta.lock);
		entry = ext5_stable_delta_find(&blob->delta,
			parent_local_id, hash32, tiebreak64, name, name_len);
		need_new = !entry;
		if (op == LIDIR_DELTA_INSERT)
			had_prior = !need_new;
		spin_unlock(&blob->delta.lock);
		if (need_new) {
			reserved = kmalloc(sizeof(*reserved) + name_len, GFP_KERNEL);
			if (!reserved) {
				mutex_unlock(&root_ei->i_li_lock);
				return -ENOMEM;
			}
		}
	}

	hdr.seq = cpu_to_le64(seq);
	hdr.op = op;
	hdr.file_type = file_type;
	hdr.name_len = name_len;
	hdr.parent_local_id = cpu_to_le32(parent_local_id);
	hdr.key32 = cpu_to_le32(hash32);
	hdr.ino = cpu_to_le32(ino);

	/* csum covers [op .. trailing _pad2] AND name bytes */
	csum = crc32_le(0,
		(const u8 *)&hdr + offsetof(struct lidir_disk_delta_record, op),
		LIDIR_DELTA_HEADER_BYTES -
			offsetof(struct lidir_disk_delta_record, op));
	csum = crc32_le(csum, name, name_len);
	hdr.csum = cpu_to_le32(csum);

	err = ext5_stable_write_delta_record(handle, root, pos, &hdr, name,
					     name_len, rec_bytes,
			blob->delta.used_bytes + rec_bytes +
				LIDIR_DELTA_END_BYTES <=
				blob->delta.region_bytes);
	if (err) {
		kfree(reserved);
		mutex_unlock(&root_ei->i_li_lock);
		return err;
	}

	spin_lock(&blob->delta.lock);
	err = ext5_stable_delta_apply(&blob->delta, op,
				      parent_local_id, hash32, tiebreak64,
				      name, name_len, ino, file_type, &reserved);
	if (!err) {
		struct ext5_stable_delta_entry *entry;

		entry = ext5_stable_delta_find(&blob->delta, parent_local_id,
			hash32, tiebreak64, name, name_len);
		if (WARN_ON_ONCE(!entry)) {
			err = -EIO;
		} else {
			entry->record_off = blob->delta.used_bytes;
			entry->record_seq = seq;
			entry->record_bytes = rec_bytes;
			entry->tail_cancellable =
				op == LIDIR_DELTA_INSERT &&
				destination_was_absent && !had_prior;
		}
	}
	if (!err) {
		blob->delta.used_bytes += rec_bytes;
		blob->delta.next_seq = seq + 1;
	}
	{
		bool over_watermark = blob->delta.trigger_bytes &&
			blob->delta.used_bytes >= blob->delta.trigger_bytes;
		bool urgent = blob->delta.region_bytes &&
			blob->delta.used_bytes >= blob->delta.region_bytes -
				blob->delta.region_bytes / 4;
		u64 generation = blob->generation;

		spin_unlock(&blob->delta.lock);
		if (!err)
			WRITE_ONCE(root_ei->i_li_last_delta_jiffies, jiffies);
		if (!err && over_watermark &&
		    !atomic_read(&root_ei->i_li_demote_pending))
			ext5_stable_queue_compact(root, generation, urgent);
	}
	kfree(reserved);

	mutex_unlock(&root_ei->i_li_lock);
	return err;
}

/* INSERT (plid, name) -> ino.  Caller holds parent_dir's lock; the record
 * commits in `handle` with the inode's link metadata. */
int ext5_stable_add_link(handle_t *handle, struct inode *parent_dir,
			 const struct qstr *name,
			 struct inode *child,
			 bool destination_was_absent)
{
	struct inode *root;
	struct ext5_stable_blob *blob;
	u32 plid;
	u32 hash32;
	u64 tb;
	u8 file_type;
	int err;

	if (name->len > EXT5_NAME_LEN || name->len == 0)
		return -ENAMETOOLONG;

	root = ext5_stable_get_subtree_root(parent_dir);
	if (IS_ERR(root))
		return PTR_ERR(root);

	plid = ext5_stable_dir_plid(parent_dir);

	err = ext5_stable_ensure_blob(root);
	if (err) {
		iput(root);
		return err;
	}

	rcu_read_lock();
	blob = rcu_dereference(EXT5_I(root)->i_stable_blob);
	if (!blob) {
		rcu_read_unlock();
		iput(root);
		return -EIO;
	}
	hash32 = ext5_stable_hash32(blob, name->name, name->len);
	tb = ext5_stable_tiebreak64(blob, name->name, name->len, hash32);
	rcu_read_unlock();

	file_type = fs_umode_to_dtype(child->i_mode);

	err = ext5_stable_append_delta(handle, root, LIDIR_DELTA_INSERT,
				       plid, hash32, tb,
				       name->name, name->len,
				       (u32)child->i_ino, file_type,
				       destination_was_absent);
	iput(root);
	return err;
}

/* DELETE (plid, name); the name must exist. */
int ext5_stable_delete_name(handle_t *handle, struct inode *parent_dir,
			    const struct qstr *name)
{
	struct inode *root;
	struct ext5_stable_blob *blob;
	u32 plid;
	u32 hash32;
	u64 tb;
	int err;

	if (name->len > EXT5_NAME_LEN || name->len == 0)
		return -ENOENT;

	root = ext5_stable_get_subtree_root(parent_dir);
	if (IS_ERR(root))
		return PTR_ERR(root);

	plid = ext5_stable_dir_plid(parent_dir);

	err = ext5_stable_ensure_blob(root);
	if (err) {
		iput(root);
		return err;
	}

	rcu_read_lock();
	blob = rcu_dereference(EXT5_I(root)->i_stable_blob);
	if (!blob) {
		rcu_read_unlock();
		iput(root);
		return -EIO;
	}
	hash32 = ext5_stable_hash32(blob, name->name, name->len);
	tb = ext5_stable_tiebreak64(blob, name->name, name->len, hash32);
	rcu_read_unlock();

	err = ext5_stable_append_delta(handle, root, LIDIR_DELTA_DELETE,
				       plid, hash32, tb,
				       name->name, name->len,
				       0, DT_UNKNOWN, false);
	iput(root);
	return err;
}

/* Both parent inode locks and the namespace journal handle belong to VFS.
 * Reserve the complete pair while holding the roots in inode-number order.
 * No ENOSPC/ENOMEM path remains after the first mapping becomes visible.
 */
int ext5_stable_rename_links(handle_t *handle, struct inode *old_dir,
			    const struct qstr *old_name, struct inode *new_dir,
			    const struct qstr *new_name, struct inode *inode)
{
	struct rename_record {
		struct inode *root;
		struct ext5_stable_blob *blob;
		const struct qstr *name;
		struct ext5_stable_delta_entry *reserved;
		struct ext5_stable_write write;
		u8 data[LIDIR_DELTA_MAX_RECORD_BYTES + LIDIR_DELTA_END_BYTES];
		u32 plid, hash, bytes;
		u64 tb, pos, seq;
	} rec[2] = { { .name = new_name }, { .name = old_name } };
	struct inode *first, *second;
	bool locked = false;
	int err = 0, i, attempts = 0;

	if (!new_name->len || new_name->len > EXT5_NAME_LEN ||
	    !old_name->len || old_name->len > EXT5_NAME_LEN)
		return -ENAMETOOLONG;
	rec[0].root = ext5_stable_get_subtree_root(new_dir);
	if (IS_ERR(rec[0].root))
		return PTR_ERR(rec[0].root);
	rec[1].root = ext5_stable_get_subtree_root(old_dir);
	if (IS_ERR(rec[1].root)) {
		err = PTR_ERR(rec[1].root);
		iput(rec[0].root);
		return err;
	}
	first = rec[0].root;
	second = rec[1].root;
	if (first->i_ino > second->i_ino)
		swap(first, second);
	rec[0].plid = ext5_stable_dir_plid(new_dir);
	rec[1].plid = ext5_stable_dir_plid(old_dir);
retry:
	for (i = 0; i < 2; i++) {
		err = ext5_stable_ensure_blob(rec[i].root);
		if (err)
			goto out;
	}
	mutex_lock(&EXT5_I(first)->i_li_lock);
	if (second != first)
		mutex_lock_nested(&EXT5_I(second)->i_li_lock, SINGLE_DEPTH_NESTING);
	locked = true;
	for (i = 0; i < 2; i++)
		rec[i].blob = rcu_dereference_protected(
			EXT5_I(rec[i].root)->i_stable_blob,
			lockdep_is_held(&EXT5_I(rec[i].root)->i_li_lock));
	if (!rec[0].blob || !rec[1].blob) {
		if (second != first)
			mutex_unlock(&EXT5_I(second)->i_li_lock);
		mutex_unlock(&EXT5_I(first)->i_li_lock);
		locked = false;
		if (++attempts < 8)
			goto retry;
		err = -EAGAIN;
		goto out;
	}
	for (i = 0; i < 2; i++)
		rec[i].bytes = lidir_delta_record_bytes(rec[i].name->len);
	for (i = 0; i < 2; i++) {
		struct rename_record *r = &rec[i];
		struct ext5_stable_delta *d = &r->blob->delta;
		struct lidir_disk_delta_record hdr = { 0 };
		u32 needed = r->bytes, write_bytes = r->bytes, csum;
		bool exists;

		if (first == second)
			needed = rec[0].bytes + rec[1].bytes;
		if (d->used_bytes > d->region_bytes ||
		    needed > d->region_bytes - d->used_bytes) {
			err = -ENOSPC;
			goto out;
		}
		r->hash = ext5_stable_hash32(r->blob, r->name->name, r->name->len);
		r->tb = ext5_stable_tiebreak64(r->blob, r->name->name,
					     r->name->len, r->hash);
		spin_lock(&d->lock);
		exists = ext5_stable_delta_find(d, r->plid, r->hash, r->tb,
					       r->name->name, r->name->len) != NULL;
		spin_unlock(&d->lock);
		if (!exists) {
			r->reserved = kmalloc(sizeof(*r->reserved) + r->name->len,
					      GFP_NOFS);
			if (!r->reserved) {
				err = -ENOMEM;
				goto out;
			}
		}
		r->pos = d->used_bytes + (i && first == second ? rec[0].bytes : 0);
		r->seq = d->next_seq + (i && first == second ? 1 : 0);
		hdr.seq = cpu_to_le64(r->seq);
		hdr.op = i ? LIDIR_DELTA_DELETE : LIDIR_DELTA_INSERT;
		hdr.file_type = i ? DT_UNKNOWN : fs_umode_to_dtype(inode->i_mode);
		hdr.name_len = r->name->len;
		hdr.parent_local_id = cpu_to_le32(r->plid);
		hdr.key32 = cpu_to_le32(r->hash);
		hdr.ino = i ? 0 : cpu_to_le32(inode->i_ino);
		csum = crc32_le(0, (u8 *)&hdr +
			offsetof(struct lidir_disk_delta_record, op),
			LIDIR_DELTA_HEADER_BYTES -
			offsetof(struct lidir_disk_delta_record, op));
		hdr.csum = cpu_to_le32(crc32_le(csum, r->name->name, r->name->len));
		memcpy(r->data, &hdr, LIDIR_DELTA_HEADER_BYTES);
		memcpy(r->data + LIDIR_DELTA_HEADER_BYTES, r->name->name, r->name->len);
		if (r->pos + r->bytes + LIDIR_DELTA_END_BYTES <= d->region_bytes)
			write_bytes += LIDIR_DELTA_END_BYTES;
		err = ext5_stable_write_prepare(handle, r->root,
			d->region_off + r->pos, write_bytes, &r->write);
		if (err)
			goto out;
	}
	/* All allocations and journal access are complete. Copy both records
	 * before dirtying either buffer, including for synchronous directories.
	 */
	for (i = 0; i < 2; i++)
		ext5_stable_write_copy(&rec[i].write, rec[i].data);
	for (i = 0; i < 2; i++) {
		struct rename_record *r = &rec[i];
		struct ext5_stable_delta *d = &r->blob->delta;
		int ret;

		spin_lock(&d->lock);
		ret = ext5_stable_delta_apply(d,
			i ? LIDIR_DELTA_DELETE : LIDIR_DELTA_INSERT,
			r->plid, r->hash, r->tb, r->name->name, r->name->len,
			i ? 0 : inode->i_ino,
			i ? DT_UNKNOWN : fs_umode_to_dtype(inode->i_mode), &r->reserved);
		WARN_ON_ONCE(ret);
		d->used_bytes = r->pos + r->bytes;
		d->next_seq = r->seq + 1;
		spin_unlock(&d->lock);
		ret = ext5_stable_write_dirty(handle, r->root, &r->write);
		if (!err)
			err = ret;
	}
	for (i = 0; i < 2; i++) {
		struct ext5_stable_delta *d = &rec[i].blob->delta;

		if (i && first == second)
			break;
		WRITE_ONCE(EXT5_I(rec[i].root)->i_li_last_delta_jiffies, jiffies);
		if (d->trigger_bytes && d->used_bytes >= d->trigger_bytes &&
		    !atomic_read(&EXT5_I(rec[i].root)->i_li_demote_pending))
			ext5_stable_queue_compact(rec[i].root, rec[i].blob->generation,
				d->used_bytes >= d->region_bytes - d->region_bytes / 4);
	}
out:
	for (i = 0; i < 2; i++) {
		ext5_stable_write_release(&rec[i].write);
		kfree(rec[i].reserved);
	}
	if (locked) {
		if (second != first)
			mutex_unlock(&EXT5_I(second)->i_li_lock);
		mutex_unlock(&EXT5_I(first)->i_li_lock);
	}
	iput(rec[0].root);
	iput(rec[1].root);
	return err;
}

static void ext5_stable_unlist_blob(struct ext5_stable_blob *blob)
{
	spin_lock(&ext5_li_blob_list_lock);
	if (blob->cache_listed) {
		list_del_init(&blob->cache_list);
		blob->cache_listed = false;
	}
	spin_unlock(&ext5_li_blob_list_lock);
}

static void ext5_stable_free_blob(struct ext5_stable_blob *blob)
{
	if (!blob)
		return;
	ext5_stable_unlist_blob(blob);
	ext5_stable_delta_free(&blob->delta);
	ext5_stable_unpin_payload_blocks(blob);
	kvfree(blob->model_buf);
	kvfree(blob->pindex_buf);
	kvfree(blob->payload_extents);
	kfree(blob);
}

static unsigned long ext5_li_model_count(struct shrinker *shrinker,
					 struct shrink_control *sc)
{
	struct ext5_stable_blob *blob;
	unsigned long pages = 0;

	spin_lock(&ext5_li_blob_list_lock);
	list_for_each_entry(blob, &ext5_li_blob_list, cache_list)
		pages += max_t(unsigned long,
			DIV_ROUND_UP(blob->nav_cache_bytes +
				     blob->payload_cache_bytes, PAGE_SIZE), 1);
	spin_unlock(&ext5_li_blob_list_lock);
	return pages;
}

static unsigned long ext5_li_model_scan(struct shrinker *shrinker,
					struct shrink_control *sc)
{
	struct ext5_stable_blob *blob;
	struct inode *root;
	unsigned long pages;

	spin_lock(&ext5_li_blob_list_lock);
	if (list_empty(&ext5_li_blob_list)) {
		spin_unlock(&ext5_li_blob_list_lock);
		return SHRINK_STOP;
	}
	blob = list_first_entry(&ext5_li_blob_list,
				struct ext5_stable_blob, cache_list);
	if (!refcount_inc_not_zero(&blob->refs)) {
		spin_unlock(&ext5_li_blob_list_lock);
		return SHRINK_STOP;
	}
	list_move_tail(&blob->cache_list, &ext5_li_blob_list);
	root = igrab(blob->host);
	pages = max_t(unsigned long,
		DIV_ROUND_UP(blob->nav_cache_bytes + blob->payload_cache_bytes,
			     PAGE_SIZE), 1);
	spin_unlock(&ext5_li_blob_list_lock);
	if (!root) {
		ext5_stable_blob_put(blob);
		return SHRINK_STOP;
	}

	if (mutex_trylock(&EXT5_I(root)->i_li_lock)) {
		struct ext5_stable_blob *installed = rcu_dereference_protected(
			EXT5_I(root)->i_stable_blob,
			lockdep_is_held(&EXT5_I(root)->i_li_lock));

		if (atomic_read(&EXT5_I(root)->i_li_compact_staging)) {
			pages = 0;
		} else if (installed == blob) {
			ext5_stable_unlist_blob(blob);
			RCU_INIT_POINTER(EXT5_I(root)->i_stable_blob, NULL);
			call_rcu(&blob->rcu, ext5_stable_free_blob_rcu_cb);
		} else {
			pages = 0;
		}
		mutex_unlock(&EXT5_I(root)->i_li_lock);
	} else {
		pages = 0;
	}
	iput(root);
	ext5_stable_blob_put(blob);
	return pages ? pages : SHRINK_STOP;
}

int ext5_li_model_cache_init(void)
{
	ext5_li_model_shrinker = shrinker_alloc(SHRINKER_MEMCG_AWARE,
					      "ext5-spline-model");
	if (!ext5_li_model_shrinker)
		return -ENOMEM;
	ext5_li_model_shrinker->count_objects = ext5_li_model_count;
	ext5_li_model_shrinker->scan_objects = ext5_li_model_scan;
	shrinker_register(ext5_li_model_shrinker);
	return 0;
}

void ext5_li_model_cache_exit(void)
{
	shrinker_free(ext5_li_model_shrinker);
	ext5_li_model_shrinker = NULL;
}

static void ext5_stable_free_blob_rcu_cb(struct rcu_head *head)
{
	struct ext5_stable_blob *blob =
		container_of(head, struct ext5_stable_blob, rcu);

	/* Drop the installed reference after both RCU and SRCU grace periods. */
	call_srcu(&ext5_li_lookup_srcu, &blob->srcu,
		  ext5_stable_free_blob_srcu_cb);
}

static void ext5_stable_free_blob_srcu_cb(struct rcu_head *head)
{
	struct ext5_stable_blob *blob =
		container_of(head, struct ext5_stable_blob, srcu);

	ext5_stable_blob_put(blob);
}

void ext5_li_lookup_barrier(void)
{
	/* Callers run rcu_barrier() first, so every SRCU callback is queued. */
	srcu_barrier(&ext5_li_lookup_srcu);
}

/* Read the directory's data stream with ext5_bread(): directories have no
 * page-cache aops. */
static int ext5_stable_read_range(struct inode *dir, loff_t pos, size_t len,
				   void *dst)
{
	struct super_block *sb = dir->i_sb;
	unsigned int bs = sb->s_blocksize;
	size_t copied = 0;

	while (copied < len) {
		ext5_lblk_t blk = (pos + copied) >> sb->s_blocksize_bits;
		size_t off = (pos + copied) & (bs - 1);
		size_t this = min_t(size_t, bs - off, len - copied);
		struct buffer_head *bh;

		bh = ext5_bread(NULL, dir, blk, 0);
		if (IS_ERR(bh))
			return PTR_ERR(bh);
		if (!bh)
			return -EIO;
		memcpy((char *)dst + copied, bh->b_data + off, this);
		brelse(bh);
		copied += this;
	}
	return 0;
}

/* As above, but a hole reads as zeros, so the sparse delta tail replays as
 * NOP.  The base region is never a hole. */
static int ext5_stable_read_range_sparse(struct inode *dir, loff_t pos,
					 size_t len, void *dst)
{
	struct super_block *sb = dir->i_sb;
	unsigned int bs = sb->s_blocksize;
	size_t copied = 0;

	while (copied < len) {
		ext5_lblk_t blk = (pos + copied) >> sb->s_blocksize_bits;
		size_t off = (pos + copied) & (bs - 1);
		size_t this = min_t(size_t, bs - off, len - copied);
		struct buffer_head *bh;

		bh = ext5_bread(NULL, dir, blk, 0);
		if (IS_ERR(bh))
			return PTR_ERR(bh);
		if (!bh) {
			memset((char *)dst + copied, 0, this);	/* hole -> zeros */
		} else {
			memcpy((char *)dst + copied, bh->b_data + off, this);
			brelse(bh);
		}
		copied += this;
	}
	return 0;
}

/* Replay the delta region.  Returns the offset of the first NOP or invalid
 * record, the next append position. */
static int ext5_stable_replay_delta(struct inode *dir,
				    const struct ext5_stable_blob *keys,
				    struct ext5_stable_delta *delta,
				    u64 replay_bytes, bool exact_cut)
{
	u64 off = 0;
	u64 region_bytes = min(replay_bytes, delta->region_bytes);
	u64 region_off = delta->region_off;
	int err = 0;

	while (off + LIDIR_DELTA_HEADER_BYTES <= region_bytes) {
		struct lidir_disk_delta_record hdr;
		u8 *namebuf = NULL;
		u32 expected_csum, computed_csum;
		u8 op, name_len, file_type;
		u64 seq;
		u32 plid;
		u32 key32;
		u32 ino;
		u64 tb;
		unsigned int rec_bytes;

		err = ext5_stable_read_range_sparse(dir, region_off + off,
					     LIDIR_DELTA_HEADER_BYTES, &hdr);
		if (err)
			break;
		op = hdr.op;
		if (op == LIDIR_DELTA_NOP) {
			if (exact_cut)
				err = -EFSCORRUPTED;
			break;	/* end of log (zeros: written NOP or sparse hole) */
		}
		if (op != LIDIR_DELTA_INSERT && op != LIDIR_DELTA_DELETE) {
			if (exact_cut)
				err = -EFSCORRUPTED;
			break;
		}
		name_len = hdr.name_len;
		if (name_len == 0) {
			if (exact_cut)
				err = -EFSCORRUPTED;
			break;
		}
		rec_bytes = lidir_delta_record_bytes(name_len);
		if (off + rec_bytes > region_bytes) {
			if (exact_cut)
				err = -EFSCORRUPTED;
			break;
		}

		namebuf = kmalloc(name_len, GFP_KERNEL);
		if (!namebuf) {
			err = -ENOMEM;
			break;
		}
		err = ext5_stable_read_range_sparse(dir,
			region_off + off + LIDIR_DELTA_HEADER_BYTES,
			name_len, namebuf);
		if (err) {
			kfree(namebuf);
			break;
		}

		expected_csum = le32_to_cpu(hdr.csum);
		/* csum covers [op .. ino] AND name bytes */
		computed_csum = crc32_le(0,
			(const u8 *)&hdr + offsetof(struct lidir_disk_delta_record, op),
			LIDIR_DELTA_HEADER_BYTES -
				offsetof(struct lidir_disk_delta_record, op));
		computed_csum = crc32_le(computed_csum, namebuf, name_len);
		if (computed_csum != expected_csum) {
			kfree(namebuf);
			if (exact_cut)
				err = -EFSCORRUPTED;
			break;	/* partial-tail */
		}
		seq = le64_to_cpu(hdr.seq);
		if (seq != delta->next_seq) {
			kfree(namebuf);
			if (exact_cut)
				err = -EFSCORRUPTED;
			break;
		}
		file_type = hdr.file_type;
		plid = le32_to_cpu(hdr.parent_local_id);
		key32 = le32_to_cpu(hdr.key32);
		ino = le32_to_cpu(hdr.ino);

		/* Recompute tiebreak64 from the name; key32 is its low half. */
		if (ext5_stable_hash32(keys, namebuf, name_len) != key32) {
			/* The descriptor pins the hash key, so a mismatch is corruption. */
			kfree(namebuf);
			err = -EFSCORRUPTED;
			break;
		}
		tb = ext5_stable_tiebreak64(keys, namebuf, name_len, key32);
		{
			struct ext5_stable_delta_entry *reserved;

			/* Reserve the node before taking delta.lock: no GFP_KERNEL in atomic
			 * context. */
			reserved = kmalloc(sizeof(*reserved) + name_len, GFP_KERNEL);
			if (!reserved) {
				kfree(namebuf);
				err = -ENOMEM;
				break;
			}

			spin_lock(&delta->lock);
			err = ext5_stable_delta_apply(delta, op,
					      plid, key32, tb,
					      namebuf, name_len,
					      ino, file_type, &reserved);
			delta->next_seq = seq + 1;
			spin_unlock(&delta->lock);
			kfree(reserved);
			kfree(namebuf);
			if (err)
				break;
		}

		off += rec_bytes;
	}

	delta->used_bytes = off;
	if (!err && exact_cut && off != replay_bytes)
		err = -EFSCORRUPTED;
	return err;
}

/* Parse a descriptor and its base region into a new blob; the caller
 * installs it under RCU. */
/* True if the descriptor's fixed header and header checksum validate. */
static bool ext5_desc_header_valid(const struct lidir_disk_descriptor *d)
{
	if (le32_to_cpu(d->magic) != LIDIR_MAGIC)
		return false;
	if (le32_to_cpu(d->version) != LIDIR_VERSION)
		return false;
	if (le32_to_cpu(d->header_bytes) != sizeof(*d))
		return false;
	return crc32_le(0, (const u8 *)d, LIDIR_DESC_CSUM_LEN) ==
	       le32_to_cpu(d->header_csum);
}

/* Pick the header-valid descriptor slot with the highest generation;
 * -EFSCORRUPTED when neither is valid. */
static int ext5_stable_read_active_desc(struct inode *dir,
					struct lidir_disk_descriptor *out,
					int *slot)
{
	struct lidir_disk_descriptor *tmp;
	int i, best = -1;
	u64 best_gen = 0;

	tmp = kzalloc(LIDIR_DESC_BYTES, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	for (i = 0; i < LIDIR_DESC_SLOTS; i++) {
		u64 gen;

		if (ext5_stable_read_range(dir, (loff_t)i * LIDIR_DESC_BYTES,
					   LIDIR_DESC_BYTES, tmp))
			continue;
		if (!ext5_desc_header_valid(tmp))
			continue;
		gen = le64_to_cpu(tmp->generation);
		if (best < 0 || gen > best_gen) {
			best_gen = gen;
			best = i;
			memcpy(out, tmp, LIDIR_DESC_BYTES);
		}
	}

	kfree(tmp);
	if (best < 0)
		return -EFSCORRUPTED;
	*slot = best;
	return 0;
}

/* Cache the payload's physical extent runs; base extents cannot move while
 * this generation has readers. */
static int ext5_stable_cache_payload_extents(struct ext5_stable_blob *blob)
{
	struct inode *inode = blob->host;
	u32 bits = inode->i_sb->s_blocksize_bits;
	u64 start_byte, end_byte;
	ext5_lblk_t block, end_block;
	u32 capacity = 0;

	if (!blob->entry_count)
		return 0;
	start_byte = blob->base_off + blob->slots_off;
	end_byte = blob->base_off + blob->names_off + blob->names_bytes;
	block = start_byte >> bits;
	end_block = DIV_ROUND_UP_ULL(end_byte, 1ULL << bits);
	while (block < end_block) {
		struct ext5_map_blocks map = {
			.m_lblk = block,
			.m_len = min_t(u64, end_block - block, UINT_MAX),
		};
		struct ext5_stable_extent_run *grown;
		int mapped;

		mapped = ext5_map_blocks(NULL, inode, &map, 0);
		if (mapped <= 0 || !(map.m_flags & EXT5_MAP_MAPPED) ||
		    (map.m_flags & EXT5_MAP_UNWRITTEN))
			return mapped < 0 ? mapped : -EFSCORRUPTED;
		if (blob->nr_payload_extents == capacity) {
			u32 new_capacity = capacity ? capacity * 2 : 8;

			grown = kvmalloc_array(new_capacity, sizeof(*grown),
					       GFP_KERNEL_ACCOUNT);
			if (!grown)
				return -ENOMEM;
			if (blob->nr_payload_extents)
				memcpy(grown, blob->payload_extents,
				       blob->nr_payload_extents * sizeof(*grown));
			kvfree(blob->payload_extents);
			blob->payload_extents = grown;
			capacity = new_capacity;
		}
		blob->payload_extents[blob->nr_payload_extents].logical = block;
		blob->payload_extents[blob->nr_payload_extents].physical =
			map.m_pblk;
		blob->payload_extents[blob->nr_payload_extents].length = mapped;
		blob->nr_payload_extents++;
		block += mapped;
	}
	blob->nav_cache_bytes +=
		blob->nr_payload_extents * sizeof(*blob->payload_extents);
	return 0;
}

static void ext5_stable_unpin_payload_blocks(struct ext5_stable_blob *blob)
{
	u32 i;

	if (!blob->payload_blocks)
		return;
	for (i = 0; i < blob->nr_payload_blocks; i++)
		if (blob->payload_blocks[i].bh)
			brelse(blob->payload_blocks[i].bh);
	kvfree(blob->payload_blocks);
	blob->payload_blocks = NULL;
	blob->payload_first_block = 0;
	blob->nr_payload_blocks = 0;
	blob->payload_cache_bytes = 0;
}

/* Optionally pin the payload blocks for the hot path; on failure release
 * all pins and read by physical run. */
static void ext5_stable_pin_payload_blocks(struct ext5_stable_blob *blob)
{
	u32 bits = blob->host->i_sb->s_blocksize_bits;
	u64 start_byte, end_byte;
	ext5_lblk_t first, end, block;
	struct ext5_stable_payload_block *blocks;
	u32 nr, i = 0;

	if (!blob->entry_count || blob->payload_blocks)
		return;
	start_byte = blob->base_off + blob->slots_off;
	end_byte = blob->base_off + blob->names_off + blob->names_bytes;
	first = start_byte >> bits;
	end = DIV_ROUND_UP_ULL(end_byte, 1ULL << bits);
	if (end <= first || end - first > UINT_MAX)
		return;
	nr = end - first;
	blocks = kvmalloc_array(nr, sizeof(*blocks),
				  GFP_KERNEL_ACCOUNT | __GFP_ZERO);
	if (!blocks)
		return;
	for (block = first; block < end; block++, i++) {
		bool borrowed;
		struct buffer_head *bh =
			ext5_stable_payload_bread(blob, block, &borrowed);

		if (IS_ERR_OR_NULL(bh) || WARN_ON_ONCE(borrowed))
			goto fail;
		blocks[i].bh = bh; /* retain bread reference for blob lifetime */
		blocks[i].data = bh->b_data;
		if (!(i & 255))
			cond_resched();
	}
	blob->payload_blocks = blocks;
	blob->payload_first_block = first;
	blob->nr_payload_blocks = nr;
	blob->payload_cache_bytes =
		(size_t)nr * blob->host->i_sb->s_blocksize +
		(size_t)nr * sizeof(*blocks);
	return;

fail:
	while (i)
		brelse(blocks[--i].bh);
	kvfree(blocks);
}

/* Parse one descriptor.  cache_listed=false builds a private candidate for
 * compaction, visible to the shrinker only once published. */
/* NOFS, as in promote.c: reclaim from here must not evict ext5 inodes. */
static int ext5_stable_parse_desc(struct inode *dir,
			 const struct lidir_disk_descriptor *desc,
			 struct ext5_stable_blob **out, bool cache_listed)
{
	struct ext5_stable_blob *blob;
	u32 base_csum, computed_base_csum;
	u64 base_off, base_bytes;
	u64 delta_off, delta_bytes;
	u32 entry_count, parent_count;
	u32 radix_count, spline_count, spline_epsilon;
	u32 spline_corridor_error, radix_bits, radix_shift;
	u64 model_min_key, model_max_key;
	u64 filter_off, radix_off, spline_off, slots_off;
	u64 parent_index_off, names_off, names_bytes;
	u32 filter_bits;
	int err;

	if (!ext5_desc_header_valid(desc))
		return -EFSCORRUPTED;

	base_off = le64_to_cpu(desc->base_off);
	base_bytes = le64_to_cpu(desc->base_bytes);
	base_csum = le32_to_cpu(desc->base_csum);
	delta_off = le64_to_cpu(desc->delta_off);
	delta_bytes = le64_to_cpu(desc->delta_bytes);
	entry_count = le32_to_cpu(desc->entry_count);
	parent_count = le32_to_cpu(desc->parent_count);
	filter_bits = le32_to_cpu(desc->filter_bits);
	filter_off = le64_to_cpu(desc->filter_off);
	radix_count = le32_to_cpu(desc->radix_count);
	spline_count = le32_to_cpu(desc->spline_count);
	spline_epsilon = le32_to_cpu(desc->spline_epsilon);
	spline_corridor_error = le32_to_cpu(desc->spline_corridor_error);
	radix_bits = le32_to_cpu(desc->radix_bits);
	radix_shift = le32_to_cpu(desc->radix_shift);
	model_min_key = le64_to_cpu(desc->model_min_key);
	model_max_key = le64_to_cpu(desc->model_max_key);
	radix_off = le64_to_cpu(desc->radix_off);
	spline_off = le64_to_cpu(desc->spline_off);
	slots_off = le64_to_cpu(desc->slots_off);
	parent_index_off = le64_to_cpu(desc->parent_index_off);
	names_off = le64_to_cpu(desc->names_off);
	names_bytes = le64_to_cpu(desc->names_bytes);

	if (!base_bytes || base_bytes > SIZE_MAX || !parent_count) {
		err = -EFSCORRUPTED;
		goto out_desc;
	}
	if (filter_bits && (filter_bits & (filter_bits - 1)) != 0) {
		err = -EFSCORRUPTED;
		goto out_desc;
	}
	BUILD_BUG_ON(sizeof(*desc) != LIDIR_DESC_BYTES);
	if (entry_count &&
	    (radix_count < 2 || spline_count < 1 || !spline_epsilon ||
	     !spline_corridor_error || spline_corridor_error > spline_epsilon ||
	     radix_bits > 24 || radix_shift > 63 ||
	     model_min_key > model_max_key)) {
		err = -EFSCORRUPTED;
		goto out_desc;
	}
	/* Bounds-check and order the arrays inside the base. */
	if (filter_off >= base_bytes ||
	    radix_off < filter_off || radix_off >= base_bytes ||
	    spline_off < radix_off || spline_off >= base_bytes ||
	    slots_off < spline_off ||
	    slots_off >= base_bytes ||
	    parent_index_off >= base_bytes ||
	    parent_index_off < slots_off ||
	    names_off > base_bytes ||
	    names_off < parent_index_off ||
	    (u64)radix_count * sizeof(__le32) > spline_off - radix_off ||
	    (u64)spline_count * sizeof(struct lidir_disk_spline_point) >
		slots_off - spline_off ||
	    (u64)entry_count * sizeof(struct lidir_disk_slot) >
		parent_index_off - slots_off ||
	    (u64)(parent_count + 1) * sizeof(__le32) >
		names_off - parent_index_off ||
	    names_bytes > base_bytes - names_off) {
		err = -EFSCORRUPTED;
		goto out_desc;
	}

	blob = kzalloc(sizeof(*blob), GFP_KERNEL);
	if (!blob) {
		err = -ENOMEM;
		goto out_desc;
	}
	/* The installed pointer owns one reference, dropped by the RCU free. */
	refcount_set(&blob->refs, 1);
	INIT_LIST_HEAD(&blob->cache_list);
	blob->host = dir;
	blob->base_size = base_bytes;
	blob->base_off = base_off;
	blob->filter_off = filter_off;
	blob->radix_off = radix_off;
	blob->spline_off = spline_off;
	blob->slots_off = slots_off;
	blob->parent_index_off = parent_index_off;
	blob->names_off = names_off;
	blob->entry_count = entry_count;
	blob->parent_count = parent_count;
	blob->filter_bits = filter_bits;
	blob->filter_mask = filter_bits - 1;
	blob->radix_count = radix_count;
	blob->spline_count = spline_count;
	blob->spline_epsilon = spline_epsilon;
	blob->spline_corridor_error = spline_corridor_error;
	blob->radix_bits = radix_bits;
	blob->radix_shift = radix_shift;
	blob->model_min_key = model_min_key;
	blob->model_max_key = model_max_key;
	blob->names_bytes = names_bytes;

	/* Only navigation state is cached, memcg-charged; slots and names stay
	 * in the mapping. */
	{
		size_t model_bytes = slots_off - filter_off;
		size_t pindex_bytes = names_off - parent_index_off;

		blob->model_buf = kvmalloc(model_bytes ? model_bytes : 1,
					   GFP_KERNEL_ACCOUNT);
		blob->pindex_buf = kvmalloc(pindex_bytes ? pindex_bytes : 1,
					    GFP_KERNEL_ACCOUNT);
		if (!blob->model_buf || !blob->pindex_buf) {
			err = -ENOMEM;
			goto out_blob;
		}
		err = ext5_stable_read_range(dir, base_off + filter_off,
					     model_bytes, blob->model_buf);
		if (err)
			goto out_blob;
		blob->nav_cache_bytes = model_bytes + pindex_bytes;
		err = ext5_stable_read_range(dir, base_off + parent_index_off,
					     pindex_bytes, blob->pindex_buf);
		if (err)
			goto out_blob;
	}

	/* Stream the base to verify its CRC without keeping it resident. */
	{
		u8 *crcbuf = kmalloc(65536, GFP_KERNEL);
		u64 done = 0;

		if (!crcbuf) {
			err = -ENOMEM;
			goto out_blob;
		}
		computed_base_csum = 0;
		while (done < base_bytes) {
			size_t this = min_t(u64, 65536, base_bytes - done);

			err = ext5_stable_read_range(dir, base_off + done,
						     this, crcbuf);
			if (err) {
				kfree(crcbuf);
				goto out_blob;
			}
			computed_base_csum = crc32_le(computed_base_csum,
						      crcbuf, this);
			done += this;
		}
		kfree(crcbuf);
	}
	if (computed_base_csum != base_csum) {
		err = -EFSCORRUPTED;
		goto out_blob;
	}

	blob->generation = le64_to_cpu(desc->generation);
	/* Validate model and parent-table monotonicity before publication. */
	if (entry_count) {
		u32 i, previous = 0;
		u64 previous_key = 0;
		u32 previous_rank = 0;

		for (i = 0; i < radix_count; i++) {
			u32 value;

			err = ext5_stable_read_radix(blob, i, &value);
			if (err)
				goto out_blob;

			if (value < previous || value > spline_count) {
				err = -EFSCORRUPTED;
				goto out_blob;
			}
			previous = value;
		}
		for (i = 0; i < spline_count; i++) {
			struct lidir_disk_spline_point point;
			u64 point_key;
			u32 point_rank;

			err = ext5_stable_read_spline(blob, i, &point);
			if (err)
				goto out_blob;
			point_key = le64_to_cpu(point.key);
			point_rank = le32_to_cpu(point.rank);

			if ((i && (point_key < previous_key ||
				   point_rank < previous_rank)) ||
			    point_rank >= entry_count) {
				err = -EFSCORRUPTED;
				goto out_blob;
			}
			previous_key = point_key;
			previous_rank = point_rank;
		}
		previous = 0;
		for (i = 0; i <= parent_count; i++) {
			u32 value;

			err = ext5_stable_read_parent_index(blob, i, &value);
			if (err)
				goto out_blob;

			if (value < previous || value > entry_count) {
				err = -EFSCORRUPTED;
				goto out_blob;
			}
			previous = value;
		}
		if (previous != entry_count) {
			err = -EFSCORRUPTED;
			goto out_blob;
		}
	}

	/* Use the descriptor's hash key, so lookups do not depend on the mount. */
	BUILD_BUG_ON(sizeof(blob->hash_key) != sizeof(desc->hash_seed));
	memcpy(&blob->hash_key, desc->hash_seed, sizeof(blob->hash_key));

	ext5_stable_delta_init(&blob->delta, delta_off, delta_bytes);
	if (le64_to_cpu(desc->delta_seq_base) == U64_MAX) {
		err = -EFSCORRUPTED;
		goto out_blob;
	}
	blob->delta.seq_base = le64_to_cpu(desc->delta_seq_base);
	blob->delta.next_seq = blob->delta.seq_base + 1;
	/* Keep a quarter of the region free for tail growth while compaction
	 * stages a replacement. */
	blob->delta.trigger_bytes = min_t(u64,
		delta_bytes - delta_bytes / 4,
		ext5_stable_delta_trigger(dir->i_sb, entry_count, names_bytes));
	ext5_stable_delta_filter_alloc(&blob->delta);
	if (delta_bytes) {
		err = ext5_stable_replay_delta(dir, blob, &blob->delta,
					       delta_bytes, false);
		if (err)
			goto out_blob;
	}
	/* After remount, start the refill epoch at the recovered delta position. */
	if (!atomic64_read(&EXT5_I(dir)->i_li_refill_start_op)) {
		atomic64_set(&EXT5_I(dir)->i_li_refill_start_used_bytes,
			     blob->delta.used_bytes);
		atomic64_cmpxchg(&EXT5_I(dir)->i_li_refill_start_op, 0,
				 ext5_li_op_clock_now_exact(dir->i_sb));
	}

	err = ext5_stable_cache_payload_extents(blob);
	if (err)
		goto out_blob;
	ext5_stable_pin_payload_blocks(blob);

	*out = blob;
	if (cache_listed) {
		spin_lock(&ext5_li_blob_list_lock);
		list_add_tail(&blob->cache_list, &ext5_li_blob_list);
		blob->cache_listed = true;
		spin_unlock(&ext5_li_blob_list_lock);
	}
	return 0;

out_blob:
	if (blob) {
		ext5_stable_unpin_payload_blocks(blob);
		kvfree(blob->model_buf);
		kvfree(blob->pindex_buf);
		kvfree(blob->payload_extents);
	}
	kfree(blob);
out_desc:
	return err;
}


struct ext5_stable_pool_state {
	u32 start;
	u32 count;
	u32 used;
};

static int ext5_stable_parse_blob(struct inode *dir,
				   struct ext5_stable_blob **out,
				   struct ext5_stable_pool_state *pool)
{
	struct lidir_disk_descriptor *descs;
	int order[LIDIR_DESC_SLOTS];
	int nr = 0;
	int err = -EFSCORRUPTED;
	int i;

	descs = kcalloc(LIDIR_DESC_SLOTS, LIDIR_DESC_BYTES, GFP_KERNEL);
	if (!descs)
		return -ENOMEM;

	/* A valid header does not prove a complete base: parse candidates newest
	 * first and fall back to the previous generation, whose extents stay live
	 * until publication is durable. */
	for (i = 0; i < LIDIR_DESC_SLOTS; i++) {
		struct lidir_disk_descriptor *desc = &descs[i];

		if (ext5_stable_read_range(dir, (loff_t)i * LIDIR_DESC_BYTES,
					   LIDIR_DESC_BYTES, desc))
			continue;
		if (ext5_desc_header_valid(desc))
			order[nr++] = i;
	}
	if (nr == 2 &&
	    le64_to_cpu(descs[order[0]].generation) <
	    le64_to_cpu(descs[order[1]].generation))
		swap(order[0], order[1]);

	for (i = 0; i < nr; i++) {
		err = ext5_stable_parse_desc(dir, &descs[order[i]], out, true);
		if (!err) {
			struct rb_node *node;

			pool->start = le32_to_cpu(descs[order[i]].inode_pool_start);
			pool->count = le32_to_cpu(descs[order[i]].inode_pool_count);
			pool->used = le32_to_cpu(descs[order[i]].inode_pool_used);
			/* The pool is an allocation hint, persisted at generation cuts.
			 * Inode eviction can precede that cut: recover a conservative
			 * cursor from live inserts without a descriptor write per create.
			 * Ordinary mounts without an explicit pool pay no tree walk.
			 */
			if (pool->count && pool->used < pool->count) {
				for (node = rb_first_cached(&(*out)->delta.by_key); node;
				     node = rb_next(node)) {
					struct ext5_stable_delta_entry *entry = rb_entry(node,
						struct ext5_stable_delta_entry, rb);

					if (entry->op == LIDIR_DELTA_INSERT &&
					    entry->ino >= pool->start &&
					    entry->ino - pool->start < pool->count)
						pool->used = max(pool->used,
							 entry->ino - pool->start + 1);
				}
			}
		}
		if (!err || err == -ENOMEM)
			break;
	}
	kfree(descs);
	return err;
}

/* Parse and install the blob on first stable access. */
int ext5_stable_ensure_blob(struct inode *dir)
{
	struct ext5_inode_info *ei = EXT5_I(dir);
	struct ext5_stable_blob *new_blob = NULL;
	struct ext5_stable_blob *old_blob;
	struct ext5_stable_pool_state pool;
	int err;

	rcu_read_lock();
	old_blob = rcu_dereference(ei->i_stable_blob);
	rcu_read_unlock();
	if (old_blob)
		return 0;

	err = ext5_stable_parse_blob(dir, &new_blob, &pool);
	if (err)
		return err;
	ext5_stable_stats_blob_parse();

	mutex_lock(&ei->i_li_lock);
	old_blob = rcu_dereference_protected(ei->i_stable_blob,
					     lockdep_is_held(&ei->i_li_lock));
	if (old_blob) {
		/* Lost the race; another thread parsed first. */
		mutex_unlock(&ei->i_li_lock);
		ext5_stable_free_blob(new_blob);
		return 0;
	}
	/* Restore the persisted pool only when none is live: a later reparse must
	 * not overwrite a reservation or rewind its cursor. */
	spin_lock(&ei->i_li_pool_lock);
	if (!ei->i_li_pool_count) {
		ei->i_li_pool_start = pool.start;
		ei->i_li_pool_count = pool.count;
		ei->i_li_pool_used = pool.used;
	}
	spin_unlock(&ei->i_li_pool_lock);
	rcu_assign_pointer(ei->i_stable_blob, new_blob);
	mutex_unlock(&ei->i_li_lock);
	return 0;
}

void ext5_stable_forget_dir(struct inode *inode)
{
	struct ext5_inode_info *ei = EXT5_I(inode);
	struct ext5_stable_blob *blob;

	if (!ei->i_li_state)
		return;
	mutex_lock(&ei->i_li_lock);
	blob = rcu_dereference_protected(ei->i_stable_blob,
					 lockdep_is_held(&ei->i_li_lock));
	/* Reclaim must stop seeing host before the inode's own RCU retirement.
	 * Blob readers can outlive it through the additional SRCU grace period.
	 */
	if (blob)
		ext5_stable_unlist_blob(blob);
	RCU_INIT_POINTER(ei->i_stable_blob, NULL);
	mutex_unlock(&ei->i_li_lock);

	if (blob)
		call_rcu(&blob->rcu, ext5_stable_free_blob_rcu_cb);
}

/* First run of `count` free inodes, as an absolute inode number, or 0.
 * Does not mark them used. */
static u32 ext5_stable_find_free_run(struct super_block *sb, u32 count)
{
	struct ext5_sb_info *sbi = EXT5_SB(sb);
	ext5_group_t g, ngroups = ext5_get_groups_count(sb);
	u32 inodes_per_group = EXT5_INODES_PER_GROUP(sb);

	for (g = 0; g < ngroups; g++) {
		struct ext5_group_desc *gdp;
		struct buffer_head *bh;
		unsigned long start, end, run = 0, run_start = 0;
		unsigned long bit;

		gdp = ext5_get_group_desc(sb, g, NULL);
		if (!gdp)
			continue;
		if (ext5_free_inodes_count(sb, gdp) < count)
			continue;
		bh = ext5_read_inode_bitmap(sb, g);
		if (IS_ERR(bh))
			continue;

		start = 0;
		end = inodes_per_group;
		bit = start;
		while (bit < end) {
			unsigned long zero_bit = find_next_zero_bit(
				(unsigned long *)bh->b_data, end, bit);
			unsigned long one_bit;

			if (zero_bit >= end)
				break;
			one_bit = find_next_bit(
				(unsigned long *)bh->b_data, end, zero_bit);
			run = one_bit - zero_bit;
			if (run >= count) {
				run_start = zero_bit;
				brelse(bh);
				return (u32)(g * inodes_per_group + run_start + 1);
			}
			bit = one_bit + 1;
		}
		brelse(bh);
	}
	(void)sbi;
	return 0;
}

/* Persist (start, count, used) in the active descriptor.  Caller holds
 * root->i_rwsem but not i_li_lock, since the journal handle comes first. */
static int ext5_stable_persist_pool(struct inode *root,
				    u32 start, u32 count, u32 used)
{
	struct lidir_disk_descriptor *desc;
	handle_t *handle = NULL;
	struct super_block *sb = root->i_sb;
	struct buffer_head *bh;
	ext5_lblk_t blk;
	int slot;
	int err;
	u32 csum;
	const size_t header_bytes = offsetofend(struct lidir_disk_descriptor,
					      header_csum);

	lockdep_assert_held_write(&root->i_rwsem);
	lockdep_assert_not_held(&EXT5_I(root)->i_li_lock);
	BUILD_BUG_ON(offsetofend(struct lidir_disk_descriptor, header_csum) >
		     EXT5_MIN_BLOCK_SIZE);

	desc = kzalloc(LIDIR_DESC_BYTES, GFP_KERNEL);
	if (!desc)
		return -ENOMEM;

	/* Modify the active descriptor in place and rewrite its slot. */
	err = ext5_stable_read_active_desc(root, desc, &slot);
	if (err)
		goto out_free;
	blk = ((ext5_lblk_t)slot * LIDIR_DESC_BYTES) >> sb->s_blocksize_bits;

	desc->inode_pool_start = cpu_to_le32(start);
	desc->inode_pool_count = cpu_to_le32(count);
	desc->inode_pool_used = cpu_to_le32(used);

	csum = crc32_le(0, (const u8 *)desc, LIDIR_DESC_CSUM_LEN);
	desc->header_csum = cpu_to_le32(csum);

	handle = ext5_journal_start(root, EXT5_HT_DIR,
				    EXT5_DATA_TRANS_BLOCKS(sb));
	if (IS_ERR(handle)) {
		err = PTR_ERR(handle);
		handle = NULL;
		goto out_free;
	}

	bh = ext5_bread(handle, root, blk, EXT5_GET_BLOCKS_CREATE);
	if (IS_ERR(bh)) {
		err = PTR_ERR(bh);
		goto out_journal;
	}
	if (!bh) {
		err = -EIO;
		goto out_journal;
	}
	err = ext5_journal_get_write_access(handle, sb, bh, EXT5_JTR_NONE);
	if (err) {
		brelse(bh);
		goto out_journal;
	}
	/* Copy only the checksummed header: on a 1 KiB filesystem the descriptor
	 * spans four blocks and this buffer head holds one. */
	memcpy(bh->b_data, desc, header_bytes);
	set_buffer_uptodate(bh);
	err = ext5_handle_dirty_metadata(handle, root, bh);
	brelse(bh);

out_journal:
	if (handle) {
		int stop_err = ext5_journal_stop(handle);

		if (!err)
			err = stop_err;
	}
out_free:
	kfree(desc);
	return err;
}

int ext5_stable_reserve_inode_pool(struct inode *root, u32 count)
{
	struct ext5_inode_info *ei = EXT5_I(root);
	struct ext5_sb_info *sbi = EXT5_SB(root->i_sb);
	u32 start;
	int err;

	if (count == 0)
		return -EINVAL;

	/* Check the mode under the locks: a demotion may have won meanwhile. */
	mutex_lock(&sbi->li_transition_lock);
	inode_lock(root);
	if (!(ei->i_flags & EXT5_LIDIR_ROOT_FL)) {
		err = -EINVAL;
		goto out_inode;
	}
	/* Load a persisted pool first, e.g. on the first access after remount. */
	err = ext5_stable_ensure_blob(root);
	if (err)
		goto out_inode;
	mutex_lock(&ei->i_li_lock);
	if (ei->i_li_pool_count) {
		err = -EEXIST;
		goto out_li;
	}

	start = ext5_stable_find_free_run(root->i_sb, count);
	if (!start) {
		err = -ENOSPC;
		goto out_li;
	}

	mutex_unlock(&ei->i_li_lock);
	err = ext5_stable_persist_pool(root, start, count, 0);
	if (err)
		goto out_inode;

	mutex_lock(&ei->i_li_lock);
	spin_lock(&ei->i_li_pool_lock);
	ei->i_li_pool_start = start;
	ei->i_li_pool_count = count;
	ei->i_li_pool_used = 0;
	spin_unlock(&ei->i_li_pool_lock);
out_li:
	mutex_unlock(&ei->i_li_lock);
out_inode:
	inode_unlock(root);
	mutex_unlock(&sbi->li_transition_lock);
	return err;
}

/* Generation and policy state for the debug ioctl. */
int ext5_stable_get_desc_info(struct inode *root, struct ext5_li_desc_info *out)
{
	struct ext5_inode_info *ei = EXT5_I(root);
	struct ext5_stable_blob *blob;
	int err;

	if (!(ei->i_flags & EXT5_LIDIR_ROOT_FL))
		return -EINVAL;
	err = ext5_stable_ensure_blob(root);
	if (err)
		return err;
	rcu_read_lock();
	blob = rcu_dereference(ei->i_stable_blob);
	if (!blob) {
		rcu_read_unlock();
		return -EIO;
	}
	memset(out, 0, sizeof(*out));
	/* Navigation excludes exact-name and slot payload arrays. */
	out->base_bytes = blob->base_size;
	out->names_bytes = blob->names_bytes;
	out->nav_bytes =
		(u64)(blob->filter_bits / 8) +
		(u64)blob->radix_count * sizeof(__le32) +
		(u64)blob->spline_count *
			sizeof(struct lidir_disk_spline_point) +
		(u64)(blob->parent_count + 1) * sizeof(__le32) +
		(u64)blob->nr_payload_extents *
			sizeof(struct ext5_stable_extent_run);
	out->delta_bytes = blob->delta.region_bytes;
	out->generation = blob->generation;
	spin_lock(&blob->delta.lock);
	out->delta_used = blob->delta.used_bytes;
	out->delta_trigger = blob->delta.trigger_bytes;
	out->delta_records = blob->delta.nr_records;
	out->delta_appended_inserts = blob->delta.appended_inserts;
	out->delta_appended_deletes = blob->delta.appended_deletes;
	spin_unlock(&blob->delta.lock);
	out->entry_count = blob->entry_count;
	out->refill_start_op = atomic64_read(&ei->i_li_refill_start_op);
	out->refill_start_used_bytes = atomic64_read(
		&ei->i_li_refill_start_used_bytes);
	{
		u64 now = ext5_li_op_clock_now_exact(root->i_sb);

		out->refill_span_ops = now > out->refill_start_op ?
			now - out->refill_start_op : 0;
	}
	out->tstar_ops = ext5_li_t_star_for_entries(root->i_sb,
						      blob->entry_count);
	out->last_refill_span_ops = atomic64_read(
		&ei->i_li_last_refill_span_ops);
	out->last_refill_projected_ops = atomic64_read(
		&ei->i_li_last_refill_projected_ops);
	out->last_refill_tstar_ops = atomic64_read(
		&ei->i_li_last_refill_tstar_ops);
	out->parent_count = blob->parent_count;
	out->filter_bits = blob->filter_bits;
	out->radix_count = blob->radix_count;
	out->spline_count = blob->spline_count;
	out->spline_epsilon = blob->spline_epsilon;
	out->radix_bits = blob->radix_bits;
	out->delta_filter_bits = blob->delta.presence_filter_bits;
	out->last_refill_class = atomic_read(&ei->i_li_last_refill_class);
	out->resident_payload_bytes = blob->payload_cache_bytes;
	out->resident_payload_blocks = blob->nr_payload_blocks;
	out->payload_extent_runs = blob->nr_payload_extents;
	rcu_read_unlock();
	return 0;
}

u32 ext5_stable_entry_count(struct inode *root)
{
	struct ext5_stable_blob *blob;
	u32 entries = 0;

	rcu_read_lock();
	blob = rcu_dereference(EXT5_I(root)->i_stable_blob);
	if (blob)
		entries = READ_ONCE(blob->entry_count);
	rcu_read_unlock();
	return entries;
}

/*
 * Allocation goal for a create under a stable directory: the reserved pool's
 * next inode, if any.  Otherwise, without a journal, advance a root-local goal
 * across groups, since searching from bit zero rescans recently deleted
 * inodes.  The goal is only a hint; the bitmap stays authoritative.
 */
ino_t ext5_stable_pool_take_goal(struct inode *parent)
{
	struct inode *root;
	struct ext5_inode_info *root_ei;
	u32 start, count, used;
	ino_t goal = 0;

	if (!parent || !ext5_dir_is_stable(parent))
		return 0;

	root = ext5_stable_get_subtree_root(parent);
	if (IS_ERR(root))
		return 0;

	root_ei = EXT5_I(root);
	spin_lock(&root_ei->i_li_pool_lock);
	start = root_ei->i_li_pool_start;
	count = root_ei->i_li_pool_count;
	used = root_ei->i_li_pool_used;
	if (count && used < count) {
		goal = (ino_t)(start + used);
		root_ei->i_li_pool_used = used + 1;
	}
	spin_unlock(&root_ei->i_li_pool_lock);

	if (!goal && !EXT5_SB(root->i_sb)->s_journal) {
		struct super_block *sb = root->i_sb;
		u64 first = EXT5_FIRST_INO(sb);
		u64 total = le32_to_cpu(EXT5_SB(sb)->s_es->s_inodes_count);
		u64 cursor = atomic64_read(&root_ei->i_li_alloc_cursor);
		u64 span, ticket;

		if (likely(total >= first)) {
			span = total - first + 1;
			if (!cursor) {
				u64 groups = ext5_get_groups_count(sb);
				u64 group = groups ?
					(root_ei->i_block_group + 1) % groups : 0;
				u64 seed = group * EXT5_INODES_PER_GROUP(sb) + 1;

				if (seed < first || seed > total)
					seed = first;
				cursor = atomic64_cmpxchg(&root_ei->i_li_alloc_cursor,
							  0, seed);
				if (!cursor)
					cursor = seed;
			}
			ticket = atomic64_fetch_inc(&root_ei->i_li_alloc_cursor);
			/* A racing initializer returns the installed nonzero cursor. */
			if (unlikely(!ticket))
				ticket = cursor;
			goal = (ino_t)(first + (ticket - first) % span);
		}
	}

	iput(root);
	return goal;
}

#define EXT5_COMPACT_FILTER_PROBES	5U
#define EXT5_COMPACT_FILTER_MIN_BITS	1024U

struct compact_slot {
	u64 key;
	u64 tiebreak64;		/* SipHash-high32 | HalfSipHash search key */
	u32 name_off;
	u8  name_len;
	u8  file_type;
	u8  flags;
	u8  _pad;
	u32 ino;
	u32 dx_hash;
};

static bool compact_slot_prefix_le(const struct compact_slot *left,
				   const struct compact_slot *right)
{
	if (left->key != right->key)
		return left->key < right->key;
	return left->tiebreak64 <= right->tiebreak64;
}

/* Merge the two sorted runs (base survivors, then delta INSERTs) rather
 * than re-sorting; ties keep base before delta. */
static int compact_merge_sorted_runs(struct compact_slot **slots_inout,
				     u32 base_nr, u32 total_nr)
{
	struct compact_slot *slots = *slots_inout;
	struct compact_slot *merged;
	u32 left = 0, right = base_nr, out = 0;

	if (!base_nr || base_nr == total_nr)
		return 0;
	merged = kvmalloc_array(total_nr, sizeof(*merged), GFP_KERNEL);
	if (!merged)
		return -ENOMEM;
	while (left < base_nr && right < total_nr) {
		if (compact_slot_prefix_le(&slots[left], &slots[right]))
			merged[out++] = slots[left++];
		else
			merged[out++] = slots[right++];
	}
	while (left < base_nr)
		merged[out++] = slots[left++];
	while (right < total_nr)
		merged[out++] = slots[right++];
	kvfree(slots);
	*slots_inout = merged;
	return 0;
}

static u32 compact_filter_bits(u32 nr)
{
	u64 bpe = clamp_t(u64, READ_ONCE(ext5_li_bloom_bits_per_entry), 4, 16);
	u64 target = (u64)nr * bpe;
	u32 bits = EXT5_COMPACT_FILTER_MIN_BITS;

	if (!READ_ONCE(ext5_li_bloom_enabled))
		return 0;
	while (bits < target && bits <= (1U << 30))
		bits <<= 1;
	return bits;
}

static void compact_filter_set(u64 *words, u32 mask, u64 key)
{
	u64 h1 = mix64(key);
	u64 h2 = ((h1 << 32) | (h1 >> 32)) ^ 0x9e3779b97f4a7c15ULL;
	u32 block_mask = ((mask + 1) / 512) - 1;
	u32 block_base = ((u32)h1 & block_mask) * 512;
	u32 i;

	h2 |= 1ULL;

	for (i = 0; i < EXT5_COMPACT_FILTER_PROBES; i++) {
		u32 bit = block_base + ((u32)(h2 + i * h1) & 511);

		words[bit >> 6] |= 1ULL << (bit & 63);
	}
}

/* Load the base slots and names into transient buffers for compaction and
 * demotion, which touch every entry.  Caller frees both. */
static int ext5_stable_load_base_tmp(const struct ext5_stable_blob *blob,
				     struct lidir_disk_slot **slots_out,
				     char **names_out)
{
	struct lidir_disk_slot *s = NULL;
	char *nm = NULL;
	int err;

	*slots_out = NULL;
	*names_out = NULL;
	if (blob->entry_count) {
		s = kvmalloc_array(blob->entry_count, sizeof(*s), GFP_KERNEL);
		if (!s)
			return -ENOMEM;
		err = ext5_stable_read_payload_range(blob,
			(loff_t)(blob->base_off + blob->slots_off),
			(size_t)blob->entry_count * sizeof(*s), s);
		if (err) {
			kvfree(s);
			return err;
		}
	}
	if (blob->names_bytes) {
		nm = kvmalloc(blob->names_bytes, GFP_KERNEL);
		if (!nm) {
			kvfree(s);
			return -ENOMEM;
		}
		err = ext5_stable_read_payload_range(blob,
			(loff_t)(blob->base_off + blob->names_off),
			blob->names_bytes, nm);
		if (err) {
			kvfree(s);
			kvfree(nm);
			return err;
		}
	}
	*slots_out = s;
	*names_out = nm;
	return 0;
}

/* Non-sleeping in-base membership test over a loaded base (sorted by key). */
static bool ext5_stable_base_has(const struct lidir_disk_slot *slots, u32 n,
				 const char *names, u32 names_bytes,
				 u64 key, const char *name, u8 name_len)
{
	u32 lo = 0, hi = n;

	while (lo < hi) {
		u32 mid = lo + (hi - lo) / 2;

		if (le64_to_cpu(slots[mid].key) < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	for (; lo < n && le64_to_cpu(slots[lo].key) == key; lo++) {
		u32 off = le32_to_cpu(slots[lo].name_off);
		u8 nl = slots[lo].name_len;

		if (off > names_bytes || nl > names_bytes - off)
			continue;
		if (nl == name_len && !memcmp(names + off, name, nl))
			return true;
	}
	return false;
}

/*
 * Merge base and delta: drop deleted slots, replace re-inserted ones, then
 * append new INSERTs.  Inputs are private copies, so this may sleep without
 * locks.  Caller frees the output buffers.
 */
static int compact_collect_slots(const struct ext5_stable_blob *blob,
				 const struct ext5_stable_delta *delta,
				 const struct lidir_disk_slot *tmp_slots,
				 const char *tmp_names,
				 struct compact_slot **out_slots,
				 u32 *out_nr,
				 char **out_names,
				 u32 *out_names_bytes)
{
	u32 base_count = blob->entry_count;
	u32 reserved = base_count + delta->nr_inserts + 16;
	struct compact_slot *slots;
	char *names;
	u32 cap_names;
	u32 nr = 0;
	u32 base_nr;
	u32 names_off = 0;
	u32 i, b;
	struct ext5_stable_delta_entry *de;

	slots = kvmalloc_array(reserved, sizeof(*slots), GFP_KERNEL);
	if (!slots)
		return -ENOMEM;
	cap_names = max_t(u32, blob->names_bytes + 4096U, 4096U);
	names = kvmalloc(cap_names, GFP_KERNEL);
	if (!names) {
		kvfree(slots);
		return -ENOMEM;
	}

	/* Phase 1: base slots, minus tombstones, with replacements. */
	for (i = 0; i < base_count; i++) {
		const struct lidir_disk_slot *s = &tmp_slots[i];
		u32 off = le32_to_cpu(s->name_off);
		u8 nl = s->name_len;
		const char *nm;
		u64 key = le64_to_cpu(s->key);
		u32 plid_i = (u32)(key >> 32);
		u32 hash32_i = (u32)key;
		u64 tb;
		u32 ino;
		u8 ftype;
		u8 flags;

		if (off > blob->names_bytes ||
		    nl > blob->names_bytes - off) {
			kvfree(slots);
			kvfree(names);
			return -EIO;
		}
		nm = tmp_names + off;
		tb = ext5_stable_tiebreak64(blob, nm, nl, hash32_i);

		de = ext5_stable_delta_find(
			(struct ext5_stable_delta *)delta,
			plid_i, hash32_i, tb, nm, nl);
		if (de) {
			if (de->op == LIDIR_DELTA_DELETE)
				continue;
			ino = de->ino;
			ftype = de->file_type;
		} else {
			ino = le32_to_cpu(s->ino);
			ftype = s->file_type;
		}
		flags = s->flags;

		if (names_off + nl > cap_names) {
			char *grown;
			u32 new_cap = cap_names * 2;

			while (new_cap < names_off + nl)
				new_cap *= 2;
			grown = kvmalloc(new_cap, GFP_KERNEL);
			if (!grown) {
				kvfree(slots);
				kvfree(names);
				return -ENOMEM;
			}
			memcpy(grown, names, names_off);
			kvfree(names);
			names = grown;
			cap_names = new_cap;
		}
		memcpy(names + names_off, nm, nl);
		slots[nr].key = key;
		slots[nr].tiebreak64 = tb;
		slots[nr].ino = ino;
		slots[nr].name_off = names_off;
		slots[nr].name_len = nl;
		slots[nr].file_type = ftype;
		slots[nr].flags = flags;
		slots[nr].dx_hash = le32_to_cpu(s->dx_hash);
		nr++;
		names_off += nl;
	}
	base_nr = nr;

	/* Phase 2: delta INSERTs absent from the base, in tree order. */
	(void)b;
	{
		struct rb_node *node;

		for (node = rb_first_cached(&delta->by_key);
		     node; node = rb_next(node)) {
			u64 key;
			bool in_base = false;

			de = rb_entry(node, struct ext5_stable_delta_entry, rb);
			if (de->op != LIDIR_DELTA_INSERT)
				continue;
			key = ext5_stable_make_key(de->plid, de->hash32);
			in_base = ext5_stable_base_has(tmp_slots, base_count,
				tmp_names, blob->names_bytes, key,
				de->name, de->name_len);
			if (in_base)
				continue;

			if (nr == reserved) {
				struct compact_slot *grown;
				u32 new_cap = reserved * 2;

				grown = kvmalloc_array(new_cap, sizeof(*grown),
						       GFP_KERNEL);
				if (!grown) {
					kvfree(slots);
					kvfree(names);
					return -ENOMEM;
				}
				memcpy(grown, slots, nr * sizeof(*slots));
				kvfree(slots);
				slots = grown;
				reserved = new_cap;
			}
			if (names_off + de->name_len > cap_names) {
				char *grown;
				u32 new_cap = cap_names * 2;

				while (new_cap < names_off + de->name_len)
					new_cap *= 2;
				grown = kvmalloc(new_cap, GFP_KERNEL);
				if (!grown) {
					kvfree(slots);
					kvfree(names);
					return -ENOMEM;
				}
				memcpy(grown, names, names_off);
				kvfree(names);
				names = grown;
				cap_names = new_cap;
			}
			memcpy(names + names_off, de->name, de->name_len);
			slots[nr].key = key;
			slots[nr].tiebreak64 = de->tiebreak64;
			slots[nr].ino = de->ino;
			slots[nr].name_off = names_off;
			slots[nr].name_len = de->name_len;
			slots[nr].file_type = de->file_type;
			slots[nr].flags = 0;
			if (ext5_stable_dx_hash(blob, de->name, de->name_len,
						&slots[nr].dx_hash)) {
				kvfree(slots);
				kvfree(names);
				return -EIO;
			}
			nr++;
			names_off += de->name_len;
		}
	}

	if (compact_merge_sorted_runs(&slots, base_nr, nr)) {
		kvfree(slots);
		kvfree(names);
		return -ENOMEM;
	}

	*out_slots = slots;
	*out_nr = nr;
	*out_names = names;
	*out_names_bytes = names_off;
	return 0;
}

/* Clone the delta under i_li_lock; the compactor then works on the clone
 * without locks. */
static int compact_clone_delta(const struct ext5_stable_delta *source,
			       struct ext5_stable_delta *snapshot)
{
	struct rb_node *node;
	int err = 0;

	ext5_stable_delta_init(snapshot, source->region_off,
			       source->region_bytes);
	for (node = rb_first_cached(&source->by_key); node;
	     node = rb_next(node)) {
		const struct ext5_stable_delta_entry *entry = rb_entry(node,
			struct ext5_stable_delta_entry, rb);

		err = ext5_stable_delta_apply(snapshot, entry->op, entry->plid,
			entry->hash32, entry->tiebreak64, entry->name,
			entry->name_len, entry->ino, entry->file_type, NULL);
		if (err)
			break;
	}
	if (err) {
		ext5_stable_delta_free(snapshot);
		return err;
	}
	snapshot->used_bytes = source->used_bytes;
	snapshot->seq_base = source->seq_base;
	snapshot->next_seq = source->next_seq;
	snapshot->trigger_bytes = source->trigger_bytes;
	snapshot->appended_inserts = source->appended_inserts;
	snapshot->appended_deletes = source->appended_deletes;
	return 0;
}

/* Pack names in slot order: name_off must increase with the slot index. */
static int compact_repack_names(struct compact_slot *slots, u32 nr,
				char **names_inout, u32 *names_bytes)
{
	char *src = *names_inout;
	char *dst;
	u32 off = 0;
	u32 i;

	dst = kvmalloc(*names_bytes ? *names_bytes : 1, GFP_KERNEL);
	if (!dst)
		return -ENOMEM;

	for (i = 0; i < nr; i++) {
		memcpy(dst + off, src + slots[i].name_off, slots[i].name_len);
		slots[i].name_off = off;
		off += slots[i].name_len;
	}
	kvfree(src);
	*names_inout = dst;
	*names_bytes = off;
	return 0;
}

/* Drop PLID ranges left unreachable by branch demotion; live PLIDs keep
 * their numbers. */
static int compact_drop_dead_plids(struct inode *root,
				   struct compact_slot *slots, u32 *nr_slots,
				   u32 parent_count)
{
	bool *live;
	u32 *ranges, *queue;
	u32 in, out = 0, plid = 0, head = 0, tail = 1;
	int err = 0;

	if (!parent_count)
		return -EFSCORRUPTED;
	/* With one parent there is no descendant range to prune. The merged
	 * slots are sorted, so checking their last PLID validates the range.
	 */
	if (parent_count == 1)
		return *nr_slots && (slots[*nr_slots - 1].key >> 32) ?
			-EFSCORRUPTED : 0;
	live = kvcalloc(parent_count, sizeof(*live), GFP_KERNEL);
	ranges = kvcalloc((size_t)parent_count + 1, sizeof(*ranges), GFP_KERNEL);
	queue = kvmalloc_array(parent_count, sizeof(*queue), GFP_KERNEL);
	if (!live || !ranges || !queue) {
		err = -ENOMEM;
		goto out;
	}
	/* Slots are sorted by PLID, but rename can reverse the ancestry order.
	 * Index their ranges first, then follow live directory links from root.
	 */
	for (in = 0; in < *nr_slots; in++) {
		u32 slot_plid = slots[in].key >> 32;

		if (slot_plid < plid || slot_plid >= parent_count) {
			err = -EFSCORRUPTED;
			goto out;
		}
		while (plid < slot_plid)
			ranges[++plid] = in;
	}
	while (plid < parent_count)
		ranges[++plid] = *nr_slots;
	live[0] = true;
	queue[0] = 0;
	while (head < tail) {
		plid = queue[head++];
		for (in = ranges[plid]; in < ranges[plid + 1]; in++) {
			struct compact_slot *slot = &slots[in];
			struct inode *child;

			if (slot->file_type != DT_DIR)
				continue;
			child = ext5_iget(root->i_sb, slot->ino, EXT5_IGET_NORMAL);
			if (IS_ERR(child)) {
				err = PTR_ERR(child);
				goto out;
			}
			if ((EXT5_I(child)->i_flags & EXT5_LIDIR_INTERIOR_FL) &&
			    EXT5_I(child)->i_subtree_root_ino == root->i_ino) {
				u32 child_plid =
					EXT5_I(child)->i_parent_local_id_in_subtree;

				if (!child_plid || child_plid >= parent_count)
					err = -EFSCORRUPTED;
				else if (!live[child_plid]) {
					live[child_plid] = true;
					queue[tail++] = child_plid;
				}
			}
			iput(child);
			if (err)
				goto out;
		}
	}
	if (tail == parent_count)
		goto out;
	for (in = 0; in < *nr_slots; in++) {
		if (!live[slots[in].key >> 32])
			continue;
		if (out != in)
			slots[out] = slots[in];
		out++;
	}
	*nr_slots = out;
out:
	kvfree(queue);
	kvfree(ranges);
	kvfree(live);
	return err;
}

/* Write `bytes` at `pos`, allocating blocks, one short transaction per
 * block.  The caller syncs afterwards. */
/* Punch out a retired range.  Caller holds i_rwsem, lifecycle callers also
 * i_li_lock.  Failure only leaks blocks until the next compaction. */
static int ext5_stable_punch_range(struct inode *inode, u64 off, u64 bytes)
{
	struct address_space *mapping = inode->i_mapping;
	int ret;

	if (!bytes)
		return 0;

	WARN_ON_ONCE(!inode_is_locked(inode));

	inode_dio_wait(inode);
	filemap_invalidate_lock(mapping);
	ret = ext5_break_layouts(inode);
	if (ret)
		goto out_invalidate;
	ret = ext5_punch_hole(inode, (loff_t)off, (loff_t)bytes);
out_invalidate:
	filemap_invalidate_unlock(mapping);
	return ret;
}

static int ext5_stable_pwrite(struct inode *inode, u64 pos,
			      const void *buf, size_t bytes)
{
	struct super_block *sb = inode->i_sb;
	unsigned int bs = sb->s_blocksize;
	size_t written = 0;
	int err;

	while (written < bytes) {
		handle_t *handle;
		struct buffer_head *bh;
		ext5_lblk_t blk = (pos + written) >> sb->s_blocksize_bits;
		u32 off = (u32)((pos + written) & (bs - 1));
		u32 this = min_t(u32, bs - off, bytes - written);

		handle = ext5_journal_start(inode, EXT5_HT_DIR,
			EXT5_DATA_TRANS_BLOCKS(sb));
		if (IS_ERR(handle))
			return PTR_ERR(handle);
		bh = ext5_bread(handle, inode, blk, EXT5_GET_BLOCKS_CREATE);
		if (IS_ERR(bh)) {
			err = PTR_ERR(bh);
			ext5_journal_stop(handle);
			return err;
		}
		if (!bh) {
			ext5_journal_stop(handle);
			return -EIO;
		}
		err = ext5_journal_get_write_access(handle, sb, bh,
						    EXT5_JTR_NONE);
		if (err) {
			brelse(bh);
			ext5_journal_stop(handle);
			return err;
		}
		memcpy(bh->b_data + off, (const char *)buf + written, this);
		set_buffer_uptodate(bh);
		err = ext5_handle_dirty_metadata(handle, inode, bh);
		brelse(bh);
		if (err) {
			ext5_journal_stop(handle);
			return err;
		}
		if ((loff_t)((blk + 1) << sb->s_blocksize_bits) >
		    i_size_read(inode)) {
			i_size_write(inode,
				(loff_t)(blk + 1) << sb->s_blocksize_bits);
			EXT5_I(inode)->i_disksize = i_size_read(inode);
			ext5_mark_inode_dirty(handle, inode);
		}
		err = ext5_journal_stop(handle);
		if (err)
			return err;
		written += this;
	}
	return 0;
}

/* Flush staged data.  No sync_inode_metadata(): the worker may run after
 * the last inode reference, and the index is rebuildable. */
static int ext5_stable_flush_file(struct inode *inode)
{
	int err;

	err = sync_mapping_buffers(inode->i_mapping);
	if (!err)
		err = filemap_write_and_wait(inode->i_mapping);
	if (!err)
		err = blkdev_issue_flush(inode->i_sb->s_bdev);
	return err;
}

/*
 * Stage-and-swap compaction.  Merge, fit, stage, flush and parse run without
 * namespace locks; a staging gate stops demotion from moving the root extent
 * tree but not lookups or appends.  Publication validates the source cut,
 * writes the inactive descriptor slot, swaps the RCU generation and reclaims
 * old extents after readers drain.  Without a journal this survives torn
 * descriptor writes and clean remount, not arbitrary power loss.
 */
int ext5_stable_compact(struct inode *root)
{
	struct ext5_inode_info *root_ei = EXT5_I(root);
	struct ext5_stable_blob *old_blob = NULL;
	struct ext5_stable_blob *new_blob = NULL;
	struct ext5_stable_delta delta_snapshot;
	struct compact_slot *slots = NULL;
	char *names = NULL;
	u32 nr_slots = 0, names_bytes = 0;
	u32 parent_count;
	u32 filter_bits, filter_words;
	size_t off_filter, off_slots,
	       off_parent_index, off_names, base_size;
	size_t off_radix = 0, off_spline = 0;
	size_t base_buf_size;
	void *base_buf = NULL;
	struct sfs_rs_model *rs_model = NULL;
	u64 *rs_keys = NULL;
	struct lidir_disk_descriptor *desc = NULL;
	struct lidir_disk_slot *out_slots;
	__le32 *out_parent_index;
	char *out_names;
	u64 *out_filter;
	u32 i;
	struct super_block *sb = root->i_sb;
	unsigned int bs = sb->s_blocksize;
	u64 stage_base_off, stage_delta_off;
	u64 delta_region_bytes;
	u64 stage_end = 0;
	u64 old_base_off, old_base_bytes;
	u64 old_delta_off, old_delta_bytes;
	u64 snapshot_used_bytes, snapshot_next_seq;
	u64 copied_used_bytes, copied_next_seq;
	u64 tail_bytes = 0;
	void *tail_buf = NULL;
	bool blob_pinned = false;
	bool compact_gate = false;
	bool delta_snapshot_valid = false;
	bool published = false;
	bool root_locked = false;
	bool li_locked = false;
	struct ext5_stable_write desc_write = { 0 };
	handle_t *publish_handle = NULL;
	u64 transition_start_ns;
	int err;

	if (!(root_ei->i_flags & EXT5_LIDIR_ROOT_FL))
		return -EINVAL;
	err = ext5_stable_ensure_blob(root);
	if (err)
		return err;
	transition_start_ns = ktime_get_ns();

	mutex_lock(&root_ei->i_li_lock);
	li_locked = true;
	if (atomic_read(&root_ei->i_li_demote_pending)) {
		err = -EAGAIN;
		goto out;
	}
	if (atomic_cmpxchg(&root_ei->i_li_compact_staging, 0, 1)) {
		err = -EBUSY;
		goto out;
	}
	compact_gate = true;
	old_blob = rcu_dereference_protected(root_ei->i_stable_blob,
					     lockdep_is_held(&root_ei->i_li_lock));
	if (!old_blob) {
		err = -ENOENT;
		goto out;
	}
	refcount_inc(&old_blob->refs);
	blob_pinned = true;

	/* An empty delta is already compact: skip the rebuild. */
	if (old_blob->delta.used_bytes == 0) {
		err = 0;
		goto out;
	}

	parent_count = old_blob->parent_count;
	/* Remember the old extents; they are reclaimed once the new descriptor
	 * is durable. */
	old_base_off = old_blob->base_off;
	old_base_bytes = ALIGN(old_blob->base_size, bs);
	old_delta_off = old_blob->delta.region_off;
	old_delta_bytes = old_blob->delta.region_bytes;
	snapshot_used_bytes = old_blob->delta.used_bytes;
	snapshot_next_seq = old_blob->delta.next_seq;
	ext5_stable_delta_init(&delta_snapshot, old_blob->delta.region_off,
			       old_blob->delta.region_bytes);
	delta_snapshot.seq_base = old_blob->delta.seq_base;
	delta_snapshot.next_seq = delta_snapshot.seq_base + 1;
	delta_snapshot.trigger_bytes = old_blob->delta.trigger_bytes;
	old_blob->delta.snapshot_floor_bytes = snapshot_used_bytes;
	delta_snapshot_valid = true;
	mutex_unlock(&root_ei->i_li_lock);
	li_locked = false;

	/* The prefix below snapshot_floor_bytes is immutable: replay it into the
	 * private tree without blocking appenders; later records are the tail. */
	err = ext5_stable_replay_delta(root, old_blob, &delta_snapshot,
				       snapshot_used_bytes, true);
	if (err)
		goto out;
	if (delta_snapshot.next_seq != snapshot_next_seq) {
		err = -EFSCORRUPTED;
		goto out;
	}

	{
		struct lidir_disk_slot *tmp_slots = NULL;
		char *tmp_names = NULL;

		/* Load the immutable base and merge it with the private snapshot. */
		err = ext5_stable_load_base_tmp(old_blob, &tmp_slots, &tmp_names);
		if (!err) {
			err = compact_collect_slots(old_blob, &delta_snapshot,
						    tmp_slots, tmp_names,
						    &slots, &nr_slots,
						    &names, &names_bytes);
		}
		kvfree(tmp_slots);
		kvfree(tmp_names);
	}
	if (err)
		goto out;

	if (nr_slots) {
		err = compact_drop_dead_plids(root, slots, &nr_slots,
					      parent_count);
		if (err)
			goto out;
		err = compact_repack_names(slots, nr_slots, &names,
					   &names_bytes);
		if (err)
			goto out;
	}

	if (nr_slots) {
		u32 collision_run = 1, run = 1;

		rs_keys = kvmalloc_array(nr_slots, sizeof(*rs_keys), GFP_KERNEL);
		if (!rs_keys) {
			err = -ENOMEM;
			goto out;
		}
		for (i = 0; i < nr_slots; i++) {
			rs_keys[i] = slots[i].key;
			if (i && rs_keys[i] == rs_keys[i - 1]) {
				run++;
				if (run > collision_run)
					collision_run = run;
			} else {
				run = 1;
			}
		}
		err = sfs_rs_build(rs_keys, nr_slots,
			READ_ONCE(ext5_li_radix_bits),
			READ_ONCE(ext5_li_spline_epsilon), collision_run,
			&rs_model);
		if (err)
			goto out;
	}
	filter_bits = compact_filter_bits(nr_slots ? nr_slots : 1);
	filter_words = filter_bits / 64;
	off_filter = 0;
	off_radix = ALIGN(filter_words * sizeof(u64), 8);
	off_spline = ALIGN(off_radix +
		(rs_model ? rs_model->radix_count : 0) * sizeof(__le32), 8);
	off_slots = ALIGN(off_spline +
		(rs_model ? rs_model->point_count : 0) *
			sizeof(struct lidir_disk_spline_point), 8);
	off_parent_index = ALIGN(off_slots +
		nr_slots * sizeof(struct lidir_disk_slot), 8);
	off_names = ALIGN(off_parent_index +
		(parent_count + 1) * sizeof(__le32), 8);
	base_size = off_names + names_bytes;
	base_buf_size = ALIGN(base_size, bs);
	base_buf = vzalloc(base_buf_size);
	if (!base_buf) {
		err = -ENOMEM;
		goto out;
	}
	out_filter = (u64 *)((char *)base_buf + off_filter);
	{
		__le32 *out_radix = (__le32 *)((char *)base_buf + off_radix);
		struct lidir_disk_spline_point *out_spline =
			(struct lidir_disk_spline_point *)
			((char *)base_buf + off_spline);

		if (rs_model) {
			for (i = 0; i < rs_model->radix_count; i++)
				out_radix[i] = cpu_to_le32(rs_model->radix[i]);
			for (i = 0; i < rs_model->point_count; i++) {
				out_spline[i].key =
					cpu_to_le64(rs_model->points[i].key);
				out_spline[i].rank =
					cpu_to_le32(rs_model->points[i].rank);
			}
		}
	}
	out_slots = (struct lidir_disk_slot *)
		((char *)base_buf + off_slots);
	out_parent_index = (__le32 *)
		((char *)base_buf + off_parent_index);
	out_names = (char *)base_buf + off_names;
	for (i = 0; i < nr_slots; i++) {
		out_slots[i].key = cpu_to_le64(slots[i].key);
		out_slots[i].ino = cpu_to_le32(slots[i].ino);
		out_slots[i].name_off = cpu_to_le32(slots[i].name_off);
		out_slots[i].dx_hash = cpu_to_le32(slots[i].dx_hash);
		out_slots[i].name_len = slots[i].name_len;
		out_slots[i].file_type = slots[i].file_type;
		out_slots[i].flags = slots[i].flags;
		if (filter_bits)
			compact_filter_set(out_filter, filter_bits - 1,
					   slots[i].key);
	}
	{
		u32 current_plid = 0;

		out_parent_index[0] = 0;
		for (i = 0; i < nr_slots; i++) {
			u32 slot_plid = (u32)(slots[i].key >> 32);

			while (current_plid < slot_plid &&
			       current_plid < parent_count) {
				current_plid++;
				out_parent_index[current_plid] = cpu_to_le32(i);
			}
		}
		while (current_plid < parent_count) {
			current_plid++;
			out_parent_index[current_plid] = cpu_to_le32(nr_slots);
		}
	}
	if (names_bytes)
		memcpy(out_names, names, names_bytes);

	/* Reserve a disjoint range under a short lock, then stage and flush the
	 * base without i_rwsem or i_li_lock. */
	delta_region_bytes = ext5_li_delta_reserve(sb, nr_slots, names_bytes);
	desc = kzalloc(LIDIR_DESC_BYTES, GFP_KERNEL);
	if (!desc) {
		err = -ENOMEM;
		goto out;
	}
	inode_lock(root);
	root_locked = true;
	mutex_lock(&root_ei->i_li_lock);
	li_locked = true;
	if (!(root_ei->i_flags & EXT5_LIDIR_ROOT_FL) ||
	    !atomic_read(&root_ei->i_li_compact_staging) ||
	    rcu_dereference_protected(root_ei->i_stable_blob,
			lockdep_is_held(&root_ei->i_li_lock)) != old_blob ||
	    old_blob->delta.used_bytes < snapshot_used_bytes) {
		err = -EAGAIN;
		goto out;
	}
	stage_base_off = ALIGN(max_t(u64, i_size_read(root),
				     old_delta_off + old_delta_bytes), bs);
	stage_delta_off = ALIGN(stage_base_off + base_buf_size, bs);
	if (stage_delta_off > sb->s_maxbytes ||
	    delta_region_bytes > sb->s_maxbytes - stage_delta_off) {
		err = -EFBIG;
		goto out;
	}
	stage_end = stage_delta_off + delta_region_bytes;
	if (stage_end > i_size_read(root)) {
		handle_t *handle;

		/* Start the journal handle before i_li_lock, as mutation paths do. */
		mutex_unlock(&root_ei->i_li_lock);
		li_locked = false;
		handle = ext5_journal_start(root, EXT5_HT_INODE, 1);
		if (IS_ERR(handle)) {
			err = PTR_ERR(handle);
			goto out;
		}
		mutex_lock(&root_ei->i_li_lock);
		li_locked = true;
		if (!(root_ei->i_flags & EXT5_LIDIR_ROOT_FL) ||
		    !atomic_read(&root_ei->i_li_compact_staging) ||
		    rcu_dereference_protected(root_ei->i_stable_blob,
				lockdep_is_held(&root_ei->i_li_lock)) != old_blob) {
			err = -EAGAIN;
			mutex_unlock(&root_ei->i_li_lock);
			li_locked = false;
			ext5_journal_stop(handle);
			goto out;
		}
		i_size_write(root, stage_end);
		root_ei->i_disksize = stage_end;
		err = ext5_mark_inode_dirty(handle, root);
		mutex_unlock(&root_ei->i_li_lock);
		li_locked = false;
		if (!err)
			err = ext5_journal_stop(handle);
		else
			ext5_journal_stop(handle);
		if (err)
			goto out;
		mutex_lock(&root_ei->i_li_lock);
		li_locked = true;
	}
	mutex_unlock(&root_ei->i_li_lock);
	li_locked = false;
	inode_unlock(root);
	root_locked = false;

	err = ext5_stable_pwrite(root, stage_base_off, base_buf, base_buf_size);
	if (err)
		goto out;
	err = ext5_stable_flush_file(root);
	if (err)
		goto out;

	/* Static descriptor fields do not change during tail catch-up. */
	desc->magic = cpu_to_le32(LIDIR_MAGIC);
	desc->version = cpu_to_le32(LIDIR_VERSION);
	desc->header_bytes = cpu_to_le32(sizeof(*desc));
	desc->flags = 0;
	desc->generation = cpu_to_le64(old_blob->generation + 1);
	memcpy(desc->hash_seed, &old_blob->hash_key,
	       sizeof(desc->hash_seed));
	desc->base_off = cpu_to_le64(stage_base_off);
	desc->base_bytes = cpu_to_le64(base_size);
	desc->base_csum = 0;
	desc->delta_off = cpu_to_le64(stage_delta_off);
	desc->delta_bytes = cpu_to_le64(delta_region_bytes);
	desc->delta_csum = 0;
	desc->entry_count = cpu_to_le32(nr_slots);
	desc->parent_count = cpu_to_le32(parent_count);
	desc->filter_bits = cpu_to_le32(filter_bits);
	desc->radix_count = cpu_to_le32(rs_model ? rs_model->radix_count : 0);
	desc->spline_count = cpu_to_le32(rs_model ? rs_model->point_count : 0);
	desc->spline_epsilon = cpu_to_le32(rs_model ? rs_model->max_error : 0);
	desc->spline_corridor_error = cpu_to_le32(
		rs_model ? rs_model->corridor_error : 0);
	desc->radix_bits = cpu_to_le32(rs_model ? rs_model->radix_bits : 0);
	desc->radix_shift = cpu_to_le32(rs_model ? rs_model->shift_bits : 0);
	desc->model_min_key = cpu_to_le64(rs_model ? rs_model->min_key : 0);
	desc->model_max_key = cpu_to_le64(rs_model ? rs_model->max_key : 0);
	desc->filter_off = cpu_to_le64(off_filter);
	desc->radix_off = cpu_to_le64(off_radix);
	desc->spline_off = cpu_to_le64(off_spline);
	desc->slots_off = cpu_to_le64(off_slots);
	desc->parent_index_off = cpu_to_le64(off_parent_index);
	desc->names_off = cpu_to_le64(off_names);
	desc->names_bytes = cpu_to_le64(names_bytes);
	desc->base_csum = cpu_to_le32(
		crc32_le(0, (const u8 *)base_buf, base_size));

	copied_used_bytes = snapshot_used_bytes;
	copied_next_seq = delta_snapshot.next_seq;
	for (i = 0; i < 8; i++) {
		u64 captured_used, captured_next, new_delta_off;

		/* Copy the post-snapshot tail with appenders briefly excluded. */
		kvfree(tail_buf);
		tail_buf = NULL;
		mutex_lock(&root_ei->i_li_lock);
		li_locked = true;
		if (!(root_ei->i_flags & EXT5_LIDIR_ROOT_FL) ||
		    !atomic_read(&root_ei->i_li_compact_staging) ||
		    rcu_dereference_protected(root_ei->i_stable_blob,
			lockdep_is_held(&root_ei->i_li_lock)) != old_blob ||
		    old_blob->delta.used_bytes < copied_used_bytes) {
			err = -EAGAIN;
			goto out;
		}
		captured_used = old_blob->delta.used_bytes;
		captured_next = old_blob->delta.next_seq;
		if (captured_used - snapshot_used_bytes > delta_region_bytes) {
			err = -EAGAIN;
			goto out;
		}
		tail_bytes = captured_used - copied_used_bytes;
		if (tail_bytes) {
			tail_buf = kvmalloc(tail_bytes, GFP_KERNEL);
			if (!tail_buf) {
				err = -ENOMEM;
				goto out;
			}
			err = ext5_stable_read_range_sparse(root,
				old_delta_off + copied_used_bytes,
				tail_bytes, tail_buf);
			if (err)
				goto out;
		}
		mutex_unlock(&root_ei->i_li_lock);
		li_locked = false;

		if (tail_bytes) {
			u64 staged_used = captured_used - snapshot_used_bytes;
			u8 end_marker[LIDIR_DELTA_END_BYTES] = { 0 };

			new_delta_off = stage_delta_off +
				(copied_used_bytes - snapshot_used_bytes);
			err = ext5_stable_pwrite(root, new_delta_off,
						 tail_buf, tail_bytes);
			if (err)
				goto out;
			/* Terminate the staged log explicitly: reused sparse extents can hold
			 * an old record that looks valid. */
			if (staged_used + LIDIR_DELTA_END_BYTES <=
			    delta_region_bytes) {
				err = ext5_stable_pwrite(root,
					stage_delta_off + staged_used,
					end_marker, sizeof(end_marker));
				if (err)
					goto out;
			}
			err = ext5_stable_flush_file(root);
			if (err)
				goto out;
			copied_used_bytes = captured_used;
			copied_next_seq = captured_next;
			continue;
		}

		/* Parse the new generation now, so no lookup pays first-parse latency. */
		desc->delta_seq_base = cpu_to_le64(delta_snapshot.next_seq - 1);
		spin_lock(&root_ei->i_li_pool_lock);
		desc->inode_pool_start = cpu_to_le32(root_ei->i_li_pool_start);
		desc->inode_pool_count = cpu_to_le32(root_ei->i_li_pool_count);
		desc->inode_pool_used = cpu_to_le32(root_ei->i_li_pool_used);
		spin_unlock(&root_ei->i_li_pool_lock);
		desc->header_csum = cpu_to_le32(
			crc32_le(0, (const u8 *)desc, LIDIR_DESC_CSUM_LEN));
		if (new_blob) {
			ext5_stable_blob_put(new_blob);
			new_blob = NULL;
		}
		err = ext5_stable_parse_desc(root, desc, &new_blob, false);
		if (err)
			goto out;

		/* Reserve journal access and pin the descriptor buffers before the
		 * final cut. Only copying/dirtying fixed-size metadata remains inside
		 * i_li_lock; no valid staged descriptor is written before validation.
		 */
		inode_lock(root);
		root_locked = true;
		publish_handle = ext5_journal_start(root, EXT5_HT_DIR,
						   EXT5_DATA_TRANS_BLOCKS(sb));
		if (IS_ERR(publish_handle)) {
			err = PTR_ERR(publish_handle);
			publish_handle = NULL;
			goto out;
		}
		err = ext5_stable_write_prepare(publish_handle, root,
			(le64_to_cpu(desc->generation) & 1) * LIDIR_DESC_BYTES,
			LIDIR_DESC_BYTES, &desc_write);
		if (err)
			goto out;
		mutex_lock(&root_ei->i_li_lock);
		li_locked = true;
		if (atomic_read(&root_ei->i_li_demote_pending)) {
			err = -EAGAIN;
			goto out;
		}
		if (!(root_ei->i_flags & EXT5_LIDIR_ROOT_FL) ||
		    rcu_dereference_protected(root_ei->i_stable_blob,
			lockdep_is_held(&root_ei->i_li_lock)) != old_blob ||
		    old_blob->delta.used_bytes != copied_used_bytes ||
		    old_blob->delta.next_seq != copied_next_seq) {
			mutex_unlock(&root_ei->i_li_lock);
			li_locked = false;
			ext5_stable_write_release(&desc_write);
			err = ext5_journal_stop(publish_handle);
			publish_handle = NULL;
			if (err)
				goto out;
			inode_unlock(root);
			root_locked = false;
			ext5_stable_blob_put(new_blob);
			new_blob = NULL;
			continue;
		}
		spin_lock(&root_ei->i_li_pool_lock);
		desc->inode_pool_start = cpu_to_le32(root_ei->i_li_pool_start);
		desc->inode_pool_count = cpu_to_le32(root_ei->i_li_pool_count);
		desc->inode_pool_used = cpu_to_le32(root_ei->i_li_pool_used);
		spin_unlock(&root_ei->i_li_pool_lock);
		desc->header_csum = cpu_to_le32(
			crc32_le(0, (const u8 *)desc, LIDIR_DESC_CSUM_LEN));
		ext5_stable_write_copy(&desc_write, desc);
		err = ext5_stable_write_dirty(publish_handle, root, &desc_write);
		/* Once descriptor bytes change, publish the matching in-memory view
		 * even if the journal/device reports an error. Keep the old extents
		 * unless the new generation's durability is confirmed below.
		 */
		spin_lock(&ext5_li_blob_list_lock);
		list_add_tail(&new_blob->cache_list, &ext5_li_blob_list);
		new_blob->cache_listed = true;
		spin_unlock(&ext5_li_blob_list_lock);
		atomic64_set(&root_ei->i_li_refill_start_used_bytes,
			     new_blob->delta.used_bytes);
		rcu_assign_pointer(root_ei->i_stable_blob, new_blob);
		new_blob = NULL; /* installed pointer owns its reference */
		ext5_promote_track_compact_publication();
		atomic64_set(&root_ei->i_li_refill_start_op,
			     ext5_li_op_clock_now_exact(root->i_sb));
		if (root_ei->i_li_active_entries) {
			ext5_promote_track_compact((s64)nr_slots -
						    root_ei->i_li_active_entries);
			root_ei->i_li_active_entries = nr_slots;
		}
		published = true;
		mutex_unlock(&root_ei->i_li_lock);
		li_locked = false;
		{
			int stop_err = ext5_journal_stop(publish_handle);

			if (!err)
				err = stop_err;
		}
		publish_handle = NULL;
		ext5_stable_write_release(&desc_write);
		inode_unlock(root);
		root_locked = false;
		break;
	}
	if (!published) {
		err = -EAGAIN;
		goto out;
	}
	if (!err)
		err = ext5_stable_flush_file(root);

	/* Reclaim the old payload after pre-publication readers drain, outside
	 * namespace locks. */
	ext5_stable_unlist_blob(old_blob);
	synchronize_rcu();
	synchronize_srcu(&ext5_li_lookup_srcu);
	ext5_stable_blob_put(old_blob); /* installed-pointer reference */
	while (refcount_read(&old_blob->refs) != 1)
		cond_resched();
	ext5_stable_unpin_payload_blocks(old_blob);
	if (!err) {
		inode_lock(root);
		(void)ext5_stable_punch_range(root, old_base_off, old_base_bytes);
		(void)ext5_stable_punch_range(root, old_delta_off, old_delta_bytes);
		inode_unlock(root);
	}

out:
	if (li_locked)
		mutex_unlock(&root_ei->i_li_lock);
	if (publish_handle)
		ext5_journal_stop(publish_handle);
	ext5_stable_write_release(&desc_write);
	if (root_locked)
		inode_unlock(root);
	if (new_blob) {
		ext5_stable_blob_put(new_blob);
		new_blob = NULL;
	}
	/* Keep a failed attempt's sparse staging tail: truncating would take the
	 * lifecycle mutex before a journal handle, the wrong order.  The old
	 * descriptor stays authoritative. */
	if (compact_gate && old_blob) {
		mutex_lock(&root_ei->i_li_lock);
		if (rcu_dereference_protected(root_ei->i_stable_blob,
				lockdep_is_held(&root_ei->i_li_lock)) == old_blob)
			old_blob->delta.snapshot_floor_bytes = 0;
		mutex_unlock(&root_ei->i_li_lock);
		atomic_set(&root_ei->i_li_compact_staging, 0);
	}
	kfree(desc);
	kvfree(tail_buf);
	kvfree(slots);
	kvfree(names);
	kvfree(rs_keys);
	sfs_rs_destroy(rs_model);
	vfree(base_buf);
	if (delta_snapshot_valid)
		ext5_stable_delta_free(&delta_snapshot);
	if (blob_pinned)
		ext5_stable_blob_put(old_blob);
	ext5_promote_track_compaction_time(ktime_get_ns() - transition_start_ns);
	return err;
}

/* Demote one branch only when a single non-root PLID holds at least half
 * of the live INSERTs; otherwise demote the region. */
static u32 ext5_stable_dominant_churn_plid(struct inode *root)
{
	struct ext5_stable_blob *blob;
	u32 *counts = NULL;
	u32 parents, best_plid = 0, best = 0, total = 0;
	struct rb_node *node;
	u32 p;

	rcu_read_lock();
	blob = rcu_dereference(EXT5_I(root)->i_stable_blob);
	if (!blob || !refcount_inc_not_zero(&blob->refs)) {
		rcu_read_unlock();
		return 0;
	}
	parents = blob->parent_count;
	rcu_read_unlock();
	counts = kvcalloc(parents, sizeof(*counts), GFP_KERNEL);
	if (!counts)
		goto out;
	spin_lock(&blob->delta.lock);
	for (node = rb_first_cached(&blob->delta.by_key); node;
	     node = rb_next(node)) {
		struct ext5_stable_delta_entry *entry = rb_entry(node,
			struct ext5_stable_delta_entry, rb);

		if (entry->op != LIDIR_DELTA_INSERT || entry->plid >= parents)
			continue;
		counts[entry->plid]++;
		total++;
	}
	spin_unlock(&blob->delta.lock);
	for (p = 1; p < parents; p++) {
		if (counts[p] > best) {
			best = counts[p];
			best_plid = p;
		}
	}
	if (!best || best * 2 < total)
		best_plid = 0;
out:
	kvfree(counts);
	ext5_stable_blob_put(blob);
	return best_plid;
}

static bool ext5_stable_delta_over_watermark(struct inode *root,
					      u64 *generation, bool *urgent)
{
	struct ext5_stable_blob *blob;
	bool over = false;

	if (urgent)
		*urgent = false;

	rcu_read_lock();
	blob = rcu_dereference(EXT5_I(root)->i_stable_blob);
	if (blob) {
		if (generation)
			*generation = READ_ONCE(blob->generation);
		spin_lock(&blob->delta.lock);
		over = blob->delta.trigger_bytes &&
		       blob->delta.used_bytes >= blob->delta.trigger_bytes;
		if (urgent && blob->delta.region_bytes)
			*urgent = blob->delta.used_bytes >=
				blob->delta.region_bytes - blob->delta.region_bytes / 4;
		spin_unlock(&blob->delta.lock);
	}
	rcu_read_unlock();
	return over;
}

/* Freeze the evidence at the first watermark crossing, so later activity
 * cannot change whether this generation filled within T*(R). */
static void ext5_stable_record_compact_crossing(struct inode *root,
						u64 generation)
{
	struct ext5_inode_info *ei = EXT5_I(root);
	struct ext5_stable_blob *blob;
	u64 used = 0, inserts = 0, deletes = 0;
	u64 op = ext5_li_op_clock_now_exact(root->i_sb);
	bool found = false;

	rcu_read_lock();
	blob = rcu_dereference(ei->i_stable_blob);
	if (blob && READ_ONCE(blob->generation) == generation) {
		spin_lock(&blob->delta.lock);
		used = blob->delta.used_bytes;
		inserts = blob->delta.appended_inserts;
		deletes = blob->delta.appended_deletes;
		spin_unlock(&blob->delta.lock);
		found = true;
	}
	rcu_read_unlock();
	if (!found)
		return;
	spin_lock(&ei->i_li_compact_evidence_lock);
	ei->i_li_compact_cross_op = op;
	ei->i_li_compact_cross_used_bytes = used;
	ei->i_li_compact_cross_inserts = inserts;
	ei->i_li_compact_cross_deletes = deletes;
	ei->i_li_compact_cross_generation = generation;
	spin_unlock(&ei->i_li_compact_evidence_lock);
}

/* Wake the worker at most once per installed generation. */
static void ext5_stable_queue_compact(struct inode *root, u64 generation,
				      bool urgent)
{
	struct ext5_inode_info *ei = EXT5_I(root);
	struct ext5_sb_info *sbi = EXT5_SB(root->i_sb);
	u64 observed;
	bool claimed = false;

	if (!generation || atomic_read(&sbi->li_shutting_down))
		return;
	observed = atomic64_read(&ei->i_li_compact_scheduled_generation);
	for (;;) {
		if (observed == generation)
			break;
		if (atomic64_try_cmpxchg(&ei->i_li_compact_scheduled_generation,
					 &observed, generation)) {
			claimed = true;
			break;
		}
	}
	if (claimed)
		ext5_stable_record_compact_crossing(root, generation);
	if (!ei->i_li_state)
		goto drop_claim;
	if (urgent) {
		u64 urgent_generation = atomic64_read(
			&ei->i_li_compact_urgent_generation);

		for (;;) {
			if (urgent_generation == generation) {
				if (!claimed)
					return;
				break;
			}
			if (atomic64_try_cmpxchg(
					&ei->i_li_compact_urgent_generation,
					&urgent_generation, generation))
				break;
		}
		if (claimed)
			ext5_promote_track_compact_wake();
		if (!(sbi->li_compact_wq ?
			ext5_li_work_mod(root, &ei->i_li_compact_work,
					&ei->i_li_state->compact_work_ref,
					sbi->li_compact_wq, 0) :
			ext5_li_work_mod(root, &ei->i_li_compact_work,
					&ei->i_li_state->compact_work_ref,
					system_unbound_wq, 0)))
			goto drop_claim;
		return;
	}
	if (!claimed)
		return;
	ext5_promote_track_compact_wake();
	if (!(sbi->li_compact_wq ?
		ext5_li_work_mod(root, &ei->i_li_compact_work,
				&ei->i_li_state->compact_work_ref,
				sbi->li_compact_wq,
				msecs_to_jiffies(READ_ONCE(ext5_li_compact_idle_ms))) :
		ext5_li_work_mod(root, &ei->i_li_compact_work,
				&ei->i_li_state->compact_work_ref,
				system_unbound_wq,
				msecs_to_jiffies(READ_ONCE(ext5_li_compact_idle_ms)))))
		goto drop_claim;
	return;

drop_claim:
	if (claimed) {
		atomic64_cmpxchg(&ei->i_li_compact_scheduled_generation,
				 generation, 0);
		atomic64_cmpxchg(&ei->i_li_compact_urgent_generation,
				 generation, 0);
	}
}

/* Compaction worker, run when the delta crosses its high-water mark. */
static void __ext5_stable_compact_work(struct work_struct *work);

void ext5_stable_compact_work(struct work_struct *work)
{
	struct ext5_li_inode_state *state = container_of(to_delayed_work(work),
			struct ext5_li_inode_state, compact_work);
	struct inode *owner = state->owner;

	__ext5_stable_compact_work(work);
	ext5_li_work_ref_finish(owner, &state->compact_work,
			&state->compact_work_ref);
}

static void __ext5_stable_compact_work(struct work_struct *work)
{
	struct ext5_li_inode_state *state = container_of(to_delayed_work(work),
		struct ext5_li_inode_state, compact_work);
	struct inode *root = state->owner;
	struct ext5_inode_info *ei = EXT5_I(root);
	bool demote_queued = false;
	bool rescan_allowed = false;
	bool urgent = false;
	u64 scheduled_generation = 0;

	ext5_promote_track_compact_worker_run();
	if (unlikely(atomic_read(&EXT5_SB(root->i_sb)->li_shutting_down)))
		return;
	if (!(ei->i_flags & EXT5_LIDIR_ROOT_FL))
		return;
	if (atomic_read(&ei->i_li_demote_pending)) {
		atomic64_set(&ei->i_li_compact_scheduled_generation, 0);
		atomic64_set(&ei->i_li_compact_urgent_generation, 0);
		return;
	}
	/* A rearm can arrive while the previous run reclaims: recheck the
	 * generation so a stale run never rebuilds an empty delta. */
	if (!ext5_stable_delta_over_watermark(root, &scheduled_generation,
					       &urgent)) {
		u64 retry_generation = 0;
		bool retry_urgent = false;

		ext5_promote_track_compact_stale();
		if (scheduled_generation)
			atomic64_cmpxchg(&ei->i_li_compact_scheduled_generation,
					 scheduled_generation, 0);
		if (scheduled_generation)
			atomic64_cmpxchg(&ei->i_li_compact_urgent_generation,
					 scheduled_generation, 0);
		/* Recheck after the cmpxchg: an appender may have seen the old claim. */
		if (ext5_stable_delta_over_watermark(root, &retry_generation,
							&retry_urgent))
			ext5_stable_queue_compact(root, retry_generation,
						  retry_urgent);
		return;
	}
	if (!urgent && READ_ONCE(ext5_li_compact_idle_ms)) {
		unsigned long due = READ_ONCE(ei->i_li_last_delta_jiffies) +
			msecs_to_jiffies(READ_ONCE(ext5_li_compact_idle_ms));

		if (time_before(jiffies, due) &&
		    !atomic_read(&EXT5_SB(root->i_sb)->li_shutting_down)) {
			ext5_promote_track_compact_idle_deferral();
			mod_delayed_work(EXT5_SB(root->i_sb)->li_compact_wq,
					 &ei->i_li_compact_work,
					 max_t(unsigned long, due - jiffies, 1));
			return;
		}
	}

	/* The work lives in the sidecar and eviction cancels it, so it needs no
	 * inode reference. */
	ext5_promote_track_compact_build_attempt();
	if (ei->i_flags & EXT5_LIDIR_ROOT_FL) {
		unsigned int demote_limit =
			READ_ONCE(ext5_li_demote_refills);
		struct ext5_stable_blob *blob;
		bool insert_dominated = false;
		bool rapid_refill = false;
		bool new_refill = false;
		u32 entries = 0;
		u64 generation = 0;
		u64 used_bytes = 0;
		u64 trigger_bytes = 0;
		u64 inserts = 0, deletes = 0;
		u64 crossing_op = 0;
		u64 refill_span = 0;
		u64 projected_span = 0;
		u64 refill_tstar = 0;

		/* Only insert-dominated refills (1024 excess inserts and a 2:1 ratio)
		 * count toward demotion; a delete-dominated refill empties the region
		 * and just compacts. */
		rcu_read_lock();
		blob = rcu_dereference(ei->i_stable_blob);
		if (blob) {
			spin_lock(&blob->delta.lock);
			inserts = blob->delta.appended_inserts;
			deletes = blob->delta.appended_deletes;
			used_bytes = blob->delta.used_bytes;
			trigger_bytes = blob->delta.trigger_bytes;
			spin_unlock(&blob->delta.lock);
			entries = READ_ONCE(blob->entry_count);
			generation = READ_ONCE(blob->generation);
			insert_dominated = inserts >= deletes + 1024 &&
					   inserts > deletes * 2;
		}
		rcu_read_unlock();
		spin_lock(&ei->i_li_compact_evidence_lock);
		if (ei->i_li_compact_cross_generation == generation) {
			crossing_op = ei->i_li_compact_cross_op;
			used_bytes = ei->i_li_compact_cross_used_bytes;
			inserts = ei->i_li_compact_cross_inserts;
			deletes = ei->i_li_compact_cross_deletes;
			insert_dominated = inserts >= deletes + 1024 &&
					   inserts > deletes * 2;
		}
		spin_unlock(&ei->i_li_compact_evidence_lock);

		if (insert_dominated && entries) {
			u64 start = atomic64_read(&ei->i_li_refill_start_op);
			u64 start_used = atomic64_read(
				&ei->i_li_refill_start_used_bytes);
			u64 now = crossing_op ? crossing_op :
				ext5_li_op_clock_now_exact(root->i_sb);
			u64 appended_bytes = used_bytes > start_used ?
				used_bytes - start_used : 0;

			refill_span = now > start ? now - start : 0;
			refill_tstar = ext5_li_t_star_for_entries(root->i_sb,
							      entries);
			if (start && appended_bytes && trigger_bytes) {
				projected_span = mul_u64_u64_div_u64(refill_span,
					trigger_bytes, appended_bytes);
				rapid_refill = projected_span <= refill_tstar;
			}
		}
		if (generation) {
			u64 old = atomic64_read(
				&ei->i_li_refill_classified_generation);

			if (old != generation &&
			    atomic64_cmpxchg(&ei->i_li_refill_classified_generation,
					     old, generation) == old)
				new_refill = true;
		}
		if (new_refill) {
			atomic64_set(&ei->i_li_last_refill_span_ops, refill_span);
			atomic64_set(&ei->i_li_last_refill_projected_ops,
				     projected_span);
			atomic64_set(&ei->i_li_last_refill_tstar_ops,
				     refill_tstar);
			if (insert_dominated && rapid_refill) {
				atomic_set(&ei->i_li_last_refill_class, 1);
				ext5_promote_track_rapid_insert_refill();
				atomic_inc(&ei->i_li_compact_count);
			} else {
				if (insert_dominated) {
					atomic_set(&ei->i_li_last_refill_class, 2);
					ext5_promote_track_slow_insert_refill();
				} else {
					atomic_set(&ei->i_li_last_refill_class, 3);
					ext5_promote_track_noninsert_refill();
				}
				atomic_set(&ei->i_li_compact_count, 0);
			}
		}

		/* Demote through the serialized transition queue, never inline:
		 * transitions must not publish concurrently. */
		if (demote_limit && insert_dominated && rapid_refill &&
		    atomic_read(&ei->i_li_compact_count) >= demote_limit) {
			u32 plid = ext5_stable_dominant_churn_plid(root);

			if (!ext5_schedule_demote_plid_idle(root, plid))
				demote_queued = true;
		} else {
			int err = ext5_stable_compact(root);

			rescan_allowed = !err || err == -EAGAIN;
			if (err && err != -EAGAIN)
				atomic64_cmpxchg(
					&ei->i_li_compact_scheduled_generation,
					scheduled_generation, 0);
			if (err && err != -EAGAIN)
				atomic64_cmpxchg(
					&ei->i_li_compact_urgent_generation,
					scheduled_generation, 0);

			/* The tail catch-up yielded to sustained mutation: retry later. */
			if (err == -EAGAIN &&
			    !atomic_read(&EXT5_SB(root->i_sb)->li_shutting_down) &&
			    (ei->i_flags & EXT5_LIDIR_ROOT_FL))
				mod_delayed_work(
					EXT5_SB(root->i_sb)->li_compact_wq,
					&ei->i_li_compact_work,
					msecs_to_jiffies(READ_ONCE(
						ext5_li_compact_idle_ms)));
		}
	}
	/* A refill during reclamation must still get a later callback. */
	if (!demote_queued && rescan_allowed &&
	    (ei->i_flags & EXT5_LIDIR_ROOT_FL)) {
		u64 retry_generation = 0;
		bool retry_urgent = false;

		if (ext5_stable_delta_over_watermark(root, &retry_generation,
							&retry_urgent)) {
			ext5_promote_track_compact_rescan();
			ext5_stable_queue_compact(root, retry_generation,
						  retry_urgent);
		}
	}
}

/* Per-plid bucket. */
struct demote_dir {
	struct inode *inode;	/* iget'd ref; inode_lock held during demote */
	struct inode *shadow;	/* hidden conventional representation */
	u32 shadow_csum_seed;
	u32 parent_plid;	/* parent plid; UINT_MAX for root */
	struct ext5_dir_bulk_entry *entries;
	u32 nr_entries, cap_entries;
	u64 subtree_entries;
	u64 subtree_dirs;
};

struct demote_ctx {
	struct super_block *sb;
	struct demote_dir *dirs;
	u32 nr_dirs;
	u32 *walk;
	u32 nr_walk;
	struct inode *external_parent;
	u32 target_plid;
	char *names;
	u32 names_off, cap_names;
};

/* Rebuild the per-directory mutable counts on demotion; otherwise later
 * unlinks subtract from stale pre-promotion counters. */
static void demote_restore_policy_state(struct demote_ctx *ctx,
					const bool *selected, bool full_root)
{
	u64 now = ext5_li_op_clock_now_exact(ctx->sb);
	u32 floor = ext5_li_effective_size_floor(ctx->sb);
	u32 p;

	for (p = 0; p < ctx->nr_dirs; p++) {
		struct demote_dir *dir = &ctx->dirs[p];
		u32 e;

		dir->subtree_entries = dir->nr_entries;
		dir->subtree_dirs = 0;
		for (e = 0; e < dir->nr_entries; e++)
			if (dir->entries[e].file_type == DT_DIR)
				dir->subtree_dirs++;
	}
	/* The captured breadth-first walk finishes children before parents. */
	for (p = ctx->nr_walk; p-- > 1;) {
		struct demote_dir *dir = &ctx->dirs[ctx->walk[p]];
		u32 parent = dir->parent_plid;

		if (!dir->inode || parent >= ctx->nr_dirs)
			continue;
		ctx->dirs[parent].subtree_entries += dir->subtree_entries;
		ctx->dirs[parent].subtree_dirs += dir->subtree_dirs;
	}
	for (p = 0; p < ctx->nr_dirs; p++) {
		struct demote_dir *dir = &ctx->dirs[p];
		struct ext5_inode_info *ei;
		bool converted = full_root ? dir->inode != NULL :
			selected && selected[p];

		if (!converted)
			continue;
		ei = EXT5_I(dir->inode);
		ext5_li_policy_forget_inode(dir->inode);
		atomic_set(&ei->i_subtree_files,
			   min_t(u64, dir->subtree_entries, INT_MAX));
		atomic_set(&ei->i_subtree_dirs,
			   min_t(u64, dir->subtree_dirs, INT_MAX));
		WRITE_ONCE(ei->i_li_last_mut, now);
		WRITE_ONCE(ei->i_li_armed,
			   dir->subtree_entries >= floor ? 1 : 0);
	}
}

static int demote_grow_names(struct demote_ctx *c, u32 add)
{
	u32 need = c->names_off + add;
	u32 newcap;
	char *grown;

	if (need <= c->cap_names)
		return 0;
	newcap = c->cap_names ? c->cap_names * 2 : 4096;
	while (newcap < need)
		newcap *= 2;
	grown = kvmalloc(newcap, GFP_KERNEL);
	if (!grown)
		return -ENOMEM;
	if (c->names_off)
		memcpy(grown, c->names, c->names_off);
	kvfree(c->names);
	c->names = grown;
	c->cap_names = newcap;
	return 0;
}

static int demote_grow_entries(struct demote_dir *d, u32 add)
{
	u32 need = d->nr_entries + add;
	u32 newcap;
	struct ext5_dir_bulk_entry *grown;

	if (need <= d->cap_entries)
		return 0;
	newcap = d->cap_entries ? d->cap_entries * 2 : 64;
	while (newcap < need)
		newcap *= 2;
	grown = kvmalloc_array(newcap, sizeof(*grown), GFP_KERNEL);
	if (!grown)
		return -ENOMEM;
	if (d->nr_entries)
		memcpy(grown, d->entries, d->nr_entries * sizeof(*grown));
	kvfree(d->entries);
	d->entries = grown;
	d->cap_entries = newcap;
	return 0;
}

static int demote_append_entry(struct demote_ctx *c, u32 plid,
			       const char *name, u8 name_len,
			       u32 ino, u8 file_type, u32 dx_hash)
{
	struct demote_dir *d = &c->dirs[plid];
	int err;

	err = demote_grow_entries(d, 1);
	if (err)
		return err;
	err = demote_grow_names(c, name_len);
	if (err)
		return err;
	memcpy(c->names + c->names_off, name, name_len);
	d->entries[d->nr_entries].ino = ino;
	d->entries[d->nr_entries].dx_hash = dx_hash;
	d->entries[d->nr_entries].file_type = file_type;
	d->entries[d->nr_entries].name_len = name_len;
	d->entries[d->nr_entries].name_off = c->names_off;
	d->nr_entries++;
	c->names_off += name_len;
	return 0;
}

/* Base and delta share the (PLID, hash32) order: advance one delta cursor
 * with the base scan and compute the full tiebreak only on a key match. */
static struct ext5_stable_delta_entry *
demote_delta_match_base(const struct ext5_stable_blob *blob,
			u32 plid, u32 hash32, const char *name, u8 name_len,
			struct ext5_stable_delta_entry **cursor)
{
	struct ext5_stable_delta_entry *de = *cursor;
	u64 tiebreak64;

	while (de && (de->plid < plid ||
	       (de->plid == plid && de->hash32 < hash32))) {
		struct rb_node *next = rb_next(&de->rb);

		de = next ? rb_entry(next, struct ext5_stable_delta_entry, rb) :
			NULL;
	}
	*cursor = de;
	if (!de || de->plid != plid || de->hash32 != hash32)
		return NULL;

	tiebreak64 = ext5_stable_tiebreak64(blob, name, name_len, hash32);
	while (de && de->plid == plid && de->hash32 == hash32) {
		if (de->tiebreak64 == tiebreak64 && de->name_len == name_len &&
		    !memcmp(de->name, name, name_len))
			return de;
		{
			struct rb_node *next = rb_next(&de->rb);

			de = next ? rb_entry(next,
				struct ext5_stable_delta_entry, rb) : NULL;
		}
	}
	return NULL;
}

/* Append one PLID's live entries to ctx->dirs[plid].  `delta` is a
 * private clone, so no lock is needed. */
static int demote_capture_plid(struct demote_ctx *c,
			       const struct ext5_stable_blob *blob,
			       const struct ext5_stable_delta *delta,
			       const struct lidir_disk_slot *tmp_slots,
			       const char *tmp_names, u32 plid)
{
	u32 slot_lo, slot_hi;
	struct ext5_stable_delta_entry *de;
	struct ext5_stable_delta_entry *cursor;
	u32 b;
	int err;

	err = ext5_stable_parent_range(blob, plid, &slot_lo, &slot_hi);
	if (err)
		return err;
	cursor = ext5_stable_delta_lower_bound(
		(struct ext5_stable_delta *)delta, plid, 0, 0);

	for (b = slot_lo; b < slot_hi; b++) {
		const struct lidir_disk_slot *s = &tmp_slots[b];
		u32 name_off = le32_to_cpu(s->name_off);
		u8 name_len = s->name_len;
		const char *name;
		u32 hash32;

		if (name_off > blob->names_bytes ||
		    name_len > blob->names_bytes - name_off)
			return -EIO;
		name = tmp_names + name_off;
		hash32 = (u32)le64_to_cpu(s->key);
		de = demote_delta_match_base(blob, plid, hash32,
					     name, name_len, &cursor);
		if (de) {
			if (de->op == LIDIR_DELTA_DELETE)
				continue;
			err = demote_append_entry(c, plid, name, name_len,
				de->ino, de->file_type, le32_to_cpu(s->dx_hash));
		} else {
			err = demote_append_entry(c, plid, name, name_len,
				le32_to_cpu(s->ino), s->file_type,
				le32_to_cpu(s->dx_hash));
		}
		if (err)
			return err;
	}

	/* This PLID's delta INSERTs are sorted: binary-search the base for
	 * membership. */
	de = ext5_stable_delta_lower_bound(
		(struct ext5_stable_delta *)delta, plid, 0, 0);
	while (de && de->plid == plid) {
		struct rb_node *next = rb_next(&de->rb);

		if (de->op == LIDIR_DELTA_INSERT &&
		    !ext5_stable_base_has(tmp_slots + slot_lo,
					  slot_hi - slot_lo, tmp_names,
					  blob->names_bytes,
					  ext5_stable_make_key(plid, de->hash32),
					  de->name, de->name_len)) {
			u32 dx_hash;

			err = ext5_stable_dx_hash(blob, de->name, de->name_len,
						  &dx_hash);
			if (!err)
				err = demote_append_entry(c, plid, de->name,
					de->name_len, de->ino, de->file_type,
					dx_hash);
			if (err)
				return err;
		}
		de = next ? rb_entry(next, struct ext5_stable_delta_entry, rb) :
			NULL;
	}
	return 0;
}

/* Capture one PLID from the installed payload, so a branch demotion reads
 * only its ranges.  The pinned blob keeps these reads valid. */
static int demote_capture_plid_direct(struct demote_ctx *c,
			       const struct ext5_stable_blob *blob,
			       const struct ext5_stable_delta *delta,
			       u32 plid)
{
	u32 slot_lo, slot_hi;
	struct ext5_stable_delta_entry *de;
	struct ext5_stable_delta_entry *cursor;
	u32 b;
	int err;

	err = ext5_stable_parent_range(blob, plid, &slot_lo, &slot_hi);
	if (err)
		return err;
	if (slot_hi < slot_lo || slot_hi > blob->entry_count)
		return -EIO;
	cursor = ext5_stable_delta_lower_bound(
		(struct ext5_stable_delta *)delta, plid, 0, 0);

	for (b = slot_lo; b < slot_hi; b++) {
		struct lidir_disk_slot slot;
		char name[EXT5_NAME_LEN];
		u32 name_off, hash32;
		u8 name_len;

		err = ext5_stable_read_slot(blob, b, &slot);
		if (err)
			return err;
		name_off = le32_to_cpu(slot.name_off);
		name_len = slot.name_len;
		if (!name_len || name_off > blob->names_bytes ||
		    name_len > blob->names_bytes - name_off)
			return -EIO;
		err = ext5_stable_read_name(blob, name_off, name_len, name);
		if (err)
			return err;
		hash32 = (u32)le64_to_cpu(slot.key);
		de = demote_delta_match_base(blob, plid, hash32,
					     name, name_len, &cursor);
		if (de) {
			if (de->op == LIDIR_DELTA_DELETE)
				continue;
			err = demote_append_entry(c, plid, name, name_len,
						  de->ino, de->file_type,
						  le32_to_cpu(slot.dx_hash));
		} else {
			err = demote_append_entry(c, plid, name, name_len,
						  le32_to_cpu(slot.ino),
						  slot.file_type,
						  le32_to_cpu(slot.dx_hash));
		}
		if (err)
			return err;
	}

	/* Add delta INSERTs absent from the base, starting at this PLID. */
	de = ext5_stable_delta_lower_bound(
		(struct ext5_stable_delta *)delta, plid, 0, 0);
	while (de && de->plid == plid) {
		struct rb_node *next = rb_next(&de->rb);

		if (de->op == LIDIR_DELTA_INSERT) {
			struct qstr child = QSTR_INIT(de->name, de->name_len);
			ino_t base_ino;

			err = ext5_stable_lookup_rs(blob, plid,
				ext5_stable_make_key(plid, de->hash32),
				&child, &base_ino);
			if (err && err != -ENOENT)
				return err;
			if (err == -ENOENT) {
				u32 dx_hash;

				err = ext5_stable_dx_hash(blob, de->name,
							  de->name_len, &dx_hash);
				if (!err)
					err = demote_append_entry(c, plid,
						de->name, de->name_len, de->ino,
						de->file_type, dx_hash);
				if (err)
					return err;
			}
		}
		de = next ? rb_entry(next, struct ext5_stable_delta_entry, rb) :
			NULL;
	}
	return 0;
}

static int demote_discover_direct_children(struct demote_ctx *c,
					    struct inode *root, u32 plid,
					    bool *selected)
{
	struct demote_dir *dir = &c->dirs[plid];
	u32 e;

	for (e = 0; e < dir->nr_entries; e++) {
		const struct ext5_dir_bulk_entry *entry = &dir->entries[e];
		struct ext5_inode_info *cei;
		struct inode *child;
		u32 child_plid;

		if (entry->file_type != DT_DIR)
			continue;
		child = ext5_iget(root->i_sb, entry->ino, EXT5_IGET_NORMAL);
		if (IS_ERR(child))
			return PTR_ERR(child);
		cei = EXT5_I(child);
		if (!(cei->i_flags & EXT5_LIDIR_INTERIOR_FL) ||
		    cei->i_subtree_root_ino != root->i_ino) {
			iput(child);
			continue;
		}
		child_plid = cei->i_parent_local_id_in_subtree;
		if (!child_plid || child_plid >= c->nr_dirs ||
		    c->dirs[child_plid].inode) {
			iput(child);
			return -EIO;
		}
		c->dirs[child_plid].inode = child;
		c->dirs[child_plid].parent_plid = plid;
		c->walk[c->nr_walk++] = child_plid;
		selected[child_plid] = true;
	}
	return 0;
}

/* Prepare a rename-requested branch by following its current child links. */
static int demote_prepare_direct_branch(struct demote_ctx *c,
					 const struct ext5_stable_blob *blob,
					 const struct ext5_stable_delta *delta,
					 struct inode *root, u32 target_plid,
					 ino_t target_ino, ino_t parent_ino,
					 u64 parent_version,
					 bool *selected)
{
	struct inode *target, *parent;
	u32 parent_plid;
	u32 p, start;
	int err;

	if (!target_plid || target_plid >= c->nr_dirs)
		return -EAGAIN;
	target = ext5_iget(root->i_sb, target_ino, EXT5_IGET_NORMAL);
	if (IS_ERR(target))
		return PTR_ERR(target);
	if (!(EXT5_I(target)->i_flags & EXT5_LIDIR_INTERIOR_FL) ||
	    EXT5_I(target)->i_subtree_root_ino != root->i_ino ||
	    EXT5_I(target)->i_parent_local_id_in_subtree != target_plid) {
		iput(target);
		return -EAGAIN;
	}

	parent = ext5_iget(root->i_sb, parent_ino, EXT5_IGET_NORMAL);
	if (IS_ERR(parent)) {
		iput(target);
		return PTR_ERR(parent);
	}
	if (parent == root) {
		if (inode_query_iversion(parent) != parent_version) {
			iput(parent);
			iput(target);
			return -EAGAIN;
		}
		parent_plid = 0;
		iput(parent);
		parent = NULL;
	} else if ((EXT5_I(parent)->i_flags & EXT5_LIDIR_INTERIOR_FL) &&
		   EXT5_I(parent)->i_subtree_root_ino == root->i_ino) {
		if (inode_query_iversion(parent) != parent_version) {
			iput(parent);
			iput(target);
			return -EAGAIN;
		}
		parent_plid = EXT5_I(parent)->i_parent_local_id_in_subtree;
		if (!parent_plid || parent_plid == target_plid ||
		    parent_plid >= c->nr_dirs ||
		    c->dirs[parent_plid].inode) {
			iput(parent);
			iput(target);
			return -EAGAIN;
		}
		c->dirs[parent_plid].inode = parent;
		c->walk[c->nr_walk++] = parent_plid;
		parent = NULL;
	} else if (S_ISDIR(parent->i_mode)) {
		parent_plid = UINT_MAX;
		c->external_parent = parent;
		parent = NULL;
	} else {
		iput(parent);
		iput(target);
		return -EAGAIN;
	}

	c->dirs[target_plid].inode = target;
	c->dirs[target_plid].parent_plid = parent_plid;
	c->target_plid = target_plid;
	selected[target_plid] = true;
	start = c->nr_walk;
	c->walk[c->nr_walk++] = target_plid;
	for (p = start; p < c->nr_walk; p++) {
		u32 plid = c->walk[p];

		err = demote_capture_plid_direct(c, blob, delta, plid);
		if (err)
			return err;
		err = demote_discover_direct_children(c, root, plid, selected);
		if (err)
			return err;
	}
	return 0;
}

/* Record every STABLE_INTERIOR child directory of this root in
 * ctx->dirs; dirs[0] is the root. */
static int demote_resolve_interior_inodes(struct demote_ctx *c,
					  struct inode *root)
{
	u32 i;

	for (i = 0; i < c->nr_walk; i++) {
		u32 p = c->walk[i];
		struct demote_dir *d = &c->dirs[p];
		u32 e;

		/* A branch-demoted PLID is a dead range: its directory now owns the
		 * descendants. */
		if (p && !d->inode)
			continue;

		for (e = 0; e < d->nr_entries; e++) {
			const struct ext5_dir_bulk_entry *en = &d->entries[e];
			struct inode *child;
			struct ext5_inode_info *cei;
			u32 cplid;

			if (en->file_type != DT_DIR)
				continue;
			child = ext5_iget(root->i_sb, en->ino,
					  EXT5_IGET_NORMAL);
			if (IS_ERR(child))
				return PTR_ERR(child);
			cei = EXT5_I(child);
			if (!(cei->i_flags & EXT5_LIDIR_INTERIOR_FL) ||
			    cei->i_subtree_root_ino != root->i_ino) {
				/* Not an interior of this subtree: a new mutable subdirectory or a
				 * boundary. */
				iput(child);
				continue;
			}
			cplid = cei->i_parent_local_id_in_subtree;
			if (cplid == 0 || cplid >= c->nr_dirs ||
			    c->dirs[cplid].inode) {
				/* Bogus or duplicate. */
				iput(child);
				return -EIO;
			}
			c->dirs[cplid].inode = child;	/* keep ref */
			c->dirs[cplid].parent_plid = p;
			c->walk[c->nr_walk++] = cplid;
		}
	}
	return 0;
}

static void demote_ctx_release(struct demote_ctx *c)
{
	u32 p;

	if (!c->dirs) {
		kvfree(c->walk);
		return;
	}
	/* Start at p = 0: dirs[0] holds a reference to the root too. */
	for (p = 0; p < c->nr_dirs; p++) {
		if (c->dirs[p].inode)
			iput(c->dirs[p].inode);
		kvfree(c->dirs[p].entries);
	}
	kvfree(c->dirs);
	kvfree(c->walk);
	if (c->external_parent)
		iput(c->external_parent);
	kvfree(c->names);
	memset(c, 0, sizeof(*c));
}

static void demote_clear_captured_entries(struct demote_ctx *c)
{
	u32 p;

	for (p = 0; p < c->nr_dirs; p++) {
		kvfree(c->dirs[p].entries);
		c->dirs[p].entries = NULL;
		c->dirs[p].nr_entries = 0;
		c->dirs[p].cap_entries = 0;
	}
	c->names_off = 0;
}

/* Cross-parent rename is excluded while acquiring the captured parent-first
 * lock set. Publication still validates the source sequence afterwards.
 */
static void demote_lock_interiors(struct demote_ctx *c)
{
	u32 p;

	for (p = 1; p < c->nr_walk; p++)
		if (c->dirs[c->walk[p]].inode)
			down_write_nest_lock(&c->dirs[c->walk[p]].inode->i_rwsem,
					     &EXT5_SB(c->sb)->li_transition_lock);
}

static void demote_unlock_interiors(struct demote_ctx *c)
{
	u32 p;

	for (p = c->nr_walk; p > 1; p--)
		if (c->dirs[c->walk[p - 1]].inode)
			inode_unlock(c->dirs[c->walk[p - 1]].inode);
}

static void demote_discard_shadow(struct inode *shadow, u32 saved_csum_seed);

static int demote_shadow_retarget_dot(struct inode *shadow, ino_t ino)
{
	struct buffer_head *bh;
	struct ext5_dir_entry_2 *dot;
	handle_t *handle;
	int err;

	handle = ext5_journal_start(shadow, EXT5_HT_DIR, 3);
	if (IS_ERR(handle))
		return PTR_ERR(handle);
	bh = ext5_bread(handle, shadow, 0, 0);
	if (IS_ERR(bh)) {
		err = PTR_ERR(bh);
		ext5_journal_stop(handle);
		return err;
	}
	if (!bh) {
		ext5_journal_stop(handle);
		return -EIO;
	}
	err = ext5_journal_get_write_access(handle, shadow->i_sb, bh,
					   EXT5_JTR_NONE);
	if (!err) {
		dot = (struct ext5_dir_entry_2 *)bh->b_data;
		dot->inode = cpu_to_le32((u32)ino);
		err = ext5_handle_dirty_dirblock(handle, shadow, bh);
	}
	brelse(bh);
	if (!err)
		err = ext5_mark_inode_dirty(handle, shadow);
	ext5_journal_stop(handle);
	return err;
}

static struct inode *demote_create_shadow(struct inode *original,
					  struct inode *parent,
					  const struct demote_dir *contents,
					  const char *names, u32 names_bytes,
					  u32 *saved_csum_seed)
{
	struct inode *shadow;
	handle_t *handle;
	uid_t owner[2] = { i_uid_read(original), i_gid_read(original) };
	int credits = EXT5_DATA_TRANS_BLOCKS(original->i_sb) +
		EXT5_INDEX_EXTRA_TRANS_BLOCKS + 8;
	int err;

	handle = ext5_journal_start(original, EXT5_HT_DIR, credits);
	if (IS_ERR(handle))
		return ERR_CAST(handle);
	shadow = ext5_new_inode(handle, original, S_IFDIR | 0700, NULL,
				original->i_ino, owner, 0);
	if (IS_ERR(shadow)) {
		err = PTR_ERR(shadow);
		ext5_journal_stop(handle);
		return ERR_PTR(err);
	}
	*saved_csum_seed = EXT5_I(shadow)->i_csum_seed;
	if (!projid_eq(EXT5_I(shadow)->i_projid,
		       EXT5_I(original)->i_projid)) {
		err = -EOPNOTSUPP;
		goto fail_new;
	}
	EXT5_I(shadow)->i_csum_seed = EXT5_I(original)->i_csum_seed;
	shadow->i_op = &ext5_dir_inode_operations;
	shadow->i_fop = &ext5_dir_operations;
	/* The immutable snapshot is rebuilt directly into directory blocks. */
	ext5_clear_inode_state(shadow, EXT5_STATE_MAY_INLINE_DATA);
	err = ext5_init_new_dir(handle, parent, shadow);
	if (!err) {
		/* Orphan-list the shadow before the multi-transaction build, so a crash
		 * cannot leak it. */
		clear_nlink(shadow);
		err = ext5_orphan_add(handle, shadow);
	}
	if (!err)
		err = ext5_mark_inode_dirty(handle, shadow);
	if (err) {
		clear_nlink(shadow);
		ext5_orphan_add(handle, shadow);
		ext5_mark_inode_dirty(handle, shadow);
		ext5_journal_stop(handle);
		unlock_new_inode(shadow);
		iput(shadow);
		return ERR_PTR(err);
	}
	err = ext5_journal_stop(handle);
	unlock_new_inode(shadow);
	if (err) {
		demote_discard_shadow(shadow, *saved_csum_seed);
		return ERR_PTR(err);
	}

	inode_lock(shadow);
	/* Retarget `.` while block zero is still linear. */
	err = demote_shadow_retarget_dot(shadow, original->i_ino);
	if (!err)
		err = ext5_dir_rebuild_bulk(shadow, contents->entries,
					   contents->nr_entries, names,
					   names_bytes);
	inode_unlock(shadow);
	if (err) {
		demote_discard_shadow(shadow, *saved_csum_seed);
		return ERR_PTR(err);
	}
	return shadow;

fail_new:
	clear_nlink(shadow);
	ext5_orphan_add(handle, shadow);
	ext5_mark_inode_dirty(handle, shadow);
	ext5_journal_stop(handle);
	unlock_new_inode(shadow);
	iput(shadow);
	return ERR_PTR(err);
}

static void demote_discard_shadow(struct inode *shadow, u32 saved_csum_seed)
{
	handle_t *handle;
	int err;

	if (!shadow)
		return;
	inode_lock(shadow);
	/* Free the tree under the namespace inode's seed, then restore the hidden
	 * inode's seed for its final checksum. */
	truncate_setsize(shadow, 0);
	err = ext5_truncate(shadow);
	truncate_inode_pages(shadow->i_mapping, 0);
	if (!err)
		EXT5_I(shadow)->i_csum_seed = saved_csum_seed;
	handle = ext5_journal_start(shadow, EXT5_HT_INODE,
		EXT5_DATA_TRANS_BLOCKS(shadow->i_sb) + 4);
	if (!IS_ERR(handle)) {
		clear_nlink(shadow);
		ext5_orphan_add(handle, shadow);
		inode_set_ctime_current(shadow);
		ext5_mark_inode_dirty(handle, shadow);
		ext5_journal_stop(handle);
	} else {
		clear_nlink(shadow);
	}
	inode_unlock(shadow);
	iput(shadow);
}

static void demote_discard_shadows(struct demote_ctx *ctx)
{
	u32 p;

	if (!ctx->dirs)
		return;
	for (p = 0; p < ctx->nr_dirs; p++) {
		demote_discard_shadow(ctx->dirs[p].shadow,
				       ctx->dirs[p].shadow_csum_seed);
		ctx->dirs[p].shadow = NULL;
	}
}

static void demote_clear_cached_root(struct ext5_inode_info *ei)
{
	struct inode *cached;

	mutex_lock(&ei->i_li_lock);
	cached = rcu_dereference_protected(ei->i_subtree_root_inode,
					   lockdep_is_held(&ei->i_li_lock));
	RCU_INIT_POINTER(ei->i_subtree_root_inode, NULL);
	mutex_unlock(&ei->i_li_lock);
	if (cached)
		iput(cached);
}

static void demote_memswap(void *left, void *right, size_t bytes)
{
	u8 *a = left, *b = right;

	while (bytes--) {
		swap(*a, *b);
		a++;
		b++;
	}
}

/* Give an interior the shadow's conventional tree.  The interior held only
 * routing words, so nothing is freed. */
static int demote_publish_interior_shadow(struct inode *inode,
					  struct inode *shadow,
					  u32 shadow_csum_seed)
{
	struct ext5_inode_info *ei = EXT5_I(inode);
	struct ext5_inode_info *sei = EXT5_I(shadow);
	handle_t *handle;
	u32 mapping_flags;
	int err;

	down_write_nest_lock(&shadow->i_rwsem,
			     &EXT5_SB(inode->i_sb)->li_transition_lock);
	handle = ext5_journal_start(inode, EXT5_HT_MOVE_EXTENTS, 8);
	if (IS_ERR(handle)) {
		inode_unlock(shadow);
		return PTR_ERR(handle);
	}
	ext5_double_down_write_data_sem(inode, shadow);
	mapping_flags = sei->i_flags & (EXT5_EXTENTS_FL | EXT5_INDEX_FL);
	memcpy(ei->i_data, sei->i_data, sizeof(ei->i_data));
	memset(sei->i_data, 0, sizeof(sei->i_data));
	i_size_write(inode, i_size_read(shadow));
	ei->i_disksize = sei->i_disksize;
	inode->i_blocks = shadow->i_blocks;
	inode->i_bytes = shadow->i_bytes;
	i_size_write(shadow, 0);
	sei->i_disksize = 0;
	shadow->i_blocks = 0;
	shadow->i_bytes = 0;
	ei->i_flags &= ~(EXT5_LIDIR_INTERIOR_FL |
			 EXT5_EXTENTS_FL | EXT5_INDEX_FL);
	ei->i_flags |= mapping_flags;
	sei->i_flags &= ~EXT5_INDEX_FL;
	sei->i_flags |= EXT5_EXTENTS_FL;
	ext5_ext_tree_init(handle, shadow);
	sei->i_csum_seed = shadow_csum_seed;
	ei->i_subtree_root_ino = 0;
	ei->i_parent_local_id_in_subtree = 0;
	atomic_set(&ei->i_li_rehome_pending, 0);
	ext5_es_remove_extent(inode, 0, EXT_MAX_BLOCKS);
	ext5_es_remove_extent(shadow, 0, EXT_MAX_BLOCKS);
	inode_inc_iversion(inode);
	inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
	err = ext5_mark_inode_dirty(handle, inode);
	if (!err)
		err = ext5_mark_inode_dirty(handle, shadow);
	ext5_double_up_write_data_sem(inode, shadow);
	inode_unlock(shadow);
	if (!err)
		err = ext5_journal_stop(handle);
	else
		ext5_journal_stop(handle);
	if (!err)
		demote_clear_cached_root(ei);
	return err;
}

/* Swap the conventional shadow with the stable root's extents; the shadow
 * keeps the retired blob until old readers drain. */
static int demote_publish_root_shadow(struct inode *root,
				      struct inode *shadow,
				      bool *published)
{
	struct ext5_inode_info *ei = EXT5_I(root);
	struct ext5_inode_info *sei = EXT5_I(shadow);
	handle_t *handle;
	loff_t size;
	blkcnt_t blocks;
	unsigned short bytes;
	u32 flags;
	int err;

	*published = false;
	down_write_nest_lock(&shadow->i_rwsem,
			     &EXT5_SB(root->i_sb)->li_transition_lock);
	handle = ext5_journal_start(root, EXT5_HT_MOVE_EXTENTS, 8);
	if (IS_ERR(handle)) {
		inode_unlock(shadow);
		return PTR_ERR(handle);
	}
	ext5_double_down_write_data_sem(root, shadow);
	demote_memswap(ei->i_data, sei->i_data, sizeof(ei->i_data));
	size = i_size_read(root);
	i_size_write(root, i_size_read(shadow));
	i_size_write(shadow, size);
	swap(ei->i_disksize, sei->i_disksize);
	blocks = root->i_blocks;
	root->i_blocks = shadow->i_blocks;
	shadow->i_blocks = blocks;
	bytes = root->i_bytes;
	root->i_bytes = shadow->i_bytes;
	shadow->i_bytes = bytes;
	flags = ei->i_flags & (EXT5_EXTENTS_FL | EXT5_INDEX_FL);
	ei->i_flags = (ei->i_flags & ~(EXT5_LIDIR_ROOT_FL |
			  EXT5_EXTENTS_FL | EXT5_INDEX_FL)) |
		(sei->i_flags & (EXT5_EXTENTS_FL | EXT5_INDEX_FL));
	sei->i_flags = (sei->i_flags & ~(EXT5_EXTENTS_FL | EXT5_INDEX_FL)) |
		flags;
	ext5_es_remove_extent(root, 0, EXT_MAX_BLOCKS);
	ext5_es_remove_extent(shadow, 0, EXT_MAX_BLOCKS);
	ei->i_li_pool_start = 0;
	ei->i_li_pool_count = 0;
	ei->i_li_pool_used = 0;
	WRITE_ONCE(ei->i_li_armed, 0);
	*published = true;
	inode_inc_iversion(root);
	inode_set_mtime_to_ts(root, inode_set_ctime_current(root));
	err = ext5_mark_inode_dirty(handle, root);
	if (!err)
		err = ext5_mark_inode_dirty(handle, shadow);
	ext5_double_up_write_data_sem(root, shadow);
	inode_unlock(shadow);
	if (!err)
		err = ext5_journal_stop(handle);
	else
		ext5_journal_stop(handle);
	return err;
}

/*
 * Demote a whole stable root (target_plid 0) or one branch to conventional
 * directories.  Rename supplies inode hints, so a branch reads only its PLID
 * ranges.  Shadows are built unlocked from a pinned generation; publication
 * takes every live parent lock in captured tree order, validates the generation
 * and delta sequence, and swaps extents and routing. Old readers finish from the
 * physical map before retired blocks are freed.  Not crash-safe across a
 * power loss between inode conversions.
 */
static int __ext5_demote_subtree(struct inode *root, u32 target_plid,
				 ino_t target_ino, ino_t target_parent_ino,
				 u64 target_parent_version,
				 struct inode *root_parent_hint)
{
	struct ext5_inode_info *root_ei = EXT5_I(root);
	struct ext5_stable_blob *blob = NULL;
	struct ext5_stable_delta delta_snapshot;
	struct demote_ctx ctx = { .sb = root->i_sb };
	struct lidir_disk_slot *tmp_slots = NULL;
	char *tmp_names = NULL;
	struct dentry *root_dentry;
	struct inode *root_parent_inode = NULL;
	bool blob_pinned = false;
	bool blob_detached = false;
	bool delta_snapshot_init = false;
	bool interiors_locked = false;
	bool rename_locked = false;
	bool li_locked = false;
	bool root_published = false;
	bool root_released = false;
	bool direct_branch = target_plid && target_ino && target_parent_ino;
	bool *selected = NULL;
	u64 snapshot_next_seq = 0;
	u64 transition_start_ns = ktime_get_ns();
	int err;
	u32 p;

	lockdep_assert_held(&root->i_rwsem);
	if (!(root_ei->i_flags & EXT5_LIDIR_ROOT_FL))
		return -EINVAL;

	err = ext5_stable_ensure_blob(root);
	if (err)
		return err;
	if (!target_plid)
		ext5_promote_track_full_demote_attempt();

	mutex_lock(&root_ei->i_li_lock);
	li_locked = true;
	if (atomic_read(&root_ei->i_li_compact_staging)) {
		err = -EAGAIN;
		goto out;
	}
	blob = rcu_dereference_protected(root_ei->i_stable_blob,
					 lockdep_is_held(&root_ei->i_li_lock));
	if (!blob) {
		err = -ENOENT;
		goto out;
	}
	/* Pin this generation while i_li_lock is dropped for the interior locks. */
	refcount_inc(&blob->refs);
	blob_pinned = true;

	ctx.nr_dirs = blob->parent_count;
	ctx.dirs = kvcalloc(ctx.nr_dirs, sizeof(*ctx.dirs), GFP_KERNEL);
	ctx.walk = kvmalloc_array(ctx.nr_dirs, sizeof(*ctx.walk), GFP_KERNEL);
	if (!ctx.dirs || !ctx.walk) {
		err = -ENOMEM;
		goto out;
	}

	ihold(root);
	ctx.dirs[0].inode = root;
	ctx.dirs[0].parent_plid = UINT_MAX;
	ctx.walk[0] = 0;
	ctx.nr_walk = 1;
	for (p = 1; p < ctx.nr_dirs; p++)
		ctx.dirs[p].parent_plid = UINT_MAX;
	/* The generation reference protects the base from compaction, so drop
	 * the lifecycle mutex before the bulk read. */
	mutex_unlock(&root_ei->i_li_lock);
	li_locked = false;
	inode_unlock(root);
	root_released = true;

	if (!direct_branch) {
		err = ext5_stable_load_base_tmp(blob, &tmp_slots, &tmp_names);
		if (err)
			goto out;
	}
	/* Clone the delta under i_li_lock; the clone lets capture sleep. */
	mutex_lock(&root_ei->i_li_lock);
	li_locked = true;
	if (rcu_dereference_protected(root_ei->i_stable_blob,
			lockdep_is_held(&root_ei->i_li_lock)) != blob) {
		err = -EAGAIN;
		goto out;
	}
	delta_snapshot_init = true;
	err = compact_clone_delta(&blob->delta, &delta_snapshot);
	if (err)
		goto out;
	snapshot_next_seq = delta_snapshot.next_seq;
	mutex_unlock(&root_ei->i_li_lock);
	li_locked = false;
	selected = kvcalloc(ctx.nr_dirs, sizeof(*selected), GFP_KERNEL);
	if (!selected) {
		err = -ENOMEM;
		goto out;
	}
	if (direct_branch) {
		err = demote_prepare_direct_branch(&ctx, blob, &delta_snapshot,
						   root, target_plid, target_ino,
						   target_parent_ino,
						   target_parent_version, selected);
		if (err)
			ext5_promote_dbg("direct branch prepare retry root=%lu plid=%u err=%d\n",
					 root->i_ino, target_plid, err);
		if (err == -ESTALE)
			err = -EAGAIN;
		if (err)
			goto out;
	} else {
		for (p = 0; p < ctx.nr_dirs; p++) {
			err = demote_capture_plid(&ctx, blob, &delta_snapshot,
						  tmp_slots, tmp_names, p);
			if (err)
				goto out;
		}

		err = demote_resolve_interior_inodes(&ctx, root);
		/* The inode left the cache after capture; no shadow exists yet, so retry. */
		if (err == -ESTALE)
			err = -EAGAIN;
		if (err)
			goto out;
		for (p = 1; p < ctx.nr_dirs; p++) {
			/* Empty or previously extracted PLIDs can have no live inode. */
			if (!ctx.dirs[p].inode)
				continue;
			if (ctx.dirs[p].parent_plid == UINT_MAX) {
				err = -EIO;
				goto out;
			}
		}
		if (target_plid >= ctx.nr_dirs ||
		    (target_plid && !ctx.dirs[target_plid].inode)) {
			err = -ESTALE;
			goto out;
		}
	}
	if (!target_plid) {
		for (p = 1; p < ctx.nr_dirs; p++)
			selected[p] = ctx.dirs[p].inode != NULL;
	} else if (!direct_branch) {
		selected[target_plid] = true;
		for (p = 1; p < ctx.nr_walk; p++) {
			u32 child = ctx.walk[p];

			if (ctx.dirs[child].parent_plid < ctx.nr_dirs &&
			    selected[ctx.dirs[child].parent_plid])
				selected[child] = true;
		}
	}

	if (!target_plid) {
		/* Resolve the root's ".." first.  Subsumption passes a validated,
		 * locked parent, so no dentry is needed. */
		if (root_parent_hint) {
			lockdep_assert_held(&root_parent_hint->i_rwsem);
			ihold(root_parent_hint);
			root_parent_inode = root_parent_hint;
		} else {
			root_dentry = d_find_alias(root);
			if (root_dentry) {
				struct dentry *parent = dget_parent(root_dentry);

				root_parent_inode = igrab(d_inode(parent));
				dput(parent);
				dput(root_dentry);
			}
		}
		if (!root_parent_inode) {
			err = -EIO;
			goto out;
		}
	}

	/* Build shadows with the root unlocked; stable lookups and appends go on. */
	if (!target_plid) {
		ctx.dirs[0].shadow = demote_create_shadow(root,
			root_parent_inode, &ctx.dirs[0], ctx.names, ctx.names_off,
			&ctx.dirs[0].shadow_csum_seed);
		if (IS_ERR(ctx.dirs[0].shadow)) {
			err = PTR_ERR(ctx.dirs[0].shadow);
			/* A stale object is a namespace conflict; nothing is published yet. */
			if (err == -ESTALE)
				err = -EAGAIN;
			ctx.dirs[0].shadow = NULL;
			goto out;
		}
	}
	for (p = 1; p < ctx.nr_dirs; p++) {
		struct inode *parent;

		if (!selected[p])
			continue;
		if (p == ctx.target_plid && ctx.external_parent)
			parent = ctx.external_parent;
		else if (ctx.dirs[p].parent_plid < ctx.nr_dirs)
			parent = ctx.dirs[ctx.dirs[p].parent_plid].inode;
		else
			parent = NULL;
		if (!parent) {
			err = -EIO;
			goto out;
		}
		ctx.dirs[p].shadow = demote_create_shadow(ctx.dirs[p].inode,
			parent, &ctx.dirs[p], ctx.names, ctx.names_off,
			&ctx.dirs[p].shadow_csum_seed);
		if (IS_ERR(ctx.dirs[p].shadow)) {
			err = PTR_ERR(ctx.dirs[p].shadow);
			if (err == -ESTALE)
				err = -EAGAIN;
			ctx.dirs[p].shadow = NULL;
			goto out;
		}
	}
	/* Flush shadow data before the swap, outside namespace locks. */
	for (p = 0; p < ctx.nr_dirs; p++) {
		if (!ctx.dirs[p].shadow)
			continue;
		err = filemap_write_and_wait(ctx.dirs[p].shadow->i_mapping);
		if (!err)
			err = sync_mapping_buffers(ctx.dirs[p].shadow->i_mapping);
		if (err)
			goto out;
	}
	/* Avoid waiting on rename while a subsuming caller holds the parent.
	 * Once acquired, ancestry cannot change while the inode set is locked.
	 */
	if (!mutex_trylock(&root->i_sb->s_vfs_rename_mutex)) {
		err = -EAGAIN;
		goto out;
	}
	rename_locked = true;
	down_write_nest_lock(&root->i_rwsem,
				     &EXT5_SB(root->i_sb)->li_transition_lock);
	root_released = false;

	/* A rename may have completed after capture but before the rename gate.
	 * Reject that graph before acquiring its interior locks.
	 */
	mutex_lock(&root_ei->i_li_lock);
	li_locked = true;
	if (rcu_dereference_protected(root_ei->i_stable_blob,
			lockdep_is_held(&root_ei->i_li_lock)) != blob ||
	    atomic_read(&root_ei->i_li_compact_staging) ||
	    blob->delta.next_seq != snapshot_next_seq) {
		err = -EAGAIN;
		goto out;
	}
	mutex_unlock(&root_ei->i_li_lock);
	li_locked = false;
	if (!target_plid && !root_parent_hint) {
		struct dentry *parent;

		root_dentry = d_find_alias(root);
		if (!root_dentry) {
			err = -EAGAIN;
			goto out;
		}
		parent = dget_parent(root_dentry);
		err = d_inode(parent) == root_parent_inode ? 0 : -EAGAIN;
		dput(parent);
		dput(root_dentry);
		if (err)
			goto out;
	}
	demote_lock_interiors(&ctx);
	interiors_locked = true;
	/* The inode set now freezes this region. Unrelated cross-directory
	 * renames need not wait for shadow publication or journal I/O.
	 */
	mutex_unlock(&root->i_sb->s_vfs_rename_mutex);
	rename_locked = false;

	/* Lock parents before i_li_lock, as appenders do, so earlier appends have
	 * finished.  A shrinker or reparse race aborts. */
	mutex_lock(&root_ei->i_li_lock);
	li_locked = true;
	if (rcu_dereference_protected(root_ei->i_stable_blob,
			lockdep_is_held(&root_ei->i_li_lock)) != blob ||
	    atomic_read(&root_ei->i_li_compact_staging) ||
	    blob->delta.next_seq != snapshot_next_seq) {
		ext5_promote_dbg("demote validation retry root=%lu plid=%u direct=%u seq=%llu/%llu staging=%d\n",
			 root->i_ino, target_plid, direct_branch ? 1 : 0,
			 (unsigned long long)blob->delta.next_seq,
			 (unsigned long long)snapshot_next_seq,
			 atomic_read(&root_ei->i_li_compact_staging));
		err = -EAGAIN;
		goto out;
	}
	/* The write-locked set keeps appenders out; release i_li_lock before the
	 * publication transactions start. */
	mutex_unlock(&root_ei->i_li_lock);
	li_locked = false;

	/* Publish the interiors; none is visible before the locks drop. */
	for (p = 1; p < ctx.nr_dirs; p++) {
		struct inode *idir = ctx.dirs[p].inode;

		if (!idir || !selected[p])
			continue;
		err = demote_publish_interior_shadow(idir, ctx.dirs[p].shadow,
						     ctx.dirs[p].shadow_csum_seed);
		if (err)
			goto out;
	}
	/* Retake i_li_lock to detach the RCU pointer; demote-pending and the root
	 * write lock keep compaction and appends out meanwhile. */
	mutex_lock(&root_ei->i_li_lock);
	li_locked = true;
	if (rcu_dereference_protected(root_ei->i_stable_blob,
			lockdep_is_held(&root_ei->i_li_lock)) != blob ||
	    atomic_read(&root_ei->i_li_compact_staging) ||
	    blob->delta.next_seq != snapshot_next_seq) {
		ext5_promote_dbg("demote final validation retry root=%lu plid=%u seq=%llu/%llu staging=%d\n",
			 root->i_ino, target_plid,
			 (unsigned long long)blob->delta.next_seq,
			 (unsigned long long)snapshot_next_seq,
			 atomic_read(&root_ei->i_li_compact_staging));
		err = -EAGAIN;
		goto out;
	}
	if (target_plid) {
		u32 rebuilt = 0, removed_entries = 0;
		bool coverage_tracked = root_ei->i_li_active_interiors ||
			root_ei->i_li_active_entries;

		for (p = 1; p < ctx.nr_dirs; p++) {
			if (selected[p]) {
				rebuilt++;
				removed_entries += ctx.dirs[p].nr_entries;
			}
		}
		demote_restore_policy_state(&ctx, selected, false);
		atomic_set(&root_ei->i_li_compact_count, 0);
		atomic64_set(&root_ei->i_li_compact_scheduled_generation, 0);
		atomic64_set(&root_ei->i_li_compact_urgent_generation, 0);
		atomic64_set(&root_ei->i_li_refill_start_op,
			     ext5_li_op_clock_now_exact(root->i_sb));
		atomic64_set(&root_ei->i_li_refill_classified_generation, 0);
		atomic64_set(&root_ei->i_li_last_refill_span_ops, 0);
		atomic64_set(&root_ei->i_li_last_refill_projected_ops, 0);
		atomic64_set(&root_ei->i_li_last_refill_tstar_ops, 0);
		atomic_set(&root_ei->i_li_last_refill_class, 0);
		root_ei->i_li_active_interiors =
			root_ei->i_li_active_interiors > rebuilt ?
			root_ei->i_li_active_interiors - rebuilt : 0;
		root_ei->i_li_active_entries =
			root_ei->i_li_active_entries > removed_entries ?
			root_ei->i_li_active_entries - removed_entries : 0;
		if (coverage_tracked)
			ext5_promote_track_branch_demote(rebuilt, removed_entries);
		ext5_promote_track_branch_demote_time(
			ktime_get_ns() - transition_start_ns);
		ext5_promote_dbg("branch-demoted root ino=%lu plid=%u dirs=%u\n",
				 root->i_ino, target_plid, rebuilt);
		err = 0;
		goto out;
	}

	/* Detach and swap with every parent frozen; old readers use the physical
	 * map, and the retirement inode keeps the blocks until the grace period. */
	RCU_INIT_POINTER(root_ei->i_stable_blob, NULL);
	blob_detached = true;
	mutex_unlock(&root_ei->i_li_lock);
	li_locked = false;
	err = demote_publish_root_shadow(root, ctx.dirs[0].shadow,
					 &root_published);
	if (err && !root_published) {
		/* Nothing changed yet: restore the installed reference. */
		mutex_lock(&root_ei->i_li_lock);
		rcu_assign_pointer(root_ei->i_stable_blob, blob);
		mutex_unlock(&root_ei->i_li_lock);
		blob_detached = false;
	}

	if (root_published) {
		u32 entries = 0;
		u32 live_dirs = 0;
		u32 tracked_dirs = root_ei->i_li_active_interiors;
		u32 tracked_entries = root_ei->i_li_active_entries;

		for (p = 0; p < ctx.nr_dirs; p++) {
			if (!p || ctx.dirs[p].inode)
				entries += ctx.dirs[p].nr_entries;
			if (p && ctx.dirs[p].inode)
				live_dirs++;
		}
		demote_restore_policy_state(&ctx, selected, true);
		if (tracked_dirs || tracked_entries)
			ext5_promote_track_demote_root(tracked_dirs + 1,
					       tracked_entries);
		else
			ext5_promote_track_demote_root(live_dirs + 1, entries);
		root_ei->i_li_active_interiors = 0;
		root_ei->i_li_active_entries = 0;
		atomic_set(&root_ei->i_li_compact_count, 0);
		atomic64_set(&root_ei->i_li_compact_scheduled_generation, 0);
		atomic64_set(&root_ei->i_li_compact_urgent_generation, 0);
		atomic64_set(&root_ei->i_li_refill_start_op, 0);
		atomic64_set(&root_ei->i_li_refill_start_used_bytes, 0);
		atomic64_set(&root_ei->i_li_refill_classified_generation, 0);
		atomic64_set(&root_ei->i_li_last_refill_span_ops, 0);
		atomic64_set(&root_ei->i_li_last_refill_projected_ops, 0);
		atomic64_set(&root_ei->i_li_last_refill_tstar_ops, 0);
		atomic_set(&root_ei->i_li_last_refill_class, 0);
		ext5_promote_dbg("demoted root ino=%lu dirs=%u entries=%u\n",
				 root->i_ino, ctx.nr_dirs, entries);
	}

out:
	if (li_locked)
		mutex_unlock(&root_ei->i_li_lock);
	if (interiors_locked)
		demote_unlock_interiors(&ctx);
	/* Reclaim outside namespace locks, then retake the caller's root lock. */
	if (!root_released) {
		inode_unlock(root);
		root_released = true;
	}
	if (rename_locked)
		mutex_unlock(&root->i_sb->s_vfs_rename_mutex);
	if (blob_detached) {
		/* Existing readers finish from physical runs before any block is freed. */
		ext5_stable_unlist_blob(blob);
		synchronize_rcu();
		synchronize_srcu(&ext5_li_lookup_srcu);
		ext5_stable_blob_put(blob); /* installed-pointer reference */
		while (refcount_read(&blob->refs) != 1)
			cond_resched();
		ext5_stable_unpin_payload_blocks(blob);
	}
	demote_discard_shadows(&ctx);
	if (root_parent_inode)
		iput(root_parent_inode);
	if (blob_pinned)
		ext5_stable_blob_put(blob);
	if (delta_snapshot_init)
		ext5_stable_delta_free(&delta_snapshot);
	kvfree(selected);
	kvfree(tmp_slots);
	kvfree(tmp_names);
	demote_ctx_release(&ctx);
	if (!target_plid)
		ext5_promote_track_full_demote_time(
			ktime_get_ns() - transition_start_ns);
	down_write_nest_lock(&root->i_rwsem,
				     &EXT5_SB(root->i_sb)->li_transition_lock);
	root_released = false;
	return err;
}

static int ext5_demote_with_transition_lock(struct inode *root, u32 plid,
					     ino_t target_ino,
					     ino_t parent_ino,
					     u64 parent_version)
{
	struct ext5_sb_info *sbi = EXT5_SB(root->i_sb);
	int err;

	mutex_lock(&sbi->li_transition_lock);
	down_write_nest_lock(&root->i_rwsem, &sbi->li_transition_lock);
	err = __ext5_demote_subtree(root, plid, target_ino, parent_ino,
					parent_version, NULL);
	inode_unlock(root);
	mutex_unlock(&sbi->li_transition_lock);
	return err;
}

int ext5_demote_subtree(struct inode *root)
{
	return ext5_demote_with_transition_lock(root, 0, 0, 0, 0);
}

/* Called unlocked.  The collector saw the parent-child link under the
 * parent's shared lock; revalidate its version and hold that lock, so rename
 * and rmdir cannot move the link during demotion. */
int ext5_demote_subtree_for_promote(struct inode *root, struct inode *parent,
				     u64 parent_version)
{
	struct ext5_sb_info *sbi = EXT5_SB(root->i_sb);
	int err = -EAGAIN;

	mutex_lock(&sbi->li_transition_lock);
	inode_lock_shared_nested(parent, I_MUTEX_PARENT);
	if (inode_query_iversion(parent) != parent_version ||
	    ext5_dir_is_stable(parent))
		goto out_parent;
	down_write_nest_lock(&root->i_rwsem, &sbi->li_transition_lock);
	if (EXT5_I(root)->i_flags & EXT5_LIDIR_ROOT_FL)
		err = __ext5_demote_subtree(root, 0, 0, 0, 0, parent);
	inode_unlock(root);
out_parent:
	inode_unlock_shared(parent);
	mutex_unlock(&sbi->li_transition_lock);
	return err;
}

int ext5_demote_subtree_plid(struct inode *root, u32 plid)
{
	return ext5_demote_with_transition_lock(root, plid, 0, 0, 0);
}

int ext5_demote_subtree_plid_hint(struct inode *root, u32 plid,
				  ino_t target_ino, ino_t parent_ino,
				  u64 parent_version)
{
	return ext5_demote_with_transition_lock(root, plid, target_ino,
					     parent_ino, parent_version);
}
