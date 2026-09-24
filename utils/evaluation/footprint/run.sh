#!/usr/bin/env bash
# Fig. 9a: directory metadata of SplineFS, ext4 and a naive per-directory
# index, over synthetic shapes and real trees.  DESTRUCTIVE: formats DEV.
#
# Bytes are st_blocks summed over every directory of the tree.  Synthetic
# shapes hardlink every name into a small inode pool, so only directory
# metadata varies.  naive is the same module with one model per directory.
#
# Environment:
#   DEV, CONFIRM_DESTROY   required
#   SHAPES="K:M ..."       synthetic shapes, K directories of M entries
#   REALDIRS="label:/dir"  real trees
#   LISTS="label:/list"    GUFI path lists
#   PROMOTE=force|auto     force (default): one model at the tree root;
#                          auto: whatever the policy promotes
#   REPS                   default 3
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
AE_ROOT=$(cd "$HERE/../../.." && pwd)
AE_UTILS=$AE_ROOT/utils
export AE_UTILS
. "$HERE/../lib/safety.sh"

DEV=${DEV:-}
MNT=${MNT:-/mnt/splinefs-footprint}
POOL=${POOL:-4096}
FS_BLOCKS=${FS_BLOCKS:-26214400}          # 25 GiB: mke2fs reads it in KiB
SHAPES=${SHAPES:-"1:1000000 10:100000 100:10000 1000:1000 10000:100 100000:10"}
REALDIRS=${REALDIRS:-""}
LISTS=${LISTS:-""}
REPS=${REPS:-3}
SETTLE_OPS=${SETTLE_OPS:-1000000}
PROMOTE=${PROMOTE:-force}
OUT=${OUT:-$HERE/results.csv}
RAW=${RAW:-$HERE/raw/$(date +%Y%m%d-%H%M%S)}

DEBUG_ARG=""
[ "$PROMOTE" = force ] && DEBUG_ARG="li_debug_controls=Y"
PROMOTE_DIR=$AE_UTILS/bin/ext5_promote_dir
BENCH=$AE_UTILS/bin/multidir_lookup_bench
LIINFO=$AE_UTILS/bin/ext5_li_info
LIWAIT=$AE_UTILS/bin/ext5_li_wait
STATS=/sys/kernel/debug/ext5/li_stats
TREE=$MNT/tree

eval_require_device

# The lookup verbs hold one fd per directory: raise the limit to the largest K.
max_k=0
for _shape in $SHAPES; do
	_k=${_shape%%:*}
	[ "$_k" -gt "$max_k" ] 2>/dev/null && max_k=$_k
done
needed_fds=$((max_k + 4096))
[ "$(ulimit -Hn)" -ge "$needed_fds" ] ||
	eval_die "hard fd limit $(ulimit -Hn) is below the required $needed_fds"
ulimit -n "$needed_fds"

mkdir -p "$MNT" "$RAW" "$(dirname "$OUT")"

teardown() {
	# Restore the caller's errexit setting.
	local _restore=+e
	case $- in *e*) _restore=-e ;; esac
	set +e
	cd /
	mountpoint -q "$MNT" && umount "$MNT" 2>/dev/null
	rmmod ext5 2>/dev/null
	rmmod jbd3 2>/dev/null
	set "$_restore"
}
trap teardown EXIT

mkfs_dev() {
	mkfs.ext4 -qF -E nodiscard,lazy_itable_init=1 \
		-O ^has_journal,large_dir -N 4194304 "$DEV" "$(eval_fs_blocks "$FS_BLOCKS")" \
		>/dev/null 2>&1
}

mount_case() {
	teardown
	mkfs_dev
	case "$1" in
	splinefs)
		insmod "$AE_ROOT/src/jbd3/jbd3.ko"
		insmod "$AE_ROOT/src/ext5/ext5.ko" $DEBUG_ARG
		mount -t ext5 -o noatime "$DEV" "$MNT" ;;
	naive)
		# One model per directory (policy mode 2, no subsume), no delta buffer.
		insmod "$AE_ROOT/src/jbd3/jbd3.ko"
		insmod "$AE_ROOT/src/ext5/ext5.ko" $DEBUG_ARG \
			li_policy_mode=2 li_promote_subsume=0 \
			li_promote_delta_bytes=0
		mount -t ext5 -o noatime "$DEV" "$MNT" ;;
	ext4)
		mount -t ext4 -o noatime "$DEV" "$MNT" ;;
	*) eval_die "unknown filesystem $1" ;;
	esac
	eval_require_mounted "$MNT"
}

# Allocated bytes of every directory under $1.  A promoted subtree's index
# lives in its root directory's blocks, so it is counted.
dir_alloc_bytes() {
	find "$1" -type d -printf '%b\n' 2>/dev/null |
		awk '{s+=$1} END {printf "%.0f", s*512}'
}

li_field() { awk -v k="$1:" '$1==k {print $2}' "$2"; }

# A real tree is rebuilt from one listing of its source (names, types, link
# targets; empty files).  Directory metadata does not depend on contents, and
# in the VM the source is on 9p, where copying file by file is slow.
list_tree() { # list_tree <src> <out>
	[ -s "$2" ] || (cd "$1" && find . -mindepth 1 -printf '%y\0%P\0%l\0') > "$2"
}
make_tree() { # make_tree <listing> <dest>
	python3 - "$1" "$2" <<'PY'
import os, sys
root = os.fsencode(sys.argv[2])
f = open(sys.argv[1], "rb").read().split(b"\0")
for i in range(0, len(f) - 2, 3):
    kind, path, target = f[i], os.path.join(root, f[i + 1]), f[i + 2]
    if kind == b"d":
        os.mkdir(path)
    elif kind == b"l":
        os.symlink(target, path)
    else:
        open(path, "wb").close()
PY
}

build_tree() { # build_tree <K> <M> <realsrc> <listfile>
	local K=$1 M=$2 src=$3 list=$4
	if [ -n "$list" ]; then
		"$BENCH" preplist "$TREE" "$list" "$POOL" >/dev/null
	elif [ -n "$src" ]; then
		mkdir -p "$TREE"
		list_tree "$src" "$RAW/$(basename "$src").listing"
		make_tree "$RAW/$(basename "$src").listing" "$TREE"
	else
		"$BENCH" preplink "$TREE" "$K" "$M" "$POOL" >/dev/null
	fi
}

# Promote the tree: by hand (PROMOTE=force) or by the policy.
settle_policy() { # settle_policy <arm> <K> <M> <rep> <logprefix> <listfile>
	local arm=$1 K=$2 M=$3 rep=$4 pfx=$5 list=${6:-}
	if [ "$PROMOTE" = force ] && [ "$arm" = naive ]; then
		# One model per directory: promote depth-first, so each directory is a root
		# before its parent is walked, and subsume=0 keeps it as a boundary.
		find "$TREE" -depth -type d -print |
			"$PROMOTE_DIR" - > "$pfx.force" 2>&1 ||
			echo "WARN: per-directory promote failed for $TREE" >&2
	elif [ "$PROMOTE" = force ]; then
		# One model over the whole subtree.
		"$PROMOTE_DIR" "$TREE" > "$pfx.force" 2>&1 ||
			echo "WARN: force promote failed for $TREE" >&2
	elif [ -n "$list" ]; then
		"$BENCH" runlistneg "$TREE" "$list" "$SETTLE_OPS" \
			$((rep * 7919 + 17)) > "$pfx.settle" 2>&1
	elif [ "$K" -gt 0 ]; then
		"$BENCH" runneguniq "$TREE" "$K" "$M" "$SETTLE_OPS" \
			$((rep * 7919 + 17)) > "$pfx.settle" 2>&1
	else
		# The policy clock advances only on dcache misses, so drop dentries and
		# inodes before each walk.
		local pass
		for pass in 1 2 3; do
			sync
			echo 2 > /proc/sys/vm/drop_caches
			find "$TREE" -mindepth 1 -printf '' 2>/dev/null
		done
	fi
	"$LIWAIT" "$TREE" >/dev/null 2>&1 || true
}

measure_one() { # measure_one <rep> <label> <K> <M> <realsrc> <listfile>
	local rep=$1 label=$2 K=$3 M=$4 src=$5 list=${6:-}
	local pfx="$RAW/$label.r$rep"
	local sfs naive e4 base nav names entries manual bounds roots ratio promoted
	local walkcsv nroots promoted_entries tree_entries coverage

	mount_case splinefs
	build_tree "$K" "$M" "$src" "$list"
	settle_policy splinefs "$K" "$M" "$rep" "$pfx.sfs" "$list"
	# Sum coverage over every root the policy chose.  No early awk exit:
	# SIGPIPE under pipefail aborts the run on large trees.
	walkcsv="$pfx.sfs.walk"
	"$AE_UTILS/bin/ext5_li_walk" --csv "$TREE" > "$walkcsv" 2>/dev/null || true
	nroots=$(awk -F, '$2=="ROOT"' "$walkcsv" | wc -l)
	# ext5_li_walk leaves entry_count blank for unreadable roots; li_stats has the total.
	promoted_entries=$(awk '$1=="live_promoted_entries" {print $2}' \
		"$STATS" 2>/dev/null || echo 0)
	promoted=$(awk -F, '$2=="ROOT" && !seen {print $1; seen=1}' "$walkcsv") || promoted=""
	if [ -n "$promoted" ]; then
		"$LIINFO" "$promoted" > "$pfx.sfs.info" 2>&1 || true
	else
		echo "WARN: $label rep$rep: policy promoted nothing under $TREE" >&2
		: > "$pfx.sfs.info"
	fi
	cp "$STATS" "$pfx.sfs.stats" 2>/dev/null || true
	manual=$(awk '$1=="manual_operations" {print $2}' "$STATS" 2>/dev/null || echo NA)
	bounds=$(awk '$1=="rs_bound_violations" {print $2}' "$STATS" 2>/dev/null || echo NA)
	roots=$(awk '$1=="live_roots" {print $2}' "$STATS" 2>/dev/null || echo NA)
	sfs=$(dir_alloc_bytes "$TREE")
	base=$(li_field base_bytes "$pfx.sfs.info"); nav=$(li_field nav_bytes "$pfx.sfs.info")
	names=$(li_field names_bytes "$pfx.sfs.info"); entries=$(li_field entry_count "$pfx.sfs.info")

	mount_case naive
	build_tree "$K" "$M" "$src" "$list"
	settle_policy naive "$K" "$M" "$rep" "$pfx.naive" "$list"
	naive=$(dir_alloc_bytes "$TREE")

	mount_case ext4
	build_tree "$K" "$M" "$src" "$list"
	e4=$(dir_alloc_bytes "$TREE")

	tree_entries=$(find "$TREE" -mindepth 1 2>/dev/null | wc -l)
	coverage=$(awk -v p="${promoted_entries:-0}" -v t="$tree_entries" \
		'BEGIN {printf (t>0 ? "%.3f" : "NA"), p/t}')
	ratio=$(awk -v a="$e4" -v b="$sfs" 'BEGIN {printf (b>0 ? "%.3f" : "NA"), a/b}')
	printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
		"$rep" "$label" "$K" "$M" "${entries:-NA}" \
		"$sfs" "${base:-NA}" "${nav:-NA}" "${names:-NA}" \
		"$naive" "$e4" "$ratio" "${manual:-NA}" "${bounds:-NA}" \
		"${nroots:-0}" "${promoted_entries:-0}" "$tree_entries" "$coverage" >> "$OUT"
	printf '[%s] %-14s rep%s  ext4=%s  sfs=%s  ratio=%sx  coverage=%s (%s roots)\n' \
		"$(date +%T)" "$label" "$rep" "$e4" "$sfs" "$ratio" \
		"$coverage" "${nroots:-0}" >&2
	teardown
}

printf 'rep,label,K,M,entries,sfs_total_bytes,sfs_base_bytes,sfs_nav_bytes,sfs_names_bytes,naive_total_bytes,ext4_total_bytes,ratio_total,manual_operations,bound_violations,promoted_roots,promoted_entries,tree_entries,coverage\n' > "$OUT"

for rep in $(seq 1 "$REPS"); do
	for shape in $SHAPES; do
		measure_one "$rep" "shape_${shape/:/x}" "${shape%%:*}" "${shape##*:}" ""
	done
	for real in $REALDIRS; do
		[ -d "${real##*:}" ] || { echo "skip missing ${real##*:}" >&2; continue; }
		measure_one "$rep" "${real%%:*}" 0 0 "${real##*:}" ""
	done
	for entry in $LISTS; do
		[ -s "${entry##*:}" ] || { echo "skip missing ${entry##*:}" >&2; continue; }
		measure_one "$rep" "${entry%%:*}" 0 0 "" "${entry##*:}"
	done
done

echo "results: $OUT" >&2
echo "raw: $RAW" >&2
