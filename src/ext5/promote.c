// SPDX-License-Identifier: GPL-2.0
/*
 * SplineFS promotion: convert a mutable directory subtree into one learned
 * index.  The target becomes the stable subtree root and owns the blob in its
 * data file; each descendant directory becomes STABLE_INTERIOR, its dirent
 * blocks retired and i_block[] holding (root inode, PLID).  A nested stable
 * root is subsumed, or kept as a boundary slot when li_promote_subsume=0.
 *
 * Collect in DFS pre-order with one directory lock at a time, radix-sort the
 * slots, fit the error-bounded spline and stage the blob in a hidden shadow
 * inode, all without namespace locks.  Then lock the whole snapshot, validate
 * every i_version, swap metadata root-first, and reclaim the old trees after.
 */

#include "ext5.h"
#include "ext5_jbd3.h"
#include "li_radix_sort.h"
#include "lidir_format.h"
#include "radix_spline.h"
#include <linux/bitmap.h>
#include <linux/buffer_head.h>
#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/highmem.h>
#include <linux/iversion.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/moduleparam.h>
#include <linux/pagemap.h>
#include <linux/random.h>
#include <linux/siphash.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#define EXT5_PROMOTE_FILTER_PROBES	5U
#define EXT5_PROMOTE_FILTER_MIN_BITS	1024U

unsigned int ext5_li_spline_epsilon = 8U;
module_param_named(li_spline_epsilon, ext5_li_spline_epsilon, uint, 0644);
MODULE_PARM_DESC(li_spline_epsilon,
		 "Total RadixSpline rank error bound; default 8");

unsigned int ext5_li_radix_bits = 12U;
module_param_named(li_radix_bits, ext5_li_radix_bits, uint, 0644);
MODULE_PARM_DESC(li_radix_bits,
		 "RadixSpline radix bits; 0 selects automatically; default 12");

unsigned int ext5_li_bloom_bits_per_entry = 8U;
module_param_named(li_bloom_bits_per_entry, ext5_li_bloom_bits_per_entry,
		   uint, 0644);
MODULE_PARM_DESC(li_bloom_bits_per_entry,
		 "Blocked Bloom-filter bits per base entry; default 8");

bool ext5_li_bloom_enabled;
module_param_named(li_bloom_enabled, ext5_li_bloom_enabled, bool, 0644);
MODULE_PARM_DESC(li_bloom_enabled,
		 "Probe the optional base Bloom filter before RadixSpline; default false");

unsigned int ext5_li_policy_mode = 1U;
module_param_named(li_policy_mode, ext5_li_policy_mode, uint, 0644);
MODULE_PARM_DESC(li_policy_mode,
		 "Promotion policy: 0=off, 1=adaptive subtree, 2=directory only, 3=subtree only");

unsigned int ext5_li_size_floor_entries;
module_param_named(li_size_floor_entries, ext5_li_size_floor_entries,
		   uint, 0644);
MODULE_PARM_DESC(li_size_floor_entries,
		 "Minimum entries before promotion; 0 adapts online (default)");

unsigned int ext5_li_buildcost_permille;
module_param_named(li_buildcost_permille,
		   ext5_li_buildcost_permille, uint, 0644);
MODULE_PARM_DESC(li_buildcost_permille,
		 "T* coefficient c in thousandths for c*N*log2(N); 0 adapts online (default)");

unsigned int ext5_li_op_cost_ns = 5000U;
module_param_named(li_op_cost_ns, ext5_li_op_cost_ns, uint, 0644);
MODULE_PARM_DESC(li_op_cost_ns,
		 "Namespace-operation cost used to convert measured build time to operations; default 5000ns");

bool ext5_li_debug_controls;
module_param_named(li_debug_controls, ext5_li_debug_controls, bool, 0644);
MODULE_PARM_DESC(li_debug_controls,
		 "Allow force-promote/compact/demote ioctls; default false");

static unsigned int ext5_li_debug_validation_delay_ms;
module_param_named(li_debug_validation_delay_ms,
		   ext5_li_debug_validation_delay_ms, uint, 0644);
MODULE_PARM_DESC(li_debug_validation_delay_ms,
		 "Debug-only delay after a policy model is ready and before validation; requires li_debug_controls");

/* Sparse delta reservation; compaction triggers are set in dir_stable.c. */
#define EXT5_PROMOTE_DELTA_BYTES	(4U * 1024U * 1024U)
/* Cap on the sparse delta reservation. */
#define EXT5_LI_DELTA_RESERVE_MAX	(256ULL * 1024 * 1024)

/* Floor of the sparse delta reservation; 0 disables the delta, as the
 * per-directory naive baseline does. */
unsigned int ext5_li_promote_delta_bytes = EXT5_PROMOTE_DELTA_BYTES;
module_param_named(li_promote_delta_bytes, ext5_li_promote_delta_bytes, uint, 0644);
MODULE_PARM_DESC(li_promote_delta_bytes,
		 "Per-promote delta floor in bytes (region is sparse, scaled up by entry count); 0 = rebuild-on-mutation, no buffer; default 4 MiB");

/*
 * Sparse delta reservation for `entries` records totalling `names_bytes`:
 * room for one tombstone per entry below the 75% compaction watermark, so a
 * bulk delete never forces a synchronous compaction.  0 when disabled.
 */
u64 ext5_li_delta_reserve(struct super_block *sb, u32 entries, u64 names_bytes)
{
	u64 floor = READ_ONCE(ext5_li_promote_delta_bytes);
	u64 avg_name, per_rec, want;

	if (floor == 0)
		return 0;	/* naive: no buffer, rebuild on mutation */
	avg_name = entries ? names_bytes / entries : 0;
	if (avg_name > EXT5_NAME_LEN)
		avg_name = EXT5_NAME_LEN;
	per_rec = ALIGN(LIDIR_DELTA_HEADER_BYTES + avg_name, 8);
	want = (u64)entries * per_rec;
	want += want / 2;		/* 1.5x: keep a full delete under the 75% watermark */
	if (want < floor)
		want = floor;
	if (want > EXT5_LI_DELTA_RESERVE_MAX)
		want = EXT5_LI_DELTA_RESERVE_MAX;
	return ALIGN(want, sb->s_blocksize);
}

/*
 * T*(R) = max(T_v, k * |R| * log2|R|), in namespace operations.  T_v is the
 * valley of the bimodal re-modification distribution; the second term is the
 * model-build cost a quiet interval must repay.  A zero module parameter
 * selects the online value; nonzero pins an override.
 */
unsigned int ext5_li_quiet_ops;
module_param_named(li_quiet_ops,
		   ext5_li_quiet_ops, uint, 0644);
MODULE_PARM_DESC(li_quiet_ops,
		 "Quiescence threshold T_v; 0 adapts from online RMD histogram (default)");

#define EXT5_LI_DEFAULT_QUIET_OPS 10000U
#define EXT5_LI_DEFAULT_SIZE_FLOOR 8192U
#define EXT5_LI_DEFAULT_BUILDCOST_PERMILLE 16U

struct ext5_li_policy_record {
	int subtree_files;
	int subtree_dirs;
	u64 last_mut;
	u8 armed;
};

void ext5_li_policy_records_init(struct ext5_sb_info *sbi)
{
	int cpu, bucket;

	xa_init(&sbi->li_policy_records);
	atomic64_set(&sbi->li_op_clock, 0);
	for (bucket = 0; bucket < EXT5_LI_RMD_BUCKETS; bucket++)
		atomic64_set(&sbi->li_rmd_hist[bucket], 0);
	atomic64_set(&sbi->li_rmd_samples, 0);
	atomic_set(&sbi->li_adaptive_quiet, EXT5_LI_DEFAULT_QUIET_OPS);
	atomic_set(&sbi->li_adaptive_floor, EXT5_LI_DEFAULT_SIZE_FLOOR);
	atomic_set(&sbi->li_adaptive_build_permille,
		   EXT5_LI_DEFAULT_BUILDCOST_PERMILLE);
	sbi->li_op_pending = alloc_percpu(u32);
	if (sbi->li_op_pending)
		for_each_possible_cpu(cpu)
			*per_cpu_ptr(sbi->li_op_pending, cpu) = 0;
}

void ext5_li_policy_records_destroy(struct ext5_sb_info *sbi)
{
	struct ext5_li_policy_record *record;
	unsigned long index;

	xa_for_each(&sbi->li_policy_records, index, record)
		kfree(record);
	xa_destroy(&sbi->li_policy_records);
	free_percpu(sbi->li_op_pending);
	sbi->li_op_pending = NULL;
}

void ext5_li_policy_forget_inode(struct inode *inode)
{
	struct ext5_li_policy_record *record;

	if (!inode || !inode->i_sb)
		return;
	record = xa_erase(&EXT5_SB(inode->i_sb)->li_policy_records,
			  inode->i_ino);
	kfree(record);
}

void ext5_li_policy_save_inode(struct inode *inode)
{
	struct ext5_inode_info *ei;
	struct ext5_li_policy_record *record, *old;

	if (!inode || !S_ISDIR(inode->i_mode))
		return;
	ei = EXT5_I(inode);
	if (!inode->i_nlink ||
	    (ei->i_flags & (EXT5_LIDIR_ROOT_FL | EXT5_LIDIR_INTERIOR_FL))) {
		ext5_li_policy_forget_inode(inode);
		return;
	}
	/* Hidden transition inodes are outside the policy: no sidecar. */
	if (!READ_ONCE(ei->i_li_state))
		return;
	if (!atomic_read(&ei->i_subtree_files) &&
	    !atomic_read(&ei->i_subtree_dirs) && !ei->i_li_last_mut &&
	    !ei->i_li_armed)
		return;
	record = kmalloc(sizeof(*record), GFP_NOFS | __GFP_ACCOUNT);
	if (!record)
		return;
	record->subtree_files = atomic_read(&ei->i_subtree_files);
	record->subtree_dirs = atomic_read(&ei->i_subtree_dirs);
	record->last_mut = READ_ONCE(ei->i_li_last_mut);
	record->armed = READ_ONCE(ei->i_li_armed) == 2 ? 1 :
		READ_ONCE(ei->i_li_armed);
	old = xa_store(&EXT5_SB(inode->i_sb)->li_policy_records,
		       inode->i_ino, record, GFP_NOFS | __GFP_ACCOUNT);
	if (xa_is_err(old)) {
		kfree(record);
		return;
	}
	kfree(old);
}

void ext5_li_policy_restore_inode(struct inode *inode)
{
	struct ext5_li_policy_record *record;
	struct ext5_inode_info *ei;
	int files, dirs;
	u64 last_mut;
	u8 armed;

	if (!inode || !S_ISDIR(inode->i_mode) || ext5_dir_is_stable(inode))
		return;
	ei = EXT5_I(inode);
	if (!READ_ONCE(ei->i_li_state))
		return;
	xa_lock(&EXT5_SB(inode->i_sb)->li_policy_records);
	record = xa_load(&EXT5_SB(inode->i_sb)->li_policy_records,
			 inode->i_ino);
	if (!record) {
		xa_unlock(&EXT5_SB(inode->i_sb)->li_policy_records);
		return;
	}
	files = record->subtree_files;
	dirs = record->subtree_dirs;
	last_mut = record->last_mut;
	armed = record->armed;
	xa_unlock(&EXT5_SB(inode->i_sb)->li_policy_records);
	atomic_set(&ei->i_subtree_files, files);
	atomic_set(&ei->i_subtree_dirs, dirs);
	WRITE_ONCE(ei->i_li_last_mut, last_mut);
	WRITE_ONCE(ei->i_li_armed, armed);
}

u32 ext5_li_effective_quiet_ops(struct super_block *sb)
{
	u32 configured = READ_ONCE(ext5_li_quiet_ops);

	return configured ? configured :
		(u32)atomic_read(&EXT5_SB(sb)->li_adaptive_quiet);
}

u32 ext5_li_effective_size_floor(struct super_block *sb)
{
	u32 configured = READ_ONCE(ext5_li_size_floor_entries);

	return configured ? configured :
		(u32)atomic_read(&EXT5_SB(sb)->li_adaptive_floor);
}

u32 ext5_li_effective_buildcost_permille(struct super_block *sb)
{
	u32 configured = READ_ONCE(ext5_li_buildcost_permille);

	return configured ? configured :
		(u32)atomic_read(&EXT5_SB(sb)->li_adaptive_build_permille);
}

u64 ext5_li_rmd_sample_count(struct super_block *sb)
{
	return atomic64_read(&EXT5_SB(sb)->li_rmd_samples);
}

static void ext5_li_recompute_quiet_threshold(struct ext5_sb_info *sbi)
{
	u64 counts[EXT5_LI_RMD_BUCKETS];
	u64 best_score = 0;
	u32 best_bucket = 0;
	u32 b, m;

	for (b = 0; b < EXT5_LI_RMD_BUCKETS; b++)
		counts[b] = atomic64_read(&sbi->li_rmd_hist[b]);
	for (b = 4; b <= 16; b++) {
		u64 below = 0, tail = 0, seen = 0;
		u64 target;

		for (m = 0; m <= b; m++)
			below += counts[m];
		for (m = b + 1; m < EXT5_LI_RMD_BUCKETS; m++)
			tail += counts[m];
		if (below < 32 || tail < 32)
			continue;
		target = DIV_ROUND_UP_ULL(tail, 2);
		for (m = b + 1; m < EXT5_LI_RMD_BUCKETS; m++) {
			seen += counts[m];
			if (seen >= target)
				break;
		}
		/* log2(MCR) is the median-tail bucket minus threshold bucket. */
		if (m > b && m - b > best_score) {
			best_score = m - b;
			best_bucket = b;
		}
	}
	if (best_bucket) {
		u32 candidate = 1U << best_bucket;
		u32 old = atomic_read(&sbi->li_adaptive_quiet);

		candidate = clamp_t(u32, candidate, 16, 65536);
		atomic_set(&sbi->li_adaptive_quiet,
			   clamp_t(u32, (old * 3 + candidate) / 4, 16, 65536));
	}
}

static void ext5_li_record_rmd(struct super_block *sb, u64 distance)
{
	struct ext5_sb_info *sbi = EXT5_SB(sb);
	u32 bucket;
	u64 samples;

	if (!distance)
		return;
	bucket = min_t(u32, fls64(distance) - 1, EXT5_LI_RMD_BUCKETS - 1);
	atomic64_inc(&sbi->li_rmd_hist[bucket]);
	samples = atomic64_inc_return(&sbi->li_rmd_samples);
	if (!(samples & 255))
		ext5_li_recompute_quiet_threshold(sbi);
}

/*
 * Per-filesystem namespace-operation clock: one tick per dcache-miss lookup
 * or completed mutation.  Per-CPU batches keep lookups local; without them
 * the mount-local atomic is exact.
 */
#define EXT5_LI_OPCLOCK_BATCH 64U

void ext5_li_op_tick(struct super_block *sb)
{
	struct ext5_sb_info *sbi = EXT5_SB(sb);
	u32 *local;
	u32 pending;
	u32 effective_quiet;

	if (!READ_ONCE(ext5_li_policy_mode))
		return;
	ext5_promote_stats_op_tick();
	effective_quiet = ext5_li_effective_quiet_ops(sb);
	if (unlikely(!sbi->li_op_pending ||
		     effective_quiet <=
		     EXT5_LI_OPCLOCK_BATCH * num_online_cpus())) {
		atomic64_inc(&sbi->li_op_clock);
		return;
	}
	preempt_disable();
	local = this_cpu_ptr(sbi->li_op_pending);
	pending = ++*local;
	if (unlikely(pending >= EXT5_LI_OPCLOCK_BATCH)) {
		atomic64_add(EXT5_LI_OPCLOCK_BATCH, &sbi->li_op_clock);
		/* Publish before clearing: observers may overcount one batch but never
		 * see the clock move backwards. */
		smp_mb__after_atomic();
		WRITE_ONCE(*local, 0);
	}
	preempt_enable();
}

static u64 ext5_li_op_now(struct super_block *sb)
{
	struct ext5_sb_info *sbi = EXT5_SB(sb);
	u64 now = (u64)atomic64_read(&sbi->li_op_clock);
	u32 effective_quiet = ext5_li_effective_quiet_ops(sb);
	int cpu;

	if (unlikely(!sbi->li_op_pending ||
		     effective_quiet <=
		     EXT5_LI_OPCLOCK_BATCH * num_online_cpus()))
		return now;
	for_each_possible_cpu(cpu)
		now += READ_ONCE(*per_cpu_ptr(sbi->li_op_pending, cpu));
	return now;
}

u64 ext5_li_op_clock_now_exact(struct super_block *sb)
{
	struct ext5_sb_info *sbi = EXT5_SB(sb);
	u64 now = (u64)atomic64_read(&sbi->li_op_clock);
	int cpu;

	if (!sbi->li_op_pending)
		return now;
	for_each_possible_cpu(cpu)
		now += READ_ONCE(*per_cpu_ptr(sbi->li_op_pending, cpu));
	return now;
}

static inline u64 ext5_li_op_distance(u64 now, u64 last)
{
	return now > last ? now - last : 0;
}

/* T*(R) in operations for a region of `entries` records. */
static u64 ext5_li_t_star(struct super_block *sb, u64 entries)
{
	u64 tv = ext5_li_effective_quiet_ops(sb);
	u32 coefficient = ext5_li_effective_buildcost_permille(sb);
	u64 work, build;

	if (entries < 2)
		return tv;
	if (check_mul_overflow(entries, (u64)(fls64(entries) - 1), &work))
		work = U64_MAX;
	build = mul_u64_u32_div(work, coefficient, 1000);
	return build > tv ? build : tv;
}

u64 ext5_li_t_star_for_entries(struct super_block *sb, u64 entries)
{
	return ext5_li_t_star(sb, entries);
}

static u64 ext5_li_region_entries(struct inode *dir)
{
	int observed = atomic_read(&EXT5_I(dir)->i_subtree_files);

	/* Use the mutation-maintained subtree count; a region not yet observed on
	 * this mount stays conventional. */
	if (observed > 0)
		return (u64)observed;
	return 0;
}

/* Accessors for the policy ioctl; they reuse the promotion path's code. */
u64 ext5_li_region_entries_for(struct inode *dir)
{
	return ext5_li_region_entries(dir);
}

u64 ext5_li_rmd_for(struct inode *dir)
{
	struct ext5_li_inode_state *state = EXT5_I(dir)->i_li_state;
	u64 now;

	if (!state)
		return 0;
	now = ext5_li_op_clock_now_exact(dir->i_sb);
	return ext5_li_op_distance(now, READ_ONCE(EXT5_I(dir)->i_li_last_mut));
}

/*
 * Churn demotion: the limit-th delta-full compaction demotes the region
 * instead of compacting again.  0 disables it.
 */
unsigned int ext5_li_demote_refills = 2U;
module_param_named(li_demote_refills,
		   ext5_li_demote_refills, uint, 0644);
MODULE_PARM_DESC(li_demote_refills,
		 "Demote after this many consecutive rapid insert-dominated refill strikes; 0 disables; default 2");

unsigned int ext5_li_demote_idle_ms = 250U;
module_param_named(li_demote_idle_ms, ext5_li_demote_idle_ms, uint, 0644);
MODULE_PARM_DESC(li_demote_idle_ms,
		 "Wait for this operation-idle interval before automatic churn demotion; urgent rename/debug demotion bypasses it; default 250ms");

unsigned int ext5_li_promote_idle_ms = 20U;
module_param_named(li_promote_idle_ms, ext5_li_promote_idle_ms, uint, 0644);
MODULE_PARM_DESC(li_promote_idle_ms,
		 "Wait for this operation-idle interval before automatic promotion snapshot/build/publication; debug promotion bypasses it; default 20ms");

/*
 * When a promotion meets a stable descendant root: 1 (default) subsumes it
 * into one model; 0 keeps it as a nested root behind a boundary slot.
 */
unsigned int ext5_li_promote_subsume = 1U;
module_param_named(li_promote_subsume,
		   ext5_li_promote_subsume, uint, 0644);
MODULE_PARM_DESC(li_promote_subsume,
		 "On promote, subsume a nested stable root into one model (1) or keep it nested (0); default 1");

/* Collected slot: src_name_off indexes the collection buffer, name_off the
 * final packed names. */
struct promote_slot {
	u64 key;
	u64 tiebreak64;		/* SipHash-high32 | HalfSipHash search key */
	union {
		u32 src_name_off;
		u32 dx_hash;
	};
	u8  name_len;
	u8  file_type;
	u8  flags;
	u8  _pad;
	u32 ino;
	u32 name_off;
};

/* One per visited dir.  Owns an inode reference (iget). */
struct promote_dir {
	struct inode *inode;
	struct inode *shadow;		/* hidden transition/retirement inode */
	u64 i_version;		/* snapshot version, validated before publish */
	u32 shadow_csum_seed;
	u32 plid;
	u32 parent_plid;		/* collection parent, for nested-root subsumption */
	bool is_root;		/* true for plid 0 */
	bool needs_demote;	/* perform outside the parent's snapshot lock */
};

/* Build context for a single subtree promotion. */
struct promote_ctx {
	struct super_block *sb;
	siphash_key_t hash_key;

	struct promote_slot *slots;
	u32 nr_slots, cap_slots;

	char *names;
	u32 names_off, cap_names;

	struct promote_dir *dirs;
	u32 nr_dirs, cap_dirs;
	u64 mutable_bytes;
	u64 snapshot_last_mut;
	u64 expected_last_mut;
	bool policy_attempt;
	bool directory_only;
	bool transition_locked;
};

static void ext5_li_adapt_after_build(struct super_block *sb, u32 entries,
				      u64 mutable_bytes,
				      const struct lidir_disk_descriptor *desc,
				      u64 build_ns)
{
	struct ext5_sb_info *sbi = EXT5_SB(sb);
	u64 fixed_bytes, variable_bytes, mutable_per, variable_per, margin;
	u32 old, candidate;
	u64 work, equivalent_ops;
	u32 coefficient;

	if (!entries)
		return;
	if (!READ_ONCE(ext5_li_size_floor_entries)) {
		fixed_bytes = LIDIR_BASE_AREA_OFF +
			(u64)le32_to_cpu(desc->radix_count) * sizeof(__le32) +
			(u64)le32_to_cpu(desc->spline_count) *
				sizeof(struct lidir_disk_spline_point) +
			(u64)(le32_to_cpu(desc->parent_count) + 1) * sizeof(__le32);
		variable_bytes = (u64)entries * sizeof(struct lidir_disk_slot) +
			le64_to_cpu(desc->names_bytes) +
			le32_to_cpu(desc->filter_bits) / 8;
		mutable_per = div64_u64(mutable_bytes, entries);
		variable_per = div64_u64(variable_bytes, entries);
		if (mutable_per > variable_per) {
			margin = mutable_per - variable_per;
			candidate = clamp_t(u64,
				DIV_ROUND_UP_ULL(fixed_bytes, margin), 1024, 65536);
		} else {
			candidate = 65536;
		}
		old = atomic_read(&sbi->li_adaptive_floor);
		atomic_set(&sbi->li_adaptive_floor,
			   clamp_t(u32, (old * 3 + candidate) / 4, 1024, 65536));
	}

	if (!READ_ONCE(ext5_li_buildcost_permille) && build_ns) {
		work = (u64)entries * max_t(u32, fls64(entries) - 1, 1);
		equivalent_ops = max_t(u64,
			DIV_ROUND_UP_ULL(build_ns,
				max_t(u32, READ_ONCE(ext5_li_op_cost_ns), 1)), 1);
		if (equivalent_ops > U64_MAX / 1000)
			coefficient = 64000;
		else
			coefficient = clamp_t(u64,
				div64_u64(equivalent_ops * 1000,
					  max_t(u64, work, 1)), 1, 64000);
		old = atomic_read(&sbi->li_adaptive_build_permille);
		atomic_set(&sbi->li_adaptive_build_permille,
			   clamp_t(u32, (old * 3 + coefficient) / 4,
				   1, 64000));
	}
}

static u64 promote_make_key(u32 plid, u32 hash32)
{
	return ((u64)plid << 32) | hash32;
}

static bool ext5_li_dir_supported(struct inode *inode)
{
	return S_ISDIR(inode->i_mode) && !IS_ENCRYPTED(inode) &&
		!IS_CASEFOLDED(inode) && !IS_DAX(inode) &&
		!ext5_has_inline_data(inode);
}

static u64 promote_mix64(u64 hash)
{
	hash ^= hash >> 33;
	hash *= 0xff51afd7ed558ccdULL;
	hash ^= hash >> 33;
	hash *= 0xc4ceb9fe1a85ec53ULL;
	hash ^= hash >> 33;
	return hash;
}

static void promote_filter_set(u64 *words, u32 mask, u64 key)
{
	u64 h1 = promote_mix64(key);
	u64 h2 = ((h1 << 32) | (h1 >> 32)) ^ 0x9e3779b97f4a7c15ULL;
	u32 block_mask = ((mask + 1) / 512) - 1;
	u32 block_base = ((u32)h1 & block_mask) * 512;
	u32 i;

	h2 |= 1ULL;

	for (i = 0; i < EXT5_PROMOTE_FILTER_PROBES; i++) {
		u32 bit = block_base + ((u32)(h2 + i * h1) & 511);

		words[bit >> 6] |= 1ULL << (bit & 63);
	}
}

static u32 promote_power2_filter_bits(u32 nr_entries)
{
	u64 bpe = clamp_t(u64, READ_ONCE(ext5_li_bloom_bits_per_entry), 4, 16);
	u64 target = (u64)nr_entries * bpe;
	u32 bits = EXT5_PROMOTE_FILTER_MIN_BITS;

	if (!READ_ONCE(ext5_li_bloom_enabled))
		return 0;
	while (bits < target && bits <= (1U << 30))
		bits <<= 1;
	return bits;
}

static int promote_grow_slots(struct promote_ctx *c, u32 want)
{
	struct promote_slot *grown;
	u32 new_cap;

	if (c->nr_slots + want <= c->cap_slots)
		return 0;
	new_cap = c->cap_slots ? c->cap_slots * 2 : 256;
	while (new_cap < c->nr_slots + want)
		new_cap *= 2;
	/* Zero the tail: every abort path may release shadow ownership. */
	grown = kvcalloc(new_cap, sizeof(*grown), GFP_KERNEL);
	if (!grown)
		return -ENOMEM;
	if (c->nr_slots)
		memcpy(grown, c->slots, c->nr_slots * sizeof(*grown));
	kvfree(c->slots);
	c->slots = grown;
	c->cap_slots = new_cap;
	return 0;
}

static int promote_grow_names(struct promote_ctx *c, u32 want)
{
	char *grown;
	u32 new_cap;

	if (c->names_off + want <= c->cap_names)
		return 0;
	new_cap = c->cap_names ? c->cap_names : 4096;
	while (new_cap < c->names_off + want)
		new_cap *= 2;
	grown = kvmalloc(new_cap, GFP_KERNEL);
	if (!grown)
		return -ENOMEM;
	if (c->names_off)
		memcpy(grown, c->names, c->names_off);
	kvfree(c->names);
	c->names = grown;
	c->cap_names = new_cap;
	return 0;
}

static int promote_grow_dirs(struct promote_ctx *c, u32 want)
{
	struct promote_dir *grown;
	u32 new_cap;

	if (c->nr_dirs + want <= c->cap_dirs)
		return 0;
	new_cap = c->cap_dirs ? c->cap_dirs * 2 : 16;
	while (new_cap < c->nr_dirs + want)
		new_cap *= 2;
	grown = kvmalloc_array(new_cap, sizeof(*grown), GFP_KERNEL);
	if (!grown)
		return -ENOMEM;
	if (c->nr_dirs)
		memcpy(grown, c->dirs, c->nr_dirs * sizeof(*grown));
	kvfree(c->dirs);
	c->dirs = grown;
	c->cap_dirs = new_cap;
	return 0;
}

static int promote_add_slot(struct promote_ctx *c, u32 plid,
			    const char *name, u8 name_len, u32 child_ino,
			    u8 file_type, u8 flags)
{
	u64 full_hash, tb;
	u32 hash32;
	int err;

	err = promote_grow_slots(c, 1);
	if (err)
		return err;
	err = promote_grow_names(c, name_len);
	if (err)
		return err;

	memcpy(c->names + c->names_off, name, name_len);
	full_hash = siphash(name, name_len, &c->hash_key);
	hash32 = hsiphash(name, name_len,
		(const hsiphash_key_t *)&c->hash_key);
	tb = (full_hash & 0xffffffff00000000ULL) | hash32;
	c->slots[c->nr_slots].key = promote_make_key(plid, hash32);
	c->slots[c->nr_slots].tiebreak64 = tb;
	c->slots[c->nr_slots].ino = child_ino;
	c->slots[c->nr_slots].src_name_off = c->names_off;
	c->slots[c->nr_slots].name_off = 0;
	c->slots[c->nr_slots].name_len = name_len;
	c->slots[c->nr_slots].file_type = file_type;
	c->slots[c->nr_slots].flags = flags;
	c->nr_slots++;
	c->names_off += name_len;
	return 0;
}

struct promote_walk_entry_ctx {
	struct promote_ctx *ctx;
	u32 plid;
};

static int promote_walk_live_entry(void *arg, const char *name, u8 name_len,
				   u32 child_ino, unsigned int file_type)
{
	struct promote_walk_entry_ctx *walk = arg;
	struct promote_ctx *c = walk->ctx;
	u8 flags = 0;

	if (!child_ino || !name_len ||
	    (name_len == 1 && name[0] == '.') ||
	    (name_len == 2 && name[0] == '.' && name[1] == '.'))
		return 0;
	if (file_type == DT_DIR) {
		struct inode *child;
		bool recurse_child = true;
		bool needs_demote = false;

		/* Mode 2 promotes one directory: every child directory is a boundary. */
		if (c->directory_only) {
			flags |= LIDIR_SLOT_SUBTREE_BOUNDARY;
			goto add_slot;
		}

		child = ext5_iget(c->sb, child_ino, EXT5_IGET_NORMAL);
		if (IS_ERR(child))
			return PTR_ERR(child);
		if (EXT5_I(child)->i_flags & EXT5_LIDIR_INTERIOR_FL) {
			iput(child);
			return -EBUSY;
		}
		if (EXT5_I(child)->i_flags & EXT5_LIDIR_ROOT_FL) {
			if (!READ_ONCE(ext5_li_promote_subsume)) {
				flags |= LIDIR_SLOT_SUBTREE_BOUNDARY;
				recurse_child = false;
			} else {
				/* Defer demotion until the parent's shared lock is dropped: demotion
				 * takes the transition mutex before namespace locks. */
				needs_demote = true;
			}
		}
		if (recurse_child) {
			int err = promote_grow_dirs(c, 1);

			if (err) {
				iput(child);
				return err;
			}
			c->dirs[c->nr_dirs] = (struct promote_dir) {
				.inode = child,
				.plid = c->nr_dirs,
				.parent_plid = walk->plid,
				.needs_demote = needs_demote,
			};
			c->nr_dirs++;
		} else {
			iput(child);
		}
	}

add_slot:
	return promote_add_slot(c, walk->plid, name, name_len, child_ino,
				file_type, flags);
}

/*
 * Emit one directory's live entries as slots under its PLID; mutable child
 * directories join c->dirs.  Caller holds the directory's shared lock; only
 * one directory is locked at a time.
 */
static int promote_walk_dir(struct promote_ctx *c, u32 dir_idx)
{
	struct super_block *sb = c->sb;
	/* Copy these by value: promote_grow_dirs() may reallocate c->dirs. */
	u32 dir_plid = c->dirs[dir_idx].plid;
	struct inode *inode = c->dirs[dir_idx].inode;
	unsigned int blocksize = sb->s_blocksize;
	loff_t i_size = i_size_read(inode);
	ext5_lblk_t nblocks = (i_size + blocksize - 1) >> sb->s_blocksize_bits;
	struct promote_walk_entry_ctx walk = { .ctx = c, .plid = dir_plid };
	unsigned long *live_blocks = NULL;
	bool indexed = ext5_test_inode_flag(inode, EXT5_INODE_INDEX);
	ext5_lblk_t n;
	int err = 0;

	if (!ext5_li_dir_supported(inode))
		return -EOPNOTSUPP;
	c->mutable_bytes += i_size;
	if (indexed) {
		if (nblocks > U32_MAX)
			return -EFBIG;
		live_blocks = bitmap_zalloc(nblocks, GFP_KERNEL);
		if (!live_blocks)
			return -ENOMEM;
		err = ext5_htree_mark_live_leaf_blocks(inode, live_blocks,
						       (u32)nblocks);
		if (err)
			goto out;
	}

	for (n = 0; n < nblocks; n++) {
		struct buffer_head *bh;
		char *p;
		size_t pos_in_block;
		size_t end;

		if (indexed && !test_bit(n, live_blocks))
			continue;
		bh = ext5_bread(NULL, inode, n, 0);
		if (IS_ERR(bh)) {
			err = PTR_ERR(bh);
			goto out;
		}
		if (!bh)
			continue;	/* sparse */

		end = blocksize;
		if ((u64)(n + 1) << sb->s_blocksize_bits > i_size)
			end = i_size - ((u64)n << sb->s_blocksize_bits);

		p = bh->b_data;
		pos_in_block = 0;
		while (pos_in_block + 8 <= end) {
			struct ext5_dir_entry_2 *de = (void *)p;
			u16 rec_len = le16_to_cpu(de->rec_len);
			u8  name_len = de->name_len;
			u32 child_ino = le32_to_cpu(de->inode);

			if (rec_len == 0 || rec_len & 3 ||
			    pos_in_block + rec_len > end) {
				err = -EIO;
				goto release_bh;
			}

			if (child_ino && name_len > 0) {
				u8 disk_ft = de->file_type;
				u8 ft = disk_ft < EXT5_FT_MAX ?
					ext5_filetype_table[disk_ft] : DT_UNKNOWN;

				err = promote_walk_live_entry(&walk, de->name,
							      name_len, child_ino, ft);
				if (err)
					goto release_bh;
			}

			p += rec_len;
			pos_in_block += rec_len;
		}
		brelse(bh);
		continue;
release_bh:
		brelse(bh);
		goto out;
	}
out:
	bitmap_free(live_blocks);
	return err;
}

/*
 * Lock the snapshot parent-before-child in PLID order and check that no
 * directory changed.  Nothing is converted yet, so a failure only aborts.
 */
static int promote_lock_and_validate(struct promote_ctx *c)
{
	u32 i;

	mutex_lock(&EXT5_SB(c->dirs[0].inode->i_sb)->li_transition_lock);
	c->transition_locked = true;
	down_write_nest_lock(&c->dirs[0].inode->i_rwsem,
				     &EXT5_SB(c->dirs[0].inode->i_sb)->li_transition_lock);
	for (i = 1; i < c->nr_dirs; i++)
		down_write_nest_lock(&c->dirs[i].inode->i_rwsem,
				     &EXT5_SB(c->dirs[0].inode->i_sb)->li_transition_lock);

	for (i = 0; i < c->nr_dirs; i++) {
		struct ext5_inode_info *ei = EXT5_I(c->dirs[i].inode);

		if (inode_query_iversion(c->dirs[i].inode) !=
		    c->dirs[i].i_version)
			return -EAGAIN;
		if (ei->i_flags & (EXT5_LIDIR_ROOT_FL |
				   EXT5_LIDIR_INTERIOR_FL))
			return -EAGAIN;
	}
	if (c->policy_attempt &&
	    (READ_ONCE(EXT5_I(c->dirs[0].inode)->i_li_last_mut) !=
					c->snapshot_last_mut ||
	     ext5_li_op_distance(ext5_li_op_now(c->sb), c->snapshot_last_mut) <
				ext5_li_t_star(c->sb, c->nr_slots)))
		return -EAGAIN;
	if (c->policy_attempt && READ_ONCE(ext5_li_promote_idle_ms) &&
	    time_before(jiffies,
		READ_ONCE(EXT5_I(c->dirs[0].inode)->i_li_last_access_jiffies) +
		msecs_to_jiffies(READ_ONCE(ext5_li_promote_idle_ms))))
		return -EAGAIN;
	return 0;
}

static void promote_unlock_all(struct promote_ctx *c)
{
	u32 i;

	for (i = c->nr_dirs; i > 1; i--)
		inode_unlock(c->dirs[i - 1].inode);
	if (c->nr_dirs)
		inode_unlock(c->dirs[0].inode);
	if (c->transition_locked) {
		c->transition_locked = false;
		mutex_unlock(&EXT5_SB(c->dirs[0].inode->i_sb)->li_transition_lock);
	}
}

static void promote_release_dirs(struct promote_ctx *c)
{
	u32 i;

	if (!c->dirs)
		return;
	for (i = 0; i < c->nr_dirs; i++) {
		if (!c->dirs[i].is_root && c->dirs[i].inode)
			iput(c->dirs[i].inode);
	}
	kvfree(c->dirs);
	c->dirs = NULL;
	c->nr_dirs = 0;
	c->cap_dirs = 0;
}

/*
 * Collect the subtree.  On return c->slots is filled (unsorted), c->dirs holds
 * every visited directory with a reference, no lock is held, and the recorded
 * i_version values define the snapshot to validate.
 */
static int promote_collect_subtree(struct promote_ctx *c, struct inode *root)
{
	int err;
	u32 i;

	err = promote_grow_dirs(c, 1);
	if (err)
		return err;
	c->dirs[0] = (struct promote_dir) {
		.inode = root,
		.is_root = true,
	};
	c->nr_dirs = 1;

	/* The shared lock excludes mutation but not lookups; a later mutation
	 * changes i_version and fails validation. */
	for (i = 0; i < c->nr_dirs; i++) {
		if (c->dirs[i].needs_demote) {
			u32 parent = c->dirs[i].parent_plid;

			/* The dirent names the parent whether or not a dentry is cached;
			 * revalidate it under the parent lock after the transition mutex. */
			err = ext5_demote_subtree_for_promote(c->dirs[i].inode,
				c->dirs[parent].inode, c->dirs[parent].i_version);
			if (err)
				return err;
			c->dirs[i].needs_demote = false;
		}
		inode_lock_shared(c->dirs[i].inode);
		if (EXT5_I(c->dirs[i].inode)->i_flags &
		    (EXT5_LIDIR_ROOT_FL | EXT5_LIDIR_INTERIOR_FL)) {
			inode_unlock_shared(c->dirs[i].inode);
			return i ? -EBUSY : -EALREADY;
		}
		err = promote_walk_dir(c, i);
		if (!err)
			c->dirs[i].i_version =
				inode_query_iversion(c->dirs[i].inode);
		if (!err && !i)
			c->snapshot_last_mut =
				READ_ONCE(EXT5_I(c->dirs[i].inode)->i_li_last_mut);
		inode_unlock_shared(c->dirs[i].inode);
		if (err)
			return err;
	}
	return 0;
}

/* Serialize the base region around an error-bounded RadixSpline. */
static int promote_build_blob_v2(struct promote_ctx *c,
				 void **blob_out, size_t *blob_size_out)
{
	u32 nr_slots = c->nr_slots;
	u32 parent_count = c->nr_dirs;
	u32 filter_bits, filter_words;
	size_t off_filter, off_radix, off_spline, off_slots;
	size_t off_parent_index, off_names, base_size;
	size_t blob_size, base_off = LIDIR_BASE_AREA_OFF;
	struct sfs_rs_model *model = NULL;
	u64 *keys = NULL;
	void *blob = NULL;
	struct lidir_disk_descriptor *desc;
	__le32 *out_radix;
	struct lidir_disk_spline_point *out_spline;
	struct lidir_disk_slot *out_slots;
	__le32 *out_parent_index;
	char *out_names;
	u64 *out_filter;
	char *packed_names = NULL;
	u32 collision_run = 1, run = 1;
	u32 final_name_off = 0;
	u32 i;
	int err = 0;

	if (nr_slots) {
		packed_names = kvmalloc(c->names_off ? c->names_off : 1,
					GFP_KERNEL);
		keys = kvmalloc_array(nr_slots, sizeof(*keys), GFP_KERNEL);
		if (!packed_names || !keys) {
			err = -ENOMEM;
			goto out;
		}
		BUILD_BUG_ON(offsetof(struct promote_slot, key) !=
			     offsetof(struct sfs_li_sort_prefix, key));
		BUILD_BUG_ON(offsetof(struct promote_slot, tiebreak64) !=
			     offsetof(struct sfs_li_sort_prefix, tiebreak64));
		BUILD_BUG_ON(offsetof(struct promote_slot, src_name_off) !=
			     offsetof(struct sfs_li_sort_prefix, stable_name_off));
		BUILD_BUG_ON(offsetof(struct promote_slot, name_len) !=
			     offsetof(struct sfs_li_sort_prefix, name_len));
		BUILD_BUG_ON(sizeof(struct promote_slot) != 32);
		err = sfs_li_radix_sort(c->slots, nr_slots);
		if (err)
			goto out;
		for (i = 0; i < nr_slots; i++) {
			u32 packed_off = final_name_off;

			memcpy(packed_names + final_name_off,
			       c->names + c->slots[i].src_name_off,
			       c->slots[i].name_len);
			c->slots[i].name_off = final_name_off;
			final_name_off += c->slots[i].name_len;
			if (ext5_has_feature_dir_index(c->sb)) {
				struct dx_hash_info hinfo = {
					.hash_version = EXT5_SB(c->sb)->s_def_hash_version,
					.seed = EXT5_SB(c->sb)->s_hash_seed,
				};
				u32 plid = (u32)(c->slots[i].key >> 32);

				if (hinfo.hash_version <= DX_HASH_TEA)
					hinfo.hash_version +=
						EXT5_SB(c->sb)->s_hash_unsigned;
				err = ext5fs_dirhash(c->dirs[plid].inode,
					packed_names + packed_off,
					c->slots[i].name_len, &hinfo);
				if (err < 0)
					goto out;
				c->slots[i].dx_hash = hinfo.hash;
			} else {
				c->slots[i].dx_hash = 0;
			}
			keys[i] = c->slots[i].key;
			if (i && keys[i] == keys[i - 1]) {
				run++;
				if (run > collision_run)
					collision_run = run;
			} else {
				run = 1;
			}
		}
		kvfree(c->names);
		c->names = packed_names;
		c->names_off = final_name_off;
		packed_names = NULL;

		err = sfs_rs_build(keys, nr_slots,
			READ_ONCE(ext5_li_radix_bits),
			READ_ONCE(ext5_li_spline_epsilon), collision_run,
			&model);
		if (err)
			goto out;
	}

	filter_bits = promote_power2_filter_bits(nr_slots ? nr_slots : 1);
	filter_words = filter_bits / 64;
	off_filter = 0;
	off_radix = ALIGN(filter_words * sizeof(u64), 8);
	off_spline = ALIGN(off_radix +
		(model ? model->radix_count : 0) * sizeof(__le32), 8);
	off_slots = ALIGN(off_spline +
		(model ? model->point_count : 0) *
			sizeof(struct lidir_disk_spline_point), 8);
	off_parent_index = ALIGN(off_slots +
		nr_slots * sizeof(struct lidir_disk_slot), 8);
	off_names = ALIGN(off_parent_index +
		(parent_count + 1) * sizeof(__le32), 8);
	base_size = off_names + c->names_off;
	blob_size = ALIGN(base_off + base_size, c->sb->s_blocksize);

	blob = vzalloc(blob_size);
	if (!blob) {
		err = -ENOMEM;
		goto out;
	}
	desc = blob;
	out_filter = (u64 *)((char *)blob + base_off + off_filter);
	out_radix = (__le32 *)((char *)blob + base_off + off_radix);
	out_spline = (struct lidir_disk_spline_point *)
		((char *)blob + base_off + off_spline);
	out_slots = (struct lidir_disk_slot *)
		((char *)blob + base_off + off_slots);
	out_parent_index = (__le32 *)
		((char *)blob + base_off + off_parent_index);
	out_names = (char *)blob + base_off + off_names;

	for (i = 0; i < nr_slots; i++) {
		out_slots[i].key = cpu_to_le64(c->slots[i].key);
		out_slots[i].ino = cpu_to_le32(c->slots[i].ino);
		out_slots[i].name_off = cpu_to_le32(c->slots[i].name_off);
		out_slots[i].dx_hash = cpu_to_le32(c->slots[i].dx_hash);
		out_slots[i].name_len = c->slots[i].name_len;
		out_slots[i].file_type = c->slots[i].file_type;
		out_slots[i].flags = c->slots[i].flags;
		if (filter_bits)
			promote_filter_set(out_filter, filter_bits - 1,
					   c->slots[i].key);
	}
	if (model) {
		for (i = 0; i < model->radix_count; i++)
			out_radix[i] = cpu_to_le32(model->radix[i]);
		for (i = 0; i < model->point_count; i++) {
			out_spline[i].key = cpu_to_le64(model->points[i].key);
			out_spline[i].rank = cpu_to_le32(model->points[i].rank);
			out_spline[i].reserved = 0;
		}
	}

	{
		u32 current_plid = 0;

		out_parent_index[0] = 0;
		for (i = 0; i < nr_slots; i++) {
			u32 slot_plid = (u32)(c->slots[i].key >> 32);

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
	if (c->names_off)
		memcpy(out_names, c->names, c->names_off);

	desc->magic = cpu_to_le32(LIDIR_MAGIC);
	desc->version = cpu_to_le32(LIDIR_VERSION);
	desc->header_bytes = cpu_to_le32(sizeof(*desc));
	desc->generation = cpu_to_le64(1);
	memcpy(desc->hash_seed, EXT5_SB(c->sb)->s_li_hash_key,
	       sizeof(desc->hash_seed));
	desc->base_off = cpu_to_le64(base_off);
	desc->base_bytes = cpu_to_le64(base_size);
	desc->delta_off = cpu_to_le64(
		ALIGN(base_off + base_size, c->sb->s_blocksize));
	desc->delta_bytes = cpu_to_le64(
		ext5_li_delta_reserve(c->sb, nr_slots, c->names_off));
	desc->entry_count = cpu_to_le32(nr_slots);
	desc->parent_count = cpu_to_le32(parent_count);
	desc->filter_bits = cpu_to_le32(filter_bits);
	desc->radix_count = cpu_to_le32(model ? model->radix_count : 0);
	desc->spline_count = cpu_to_le32(model ? model->point_count : 0);
	desc->spline_epsilon = cpu_to_le32(model ? model->max_error : 0);
	desc->spline_corridor_error = cpu_to_le32(
		model ? model->corridor_error : 0);
	desc->radix_bits = cpu_to_le32(model ? model->radix_bits : 0);
	desc->radix_shift = cpu_to_le32(model ? model->shift_bits : 0);
	desc->model_min_key = cpu_to_le64(model ? model->min_key : 0);
	desc->model_max_key = cpu_to_le64(model ? model->max_key : 0);
	desc->filter_off = cpu_to_le64(off_filter);
	desc->radix_off = cpu_to_le64(off_radix);
	desc->spline_off = cpu_to_le64(off_spline);
	desc->slots_off = cpu_to_le64(off_slots);
	desc->parent_index_off = cpu_to_le64(off_parent_index);
	desc->names_off = cpu_to_le64(off_names);
	desc->names_bytes = cpu_to_le64(c->names_off);
	desc->base_csum = cpu_to_le32(crc32_le(0,
		(const u8 *)blob + base_off, base_size));
	desc->header_csum = cpu_to_le32(crc32_le(0,
		(const u8 *)desc, LIDIR_DESC_CSUM_LEN));
	memcpy((char *)blob + LIDIR_DESC_BYTES, blob, LIDIR_DESC_BYTES);

	*blob_out = blob;
	*blob_size_out = blob_size;
	blob = NULL;
out:
	vfree(blob);
	kvfree(keys);
	kvfree(packed_names);
	sfs_rs_destroy(model);
	return err;
}

/* Write the blob, one short transaction per block to bound credits. */
static int promote_write_blob(struct inode *inode, const void *buf,
			      size_t size)
{
	struct super_block *sb = inode->i_sb;
	unsigned int blocksize = sb->s_blocksize;
	ext5_lblk_t nblocks = (size + blocksize - 1) >> sb->s_blocksize_bits;
	ext5_lblk_t blk;
	int err = 0;

	for (blk = 0; blk < nblocks; blk++) {
		handle_t *handle;
		struct buffer_head *bh;
		size_t off = (size_t)blk << sb->s_blocksize_bits;
		size_t this = min_t(size_t, blocksize, size - off);

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

		BUFFER_TRACE(bh, "get_write_access");
		err = ext5_journal_get_write_access(handle, sb, bh,
						    EXT5_JTR_NONE);
		if (err) {
			brelse(bh);
			ext5_journal_stop(handle);
			return err;
		}

		memcpy(bh->b_data, (const char *)buf + off, this);
		if (this < blocksize)
			memset(bh->b_data + this, 0, blocksize - this);
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
	}

	if ((loff_t)size != i_size_read(inode)) {
		handle_t *handle = ext5_journal_start(inode, EXT5_HT_INODE, 1);

		if (IS_ERR(handle))
			return PTR_ERR(handle);
		i_size_write(inode, (loff_t)size);
		EXT5_I(inode)->i_disksize = size;
		ext5_mark_inode_dirty(handle, inode);
		err = ext5_journal_stop(handle);
		if (err)
			return err;
	}

	return sync_mapping_buffers(inode->i_mapping);
}

static void promote_memswap(void *left, void *right, size_t bytes)
{
	u8 *a = left, *b = right;

	while (bytes--) {
		swap(*a, *b);
		a++;
		b++;
	}
}

/* Allocate an unreachable, orphan-tracked inode with `original`'s quota
 * owners.  It borrows the original's checksum seed, as extent migration does,
 * so extent blocks stay valid when their roots move. */
static struct inode *promote_create_shadow(struct inode *original,
					    u32 *saved_csum_seed)
{
	struct inode *shadow;
	handle_t *handle;
	uid_t owner[2] = { i_uid_read(original), i_gid_read(original) };
	int credits = EXT5_DATA_TRANS_BLOCKS(original->i_sb) +
		EXT5_INDEX_EXTRA_TRANS_BLOCKS + 8;
	int err, stop_err;

	handle = ext5_journal_start(original, EXT5_HT_INODE, credits);
	if (IS_ERR(handle))
		return ERR_CAST(handle);
	shadow = ext5_new_inode(handle, original, S_IFREG | 0600, NULL,
				original->i_ino, owner, 0);
	if (IS_ERR(shadow)) {
		err = PTR_ERR(shadow);
		ext5_journal_stop(handle);
		return ERR_PTR(err);
	}
	*saved_csum_seed = EXT5_I(shadow)->i_csum_seed;
	shadow->i_op = &ext5_file_inode_operations;
	shadow->i_fop = &ext5_file_operations;
	ext5_set_aops(shadow);

	/* Swapping quota-charged blocks needs identical owners; a non-inherited
	 * project ID is refused. */
	if (!projid_eq(EXT5_I(shadow)->i_projid,
		       EXT5_I(original)->i_projid)) {
		err = -EOPNOTSUPP;
		goto fail_new;
	}
	clear_nlink(shadow);
	err = ext5_orphan_add(handle, shadow);
	if (!err)
		err = ext5_mark_inode_dirty(handle, shadow);
	stop_err = ext5_journal_stop(handle);
	if (!err)
		err = stop_err;
	unlock_new_inode(shadow);
	if (err) {
		iput(shadow);
		return ERR_PTR(err);
	}
	/* Direct staging bypasses the write path, so attach the ordered-data
	 * inode to jbd3 before mapping blocks. */
	err = ext5_inode_attach_jinode(shadow);
	if (err) {
		iput(shadow);
		return ERR_PTR(err);
	}
	EXT5_I(shadow)->i_csum_seed = EXT5_I(original)->i_csum_seed;
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

/* Truncate a retired tree under its original checksum seed, then restore
 * the hidden inode's own seed.  Runs after namespace locks drop. */
static void promote_discard_shadow(struct inode *shadow, u32 saved_csum_seed)
{
	handle_t *handle;
	int err;

	if (!shadow)
		return;
	inode_lock(shadow);
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
	}
	inode_unlock(shadow);
	iput(shadow);
}

static void promote_discard_shadows(struct promote_ctx *ctx)
{
	u32 i;

	for (i = 0; i < ctx->nr_dirs; i++) {
		promote_discard_shadow(ctx->dirs[i].shadow,
				       ctx->dirs[i].shadow_csum_seed);
		ctx->dirs[i].shadow = NULL;
	}
}

static int promote_prepare_shadows(struct promote_ctx *ctx,
				   const void *blob, size_t blob_size)
{
	u32 i;
	int err;

	for (i = 0; i < ctx->nr_dirs; i++) {
		ctx->dirs[i].shadow = promote_create_shadow(ctx->dirs[i].inode,
						&ctx->dirs[i].shadow_csum_seed);
		if (IS_ERR(ctx->dirs[i].shadow)) {
			err = PTR_ERR(ctx->dirs[i].shadow);
			ctx->dirs[i].shadow = NULL;
			return err;
		}
	}

	/* Only the root shadow holds the stream; interior shadows stay empty. */
	inode_lock(ctx->dirs[0].shadow);
	err = promote_write_blob(ctx->dirs[0].shadow, blob, blob_size);
	inode_unlock(ctx->dirs[0].shadow);
	return err;
}

/* Swap the prebuilt blob's extent tree with the root's conventional tree;
 * the old tree stays with the orphaned shadow until cleanup. */
static int promote_publish_root_shadow(struct inode *root,
				       struct inode *shadow)
{
	struct ext5_inode_info *ei = EXT5_I(root);
	struct ext5_inode_info *sei = EXT5_I(shadow);
	handle_t *handle;
	loff_t size;
	blkcnt_t blocks;
	unsigned short bytes;
	u32 flags;
	int err, stop_err;

	down_write_nest_lock(&shadow->i_rwsem,
			     &EXT5_SB(root->i_sb)->li_transition_lock);
	handle = ext5_journal_start(root, EXT5_HT_MOVE_EXTENTS, 8);
	if (IS_ERR(handle)) {
		inode_unlock(shadow);
		return PTR_ERR(handle);
	}
	ext5_double_down_write_data_sem(root, shadow);
	promote_memswap(ei->i_data, sei->i_data, sizeof(ei->i_data));
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
	ei->i_flags = (ei->i_flags & ~(EXT5_EXTENTS_FL | EXT5_INDEX_FL)) |
		(sei->i_flags & (EXT5_EXTENTS_FL | EXT5_INDEX_FL)) |
		EXT5_LIDIR_ROOT_FL;
	sei->i_flags = (sei->i_flags & ~(EXT5_EXTENTS_FL | EXT5_INDEX_FL)) |
		flags;
	ext5_es_remove_extent(root, 0, EXT_MAX_BLOCKS);
	ext5_es_remove_extent(shadow, 0, EXT_MAX_BLOCKS);
	atomic_set(&ei->i_li_compact_count, 0);
	atomic64_set(&ei->i_li_compact_scheduled_generation, 0);
	atomic64_set(&ei->i_li_compact_urgent_generation, 0);
	atomic64_set(&ei->i_li_refill_classified_generation, 0);
	WRITE_ONCE(ei->i_li_armed, 0);
	inode_inc_iversion(root);
	inode_set_mtime_to_ts(root, inode_set_ctime_current(root));
	err = ext5_mark_inode_dirty(handle, root);
	if (!err)
		err = ext5_mark_inode_dirty(handle, shadow);
	ext5_double_up_write_data_sem(root, shadow);
	inode_unlock(shadow);
	stop_err = ext5_journal_stop(handle);
	if (!err)
		err = stop_err;
	return err;
}

/* Move an interior's extent tree to its retirement inode and install the
 * routing pair.  No block is freed under the namespace locks. */
static int promote_publish_interior_shadow(struct inode *dir,
					   struct inode *shadow,
					   ino_t root_ino, u32 plid)
{
	struct ext5_inode_info *ei = EXT5_I(dir);
	struct ext5_inode_info *sei = EXT5_I(shadow);
	handle_t *handle;
	loff_t size;
	blkcnt_t blocks;
	unsigned short bytes;
	u32 flags;
	int err, stop_err;
	int i;

	down_write_nest_lock(&shadow->i_rwsem,
			     &EXT5_SB(dir->i_sb)->li_transition_lock);
	handle = ext5_journal_start(dir, EXT5_HT_MOVE_EXTENTS, 8);
	if (IS_ERR(handle)) {
		inode_unlock(shadow);
		return PTR_ERR(handle);
	}
	ext5_double_down_write_data_sem(dir, shadow);
	promote_memswap(ei->i_data, sei->i_data, sizeof(ei->i_data));
	size = i_size_read(dir);
	i_size_write(dir, i_size_read(shadow));
	i_size_write(shadow, size);
	swap(ei->i_disksize, sei->i_disksize);
	blocks = dir->i_blocks;
	dir->i_blocks = shadow->i_blocks;
	shadow->i_blocks = blocks;
	bytes = dir->i_bytes;
	dir->i_bytes = shadow->i_bytes;
	shadow->i_bytes = bytes;
	flags = ei->i_flags & (EXT5_EXTENTS_FL | EXT5_INDEX_FL);
	sei->i_flags = (sei->i_flags & ~(EXT5_EXTENTS_FL | EXT5_INDEX_FL)) |
		flags;

	/* Replace the empty extent header with the route words. */
	ei->i_data[0] = cpu_to_le32((u32)root_ino);
	ei->i_data[1] = cpu_to_le32(plid);
	for (i = 2; i < EXT5_N_BLOCKS; i++)
		ei->i_data[i] = 0;
	ei->i_flags |= EXT5_LIDIR_INTERIOR_FL;
	ei->i_flags &= ~(EXT5_EXTENTS_FL | EXT5_INDEX_FL);
	ei->i_subtree_root_ino = root_ino;
	ei->i_parent_local_id_in_subtree = plid;
	ext5_es_remove_extent(dir, 0, EXT_MAX_BLOCKS);
	ext5_es_remove_extent(shadow, 0, EXT_MAX_BLOCKS);
	inode_inc_iversion(dir);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	err = ext5_mark_inode_dirty(handle, dir);
	if (!err)
		err = ext5_mark_inode_dirty(handle, shadow);
	ext5_double_up_write_data_sem(dir, shadow);
	inode_unlock(shadow);
	stop_err = ext5_journal_stop(handle);
	if (!err)
		err = stop_err;
	return err;
}

static int __ext5_promote_subtree(struct inode *root_dir,
				  bool policy_attempt, u64 expected_last_mut)
{
	struct ext5_inode_info *root_ei = EXT5_I(root_dir);
	struct promote_ctx ctx = {
		.sb = root_dir->i_sb,
		.policy_attempt = policy_attempt,
		.directory_only = policy_attempt &&
			READ_ONCE(ext5_li_policy_mode) == 2,
		.expected_last_mut = expected_last_mut,
	};
	void *blob = NULL;
	size_t blob_size = 0;
	u64 build_start_ns;
	u64 attempt_start_ns = 0;
	bool publish_locked = false;
	bool publication_started = false;
	bool full_build_ready = false;
	bool validation_abort = false;
	bool ready_tracked = false;
	u32 i;
	int err;

	if (!S_ISDIR(root_dir->i_mode))
		return -ENOTDIR;
	if (!ext5_li_dir_supported(root_dir))
		return -EOPNOTSUPP;
	if (root_ei->i_flags & EXT5_LIDIR_ROOT_FL)
		return -EALREADY;
	if (root_ei->i_flags & EXT5_LIDIR_INTERIOR_FL)
		return -EBUSY;
	build_start_ns = ktime_get_ns();
	attempt_start_ns = build_start_ns;
	if (policy_attempt)
		ext5_promote_track_policy_start();

	memcpy(&ctx.hash_key, EXT5_SB(ctx.sb)->s_li_hash_key,
	       sizeof(ctx.hash_key));

	ext5_promote_stats_call();

	err = promote_collect_subtree(&ctx, root_dir);
	if (err)
		goto out_dirs;
	if (ctx.directory_only &&
	    ctx.nr_slots < ext5_li_effective_size_floor(ctx.sb)) {
		err = -EAGAIN;
		goto out_dirs;
	}
	/* Recheck T* with the exact size before sorting and building. */
	if (policy_attempt &&
	    (ctx.snapshot_last_mut != ctx.expected_last_mut ||
	     ext5_li_op_distance(ext5_li_op_now(ctx.sb),
				 ctx.snapshot_last_mut) <
		ext5_li_t_star(ctx.sb, ctx.nr_slots))) {
		err = -EAGAIN;
		goto out_dirs;
	}

	err = promote_build_blob_v2(&ctx, &blob, &blob_size);
	if (err)
		goto out_dirs;
	if (policy_attempt &&
	    READ_ONCE(root_ei->i_li_last_mut) != ctx.expected_last_mut) {
		err = -EAGAIN;
		goto out_dirs;
	}
	err = promote_prepare_shadows(&ctx, blob, blob_size);
	if (err)
		goto out_dirs;
	full_build_ready = true;
	if (policy_attempt) {
		ext5_promote_track_policy_ready(true);
		ready_tracked = true;
		if (READ_ONCE(ext5_li_debug_controls) &&
		    READ_ONCE(ext5_li_debug_validation_delay_ms)) {
			u64 delay_start_ns = ktime_get_ns();

			msleep(READ_ONCE(ext5_li_debug_validation_delay_ms));
			build_start_ns += ktime_get_ns() - delay_start_ns;
		}
	}
	if (policy_attempt && READ_ONCE(ext5_li_promote_idle_ms)) {
		unsigned long due = READ_ONCE(root_ei->i_li_last_access_jiffies) +
			msecs_to_jiffies(READ_ONCE(ext5_li_promote_idle_ms));

		if (time_before(jiffies, due)) {
			u64 idle_start_ns = ktime_get_ns();

			msleep(jiffies_to_msecs(due - jiffies) + 1);
			/* The quiet wait is not build cost: keep it out of T* learning. */
			build_start_ns += ktime_get_ns() - idle_start_ns;
		}
	}

	err = promote_lock_and_validate(&ctx);
	if (ready_tracked) {
		ext5_promote_track_policy_ready(false);
		ready_tracked = false;
	}
	publish_locked = true;
	if (err == -EAGAIN && full_build_ready)
		validation_abort = true;
	if (err)
		goto out_unlock;

	/* Only a validated attempt changes the filesystem format feature. */
	ext5_set_feature_lidir(root_dir->i_sb);
	for (i = 0; i < ctx.nr_dirs; i++)
		ext5_li_policy_forget_inode(ctx.dirs[i].inode);

	/* Publish the root before any interior; its old tree is reclaimed after
	 * the locks drop. */
	err = promote_publish_root_shadow(root_dir, ctx.dirs[0].shadow);
	if (err)
		goto out_unlock;
	publication_started = true;

	for (i = 1; i < ctx.nr_dirs; i++) {
		err = promote_publish_interior_shadow(ctx.dirs[i].inode,
						      ctx.dirs[i].shadow,
						      root_dir->i_ino,
						      ctx.dirs[i].plid);
		if (err)
			goto out_unlock;
	}
	err = 0;

out_unlock:
	if (publish_locked)
		promote_unlock_all(&ctx);
	/* Old pages are stale after the swap; drop them outside the locks. */
	if (publication_started)
		for (i = 0; i < ctx.nr_dirs; i++)
			truncate_inode_pages(ctx.dirs[i].inode->i_mapping, 0);
	/* The model is rebuildable, so no synchronous metadata flush here. */
	promote_discard_shadows(&ctx);
	if (!err && publication_started) {
		root_ei->i_li_active_interiors = ctx.nr_dirs - 1;
		root_ei->i_li_active_entries = ctx.nr_slots;
		atomic64_set(&root_ei->i_li_refill_start_op,
			     ext5_li_op_clock_now_exact(ctx.sb));
		atomic64_set(&root_ei->i_li_refill_start_used_bytes, 0);
		atomic64_set(&root_ei->i_li_refill_classified_generation, 0);
		atomic64_set(&root_ei->i_li_last_refill_span_ops, 0);
		atomic64_set(&root_ei->i_li_last_refill_projected_ops, 0);
		atomic64_set(&root_ei->i_li_last_refill_tstar_ops, 0);
		atomic_set(&root_ei->i_li_last_refill_class, 0);
		ext5_li_adapt_after_build(ctx.sb, ctx.nr_slots, ctx.mutable_bytes,
			(struct lidir_disk_descriptor *)blob,
			ktime_get_ns() - build_start_ns);
		ext5_promote_track_new_root(ctx.nr_dirs, ctx.nr_slots);
		ext5_promote_dbg("promoted root ino=%lu dirs=%u entries=%u blob=%zu\n",
				 root_dir->i_ino, ctx.nr_dirs, ctx.nr_slots,
				 blob_size);
	} else if (publication_started) {
		ext5_promote_dbg("promote root ino=%lu failed at publish/sync err=%d\n",
				 root_dir->i_ino, err);
	}
out_dirs:
	/* Validation/build failures arrive here before out_unlock. */
	if (ready_tracked)
		ext5_promote_track_policy_ready(false);
	promote_discard_shadows(&ctx);
	promote_release_dirs(&ctx);
	kvfree(ctx.slots);
	kvfree(ctx.names);
	vfree(blob);
	if (policy_attempt && attempt_start_ns)
		ext5_promote_track_policy_attempt(ktime_get_ns() - attempt_start_ns,
						 validation_abort);
	return err;
}

int ext5_promote_subtree(struct inode *root_dir)
{
	return __ext5_promote_subtree(root_dir, false, 0);
}

/* The work lives in the directory sidecar; eviction cancels it. */

/* Queue on the node that ran the lookup, so the model is allocated where
 * it is used.  Correctness does not depend on this. */
static int ext5_li_numa_work_cpu(int node)
{
	int cpu;

	if (node < 0 || node >= MAX_NUMNODES || !node_online(node))
		return WORK_CPU_UNBOUND;
	cpu = cpumask_any_and(cpumask_of_node(node), cpu_online_mask);
	return cpu < nr_cpu_ids ? cpu : WORK_CPU_UNBOUND;
}

/*
 * Transition allocations can enter memcg reclaim, which can evict ext5
 * inodes, and evicting a directory drains its transition work.  NOFS keeps
 * these allocations out of filesystem reclaim, and each queued transition
 * holds an inode reference until its callback finishes, so eviction never
 * waits on a worker that waits on eviction.
 */
static void __ext5_auto_promote_work_fn(struct work_struct *work);

void ext5_auto_promote_work_fn(struct work_struct *work)
{
	struct ext5_li_inode_state *state = container_of(to_delayed_work(work),
			struct ext5_li_inode_state, promote_work);
	struct inode *owner = state->owner;

	__ext5_auto_promote_work_fn(work);
	ext5_li_work_ref_finish(owner, &state->promote_work,
			&state->promote_work_ref);
}

static void __ext5_auto_promote_work_fn(struct work_struct *work)
{
	struct ext5_li_inode_state *state = container_of(to_delayed_work(work),
			struct ext5_li_inode_state, promote_work);
	struct inode *dir = state->owner;
	struct ext5_inode_info *ei = EXT5_I(dir);
	struct workqueue_struct *wq = EXT5_SB(dir->i_sb)->li_promote_wq;
	u32 idle_ms = READ_ONCE(ext5_li_promote_idle_ms);
	u64 observed_last_mut = READ_ONCE(state->promote_observed_last_mut);
	int numa_node = READ_ONCE(state->promote_numa_node);
	u64 entries;
	u64 last_mut;
	u64 rmd;
	int err;

	if (unlikely(atomic_read(&EXT5_SB(dir->i_sb)->li_shutting_down)))
		return;
	if (idle_ms) {
		unsigned long due = READ_ONCE(ei->i_li_last_access_jiffies) +
			msecs_to_jiffies(idle_ms);

		if (time_before(jiffies, due) &&
		    !atomic_read(&EXT5_SB(dir->i_sb)->li_shutting_down)) {
			queue_delayed_work_on(
				ext5_li_numa_work_cpu(numa_node), wq,
					&state->promote_work,
					max_t(unsigned long, due - jiffies, 1));
			return;
		}
	}

	ext5_promote_stats_run();
	ext5_promote_dbg("auto-run begin ino=%lu\n", dir->i_ino);
	inode_lock(dir);
	/*
	 * A mutation can start with a negative lookup that queues this work before
	 * i_li_last_mut moves.  Recheck under the directory lock; re-arm on failure.
	 */
	entries = ext5_li_region_entries(dir);
	last_mut = READ_ONCE(ei->i_li_last_mut);
	rmd = ext5_li_op_distance(ext5_li_op_now(dir->i_sb), last_mut);
	if (last_mut != observed_last_mut ||
	    rmd < ext5_li_t_star(dir->i_sb, entries)) {
		ext5_promote_track_prebuild_suppression();
		WRITE_ONCE(ei->i_li_armed, 1);
		inode_unlock(dir);
		ext5_promote_dbg("auto-run ino=%lu deferred last=%llu observed=%llu rmd=%llu\n",
				 dir->i_ino, last_mut, observed_last_mut,
				 rmd);
		goto out;
	}
	inode_unlock(dir);
	err = __ext5_promote_subtree(dir, true, observed_last_mut);

	if (err == -EALREADY || err == -EBUSY) {
		WRITE_ONCE(ei->i_li_armed, 0);
		ext5_promote_stats_run_already();
		ext5_promote_dbg("auto-run ino=%lu already (err=%d)\n",
				 dir->i_ino, err);
	} else if (err) {
		WRITE_ONCE(ei->i_li_armed, 1);
		ext5_promote_stats_run_failed();
		ext5_promote_dbg("auto-run ino=%lu FAILED err=%d\n",
				 dir->i_ino, err);
		/* -EAGAIN means the directory changed or cooled below T*: expected, so
		 * only other errors are logged. */
		if (err != -EAGAIN)
			printk_ratelimited(KERN_WARNING
				"ext5: auto-promote (subtree) ino %lu failed: %d\n",
				dir->i_ino, err);
	} else {
		ext5_promote_dbg("auto-run ino=%lu ok\n", dir->i_ino);
	}

out:
	return;
}

/* Caller holds an inode reference across this call. */
static void ext5_li_schedule_promote(struct inode *root)
{
	struct ext5_sb_info *sbi = EXT5_SB(root->i_sb);
	struct workqueue_struct *wq = sbi->li_promote_wq;
	struct ext5_li_inode_state *state = EXT5_I(root)->i_li_state;
	bool queued;

	if (!wq || !state || atomic_read(&sbi->li_shutting_down)) {
		ext5_promote_stats_alloc_fail();
		WRITE_ONCE(EXT5_I(root)->i_li_armed, 1);
		return;
	}
	WRITE_ONCE(state->promote_observed_last_mut,
		   READ_ONCE(EXT5_I(root)->i_li_last_mut));
	WRITE_ONCE(state->promote_numa_node, numa_node_id());
	queued = ext5_li_work_queue(root, &state->promote_work,
			&state->promote_work_ref, wq,
			msecs_to_jiffies(READ_ONCE(ext5_li_promote_idle_ms)),
			ext5_li_numa_work_cpu(READ_ONCE(state->promote_numa_node)));
	if (!state->promote_work_ref) {
		ext5_promote_stats_alloc_fail();
		WRITE_ONCE(EXT5_I(root)->i_li_armed, 1);
		return;
	}
	if (queued) {
		ext5_promote_stats_scheduled();
	} else {
		ext5_promote_stats_already_queued();
		WRITE_ONCE(EXT5_I(root)->i_li_armed, 1);
	}
}

/*
 * Lookup-path promotion check for a mutable directory on a dcache miss: a
 * candidate with enough entries promotes once RMD = op_now - last_mut exceeds
 * T*(R).  The worker rechecks the mutation position under the lock.
 */
void ext5_li_maybe_promote_on_lookup(struct dentry *lookup_dentry)
{
	u32 mode = READ_ONCE(ext5_li_policy_mode);
	u32 floor;
	u64 now = 0;
	bool have_now = false;
	struct dentry *p;
	struct inode *target = NULL;

	if (!lookup_dentry || !mode)
		return;
	floor = ext5_li_effective_size_floor(lookup_dentry->d_sb);
	if (!floor)
		return;

	/* Walk toward the mount root and keep the highest qualifying ancestor. */
	rcu_read_lock();
	for (p = READ_ONCE(lookup_dentry->d_parent);
	     p && p != READ_ONCE(p->d_parent);
	     p = READ_ONCE(p->d_parent)) {
		struct inode *inode = d_inode_rcu(p);
		struct ext5_inode_info *ei;
		u64 entries, last_mut;

		if (!inode || !S_ISDIR(inode->i_mode))
			break;
		ei = EXT5_I(inode);
		if (ei->i_flags & (EXT5_LIDIR_ROOT_FL |
				   EXT5_LIDIR_INTERIOR_FL)) {
			/* Covered here, but a mutable ancestor may still absorb it. */
			if (mode == 2)
				break;
			continue;
		}
		ext5_li_note_stable_access(inode);
		if (READ_ONCE(ei->i_li_armed) == 2) {
			/* A pending ancestor supersedes every lower candidate. */
			target = NULL;
			break;
		}
		entries = ext5_li_region_entries(inode);
		if ((READ_ONCE(ei->i_li_armed) || entries >= floor) &&
		    entries >= floor) {
			if (!have_now) {
				now = ext5_li_op_now(lookup_dentry->d_sb);
				have_now = true;
			}
			last_mut = READ_ONCE(ei->i_li_last_mut);
			if (ext5_li_op_distance(now, last_mut) >=
			    ext5_li_t_star(lookup_dentry->d_sb, entries))
				target = inode;
		}
		if (mode == 2)
			break;
	}
	if (target && !igrab(target))
		target = NULL;
	rcu_read_unlock();
	if (!target)
		return;
	WRITE_ONCE(EXT5_I(target)->i_li_last_access_jiffies, jiffies);
	WRITE_ONCE(EXT5_I(target)->i_li_armed, 2);
	ext5_li_schedule_promote(target);
	iput(target);
}

/*
 * Transition worker.  After a cross-model rename commits, the moved branch
 * keeps its source route; this worker builds conventional directories for it
 * off-lock, validates the source generation and switches the route.  Source
 * compaction waits on i_li_demote_pending until then.  Policy demotion uses
 * the same worker.
 */
static void __ext5_demote_work_fn(struct work_struct *work);

void ext5_demote_work_fn(struct work_struct *work)
{
	struct ext5_li_inode_state *state = container_of(to_delayed_work(work),
			struct ext5_li_inode_state, demote_work);
	struct inode *owner = state->owner;

	__ext5_demote_work_fn(work);
	ext5_li_work_ref_finish(owner, &state->demote_work,
			&state->demote_work_ref);
}

static void __ext5_demote_work_fn(struct work_struct *work)
{
	struct ext5_li_inode_state *state = container_of(to_delayed_work(work),
			struct ext5_li_inode_state, demote_work);
	struct inode *root = state->owner;
	struct ext5_inode_info *ei = EXT5_I(root);
	struct ext5_sb_info *sbi = EXT5_SB(root->i_sb);
	u32 idle_ms = READ_ONCE(ext5_li_demote_idle_ms);
	u32 plid = (u32)atomic_read(&EXT5_I(root)->i_li_demote_plid);
	ino_t target_ino = (ino_t)atomic64_read(
		&EXT5_I(root)->i_li_demote_target_ino);
	ino_t parent_ino = (ino_t)atomic64_read(
		&EXT5_I(root)->i_li_demote_parent_ino);
	u64 parent_version = atomic64_read(
		&EXT5_I(root)->i_li_demote_parent_version);
	struct inode *rehome_target = READ_ONCE(state->demote_target_ref);
	bool rehome = rehome_target && EXT5_I(rehome_target)->i_li_state &&
		atomic_read(&EXT5_I(rehome_target)->i_li_rehome_pending);
	int err;

	if (unlikely(atomic_read(&sbi->li_shutting_down)))
		goto out_release;
	if (!atomic_read(&ei->i_li_demote_urgent) && idle_ms) {
		unsigned long due = READ_ONCE(ei->i_li_last_access_jiffies) +
			msecs_to_jiffies(idle_ms);

		if (time_before(jiffies, due) &&
		    !atomic_read(&sbi->li_shutting_down)) {
			queue_delayed_work(sbi->li_promote_wq, &state->demote_work,
					   max_t(unsigned long, due - jiffies, 1));
			return;
		}
	}

	/* A concurrent demotion or unmount may have cleared the flag. */
	if (EXT5_I(root)->i_flags & EXT5_LIDIR_ROOT_FL) {
		if (plid && target_ino && parent_ino)
			err = ext5_demote_subtree_plid_hint(root, plid,
						 target_ino, parent_ino,
						 parent_version);
		else
			err = plid ? ext5_demote_subtree_plid(root, plid) :
				ext5_demote_subtree(root);
		/* A rename may upgrade a queued branch demotion to full-root meanwhile. */
		if (!err && plid &&
		    atomic_read(&EXT5_I(root)->i_li_demote_plid) == 0 &&
		    (EXT5_I(root)->i_flags & EXT5_LIDIR_ROOT_FL))
			err = ext5_demote_subtree(root);
		/* Validation lost to a concurrent compaction or mutation: keep the
		 * request and retry after an idle interval. */
		if (err == -EAGAIN) {
			unsigned long retry_ms;

			atomic_set(&ei->i_li_demote_urgent, 0);
			/* Stale rename hints: retry through the full-source walk. */
			if (!rehome) {
				atomic64_set(&ei->i_li_demote_target_ino, 0);
				atomic64_set(&ei->i_li_demote_parent_ino, 0);
				atomic64_set(&ei->i_li_demote_parent_version, 0);
			}
			WRITE_ONCE(ei->i_li_last_access_jiffies, jiffies);
			retry_ms = max_t(unsigned long, idle_ms, 10);
			if (!atomic_read(&sbi->li_shutting_down)) {
				mod_delayed_work(sbi->li_promote_wq,
						 &state->demote_work,
						 max_t(unsigned long,
						       msecs_to_jiffies(retry_ms), 1));
				return;
			}
			goto out_release;
		}
		if (err && rehome && !atomic_read(&sbi->li_shutting_down)) {
			/* The committed route stays valid: keep ownership, retry with backoff. */
			printk_ratelimited(KERN_INFO
				"ext5: branch rehome of ino %lu delayed: %d\n",
				target_ino, err);
			atomic_set(&ei->i_li_demote_urgent, 0);
			mod_delayed_work(sbi->li_promote_wq, &state->demote_work,
					 msecs_to_jiffies(1000));
			return;
		}
		if (err)
			printk_ratelimited(KERN_INFO
				"ext5: transition demote of ino %lu plid %u failed: %d\n",
				root->i_ino, plid, err);
	}
out_release:
	rehome_target = xchg(&state->demote_target_ref, NULL);
	if (rehome_target && EXT5_I(rehome_target)->i_li_state)
		atomic_set(&EXT5_I(rehome_target)->i_li_rehome_pending, 0);
	atomic_set(&EXT5_I(root)->i_li_demote_urgent, 0);
	atomic64_set(&EXT5_I(root)->i_li_demote_target_ino, 0);
	atomic64_set(&EXT5_I(root)->i_li_demote_parent_ino, 0);
	atomic64_set(&EXT5_I(root)->i_li_demote_parent_version, 0);
	/* Publish IDLE last, after the request fields are cleared. */
	atomic_set(&EXT5_I(root)->i_li_demote_pending, 0);
	if (rehome_target)
		iput(rehome_target);
}

/* Schedule a demotion: PLID 0 is the whole root, nonzero a branch. */
static int __ext5_schedule_demote_plid(struct inode *root, u32 plid,
				       bool wait_for_idle, ino_t target_ino,
				       ino_t parent_ino, u64 parent_version)
{
	struct ext5_sb_info *sbi = EXT5_SB(root->i_sb);
	struct ext5_li_inode_state *state = EXT5_I(root)->i_li_state;

	if (!sbi->li_promote_wq || !state ||
	    atomic_read(&sbi->li_shutting_down))
		return -ENXIO;
	if (!(EXT5_I(root)->i_flags & EXT5_LIDIR_ROOT_FL))
		return 0;
	{
		int pending = atomic_cmpxchg(&EXT5_I(root)->i_li_demote_pending,
					     0, 1);

		/* States 2 and 3 own a cross-model handoff; merge nothing into them. */
		if (pending >= 2)
			return -EAGAIN;
		if (!pending)
			goto claimed;
		if (!plid) {
			atomic_set(&EXT5_I(root)->i_li_demote_plid, 0);
			atomic64_set(&EXT5_I(root)->i_li_demote_target_ino, 0);
			atomic64_set(&EXT5_I(root)->i_li_demote_parent_ino, 0);
			atomic64_set(&EXT5_I(root)->i_li_demote_parent_version, 0);
		}
		if (!wait_for_idle) {
			atomic_set(&EXT5_I(root)->i_li_demote_urgent, 1);
			/* Shorten a queued delay, but never rearm a running worker. */
			if (delayed_work_pending(&state->demote_work))
				mod_delayed_work(sbi->li_promote_wq,
						 &state->demote_work, 0);
		}
		return 0;
	}
claimed:
	{
	atomic_set(&EXT5_I(root)->i_li_demote_plid, plid);
	atomic_set(&EXT5_I(root)->i_li_demote_urgent, !wait_for_idle);
	atomic64_set(&EXT5_I(root)->i_li_demote_target_ino, target_ino);
	atomic64_set(&EXT5_I(root)->i_li_demote_parent_ino, parent_ino);
	atomic64_set(&EXT5_I(root)->i_li_demote_parent_version, parent_version);
	if (wait_for_idle)
		WRITE_ONCE(EXT5_I(root)->i_li_last_access_jiffies, jiffies);
	if (!ext5_li_work_queue(root, &state->demote_work,
			&state->demote_work_ref, sbi->li_promote_wq, 0,
			WORK_CPU_UNBOUND)) {
		atomic_set(&EXT5_I(root)->i_li_demote_urgent, 0);
		atomic64_set(&EXT5_I(root)->i_li_demote_target_ino, 0);
		atomic64_set(&EXT5_I(root)->i_li_demote_parent_ino, 0);
		atomic64_set(&EXT5_I(root)->i_li_demote_parent_version, 0);
		atomic_set(&EXT5_I(root)->i_li_demote_pending, 0);
		return -ESTALE;
	}
	}
	return 0;
}

int ext5_schedule_demote_plid(struct inode *root, u32 plid)
{
	return __ext5_schedule_demote_plid(root, plid, false, 0, 0, 0);
}

int ext5_schedule_demote_plid_idle(struct inode *root, u32 plid)
{
	return __ext5_schedule_demote_plid(root, plid, true, 0, 0, 0);
}

int ext5_schedule_demote_plid_rename(struct inode *root, u32 plid,
				     ino_t target_ino, ino_t parent_ino,
				     u64 parent_version)
{
	return __ext5_schedule_demote_plid(root, plid, false,
					 target_ino, parent_ino, parent_version);
}

/* Reserve state 2 under the VFS parent locks: it blocks compaction and
 * coalescing, and queues nothing until the rename commits. */
int ext5_reserve_demote_plid_rename(struct inode *root, u32 plid,
				    struct inode *target, ino_t parent_ino)
{
	struct ext5_sb_info *sbi = EXT5_SB(root->i_sb);
	struct ext5_li_inode_state *state = EXT5_I(root)->i_li_state;
	struct inode *target_ref;

	if (!plid || !target || !parent_ino || !sbi->li_promote_wq || !state ||
	    atomic_read(&sbi->li_shutting_down))
		return -ENXIO;
	if (!(EXT5_I(root)->i_flags & EXT5_LIDIR_ROOT_FL))
		return -ESTALE;
	if (atomic_cmpxchg(&EXT5_I(root)->i_li_demote_pending, 0, 2))
		return -EAGAIN;
	target_ref = igrab(target);
	if (!target_ref) {
		atomic_set(&EXT5_I(root)->i_li_demote_pending, 0);
		return -ESTALE;
	}
	if (WARN_ON_ONCE(READ_ONCE(state->demote_target_ref))) {
		iput(target_ref);
		atomic_set(&EXT5_I(root)->i_li_demote_pending, 0);
		return -EIO;
	}
	WRITE_ONCE(state->demote_target_ref, target_ref);
	atomic_set(&EXT5_I(root)->i_li_demote_plid, plid);
	atomic_set(&EXT5_I(root)->i_li_demote_urgent, 1);
	atomic64_set(&EXT5_I(root)->i_li_demote_target_ino, target->i_ino);
	atomic64_set(&EXT5_I(root)->i_li_demote_parent_ino, parent_ino);
	atomic64_set(&EXT5_I(root)->i_li_demote_parent_version, 0);
	return 0;
}

void ext5_start_reserved_demote_plid_rename(struct inode *root,
					    u64 parent_version)
{
	struct ext5_sb_info *sbi = EXT5_SB(root->i_sb);
	struct ext5_li_inode_state *state = EXT5_I(root)->i_li_state;

	atomic64_set(&EXT5_I(root)->i_li_demote_parent_version, parent_version);
	if (WARN_ON_ONCE(atomic_cmpxchg(&EXT5_I(root)->i_li_demote_pending,
					  2, 3) != 2))
		return;
	if (!ext5_li_work_mod(root, &state->demote_work,
			&state->demote_work_ref, sbi->li_promote_wq, 0))
		goto start_failed;
	return;

start_failed:
	{
		struct inode *target = xchg(&state->demote_target_ref, NULL);

		if (target && EXT5_I(target)->i_li_state)
			atomic_set(&EXT5_I(target)->i_li_rehome_pending, 0);
		if (target)
			iput(target);
	}
	atomic_set(&EXT5_I(root)->i_li_demote_urgent, 0);
	atomic64_set(&EXT5_I(root)->i_li_demote_target_ino, 0);
	atomic64_set(&EXT5_I(root)->i_li_demote_parent_ino, 0);
	atomic64_set(&EXT5_I(root)->i_li_demote_parent_version, 0);
	atomic_set(&EXT5_I(root)->i_li_demote_pending, 0);
}

void ext5_cancel_reserved_demote_plid_rename(struct inode *root)
{
	struct ext5_inode_info *ei = EXT5_I(root);
	struct inode *target;

	if (WARN_ON_ONCE(atomic_read(&ei->i_li_demote_pending) != 2))
		return;
	atomic_set(&ei->i_li_demote_urgent, 0);
	atomic64_set(&ei->i_li_demote_target_ino, 0);
	atomic64_set(&ei->i_li_demote_parent_ino, 0);
	atomic64_set(&ei->i_li_demote_parent_version, 0);
	target = xchg(&ei->i_li_state->demote_target_ref, NULL);
	if (target && EXT5_I(target)->i_li_state)
		atomic_set(&EXT5_I(target)->i_li_rehome_pending, 0);
	atomic_set(&ei->i_li_demote_pending, 0);
	if (target)
		iput(target);
}

int ext5_schedule_demote(struct inode *root)
{
	return ext5_schedule_demote_plid(root, 0);
}

/*
 * Add nfiles/ndirs to every ancestor's subtree count and stamp its mutation
 * clock, under RCU.  Stable nodes are passed through: mutable ancestors above
 * them still need the evidence.  Costs O(depth) atomics per create/unlink.
 */
static void ext5_subtree_count_walk(struct dentry *child,
				    int nfiles, int ndirs)
{
	u32 thresh_files;
	u32 thresh_dirs;
	u64 op_now;
	struct dentry *p;
	bool incrementing = (nfiles > 0 || ndirs > 0);
	bool recorded_distance = false;

	if (!child)
		return;
	thresh_files = ext5_li_effective_size_floor(child->d_sb);
	thresh_dirs = 0;
	if (!READ_ONCE(ext5_li_policy_mode) || !thresh_files)
		return;
	/* One stamp for every ancestor: a subtree with an active branch keeps
	 * resetting its distance and never promotes. */
	op_now = ext5_li_op_now(child->d_sb);

	rcu_read_lock();

	for (p = READ_ONCE(child->d_parent); p && p != READ_ONCE(p->d_parent);
	     p = READ_ONCE(p->d_parent)) {
		struct inode *pino = d_inode_rcu(p);
		struct ext5_inode_info *pei;
		int new_files = 0, new_dirs = 0;

		if (!pino || !S_ISDIR(pino->i_mode))
			break;
		pei = EXT5_I(pino);
		if (pei->i_flags & (EXT5_LIDIR_ROOT_FL |
				    EXT5_LIDIR_INTERIOR_FL)) {
			struct inode *stable_root = NULL;
			struct ext5_inode_info *root_ei;
			u64 previous;

			if (pei->i_flags & EXT5_LIDIR_ROOT_FL)
				stable_root = pino;
			else
				stable_root = rcu_dereference(
					pei->i_subtree_root_inode);
			if (!stable_root)
				continue;
			root_ei = EXT5_I(stable_root);
			previous = READ_ONCE(root_ei->i_li_last_mut);
			if (previous && op_now > previous) {
				u32 entries = ext5_stable_entry_count(stable_root);
				u64 gap = op_now - previous;

				/* A mutation after a full quiet interval starts a new burst and
				 * forgets earlier refills. */
				if (entries && gap >=
				    ext5_li_t_star(child->d_sb, entries))
					atomic_set(&root_ei->i_li_compact_count, 0);
			}
			if (!recorded_distance && previous && op_now > previous) {
				ext5_li_record_rmd(child->d_sb,
					  op_now - previous);
				recorded_distance = true;
			}
			WRITE_ONCE(root_ei->i_li_last_mut, op_now);
			ext5_li_note_stable_access(stable_root);
			continue;
		}
		ext5_li_note_stable_access(pino);

		ext5_promote_stats_subtree_step();
		if (nfiles)
			new_files = atomic_add_return(nfiles,
						      &pei->i_subtree_files);
		if (ndirs)
			new_dirs = atomic_add_return(ndirs,
						     &pei->i_subtree_dirs);

		if (!recorded_distance) {
			u64 previous = READ_ONCE(pei->i_li_last_mut);

			if (previous && op_now > previous)
				ext5_li_record_rmd(child->d_sb,
					  op_now - previous);
			recorded_distance = true;
		}
		WRITE_ONCE(pei->i_li_last_mut, op_now);

		if (incrementing) {
			bool just_crossed = false;

			if (thresh_files && nfiles &&
			    new_files >= (int)thresh_files &&
			    new_files - nfiles < (int)thresh_files)
				just_crossed = true;
			if (thresh_dirs && ndirs &&
			    new_dirs >= (int)thresh_dirs &&
			    new_dirs - ndirs < (int)thresh_dirs)
				just_crossed = true;

			if (just_crossed) {
				ext5_promote_stats_thresh_crossed();
				/* Arm every qualifying region; the lookup check picks the highest
				 * quiet one, so a quiet sibling can promote beside an active branch. */
				WRITE_ONCE(pei->i_li_armed, 1);
				ext5_promote_stats_scheduled();
				ext5_promote_dbg("armed ino=%lu entries=%d\n",
						 pino->i_ino, new_files);
			}
		}
	}
	rcu_read_unlock();
}

void ext5_subtree_count_inc(struct dentry *child, bool is_dir)
{
	if (!child)
		return;
	ext5_li_op_tick(child->d_sb);
	ext5_promote_stats_subtree_inc();
	ext5_subtree_count_walk(child, 1, is_dir ? 1 : 0);
}

void ext5_subtree_count_dec(struct dentry *child, bool is_dir)
{
	if (!child)
		return;
	ext5_li_op_tick(child->d_sb);
	ext5_promote_stats_subtree_dec();
	ext5_subtree_count_walk(child, -1, is_dir ? -1 : 0);
}

static void ext5_li_inode_region_size(struct inode *inode,
				      int *files, int *dirs)
{
	u64 observed_files = 0, observed_dirs = 0;

	if (S_ISDIR(inode->i_mode)) {
		observed_files = max_t(int,
			atomic_read(&EXT5_I(inode)->i_subtree_files), 0);
		observed_dirs = max_t(int,
			atomic_read(&EXT5_I(inode)->i_subtree_dirs), 0);
	}
	*files = min_t(u64, observed_files + 1, INT_MAX);
	*dirs = S_ISDIR(inode->i_mode) ?
		min_t(u64, observed_dirs + 1, INT_MAX) : 0;
}

/* After a rename, move the size evidence and stamp both ancestor chains. */
void ext5_subtree_rename_update(struct dentry *old_dentry,
				struct dentry *new_dentry,
				struct inode *moved,
				struct inode *replaced,
				bool exchange)
{
	int moved_files, moved_dirs;
	int replaced_files = 0, replaced_dirs = 0;

	if (!old_dentry || !new_dentry || !moved)
		return;
	ext5_li_op_tick(moved->i_sb);
	ext5_li_inode_region_size(moved, &moved_files, &moved_dirs);
	if (replaced)
		ext5_li_inode_region_size(replaced, &replaced_files,
					  &replaced_dirs);

	if (old_dentry->d_parent == new_dentry->d_parent) {
		ext5_subtree_count_walk(old_dentry,
			exchange ? 0 : -replaced_files,
			exchange ? 0 : -replaced_dirs);
		return;
	}
	if (exchange) {
		ext5_subtree_count_walk(old_dentry,
			replaced_files - moved_files,
			replaced_dirs - moved_dirs);
		ext5_subtree_count_walk(new_dentry,
			moved_files - replaced_files,
			moved_dirs - replaced_dirs);
	} else {
		ext5_subtree_count_walk(old_dentry, -moved_files, -moved_dirs);
		ext5_subtree_count_walk(new_dentry,
			moved_files - replaced_files,
			moved_dirs - replaced_dirs);
	}
}
