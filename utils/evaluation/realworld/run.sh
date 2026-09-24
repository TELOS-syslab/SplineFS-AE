#!/usr/bin/env bash
# Fig. 10: cold scans of real namespaces under a memcg cap.  DESTRUCTIVE.
#
# Every filesystem replays the same recorded visit order, so the comparison
# measures lookups rather than each filesystem's readdir order.
#
# Environment:
#   DEV, CONFIRM_DESTROY   required
#   TREES="label:/list ..."                 GUFI path lists: built with
#                                           preplist, scanned with scanlist
#   TRACES="label:prep:scan:recroot ..."    recorded syscall streams: prep
#                                           builds the tree, scan is timed
#   TRACE_FLAGS            extra fstrace_replay flags for the timed scan
#   CAPS                   memcg caps in bytes, default 2 GB
#   FSES                   default "splinefs ext4 xfs btrfs f2fs"
#   REPS                   default 3
#   PROMOTE=force|auto     force (default, the paper's figure): one model at
#                          the first directory wider than FANOUT_MIN (default
#                          100), placed before the caches are dropped;
#                          auto: whatever the policy promotes
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
AE_ROOT=$(cd "$HERE/../../.." && pwd)
AE_UTILS=$AE_ROOT/utils
export AE_UTILS
. "$HERE/../lib/safety.sh"

DEV=${DEV:-}
MNT=${MNT:-/mnt/splinefs-realworld}
POOL=${POOL:-4096}
FS_BLOCKS=${FS_BLOCKS:-121634816}         # 116 GiB: mke2fs reads it in KiB
TREES=${TREES:-""}
TRACES=${TRACES:-""}
TRACE_FLAGS=${TRACE_FLAGS:-""}
CAPS=${CAPS:-2147483648}
FSES=${FSES:-"splinefs ext4 xfs btrfs f2fs"}
REPS=${REPS:-3}
SETTLE_OPS=${SETTLE_OPS:-1000000}
PROMOTE=${PROMOTE:-force}
FANOUT_MIN=${FANOUT_MIN:-100}
case "$PROMOTE" in
force) EXT5_ARGS="li_policy_mode=0 li_debug_controls=Y" ;;
auto)  EXT5_ARGS="" ;;
*) echo "ERROR: PROMOTE must be force or auto" >&2; exit 1 ;;
esac
OUT=${OUT:-$HERE/results.csv}
RAW=${RAW:-$HERE/raw/$(date +%Y%m%d-%H%M%S)}

BENCH=$AE_UTILS/bin/multidir_lookup_bench
REPLAY=$AE_UTILS/bin/fstrace_replay
LIWALK=$AE_UTILS/bin/ext5_li_walk
LIWAIT=$AE_UTILS/bin/ext5_li_wait
PROMOTE_DIR=$AE_UTILS/bin/ext5_promote_dir
COMPACT=$AE_UTILS/bin/ext5_compact
STATS=/sys/kernel/debug/ext5/li_stats
TREE=$MNT/tree
CG=/sys/fs/cgroup/splinefs-realworld-$$

eval_require_device
[ -n "$TREES$TRACES" ] ||
	eval_die "set TREES to \"label:/path/to/pathlist ...\" or TRACES to \"label:prep:scan:recroot ...\""
[ -z "$TRACES" ] || [ -x "$REPLAY" ] || eval_die "build utils first: $REPLAY is missing"
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
	case "$1" in
	splinefs)
		mkfs.ext4 -qF -E nodiscard,lazy_itable_init=1 -O ^has_journal,large_dir \
			-N 8388608 "$DEV" "$(eval_fs_blocks "$FS_BLOCKS")" >/dev/null 2>&1
		insmod "$AE_ROOT/src/jbd3/jbd3.ko"
		# force needs promotion off and the debug controls on; auto takes the defaults.
		insmod "$AE_ROOT/src/ext5/ext5.ko" $EXT5_ARGS
		mount -t ext5 -o noatime "$DEV" "$MNT" ;;
	ext4)
		mkfs.ext4 -qF -E nodiscard,lazy_itable_init=1 -O ^has_journal,large_dir \
			-N 8388608 "$DEV" "$(eval_fs_blocks "$FS_BLOCKS")" >/dev/null 2>&1
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
	*) eval_die "unknown filesystem $1" ;;
	esac
	eval_require_mounted "$MNT"
}

run_cgroup() {
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


printf 'rep,label,source,fs,cap_bytes,entries,seconds,ops_per_sec,promoted_roots,manual_operations,bound_violations\n' > "$OUT"

value_from() { awk -v k="$1=" '{for(i=1;i<=NF;i++) if(index($i,k)==1) print substr($i,length(k)+1)}' "$2"; }

# Build the tree and run the untimed settling pass, on every filesystem so
# that setup and cache history match.
prepare_tree() {
	local kind=$1 pfx=$2 rep=$3
	case "$kind" in
	list)
		"$BENCH" preplist "$TREE" "$LIST" "$POOL" > "$pfx.prep" 2>&1
		"$BENCH" runlistneg "$TREE" "$LIST" "$SETTLE_OPS" \
			$((rep * 7919 + 17)) > "$pfx.settle" 2>&1 ;;
	trace)
		# The recorded root existed before recording, so the trace never creates it.
		mkdir -p "$TREE"
		"$REPLAY" --warmup --map "$RECROOT/=$TREE/" "$PREP" \
			> "$pfx.prep" 2>&1
		# A failed prepare op is reported, not fatal.
		local failed
		failed=$(value_from unexpected_failures "$pfx.prep" | tail -1)
		[ "${failed:-0}" = 0 ] ||
			echo "WARN: $label $fs: prepare replay had $failed unexpected failures" >&2
		# The recorded scan itself is the settling pass.
		"$REPLAY" --map "$RECROOT/=$TREE/" "$SCAN" > "$pfx.settle" 2>&1 ;;
	esac
}

# Descend single-child chains to the first directory wide enough for one model.
find_fanout_point() {
	local d=$1 n only
	while :; do
		# Stop counting at FANOUT_MIN + 1.
		n=$(find "$d" -mindepth 1 -maxdepth 1 -printf . 2>/dev/null |
			head -c $((FANOUT_MIN + 1)) | wc -c)
		if [ "$n" -gt "$FANOUT_MIN" ]; then
			printf '%s\n' "$d"; return 0
		fi
		if [ "$n" -eq 1 ]; then
			only=$(find "$d" -mindepth 1 -maxdepth 1 2>/dev/null)
			if [ -d "$only" ]; then d=$only; continue; fi
		fi
		printf '%s\n' "$d"; return 0
	done
}

# PROMOTE=force only.  Runs before drop_caches, outside the timer.
place_model() {
	local pfx=$1 fp
	fp=$(find_fanout_point "$TREE")
	echo "    fanout point: ${fp#$MNT}" >&2
	printf '%s\n' "$fp" > "$pfx.fanout"
	if ! "$PROMOTE_DIR" "$fp" > "$pfx.force" 2>&1; then
		echo "WARN: $label rep$rep: promote failed at $fp" >&2
		return
	fi
	# A failed compaction leaves a valid, unmerged index.
	"$COMPACT" "$fp" >> "$pfx.force" 2>&1 ||
		echo "WARN: $label rep$rep: compact failed at $fp" >&2
}

timed_scan() {
	local kind=$1 out=$2
	case "$kind" in
	list)  "$BENCH" scanlist "$TREE" "$LIST" > "$out" 2>&1 ;;
	trace) "$REPLAY" $TRACE_FLAGS --map "$RECROOT/=$TREE/" "$SCAN" \
			> "$out" 2>&1 ;;
	esac
}

for rep in $(seq 1 "$REPS"); do
	# Alternate the order so drift cannot favour one system.
	if ((rep % 2)); then order=$FSES; else order=$(echo "$FSES" | awk '{for(i=NF;i;i--) printf "%s%s",$i,(i==1?ORS:OFS)}'); fi
	for entry in $TREES $TRACES; do
		LIST=""; PREP=""; SCAN=""; RECROOT=""
		case "$entry" in
		*:*:*:*)
			kind=trace
			label=${entry%%:*}; rest=${entry#*:}
			PREP=${rest%%:*}; rest=${rest#*:}
			SCAN=${rest%%:*}; RECROOT=${rest#*:}
			RECROOT=${RECROOT%/}
			if ! [ -s "$PREP" ] || ! [ -s "$SCAN" ]; then
				echo "skip $label: $PREP or $SCAN missing" >&2
				continue
			fi ;;
		*)
			kind=list
			label=${entry%%:*}; LIST=${entry#*:}
			[ -s "$LIST" ] || { echo "skip $label: $LIST missing" >&2; continue; } ;;
		esac
		for fs in $order; do
			pfx="$RAW/$label.$fs.r$rep"
			echo "[$(date +%T)] $label ($kind) $fs rep$rep" >&2
			mount_case "$fs"
			prepare_tree "$kind" "$pfx" "$rep"
			roots=0; manual=0; bounds=0
			if [ "$fs" = splinefs ]; then
				[ "$PROMOTE" = force ] && place_model "$pfx"
				"$LIWAIT" "$TREE" >/dev/null 2>&1 || true
				roots=$("$LIWALK" --csv "$TREE" 2>/dev/null |
					awk -F, '$2=="ROOT"' | wc -l) || roots=0
				cp "$STATS" "$pfx.stats" 2>/dev/null || true
				manual=$(awk '$1=="manual_operations" {print $2}' "$STATS" 2>/dev/null || echo NA)
				bounds=$(awk '$1=="rs_bound_violations" {print $2}' "$STATS" 2>/dev/null || echo NA)
				if [ "${roots:-0}" -eq 0 ]; then
					if [ "$PROMOTE" = force ]; then
						echo "WARN: $label rep$rep: forced placement produced no root" >&2
					else
						echo "WARN: $label rep$rep: policy promoted nothing" >&2
					fi
				fi
			fi
			for cap in $CAPS; do
				eval_drop_caches
				run_cgroup "$cap" timed_scan "$kind" "$pfx.cap$cap.out"
				# The two tools name their fields differently.
				if [ "$kind" = trace ]; then
					entries=$(value_from replayed "$pfx.cap$cap.out" | tail -1)
					secs=$(value_from elapsed "$pfx.cap$cap.out" | tail -1)
					rate=$(value_from replay_ops_per_sec "$pfx.cap$cap.out" | tail -1)
				else
					entries=$(value_from entries "$pfx.cap$cap.out" | tail -1)
					secs=$(value_from seconds "$pfx.cap$cap.out" | tail -1)
					rate=$(value_from ops_per_sec "$pfx.cap$cap.out" | tail -1)
				fi
				printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
					"$rep" "$label" "$kind" "$fs" "$cap" \
					"$entries" "$secs" "$rate" \
					"$roots" "$manual" "$bounds" >> "$OUT"
			done
			cleanup
		done
	done
done

echo "results: $OUT" >&2
echo "raw: $RAW" >&2
