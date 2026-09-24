// SPDX-License-Identifier: GPL-2.0
/*
 * SplineFS attribution counters at /sys/kernel/debug/ext5/li_stats, gated by
 * li_stats_enabled.  Writing to the file resets them.
 */

#include "ext5.h"
#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/jump_label.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/seq_file.h>

DEFINE_STATIC_KEY_FALSE(ext5_li_stats_key);
static bool ext5_li_stats_enabled;

/* Log each promote/demote decision (li_promote_log). */
bool ext5_li_promote_log;
EXPORT_SYMBOL_GPL(ext5_li_promote_log);

module_param_named(li_promote_log, ext5_li_promote_log, bool, 0644);
MODULE_PARM_DESC(li_promote_log,
		 "Log every promote decision (threshold-cross, schedule, run, fail) to dmesg.");

static int ext5_li_stats_param_set(const char *val,
				   const struct kernel_param *kp)
{
	bool prev = ext5_li_stats_enabled;
	int ret = param_set_bool(val, kp);

	if (ret)
		return ret;
	if (ext5_li_stats_enabled && !prev)
		static_branch_enable(&ext5_li_stats_key);
	else if (!ext5_li_stats_enabled && prev)
		static_branch_disable(&ext5_li_stats_key);
	return 0;
}

static const struct kernel_param_ops ext5_li_stats_param_ops = {
	.set = ext5_li_stats_param_set,
	.get = param_get_bool,
};

module_param_cb(li_stats_enabled, &ext5_li_stats_param_ops,
		&ext5_li_stats_enabled, 0644);
MODULE_PARM_DESC(li_stats_enabled,
		 "Enable SplineFS LI-dir attribution counters (debugfs).");

struct ext5_li_stats {
	atomic64_t stable_lookups;
	atomic64_t stable_filter_negatives;
	atomic64_t stable_hits;
	atomic64_t stable_misses;
	atomic64_t stable_readdir_calls;
	atomic64_t stable_blob_parses;
	atomic64_t promote_calls;
	atomic64_t rs_predictions;
	atomic64_t rs_bound_violations;
	atomic64_t rs_error_max;
	atomic64_t rs_error_hist[7];
	atomic64_t subtree_count_inc_calls;
	atomic64_t policy_op_ticks;
	atomic64_t subtree_count_dec_calls;
	atomic64_t subtree_count_walk_steps;	/* atomic_add_return calls */
	atomic64_t auto_promote_thresh_crossed;	/* total threshold-crossings seen */
	atomic64_t auto_promote_skip_low_fanout;
	atomic64_t auto_promote_scheduled;	/* schedule_work() succeeded */
	atomic64_t auto_promote_alloc_fail;
	atomic64_t auto_promote_already_queued;
	atomic64_t auto_promote_run;		/* work fn invocations */
	atomic64_t auto_promote_run_already;	/* hit -EALREADY/-EBUSY */
	atomic64_t auto_promote_run_failed;	/* other error */
	atomic64_t manual_operations;
	/* Lifecycle counters stay live when lookup stats are disabled. */
	atomic64_t promotion_publications;
	atomic64_t compaction_publications;
	atomic64_t full_demotion_publications;
	atomic64_t full_demotion_attempts;
	atomic64_t branch_demotion_publications;
	atomic64_t branch_demotion_ns_total;
	atomic64_t branch_demotion_ns_max;
	atomic64_t branch_demotion_ns_last;
	atomic64_t compaction_ns_total;
	atomic64_t compaction_ns_max;
	atomic64_t compaction_ns_last;
	atomic64_t full_demotion_ns_total;
	atomic64_t full_demotion_ns_max;
	atomic64_t full_demotion_ns_last;
	atomic64_t rapid_insert_refills;
	atomic64_t slow_insert_refills;
	atomic64_t noninsert_refills;
	atomic64_t compact_wake_requests;
	atomic64_t compact_worker_runs;
	atomic64_t compact_idle_deferrals;
	atomic64_t compact_build_attempts;
	atomic64_t compact_stale_runs;
	atomic64_t compact_rescans;
	atomic64_t policy_build_attempts;
	atomic64_t policy_build_started;
	atomic64_t policy_build_inflight;
	atomic64_t policy_ready_inflight;
	atomic64_t policy_validation_aborts;
	atomic64_t policy_prebuild_suppressions;
	atomic64_t policy_build_ns_total;
	atomic64_t policy_build_ns_max;
	/* Current promoted state, always tracked. */
	atomic64_t live_roots;             /* directories with LIDIR_ROOT_FL set */
	atomic64_t live_interior_dirs;     /* total INTERIOR dirs across all roots */
	atomic64_t live_promoted_entries;  /* total entry_count across all roots */
};

static struct ext5_li_stats stats;
static struct dentry *ext5_li_debugfs_dir;

#define BUMP(name)							\
	do {								\
		if (static_branch_unlikely(&ext5_li_stats_key))		\
			atomic64_inc(&stats.name);			\
	} while (0)

void __ext5_stable_stats_lookup(void)           { atomic64_inc(&stats.stable_lookups); }
void __ext5_stable_stats_filter_negative(void)  { atomic64_inc(&stats.stable_filter_negatives); }
void __ext5_stable_stats_hit(void)              { atomic64_inc(&stats.stable_hits); }
void __ext5_stable_stats_miss(void)             { atomic64_inc(&stats.stable_misses); }
void ext5_stable_stats_readdir(void)           { BUMP(stable_readdir_calls); }
void ext5_stable_stats_blob_parse(void)        { BUMP(stable_blob_parses); }
void ext5_promote_stats_call(void)             { BUMP(promote_calls); }
void __ext5_stable_stats_rs_prediction(void)    { atomic64_inc(&stats.rs_predictions); }
/* Correctness guards stay live when attribution is disabled. */
void ext5_stable_stats_rs_bound_violation(void)
{
	atomic64_inc(&stats.rs_bound_violations);
}
void __ext5_stable_stats_rs_error(u32 error)
{
	u32 bucket;
	s64 old;

	if (!error)
		bucket = 0;
	else if (error == 1)
		bucket = 1;
	else if (error <= 3)
		bucket = 2;
	else if (error <= 7)
		bucket = 3;
	else if (error <= 15)
		bucket = 4;
	else if (error <= 31)
		bucket = 5;
	else
		bucket = 6;
	atomic64_inc(&stats.rs_error_hist[bucket]);
	old = atomic64_read(&stats.rs_error_max);
	while (old < error) {
		s64 previous = atomic64_cmpxchg(&stats.rs_error_max, old, error);

		if (previous == old)
			break;
		old = previous;
	}
}
void ext5_promote_stats_subtree_inc(void)      { BUMP(subtree_count_inc_calls); }
void __ext5_promote_stats_op_tick(void)        { atomic64_inc(&stats.policy_op_ticks); }
void ext5_promote_stats_subtree_dec(void)      { BUMP(subtree_count_dec_calls); }
void ext5_promote_stats_subtree_step(void)     { BUMP(subtree_count_walk_steps); }
void ext5_promote_stats_thresh_crossed(void)   { BUMP(auto_promote_thresh_crossed); }
void ext5_promote_stats_low_fanout(void)       { BUMP(auto_promote_skip_low_fanout); }
void ext5_promote_stats_scheduled(void)        { BUMP(auto_promote_scheduled); }
void ext5_promote_stats_alloc_fail(void)       { BUMP(auto_promote_alloc_fail); }
void ext5_promote_stats_already_queued(void)   { BUMP(auto_promote_already_queued); }
void ext5_promote_stats_run(void)              { BUMP(auto_promote_run); }
void ext5_promote_stats_run_already(void)      { BUMP(auto_promote_run_already); }
void ext5_promote_stats_run_failed(void)       { BUMP(auto_promote_run_failed); }
void ext5_li_stats_manual_operation(void)
{
	atomic64_inc(&stats.manual_operations);
}

static void ext5_li_atomic64_sub_floor_zero(atomic64_t *counter, u64 amount)
{
	s64 old, next;

	do {
		old = atomic64_read(counter);
		next = old > (s64)amount ? old - amount : 0;
	} while (atomic64_cmpxchg(counter, old, next) != old);
}

void ext5_promote_track_new_root(u32 parent_count, u32 entry_count)
{
	atomic64_inc(&stats.promotion_publications);
	atomic64_inc(&stats.live_roots);
	/* parent_count includes the root itself; INTERIOR dirs = parent_count - 1 */
	if (parent_count > 0)
		atomic64_add(parent_count - 1, &stats.live_interior_dirs);
	atomic64_add(entry_count, &stats.live_promoted_entries);
}
EXPORT_SYMBOL_GPL(ext5_promote_track_new_root);

void ext5_promote_track_demote_root(u32 parent_count, u32 entry_count)
{
	atomic64_inc(&stats.full_demotion_publications);
	ext5_li_atomic64_sub_floor_zero(&stats.live_roots, 1);
	if (parent_count > 0)
		ext5_li_atomic64_sub_floor_zero(&stats.live_interior_dirs,
						parent_count - 1);
	ext5_li_atomic64_sub_floor_zero(&stats.live_promoted_entries,
					entry_count);
}
EXPORT_SYMBOL_GPL(ext5_promote_track_demote_root);

void ext5_promote_track_branch_demote(u32 dir_count, u32 entry_count)
{
	atomic64_inc(&stats.branch_demotion_publications);
	ext5_li_atomic64_sub_floor_zero(&stats.live_interior_dirs, dir_count);
	ext5_li_atomic64_sub_floor_zero(&stats.live_promoted_entries, entry_count);
}
EXPORT_SYMBOL_GPL(ext5_promote_track_branch_demote);

void ext5_promote_track_branch_demote_time(u64 elapsed_ns)
{
	s64 old;

	atomic64_add(elapsed_ns, &stats.branch_demotion_ns_total);
	atomic64_set(&stats.branch_demotion_ns_last, elapsed_ns);
	old = atomic64_read(&stats.branch_demotion_ns_max);
	while (old < elapsed_ns) {
		s64 previous = atomic64_cmpxchg(&stats.branch_demotion_ns_max,
						old, elapsed_ns);

		if (previous == old)
			break;
		old = previous;
	}
}
EXPORT_SYMBOL_GPL(ext5_promote_track_branch_demote_time);

static void ext5_li_track_duration(atomic64_t *total, atomic64_t *maximum,
				   atomic64_t *last, u64 elapsed_ns)
{
	s64 old;

	atomic64_add(elapsed_ns, total);
	atomic64_set(last, elapsed_ns);
	old = atomic64_read(maximum);
	while (old < elapsed_ns) {
		s64 previous = atomic64_cmpxchg(maximum, old, elapsed_ns);

		if (previous == old)
			break;
		old = previous;
	}
}

void ext5_promote_track_compaction_time(u64 elapsed_ns)
{
	ext5_li_track_duration(&stats.compaction_ns_total,
			       &stats.compaction_ns_max,
			       &stats.compaction_ns_last, elapsed_ns);
}
EXPORT_SYMBOL_GPL(ext5_promote_track_compaction_time);

void ext5_promote_track_full_demote_time(u64 elapsed_ns)
{
	ext5_li_track_duration(&stats.full_demotion_ns_total,
			       &stats.full_demotion_ns_max,
			       &stats.full_demotion_ns_last, elapsed_ns);
}
EXPORT_SYMBOL_GPL(ext5_promote_track_full_demote_time);

void ext5_promote_track_full_demote_attempt(void)
{
	atomic64_inc(&stats.full_demotion_attempts);
}
EXPORT_SYMBOL_GPL(ext5_promote_track_full_demote_attempt);

void ext5_promote_track_compact(s64 entry_delta)
{
	if (entry_delta < 0)
		ext5_li_atomic64_sub_floor_zero(&stats.live_promoted_entries,
						-entry_delta);
	else
		atomic64_add(entry_delta, &stats.live_promoted_entries);
}
EXPORT_SYMBOL_GPL(ext5_promote_track_compact);

void ext5_promote_track_compact_publication(void)
{
	atomic64_inc(&stats.compaction_publications);
}
EXPORT_SYMBOL_GPL(ext5_promote_track_compact_publication);

void ext5_promote_track_rapid_insert_refill(void)
{
	atomic64_inc(&stats.rapid_insert_refills);
}

void ext5_promote_track_slow_insert_refill(void)
{
	atomic64_inc(&stats.slow_insert_refills);
}

void ext5_promote_track_noninsert_refill(void)
{
	atomic64_inc(&stats.noninsert_refills);
}

void ext5_promote_track_compact_wake(void)
{
	BUMP(compact_wake_requests);
}

void ext5_promote_track_compact_worker_run(void)
{
	BUMP(compact_worker_runs);
}

void ext5_promote_track_compact_idle_deferral(void)
{
	BUMP(compact_idle_deferrals);
}

void ext5_promote_track_compact_build_attempt(void)
{
	BUMP(compact_build_attempts);
}

void ext5_promote_track_compact_stale(void)
{
	BUMP(compact_stale_runs);
}

void ext5_promote_track_compact_rescan(void)
{
	BUMP(compact_rescans);
}

void ext5_promote_track_policy_attempt(u64 elapsed_ns, bool validation_abort)
{
	s64 old;

	atomic64_inc(&stats.policy_build_attempts);
	ext5_li_atomic64_sub_floor_zero(&stats.policy_build_inflight, 1);
	atomic64_add(elapsed_ns, &stats.policy_build_ns_total);
	old = atomic64_read(&stats.policy_build_ns_max);
	while (old < elapsed_ns) {
		s64 previous = atomic64_cmpxchg(&stats.policy_build_ns_max,
						old, elapsed_ns);

		if (previous == old)
			break;
		old = previous;
	}
	if (validation_abort)
		atomic64_inc(&stats.policy_validation_aborts);
}

void ext5_promote_track_policy_start(void)
{
	atomic64_inc(&stats.policy_build_started);
	atomic64_inc(&stats.policy_build_inflight);
}

void ext5_promote_track_policy_ready(bool ready)
{
	if (ready)
		atomic64_inc(&stats.policy_ready_inflight);
	else
		ext5_li_atomic64_sub_floor_zero(&stats.policy_ready_inflight, 1);
}

void ext5_promote_track_prebuild_suppression(void)
{
	atomic64_inc(&stats.policy_prebuild_suppressions);
}

static void ext5_li_stats_reset(void)
{
	/* Reset attribution only; live-state counters describe the mount. */
	s64 r = atomic64_read(&stats.live_roots);
	s64 i = atomic64_read(&stats.live_interior_dirs);
	s64 e = atomic64_read(&stats.live_promoted_entries);

	memset(&stats, 0, sizeof(stats));
	atomic64_set(&stats.live_roots, r);
	atomic64_set(&stats.live_interior_dirs, i);
	atomic64_set(&stats.live_promoted_entries, e);
}

static int ext5_li_stats_show(struct seq_file *m, void *v)
{
	seq_printf(m, "enabled %u\n",
		   static_branch_unlikely(&ext5_li_stats_key) ? 1U : 0U);
	seq_printf(m, "stable_lookups %lld\n",
		   atomic64_read(&stats.stable_lookups));
	seq_printf(m, "stable_filter_negatives %lld\n",
		   atomic64_read(&stats.stable_filter_negatives));
	seq_printf(m, "stable_hits %lld\n",
		   atomic64_read(&stats.stable_hits));
	seq_printf(m, "stable_misses %lld\n",
		   atomic64_read(&stats.stable_misses));
	seq_printf(m, "stable_readdir_calls %lld\n",
		   atomic64_read(&stats.stable_readdir_calls));
	seq_printf(m, "stable_blob_parses %lld\n",
		   atomic64_read(&stats.stable_blob_parses));
	seq_printf(m, "promote_calls %lld\n",
		   atomic64_read(&stats.promote_calls));
	seq_printf(m, "rs_predictions %lld\n",
		   atomic64_read(&stats.rs_predictions));
	seq_printf(m, "rs_bound_violations %lld\n",
		   atomic64_read(&stats.rs_bound_violations));
	seq_printf(m, "rs_error_max %lld\n",
		   atomic64_read(&stats.rs_error_max));
	seq_printf(m, "rs_error_0 %lld\n",
		   atomic64_read(&stats.rs_error_hist[0]));
	seq_printf(m, "rs_error_1 %lld\n",
		   atomic64_read(&stats.rs_error_hist[1]));
	seq_printf(m, "rs_error_2_3 %lld\n",
		   atomic64_read(&stats.rs_error_hist[2]));
	seq_printf(m, "rs_error_4_7 %lld\n",
		   atomic64_read(&stats.rs_error_hist[3]));
	seq_printf(m, "rs_error_8_15 %lld\n",
		   atomic64_read(&stats.rs_error_hist[4]));
	seq_printf(m, "rs_error_16_31 %lld\n",
		   atomic64_read(&stats.rs_error_hist[5]));
	seq_printf(m, "rs_error_32_plus %lld\n",
		   atomic64_read(&stats.rs_error_hist[6]));
	seq_printf(m, "subtree_count_inc_calls %lld\n",
		   atomic64_read(&stats.subtree_count_inc_calls));
	seq_printf(m, "policy_op_clock %lld\n",
		   atomic64_read(&stats.policy_op_ticks));
	seq_printf(m, "subtree_count_dec_calls %lld\n",
		   atomic64_read(&stats.subtree_count_dec_calls));
	seq_printf(m, "subtree_count_walk_steps %lld\n",
		   atomic64_read(&stats.subtree_count_walk_steps));
	seq_printf(m, "auto_promote_thresh_crossed %lld\n",
		   atomic64_read(&stats.auto_promote_thresh_crossed));
	seq_printf(m, "auto_promote_skip_low_fanout %lld\n",
		   atomic64_read(&stats.auto_promote_skip_low_fanout));
	seq_printf(m, "auto_promote_scheduled %lld\n",
		   atomic64_read(&stats.auto_promote_scheduled));
	seq_printf(m, "auto_promote_alloc_fail %lld\n",
		   atomic64_read(&stats.auto_promote_alloc_fail));
	seq_printf(m, "auto_promote_already_queued %lld\n",
		   atomic64_read(&stats.auto_promote_already_queued));
	seq_printf(m, "auto_promote_run %lld\n",
		   atomic64_read(&stats.auto_promote_run));
	seq_printf(m, "auto_promote_run_already %lld\n",
		   atomic64_read(&stats.auto_promote_run_already));
	seq_printf(m, "auto_promote_run_failed %lld\n",
		   atomic64_read(&stats.auto_promote_run_failed));
	seq_printf(m, "manual_operations %lld\n",
		   atomic64_read(&stats.manual_operations));
	seq_printf(m, "promotion_publications %lld\n",
		   atomic64_read(&stats.promotion_publications));
	seq_printf(m, "compaction_publications %lld\n",
		   atomic64_read(&stats.compaction_publications));
	seq_printf(m, "full_demotion_publications %lld\n",
		   atomic64_read(&stats.full_demotion_publications));
	seq_printf(m, "full_demotion_attempts %lld\n",
		   atomic64_read(&stats.full_demotion_attempts));
	seq_printf(m, "branch_demotion_publications %lld\n",
		   atomic64_read(&stats.branch_demotion_publications));
	seq_printf(m, "branch_demotion_ns_total %lld\n",
		   atomic64_read(&stats.branch_demotion_ns_total));
	seq_printf(m, "branch_demotion_ns_max %lld\n",
		   atomic64_read(&stats.branch_demotion_ns_max));
	seq_printf(m, "branch_demotion_ns_last %lld\n",
		   atomic64_read(&stats.branch_demotion_ns_last));
	seq_printf(m, "compaction_ns_total %lld\n",
		   atomic64_read(&stats.compaction_ns_total));
	seq_printf(m, "compaction_ns_max %lld\n",
		   atomic64_read(&stats.compaction_ns_max));
	seq_printf(m, "compaction_ns_last %lld\n",
		   atomic64_read(&stats.compaction_ns_last));
	seq_printf(m, "full_demotion_ns_total %lld\n",
		   atomic64_read(&stats.full_demotion_ns_total));
	seq_printf(m, "full_demotion_ns_max %lld\n",
		   atomic64_read(&stats.full_demotion_ns_max));
	seq_printf(m, "full_demotion_ns_last %lld\n",
		   atomic64_read(&stats.full_demotion_ns_last));
	seq_printf(m, "rapid_insert_refills %lld\n",
		   atomic64_read(&stats.rapid_insert_refills));
	seq_printf(m, "slow_insert_refills %lld\n",
		   atomic64_read(&stats.slow_insert_refills));
	seq_printf(m, "noninsert_refills %lld\n",
		   atomic64_read(&stats.noninsert_refills));
	seq_printf(m, "compact_wake_requests %lld\n",
		   atomic64_read(&stats.compact_wake_requests));
	seq_printf(m, "compact_worker_runs %lld\n",
		   atomic64_read(&stats.compact_worker_runs));
	seq_printf(m, "compact_idle_deferrals %lld\n",
		   atomic64_read(&stats.compact_idle_deferrals));
	seq_printf(m, "compact_build_attempts %lld\n",
		   atomic64_read(&stats.compact_build_attempts));
	seq_printf(m, "compact_stale_runs %lld\n",
		   atomic64_read(&stats.compact_stale_runs));
	seq_printf(m, "compact_rescans %lld\n",
		   atomic64_read(&stats.compact_rescans));
	seq_printf(m, "policy_build_attempts %lld\n",
		   atomic64_read(&stats.policy_build_attempts));
	seq_printf(m, "policy_build_started %lld\n",
		   atomic64_read(&stats.policy_build_started));
	seq_printf(m, "policy_build_inflight %lld\n",
		   atomic64_read(&stats.policy_build_inflight));
	seq_printf(m, "policy_ready_inflight %lld\n",
		   atomic64_read(&stats.policy_ready_inflight));
	seq_printf(m, "policy_validation_aborts %lld\n",
		   atomic64_read(&stats.policy_validation_aborts));
	seq_printf(m, "policy_prebuild_suppressions %lld\n",
		   atomic64_read(&stats.policy_prebuild_suppressions));
	seq_printf(m, "policy_build_ns_total %lld\n",
		   atomic64_read(&stats.policy_build_ns_total));
	seq_printf(m, "policy_build_ns_max %lld\n",
		   atomic64_read(&stats.policy_build_ns_max));
	seq_printf(m, "live_roots %lld\n",
		   atomic64_read(&stats.live_roots));
	seq_printf(m, "live_interior_dirs %lld\n",
		   atomic64_read(&stats.live_interior_dirs));
	seq_printf(m, "live_promoted_entries %lld\n",
		   atomic64_read(&stats.live_promoted_entries));
	seq_printf(m, "promote_log %u\n", ext5_li_promote_log ? 1U : 0U);
	seq_printf(m, "policy_quiet_ops_configured %u\n",
		   READ_ONCE(ext5_li_quiet_ops));
	seq_printf(m, "policy_size_floor_configured %u\n",
		   READ_ONCE(ext5_li_size_floor_entries));
	seq_printf(m, "policy_buildcost_permille_configured %u\n",
		   READ_ONCE(ext5_li_buildcost_permille));
	seq_puts(m, "policy_effective_scope per-filesystem-sysfs\n");
	seq_printf(m, "policy_op_cost_ns %u\n",
		   READ_ONCE(ext5_li_op_cost_ns));
	seq_printf(m, "policy_demote_refills %u\n",
		   READ_ONCE(ext5_li_demote_refills));
	seq_printf(m, "policy_promote_idle_ms %u\n",
		   READ_ONCE(ext5_li_promote_idle_ms));
	seq_printf(m, "policy_demote_idle_ms %u\n",
		   READ_ONCE(ext5_li_demote_idle_ms));
	seq_printf(m, "policy_compact_idle_ms %u\n",
		   READ_ONCE(ext5_li_compact_idle_ms));
	return 0;
}

static int ext5_li_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, ext5_li_stats_show, inode->i_private);
}

static ssize_t ext5_li_stats_write(struct file *file, const char __user *buf,
				   size_t count, loff_t *ppos)
{
	ext5_li_stats_reset();
	return count;
}

static const struct file_operations ext5_li_stats_fops = {
	.owner = THIS_MODULE,
	.open = ext5_li_stats_open,
	.read = seq_read,
	.write = ext5_li_stats_write,
	.llseek = seq_lseek,
	.release = single_release,
};

int ext5_li_debugfs_init(void)
{
	ext5_li_stats_reset();
	ext5_li_debugfs_dir = debugfs_create_dir("ext5", NULL);
	debugfs_create_file("li_stats", 0644, ext5_li_debugfs_dir, NULL,
			    &ext5_li_stats_fops);
	return 0;
}

void ext5_li_debugfs_exit(void)
{
	debugfs_remove_recursive(ext5_li_debugfs_dir);
	ext5_li_debugfs_dir = NULL;
}
