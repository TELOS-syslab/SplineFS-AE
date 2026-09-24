#!/usr/bin/env bash
# Fig. 11 driver: one application stage, staged and timed cold on each filesystem.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
AE_ROOT=$(cd "$HERE/../../.." && pwd)
AE_UTILS=$AE_ROOT/utils
export AE_UTILS
. "$HERE/../lib/safety.sh"

DEV=${DEV:-}
MNT=${MNT:-/mnt/splinefs-app}
TREE=$MNT/tree
WORKLOAD=${WORKLOAD:?set WORKLOAD}
STAGE_CMD=${STAGE_CMD:?set STAGE_CMD}
ADAPT_CMD=${ADAPT_CMD:?set ADAPT_CMD to a workload-native namespace phase}
RUN_CMD=${RUN_CMD:-$ADAPT_CMD}
SETTLE_REPEATS=${SETTLE_REPEATS:-1}
RUN_REPEATS=${RUN_REPEATS:-1}
ADAPT_DROP_CACHES=${ADAPT_DROP_CACHES:-1}
CAPS=${CAPS:-2147483648}
REPS=${REPS:-3}
FSES=${FSES:-"splinefs ext4"}
STATS_ENABLED=${STATS_ENABLED:-N}
OUT=${OUT:-$HERE/results.csv}
RAW=${RAW:-$HERE/raw/$(date +%Y%m%d-%H%M%S)}
CG=/sys/fs/cgroup/splinefs-app-$$

cleanup_fs()
{
	set +e
	cd /
	if mountpoint -q "$MNT"; then umount "$MNT" 2>/dev/null || true; fi
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
mkdir -p "$MNT" "$RAW" "$CG"
# SWAP_MAX caps swap; unset, it is not capped.
echo "${SWAP_MAX:-max}" > "$CG/memory.swap.max" 2>/dev/null || true
mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug
[ "$SETTLE_REPEATS" -ge 1 ] || eval_die "SETTLE_REPEATS must be positive"
[ "$RUN_REPEATS" -ge 1 ] || eval_die "RUN_REPEATS must be positive"

printf 'workload,rep,pass,fs,cap_bytes,settle_repeats,adapt_drop_caches,adapt_seconds,steady_seconds,manual_operations,bound_violations,live_roots\n' > "$OUT"

mount_case()
{
	local fs=$1
	cleanup_fs
	case "$fs" in
	splinefs)
		mkfs.ext4 -q -F -E nodiscard,lazy_itable_init=1 -O ^has_journal "$DEV"
		insmod "$AE_ROOT/src/jbd3/jbd3.ko"
		insmod "$AE_ROOT/src/ext5/ext5.ko" li_stats_enabled="$STATS_ENABLED"
		mount -t ext5 -o noatime "$DEV" "$MNT" ;;
	ext4)
		mkfs.ext4 -q -F -E nodiscard,lazy_itable_init=1 -O ^has_journal "$DEV"
		mount -t ext4 -o noatime "$DEV" "$MNT" ;;
	# Baseline options as in the paper's setup.
	xfs)
		mkfs.xfs -q -f -l size=64m,lazy-count=1 "$DEV"
		mount -t xfs -o noatime,logbsize=256k "$DEV" "$MNT" ;;
	btrfs)
		mkfs.btrfs -q -f -d single -m single -O '^extref' "$DEV"
		mount -t btrfs -o noatime,space_cache=v2 "$DEV" "$MNT" ;;
	f2fs)
		mkfs.f2fs -q -f "$DEV"
		mount -t f2fs -o noatime,active_logs=2 "$DEV" "$MNT" ;;
	*) eval_die "unknown filesystem $fs" ;;
	esac
}

run_cgroup_shell()
{
	local cap=$1 command=$2 log=$3
	echo max > "$CG/memory.max"
	echo "$cap" > "$CG/memory.max"
	(
		echo "$BASHPID" > "$CG/cgroup.procs"
		export TREE
		exec bash -c "$command"
	) > "$log" 2>&1
	echo max > "$CG/memory.max"
}

seconds_for()
{
	local start=$1 end=$2
	awk -v a="$start" -v b="$end" 'BEGIN { printf "%.6f", b-a }'
}

for rep in $(seq 1 "$REPS"); do
	if ((rep % 2)); then order=$FSES; else order=$(echo "$FSES" | awk '{for(i=NF;i;i--) printf "%s%s",$i,(i==1?ORS:OFS)}'); fi
	for fs in $order; do
		mount_case "$fs"
		export TREE
		mkdir -p "$TREE"
		bash -c "$STAGE_CMD" > "$RAW/$WORKLOAD.$fs.r$rep.stage" 2>&1
		adapt_start=$(date +%s.%N)
		for settle in $(seq 1 "$SETTLE_REPEATS"); do
			if [ "$ADAPT_DROP_CACHES" -eq 1 ]; then
				sync
				echo 2 > /proc/sys/vm/drop_caches
			fi
			bash -c "$ADAPT_CMD" > "$RAW/$WORKLOAD.$fs.r$rep.adapt$settle" 2>&1
		done
		adapt_end=$(date +%s.%N)
		manual=0; bounds=0; roots=0
		if [ "$fs" = splinefs ]; then
			eval_assert_splinefs_run "$TREE"
			cp /sys/kernel/debug/ext5/li_stats "$RAW/$WORKLOAD.$fs.r$rep.stats"
			manual=$(awk '$1=="manual_operations" {print $2}' /sys/kernel/debug/ext5/li_stats)
			bounds=$(awk '$1=="rs_bound_violations" {print $2}' /sys/kernel/debug/ext5/li_stats)
			roots=$(awk '$1=="live_roots" {print $2}' /sys/kernel/debug/ext5/li_stats)
		fi
		for cap in $CAPS; do
			# Each pass drops caches first, so every sample is cold.
			for pass in $(seq 1 "$RUN_REPEATS"); do
				eval_drop_caches
				steady_start=$(date +%s.%N)
				run_cgroup_shell "$cap" "$RUN_CMD" \
					"$RAW/$WORKLOAD.$fs.r$rep.cap$cap.p$pass.run"
				steady_end=$(date +%s.%N)
				printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
					"$WORKLOAD" "$rep" "$pass" "$fs" "$cap" \
					"$SETTLE_REPEATS" "$ADAPT_DROP_CACHES" \
					"$(seconds_for "$adapt_start" "$adapt_end")" \
					"$(seconds_for "$steady_start" "$steady_end")" \
					"$manual" "$bounds" "$roots" >> "$OUT"
			done
		done
		cleanup_fs
	done
done

trap - EXIT
cleanup
echo "results: $OUT"
echo "raw: $RAW"
