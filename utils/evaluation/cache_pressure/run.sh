#!/usr/bin/env bash
# Fig. 9b/9c driver: cold lookups under memcg caps.  DESTRUCTIVE.
# FSES names the arms: splinefs, ext4, and naive (the same module with one
# model per directory, promoted by hand).
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
AE_ROOT=$(cd "$HERE/../../.." && pwd)
MODULE_DIR=${MODULE_DIR:-$AE_ROOT/src}
AE_UTILS=$AE_ROOT/utils
export AE_UTILS
. "$HERE/../lib/safety.sh"

DEV=${DEV:-}
MNT=${MNT:-/mnt/splinefs-eval}
K=${K:-2000}
M=${M:-1000}
POOL=${POOL:-1024}
LOOKUPS=${LOOKUPS:-1000000}
SETTLE_OPS=${SETTLE_OPS:-1000000}
CAPS=${CAPS:-"536870912 1073741824 2147483648"}
REPS=${REPS:-3}
FSES=${FSES:-"splinefs ext4"}
# A naive timed run unfinished after NAIVE_TIMEOUT seconds is recorded as
# not finished: fewer than LOOKUPS / NAIVE_TIMEOUT lookups per second.
NAIVE_TIMEOUT=${NAIVE_TIMEOUT:-1200}
MODULE_ARGS=${MODULE_ARGS:-}
STATS_ENABLED=${STATS_ENABLED:-N}
# Set by the ablation's promotion-off arm: there, nothing may promote.
EVAL_EXPECT_NO_PROMOTION=${EVAL_EXPECT_NO_PROMOTION:-0}
export EVAL_EXPECT_NO_PROMOTION
# Set by the ablation's subtree arm: the shared model must sit at TREE.
EVAL_REQUIRE_TREE_ROOT=${EVAL_REQUIRE_TREE_ROOT:-0}
EVAL_REQUIRE_PARENT_COUNT=${EVAL_REQUIRE_PARENT_COUNT:-0}
# positive (Fig. 9b) or negative (Fig. 9c, every probe a miss).
LOOKUP_MODE=${LOOKUP_MODE:-positive}
# LIST_FILE: a GUFI path list, built with preplist and probed over its real
# paths.  Unset: a synthetic K x M tree.
LIST_FILE=${LIST_FILE:-}
OUT=${OUT:-$HERE/results.csv}
RAW=${RAW:-$HERE/raw/$(date +%Y%m%d-%H%M%S)}
if [ -n "$LIST_FILE" ]; then
	[ -s "$LIST_FILE" ] || { echo "ERROR: LIST_FILE $LIST_FILE is missing or empty" >&2; exit 1; }
	case "$LOOKUP_MODE" in
	positive) TIMED_VERB=runlist ;;
	negative) TIMED_VERB=runlistneg ;;
	*) echo "ERROR: LOOKUP_MODE must be positive or negative" >&2; exit 1 ;;
	esac
	WORKLOAD_KIND=gufi-list
else
	case "$LOOKUP_MODE" in
	positive) TIMED_VERB=run ;;
	negative) TIMED_VERB=runneguniq ;;
	*) echo "ERROR: LOOKUP_MODE must be positive or negative" >&2; exit 1 ;;
	esac
	WORKLOAD_KIND=synthetic-preplink
fi

CG=/sys/fs/cgroup/splinefs-cache-$$
TREE=$MNT/tree
BENCH=$AE_UTILS/bin/multidir_lookup_bench
PROMOTE_DIR=$AE_UTILS/bin/ext5_promote_dir

cleanup_fs()
{
	set +e
	cd /
	if mountpoint -q "$MNT"; then
		umount "$MNT" 2>/dev/null || true
	fi
	rmmod ext5 2>/dev/null || true
	rmmod jbd3 2>/dev/null || true
	set -e
}

cleanup()
{
	set +e
	cleanup_fs
	if [ -d "$CG" ]; then
		echo max > "$CG/memory.max" 2>/dev/null || true
		rmdir "$CG" 2>/dev/null || true
	fi
}
trap cleanup EXIT

eval_require_device
[ -x "$BENCH" ] || eval_die "build utils first"
[ -s "$MODULE_DIR/ext5/ext5.ko" ] || eval_die "build the selected module first"
needed_fds=$((K + 4096))
[ "$(ulimit -Hn)" -ge "$needed_fds" ] ||
	eval_die "hard fd limit is below required $needed_fds"
ulimit -n "$needed_fds"
mkdir -p "$MNT" "$RAW" "$CG"
echo 0 > "$CG/memory.swap.max" 2>/dev/null || true
mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug

printf 'rep,fs,workload,lookup_mode,cap_bytes,K,M,lookups,settle_ops,settle_seconds,seconds,ops_per_sec,manual_operations,bound_violations,live_roots\n' > "$OUT"

mount_case()
{
	local fs=$1
	cleanup_fs
	mkfs.ext4 -q -F -E nodiscard,lazy_itable_init=1 -O ^has_journal "$DEV"
	case "$fs" in
	splinefs)
		insmod "$MODULE_DIR/jbd3/jbd3.ko"
		insmod "$MODULE_DIR/ext5/ext5.ko" \
			li_stats_enabled="$STATS_ENABLED" $MODULE_ARGS
		mount -t ext5 -o noatime "$DEV" "$MNT"
		;;
	naive)
		# li_debug_controls=Y unlocks the promote ioctl this arm uses.
		insmod "$MODULE_DIR/jbd3/jbd3.ko"
		insmod "$MODULE_DIR/ext5/ext5.ko" \
			li_stats_enabled="$STATS_ENABLED" $MODULE_ARGS \
			li_policy_mode=2 li_promote_subsume=0 li_promote_delta_bytes=0 \
			li_debug_controls=Y
		mount -t ext5 -o noatime "$DEV" "$MNT"
		;;
	ext4)
		mount -t ext4 -o noatime "$DEV" "$MNT"
		;;
	*) eval_die "unknown filesystem $fs" ;;
	esac
}

run_cgroup()
{
	local cap=$1 rc=0
	shift
	echo max > "$CG/memory.max"
	echo "$cap" > "$CG/memory.max"
	(
		echo "$BASHPID" > "$CG/cgroup.procs"
		exec "$@"
	) || rc=$?
	echo max > "$CG/memory.max"
	return "$rc"
}

oom_kills() { awk '$1=="oom_kill" {print $2}' "$CG/memory.events" 2>/dev/null || echo 0; }

value_from()
{
	local key=$1 file=$2
	sed -n "s/.*${key}=\([0-9.]*\).*/\1/p" "$file" | tail -1
}

run_one()
{
	local rep=$1 fs=$2
	local settle_file=$RAW/$fs.r$rep.settle
	local stats=/sys/kernel/debug/ext5/li_stats
	local settle_seconds settle_ops=$SETTLE_OPS manual=0 bounds=0 roots=0
	local cap timed seconds rate kills rc lim policy_line policy_path policy_mode gate
	local files dirs entries floor rmd tstar root_ino extra attempt topup parent_count

	echo "[$(date +%T)] prepare fs=$fs rep=$rep" >&2
	mount_case "$fs"
	if [ -n "$LIST_FILE" ]; then
		"$BENCH" preplist "$TREE" "$LIST_FILE" "$POOL"
	else
		"$BENCH" preplink "$TREE" "$K" "$M" "$POOL"
	fi
	if [ "$fs" = naive ]; then
		# Depth-first, so every directory is a root before its parent is walked;
		# subsume=0 then keeps each one as its own model.
		find "$TREE" -depth -type d -print |
			"$PROMOTE_DIR" - > "$RAW/$fs.r$rep.force" 2>&1 ||
			echo "WARN: some per-directory promotions failed; see $RAW/$fs.r$rep.force" >&2
		"$AE_UTILS/bin/ext5_li_wait" "$TREE" >/dev/null 2>&1 || true
	fi
	# The same untimed settling phase on every filesystem.
	if [ -n "$LIST_FILE" ]; then
		"$BENCH" runlistneg "$TREE" "$LIST_FILE" "$SETTLE_OPS" \
			$((rep * 7919 + 17)) > "$settle_file"
	else
		"$BENCH" runneguniq "$TREE" "$K" "$M" "$SETTLE_OPS" \
			$((rep * 7919 + 17)) > "$settle_file"
	fi
	settle_seconds=$(value_from seconds "$settle_file")
	if [ "$fs" = splinefs ]; then
		if [ "$EVAL_REQUIRE_TREE_ROOT" = 1 ]; then
			for attempt in 1 2 3 4 5 6; do
				"$AE_UTILS/bin/ext5_li_wait" "$TREE" >/dev/null 2>&1 || true
				policy_line=$("$AE_UTILS/bin/ext5_li_dir_policy" --csv "$TREE" | tail -n 1)
				IFS=, read -r policy_path policy_mode gate files dirs entries floor rmd tstar root_ino <<< "$policy_line"
				[ "$policy_mode" != ROOT ] || break
				[ "$policy_mode" = MUTABLE ] || eval_die "tree has unexpected policy mode: $policy_line"
				[ "$attempt" -lt 6 ] || eval_die "subtree model did not settle at $TREE: $policy_line"
				extra=$((tstar > rmd ? tstar - rmd + 65536 : 65536))
				[ "$extra" -le 5000000 ] || eval_die "tree T* is too large for bounded settling: $policy_line"
				topup=$RAW/$fs.r$rep.settle-extra$attempt
				if [ -n "$LIST_FILE" ]; then
					"$BENCH" runlistneg "$TREE" "$LIST_FILE" "$extra" \
						$((rep * 7919 + attempt * 104729 + 17)) > "$topup"
				else
					"$BENCH" runneguniq "$TREE" "$K" "$M" "$extra" \
						$((rep * 7919 + attempt * 104729 + 17)) > "$topup"
				fi
				settle_ops=$((settle_ops + extra))
				settle_seconds=$(awk -v a="$settle_seconds" -v b="$(value_from seconds "$topup")" 'BEGIN { printf "%.6f", a+b }')
			done
			printf '%s\n' "$policy_line" > "$RAW/$fs.r$rep.tree-policy"
			if [ "$EVAL_REQUIRE_PARENT_COUNT" -gt 0 ]; then
				parent_count=$("$AE_UTILS/bin/ext5_li_info" "$TREE" |
					awk '$1=="parent_count:" {print $2}')
				[ "$parent_count" -eq "$EVAL_REQUIRE_PARENT_COUNT" ] ||
					eval_die "shared model covers $parent_count directories; expected $EVAL_REQUIRE_PARENT_COUNT"
			fi
		fi
		eval_assert_splinefs_run "$TREE"
	fi
	if [ "$fs" != ext4 ]; then
		cp "$stats" "$RAW/$fs.r$rep.stats"
		manual=$(awk '$1=="manual_operations" {print $2}' "$stats")
		bounds=$(awk '$1=="rs_bound_violations" {print $2}' "$stats")
		roots=$(awk '$1=="live_roots" {print $2}' "$stats")
		if [ "$fs" = naive ]; then
			[ "${roots:-0}" -gt 1 ] ||
				eval_die "naive arm holds $roots models; expected one per directory"
		elif [ "${EVAL_EXPECT_NO_PROMOTION:-0}" = 1 ]; then
			echo "no promotion expected for this arm" \
				> "$RAW/$fs.r$rep.info"
		else
			"$AE_UTILS/bin/ext5_li_info" "${EVAL_PROMOTED_ROOT:-$TREE}" \
				> "$RAW/$fs.r$rep.info"
		fi
	fi

	for cap in $CAPS; do
		eval_drop_caches
		timed=$RAW/$fs.r$rep.cap$cap.out
		# runneguniq, not runneg: every probe must reach the filesystem.
		kills=$(oom_kills); rc=0
		lim=()
		[ "$fs" != naive ] || lim=(timeout "$NAIVE_TIMEOUT")
		if [ -n "$LIST_FILE" ]; then
			run_cgroup "$cap" "${lim[@]}" "$BENCH" "$TIMED_VERB" "$TREE" \
				"$LIST_FILE" "$LOOKUPS" \
				$((rep * 104729 + cap / 4096)) > "$timed" || rc=$?
		else
			run_cgroup "$cap" "${lim[@]}" "$BENCH" "$TIMED_VERB" "$TREE" "$K" "$M" \
				"$LOOKUPS" $((rep * 104729 + cap / 4096)) > "$timed" || rc=$?
		fi
		seconds=$(value_from seconds "$timed")
		rate=$(value_from ops_per_sec "$timed")
		if [ "$rc" != 0 ]; then
			# An OOM kill, or naive not finishing in time, is a row with no throughput.
			if [ "$rc" = 124 ] && [ "$fs" = naive ]; then
				echo "WARN: naive did not finish $LOOKUPS lookups in ${NAIVE_TIMEOUT}s at cap $cap" >&2
				echo "did not finish within ${NAIVE_TIMEOUT}s at memory.max=$cap" >> "$timed"
			elif [ "$(oom_kills)" -gt "$kills" ]; then
				echo "WARN: $fs killed by the OOM killer at cap $cap" >&2
				echo "killed by the OOM killer at memory.max=$cap" >> "$timed"
			else
				eval_die "lookups on $fs at cap $cap failed (rc=$rc); see $timed"
			fi
			seconds=""; rate=""
		fi
		printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
			"$rep" "$fs" "$WORKLOAD_KIND" "$LOOKUP_MODE" "$cap" "$K" "$M" "$LOOKUPS" \
			"$settle_ops" "$settle_seconds" "$seconds" "$rate" \
			"$manual" "$bounds" "$roots" >> "$OUT"
		if [ "$fs" != ext4 ]; then
			cp "$stats" "$RAW/$fs.r$rep.cap$cap.poststats"
		fi
	done
	cleanup_fs
}

for rep in $(seq 1 "$REPS"); do
	if ((rep % 2)); then order=$FSES; else order=$(echo "$FSES" | awk '{for(i=NF;i;i--) printf "%s%s",$i,(i==1?ORS:OFS)}'); fi
	for fs in $order; do
		run_one "$rep" "$fs"
	done
done

trap - EXIT
cleanup
echo "results: $OUT"
echo "raw: $RAW"
