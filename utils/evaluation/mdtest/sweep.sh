#!/usr/bin/env bash
# Fig. 13: mdtest create (-C), stat (-T), read (-E) and remove (-r) across
# file counts and memcg caps, each cell on a freshly formatted device.
# DESTRUCTIVE.
#
# Environment:
#   DEV, CONFIRM_DESTROY   required
#   COUNTS                 files per cell, default 100K..1M
#   CAPS                   memcg caps, default 512 MB..8 GB
#   REPS                   default 3
#   FSES                   default "splinefs ext4"
#   PROMOTE=force|auto     force (default): promote between create and stat,
#                          outside the timers; auto: the policy decides
#   MODULE_ARGS            extra insmod parameters
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
AE_ROOT=$(cd "$HERE/../../.." && pwd)
MODULE_DIR=${MODULE_DIR:-$AE_ROOT/src}
AE_UTILS=$AE_ROOT/utils
export AE_UTILS
. "$HERE/../lib/safety.sh"

DEV=${DEV:-}
MNT=${MNT:-/mnt/splinefs-mdtest}
COUNTS=${COUNTS:-"100000 200000 400000 800000 1000000"}
CAPS=${CAPS:-"536870912 1073741824 2147483648 4294967296 8589934592"}
REPS=${REPS:-3}
FSES=${FSES:-"splinefs ext4"}
CPU=${CPU:-4}
FS_BLOCKS=${FS_BLOCKS:-26214400}
MODULE_ARGS=${MODULE_ARGS:-}
PROMOTE=${PROMOTE:-force}
OUT=${OUT:-$HERE/results.csv}
RAW=${RAW:-$HERE/raw/$(date +%Y%m%d-%H%M%S)}
CG=/sys/fs/cgroup/splinefs-mdtest-$$
STATS=/sys/kernel/debug/ext5/li_stats
DEBUG_ARG=""; POLICY_ARG=""
if [ "$PROMOTE" = force ]; then
	DEBUG_ARG="li_debug_controls=Y"
	POLICY_ARG="li_policy_mode=0"
fi
PROMOTE_DIR=$AE_UTILS/bin/ext5_promote_dir
COMPACT=$AE_UTILS/bin/ext5_compact

eval_require_device
command -v mdtest >/dev/null || eval_die "mdtest not found (IOR 4.0.0)"
mkdir -p "$MNT" "$RAW" "$(dirname "$OUT")" "$CG" 2>/dev/null

cleanup() {
	# Restore the caller's errexit setting.
	local _restore=+e
	case $- in *e*) _restore=-e ;; esac
	set +e
	cd /
	mountpoint -q "$MNT" && umount "$MNT" 2>/dev/null
	rmmod ext5 2>/dev/null
	rmmod jbd3 2>/dev/null
	[ -d "$CG" ] && { echo max > "$CG/memory.max" 2>/dev/null; rmdir "$CG" 2>/dev/null; }
	set "$_restore"
}
trap cleanup EXIT

grep -qw memory /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null ||
	echo +memory > /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null || true

mount_case() {
	cleanup
	mkdir -p "$MNT" "$CG" 2>/dev/null
	mkfs.ext4 -qF -E nodiscard,lazy_itable_init=1 -O ^has_journal,large_dir \
		-N 4194304 "$DEV" "$(eval_fs_blocks "$FS_BLOCKS")" >/dev/null 2>&1
	case "$1" in
	splinefs)
		insmod "$MODULE_DIR/jbd3/jbd3.ko"
		insmod "$MODULE_DIR/ext5/ext5.ko" li_stats_enabled=Y \
			$DEBUG_ARG $POLICY_ARG $MODULE_ARGS
		mount -t ext5 -o noatime "$DEV" "$MNT" ;;
	ext4)
		mount -t ext4 -o noatime "$DEV" "$MNT" ;;
	*) eval_die "unknown filesystem $1" ;;
	esac
	eval_require_mounted "$MNT"
}

in_cgroup() {
	local cap=$1; shift
	echo "$cap" > "$CG/memory.max"
	echo 0 > "$CG/memory.swap.max" 2>/dev/null || true
	echo $$ > "$CG/cgroup.procs" 2>/dev/null || true
	"$@"
	local rc=$?
	echo $$ > /sys/fs/cgroup/cgroup.procs 2>/dev/null || true
	echo max > "$CG/memory.max" 2>/dev/null || true
	return $rc
}

# The rate column of the named operation in mdtest's summary.
rate_of() { awk -v a="$2" -v b="$3" '$1==a && $2==b {print $5; exit}' "$1"; }

printf 'rep,fs,files,cap_bytes,create_ops,stat_ops,read_ops,remove_ops,manual_operations,bound_violations,live_roots\n' > "$OUT"

for rep in $(seq 1 "$REPS"); do
	if ((rep % 2)); then order=$FSES; else order=$(echo "$FSES" | awk '{for(i=NF;i;i--) printf "%s%s",$i,(i==1?ORS:OFS)}'); fi
	for files in $COUNTS; do
		for cap in $CAPS; do
			for fs in $order; do
				pfx="$RAW/$fs.n$files.c$cap.r$rep"
				echo "[$(date +%T)] $fs files=$files cap=$((cap/1024/1024))M rep$rep" >&2
				mount_case "$fs"
				root=$MNT/mdtest
				mkdir -p "$root"
				# Each phase runs cold, under the cap.
				for phase in C T E r; do
					if [ "$PROMOTE" = force ] && [ "$phase" = T ] &&
					   [ "$fs" = splinefs ]; then
						"$PROMOTE_DIR" "$root" > "$pfx.promote" 2>&1 ||
							echo "WARN: promote failed at $pfx" >&2
						"$COMPACT" "$root" > "$pfx.compact" 2>&1 || true
					fi
					eval_drop_caches
					in_cgroup "$cap" taskset -c "$CPU" mdtest \
						-d "$root" -n "$files" -F -i 1 -P "-$phase" \
						> "$pfx.$phase" 2>&1
				done
				manual=NA; bounds=NA; roots=0
				if [ "$fs" = splinefs ]; then
					cp "$STATS" "$pfx.stats" 2>/dev/null || true
					awk '$1=="compaction_publications"||$1=="compact_build_attempts"||$1=="compaction_ns_total"' \
						"$STATS" > "$pfx.compact" 2>/dev/null || true
					manual=$(awk '$1=="manual_operations" {print $2}' "$STATS" 2>/dev/null || echo NA)
					bounds=$(awk '$1=="rs_bound_violations" {print $2}' "$STATS" 2>/dev/null || echo NA)
					roots=$(awk '$1=="live_roots" {print $2}' "$STATS" 2>/dev/null || echo 0)
				fi
				printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
					"$rep" "$fs" "$files" "$cap" \
					"$(rate_of "$pfx.C" File creation)" \
					"$(rate_of "$pfx.T" File stat)" \
					"$(rate_of "$pfx.E" File read)" \
					"$(rate_of "$pfx.r" File removal)" \
					"$manual" "$bounds" "$roots" >> "$OUT"
				# Unlink failures show up in the remove phase.
				if grep -qi 'unlink() failed\|Error' "$pfx.r"; then
					echo "WARN: $pfx.r reports unlink failures" >&2
				fi
				cleanup
			done
		done
	done
done

echo "results: $OUT" >&2
echo "raw: $RAW" >&2
