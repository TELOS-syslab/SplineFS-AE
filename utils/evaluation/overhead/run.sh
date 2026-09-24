#!/usr/bin/env bash
# Per-lookup CPU with a warm page cache and a cold dcache.  DESTRUCTIVE.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
AE_ROOT=$(cd "$HERE/../../.." && pwd)
MODULE_DIR=${MODULE_DIR:-$AE_ROOT/src}
AE_UTILS=$AE_ROOT/utils
export AE_UTILS
. "$HERE/../lib/safety.sh"

DEV=${DEV:-}
MNT=${MNT:-/mnt/splinefs-overhead}
FILES=${FILES:-200000}
LOOKUPS=${LOOKUPS:-200000}
SETTLE_OPS=${SETTLE_OPS:-100000}
REPS=${REPS:-5}
STATS_ENABLED=${STATS_ENABLED:-N}
FSES=${FSES:-"splinefs ext4 xfs btrfs"}
OUT=${OUT:-$HERE/results.csv}
RAW=${RAW:-$HERE/raw/$(date +%Y%m%d-%H%M%S)}
BENCH=$AE_UTILS/bin/lookup_bench
TREE=$MNT/lookup

cleanup()
{
	set +e
	cd /
	if mountpoint -q "$MNT"; then umount "$MNT" 2>/dev/null || true; fi
	rmmod ext5 2>/dev/null || true
	rmmod jbd3 2>/dev/null || true
}
trap cleanup EXIT

eval_require_device
mkdir -p "$MNT" "$RAW"
mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug
printf 'rep,fs,files,lookups,cycles_per_lookup,instructions_per_lookup,ns_per_lookup,manual_operations,bound_violations\n' > "$OUT"

mount_case()
{
	local fs=$1
	cleanup
	case "$fs" in
	splinefs)
		mkfs.ext4 -q -F -O ^has_journal "$DEV"
		insmod "$MODULE_DIR/jbd3/jbd3.ko"
		insmod "$MODULE_DIR/ext5/ext5.ko" li_stats_enabled="$STATS_ENABLED"
		mount -t ext5 -o noatime "$DEV" "$MNT" ;;
	ext4)
		mkfs.ext4 -q -F -O ^has_journal "$DEV"
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

for rep in $(seq 1 "$REPS"); do
	for fs in $FSES; do
		mount_case "$fs"
		"$BENCH" prepare "$TREE" "$FILES"
		LOOKUP_BENCH_MISS_BASE=$((FILES * 2)) \
			"$BENCH" run "$TREE" negative-unique "$SETTLE_OPS" "$FILES" \
			> "$RAW/$fs.r$rep.settle"
		manual=0; bounds=0
		if [ "$fs" = splinefs ]; then
			eval_assert_splinefs_run "$TREE"
			manual=$(awk '$1=="manual_operations" {print $2}' /sys/kernel/debug/ext5/li_stats)
			bounds=$(awk '$1=="rs_bound_violations" {print $2}' /sys/kernel/debug/ext5/li_stats)
			"$AE_UTILS/bin/ext5_li_info" "${EVAL_PROMOTED_ROOT:-$TREE}" \
				> "$RAW/$fs.r$rep.info"
		fi
		# Warm all filesystem-private index pages, then drop dentries and inodes only.
		"$BENCH" run "$TREE" positive-unique "$LOOKUPS" "$FILES" "$rep" \
			> /dev/null
	echo 2 > /proc/sys/vm/drop_caches
		perf stat -x, -e cycles,instructions \
			-o "$RAW/$fs.r$rep.perf" -- \
			"$BENCH" run "$TREE" positive-unique "$LOOKUPS" "$FILES" \
			$((rep + 1000)) > "$RAW/$fs.r$rep.out"
		cycles=$(awk -F, '$3=="cycles" {print $1}' "$RAW/$fs.r$rep.perf")
		instructions=$(awk -F, '$3=="instructions" {print $1}' "$RAW/$fs.r$rep.perf")
		ns=$(awk -F= '$1=="ns_per_op" {print $2}' "$RAW/$fs.r$rep.out")
		awk -v r="$rep" -v fs="$fs" -v n="$FILES" -v l="$LOOKUPS" \
			-v c="$cycles" -v i="$instructions" -v ns="$ns" \
			-v m="$manual" -v b="$bounds" \
			'BEGIN { printf "%d,%s,%d,%d,%.3f,%.3f,%.3f,%d,%d\n", r,fs,n,l,c/l,i/l,ns,m,b }' \
			>> "$OUT"
		cleanup
	done
done

trap - EXIT
cleanup
echo "results: $OUT"
