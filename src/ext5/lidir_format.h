/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SplineFS on-disk directory format, shared with utils/.  Little-endian.
 *
 * A stable subtree root keeps its learned index in its own data file:
 *   [0, 8 KiB)            two descriptor slots
 *   [base_off, +bytes)    base region, immutable per generation: Bloom
 *                         filter, radix table, spline points, slots,
 *                         parent_index, packed names (offsets base-relative)
 *   [delta_off, +bytes)   delta region, an append-only mutation log
 *
 * Slot key = (parent_local_id << 32) | hash32(name).  PLIDs are dense per
 * blob, initially assigned in DFS pre-order. Rename preserves the IDs, so
 * their numeric order does not define ancestry. hash32 is keyed
 * HalfSipHash of the full name; full SipHash breaks ties within a key.
 */

#ifndef _EXT5_LIDIR_FORMAT_H
#define _EXT5_LIDIR_FORMAT_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#define __le16  uint16_t
#define __le32  uint32_t
#define __le64  uint64_t
#define __u8    uint8_t
#endif

#define LIDIR_MAGIC		0x5244494cU	/* 'L','I','D','R' (little-endian) */
#define LIDIR_VERSION		10U

#define LIDIR_DESC_BYTES	4096U		/* one descriptor slot is exactly 4 KiB */

/*
 * Two descriptor slots: generation g goes to slot (g & 1), so a torn write
 * leaves generation g-1 intact.  Mount takes the highest generation whose
 * checksums validate.
 */
#define LIDIR_DESC_SLOTS	2U
#define LIDIR_BASE_AREA_OFF	(LIDIR_DESC_SLOTS * LIDIR_DESC_BYTES)	/* 8 KiB */

/* slot.flags */
#define LIDIR_SLOT_SUBTREE_BOUNDARY	0x01	/* child directory outside this blob */

/* One descriptor slot: exactly LIDIR_DESC_BYTES, written as one 4 KiB unit. */
struct lidir_disk_descriptor {
	__le32 magic;
	__le32 version;
	__le32 header_bytes;		/* = sizeof(struct lidir_disk_descriptor) */
	__le32 flags;

	__le64 generation;		/* bumped on every compaction */
	__u8   hash_seed[16];		/* copy of sb->s_li_hash_seed at build time */

	/* base region: location within the inode's blob file + integrity */
	__le64 base_off;		/* byte offset into the inode's data file */
	__le64 base_bytes;		/* length of base region in bytes */
	__le32 base_csum;		/* crc32c over the entire base region */
	__le32 _pad_base;

	/* delta region */
	__le64 delta_off;
	__le64 delta_bytes;
	__le64 delta_seq_base;		/* sequence immediately before first record */
	__le32 delta_csum;
	__le32 _pad_delta;

	/* counts within the base extent */
	__le32 entry_count;		/* total slots */
	__le32 parent_count;		/* # of distinct parent_local_ids */
	__le32 filter_bits;		/* # of Bloom-filter bits (power of 2) */

	/* RadixSpline model.  The total epsilon includes collision slack. */
	__le32 radix_count;
	__le32 spline_count;
	__le32 spline_epsilon;
	__le32 spline_corridor_error;
	__le32 radix_bits;
	__le32 radix_shift;
	__le32 model_flags;
	__le32 _pad_model;
	__le64 model_min_key;
	__le64 model_max_key;

	/* byte offsets into the base extent */
	__le64 filter_off;
	__le64 radix_off;
	__le64 spline_off;
	__le64 slots_off;
	__le64 parent_index_off;
	__le64 names_off;
	__le64 names_bytes;

	/* Inode-table range reserved for this subtree; count 0 means none.
	 * Reservation and compaction persist the live used count. */
	__le32 inode_pool_start;
	__le32 inode_pool_count;
	__le32 inode_pool_used;

	/* crc32 of the descriptor before this field; see LIDIR_DESC_CSUM_LEN. */
	__le32 header_csum;
	__le32 _pad_header;

	/* reserved padding to LIDIR_DESC_BYTES */
	__u8   reserved[LIDIR_DESC_BYTES - 236];
};

/* header_csum covers bytes [0, offsetof(header_csum)). */
#define LIDIR_DESC_CSUM_LEN	(offsetof(struct lidir_disk_descriptor, header_csum))

/* Slot, 24 bytes, sorted by (PLID, hash32, SipHash-high32); complete ties
 * keep input order. */
struct lidir_disk_slot {
	__le64 key;			/* (parent_local_id << 32) | hash32(name) */
	__le32 ino;			/* inode number */
	__le32 name_off;		/* offset into packed name area */
	__le32 dx_hash;			/* ext4-compatible major hash for demotion */
	__u8   name_len;
	__u8   file_type;		/* DT_REG / DT_DIR / ... */
	__u8   flags;			/* LIDIR_SLOT_* */
	__u8   _pad;
};

/* RadixSpline point; radix table entries are raw little-endian u32s. */
struct lidir_disk_spline_point {
	__le64 key;
	__le32 rank;
	__le32 reserved;
};

/* parent_index[i] is the first slot of PLID i; entry [parent_count] is the
 * slot count, so PLID i owns [parent_index[i], parent_index[i + 1]). */
/* (no struct -- raw __le32 array of length parent_count + 1) */

/*
 * Delta region: 8-byte aligned records with consecutive sequence numbers
 * from delta_seq_base + 1, replayed at mount.  Only the newest INSERT may be
 * cancelled, by a journaled rewrite to NOP.  Each csum covers op..ino and the
 * name; replay stops at the first bad csum or sequence gap.  Compaction
 * merges base and delta into a new base.
 */
enum lidir_delta_op {
	LIDIR_DELTA_NOP    = 0,	/* padding */
	LIDIR_DELTA_INSERT = 1,	/* (parent, name) -> ino */
	LIDIR_DELTA_DELETE = 2,	/* (parent, name) -> tombstone */
};

struct lidir_disk_delta_record {
	__le64 seq;			/* exact global sequence: previous + 1 */
	__le32 csum;			/* crc32_le over op..ino + name_bytes */
	__u8   op;			/* lidir_delta_op */
	__u8   file_type;		/* DT_* */
	__u8   name_len;		/* 1..255 */
	__u8   _pad;
	__le32 parent_local_id;
	__le32 key32;			/* hash32(name) */
	__le32 ino;
	__le32 _pad2;
	/* followed by `name_len` bytes of name, padded with zeros to
	 * the next 8-byte boundary */
};

#define LIDIR_DELTA_HEADER_BYTES	sizeof(struct lidir_disk_delta_record)

/* Total on-disk record size including aligned trailing name. */
static inline unsigned int lidir_delta_record_bytes(unsigned int name_len)
{
	unsigned int v = LIDIR_DELTA_HEADER_BYTES + name_len;

	return (v + 7U) & ~7U;
}

#endif /* _EXT5_LIDIR_FORMAT_H */
