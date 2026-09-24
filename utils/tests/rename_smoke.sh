#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TMP=
IMAGE=
MNT=

cleanup()
{
	set +e
	if [ -n "${MNT:-}" ] && mountpoint -q "$MNT"; then sudo umount "$MNT"; fi
	sudo rmmod ext5 2>/dev/null || true
	sudo rmmod jbd3 2>/dev/null || true
	[ -z "${IMAGE:-}" ] || [ ! -f "$IMAGE" ] || rm "$IMAGE"
	[ -z "${MNT:-}" ] || [ ! -d "$MNT" ] || rmdir "$MNT" 2>/dev/null || true
	[ -z "${TMP:-}" ] || [ ! -d "$TMP" ] || rmdir "$TMP" 2>/dev/null || true
}
trap cleanup EXIT

sudo -n true
! findmnt -rn -t ext5 | grep -q . || exit 1
TMP=$(mktemp -d /tmp/splinefs-rename.XXXXXX)
IMAGE=$TMP/fs.img
MNT=$TMP/mnt
mkdir "$MNT"
truncate -s 512M "$IMAGE"
mkfs.ext4 -q -F -O ^has_journal "$IMAGE"
sudo insmod "$ROOT/src/jbd3/jbd3.ko"
sudo insmod "$ROOT/src/ext5/ext5.ko" li_policy_mode=0 \
	li_debug_controls=Y li_stats_enabled=Y
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
sudo chown "$(id -u):$(id -g)" "$MNT"

mkdir -p "$MNT/r1/a/sub/deep" "$MNT/r1/a/moving" \
	"$MNT/r1/a/pending" "$MNT/r1/b" "$MNT/r2/c" \
	"$MNT/r1/a/cross.src" "$MNT/r1/b/cross.dst" \
	"$MNT/r1/a/same.src" "$MNT/r1/a/same.dst"
printf payload > "$MNT/r1/a/sub/file"
printf nested > "$MNT/r1/a/sub/deep/file"
printf payload > "$MNT/r1/a/moving/payload"
printf old > "$MNT/r1/source"
printf target > "$MNT/r1/destination"
printf cross-file > "$MNT/r1/cross-file"
printf payload > "$MNT/r1/race-file"
# Make the pending-route interval long enough for a second syscall from the
# same shell to race it without a test-only kernel delay.
"$ROOT/utils/bin/delta_bench" fill "$MNT/r1/a/pending" 0 16384 >/dev/null
"$ROOT/utils/bin/ext5_promote_dir" "$MNT/r1"
"$ROOT/utils/bin/ext5_promote_dir" "$MNT/r2"
# Keep one moved-branch entry delta-only so selective capture exercises both
# the immutable per-PLID range and the exact delta-only INSERT path.
printf delta > "$MNT/r1/a/moving/delta-file"

# VFS parent locks make the same-generation INSERT-then-DELETE sequence
# indivisible to namespace lookup.  Race a stable-interior directory between
# parents while readers resolve a file below it.
"$ROOT/utils/tests/rename_race" "$MNT/r1" 2000

# Directory replacement has three distinct link-count cases. Cross-parent
# replacement removes one link from the source parent and adds no net link to
# the already-occupied destination; same-parent replacement turns two child
# directories into one.
a_links=$(stat -c %h "$MNT/r1/a")
b_links=$(stat -c %h "$MNT/r1/b")
mv -fT "$MNT/r1/a/cross.src" "$MNT/r1/b/cross.dst"
[ "$(stat -c %h "$MNT/r1/a")" -eq $((a_links - 1)) ]
[ "$(stat -c %h "$MNT/r1/b")" -eq "$b_links" ]
a_links=$(stat -c %h "$MNT/r1/a")
mv -fT "$MNT/r1/a/same.src" "$MNT/r1/a/same.dst"
[ "$(stat -c %h "$MNT/r1/a")" -eq $((a_links - 1)) ]

# Same-root cross-parent directory move.
mv "$MNT/r1/a/sub" "$MNT/r1/b/sub"
[ "$(sed -n '1p' "$MNT/r1/b/sub/file")" = payload ]
# POSIX rename replacement in a stable destination.
mv -f "$MNT/r1/source" "$MNT/r1/destination"
[ "$(sed -n '1p' "$MNT/r1/destination")" = old ]
# A regular file has no descendant PLIDs, so a cross-root rename is two delta
# records and leaves both learned roots installed.
mv "$MNT/r1/cross-file" "$MNT/r2/cross-file"
[ "$(sed -n '1p' "$MNT/r2/cross-file")" = cross-file ]
"$ROOT/utils/bin/ext5_li_info" "$MNT/r1" >/dev/null
"$ROOT/utils/bin/ext5_li_info" "$MNT/r2" >/dev/null
"$ROOT/utils/tests/cross_root_file_race" "$MNT/r1" "$MNT/r2" 2000
"$ROOT/utils/bin/ext5_li_info" "$MNT/r1" >/dev/null
"$ROOT/utils/bin/ext5_li_info" "$MNT/r2" >/dev/null
# Cross-root directory rename commits the two parent deltas immediately while
# the moved inode temporarily retains its source-model route. The branch then
# becomes conventional in the worker.
mv "$MNT/r1/b/sub" "$MNT/r2/c/sub"
[ ! -e "$MNT/r1/b/sub" ]
[ "$(sed -n '1p' "$MNT/r2/c/sub/file")" = payload ]
[ "$(sed -n '1p' "$MNT/r2/c/sub/deep/file")" = nested ]
"$ROOT/utils/bin/ext5_li_wait" "$MNT"
[ "$(sed -n '1p' "$MNT/r2/cross-file")" = cross-file ]
[ "$(sed -n '1p' "$MNT/r1/race-file")" = payload ]
[ "$(sudo awk '$1=="branch_demotion_publications" {print $2}' \
	/sys/kernel/debug/ext5/li_stats)" -eq 1 ]
[ "$(sudo awk '$1=="full_demotion_publications" {print $2}' \
	/sys/kernel/debug/ext5/li_stats)" -eq 0 ]
"$ROOT/utils/bin/ext5_li_info" "$MNT/r1" >/dev/null
"$ROOT/utils/bin/ext5_li_info" "$MNT/r2" >/dev/null
"$ROOT/utils/bin/ext5_li_walk" --csv "$MNT" > "$TMP/after-cross.csv"
awk -F, -v p="$MNT/r2/c/sub" '$1==p && $2=="MUTABLE" {ok=1}
	END {exit !ok}' "$TMP/after-cross.csv"
[ "$(stat -c %i "$MNT/r2/c")" = "$(stat -c %i "$MNT/r2/c/sub/..")" ]
[ "$(sed -n '1p' "$MNT/r1/a/moving/payload")" = payload ]

# Mutate the foreign destination parent while a second asynchronous branch
# conversion runs. The moved directory must remain reachable through its old
# learned route until the HTree is published.
mv "$MNT/r1/a/moving" "$MNT/r2/c/moving"
for i in $(seq 0 31); do
	touch "$MNT/r2/c/conflict.$i"
done
[ ! -e "$MNT/r1/a/moving" ]
[ "$(sed -n '1p' "$MNT/r2/c/moving/payload")" = payload ]
"$ROOT/utils/bin/ext5_li_wait" "$MNT"
[ "$(sed -n '1p' "$MNT/r2/c/moving/payload")" = payload ]
[ "$(sed -n '1p' "$MNT/r2/c/moving/delta-file")" = delta ]
[ "$(sudo awk '$1=="branch_demotion_publications" {print $2}' \
	/sys/kernel/debug/ext5/li_stats)" -eq 2 ]

# A second ownership change cannot overtake an active routing handoff. The
# initial large-branch rename succeeds, an immediate second rename sees
# EAGAIN with the committed name intact, and the same operation succeeds after
# the selective conversion publishes.
"$ROOT/utils/bin/rename_bench" once "$MNT/r1/a/pending" \
	"$MNT/r2/c/pending" > "$TMP/pending-first.out"
grep -qx 'result=success' "$TMP/pending-first.out"
"$ROOT/utils/bin/rename_bench" once "$MNT/r2/c/pending" \
	"$MNT/r1/a/pending2" > "$TMP/pending-second.out"
grep -qx 'result=eagain' "$TMP/pending-second.out"
grep -qx 'errno=11' "$TMP/pending-second.out"
[ -e "$MNT/r2/c/pending/delta.00000000000000000000" ]
[ ! -e "$MNT/r1/a/pending2" ]
"$ROOT/utils/bin/ext5_li_wait" "$MNT"
"$ROOT/utils/bin/rename_bench" once "$MNT/r2/c/pending" \
	"$MNT/r1/a/pending2" > "$TMP/pending-final.out"
grep -qx 'result=success' "$TMP/pending-final.out"
[ -e "$MNT/r1/a/pending2/delta.00000000000000000000" ]
[ "$(sudo awk '$1=="branch_demotion_publications" {print $2}' \
	/sys/kernel/debug/ext5/li_stats)" -eq 3 ]

sudo umount "$MNT"
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
[ "$(sed -n '1p' "$MNT/r2/c/sub/file")" = payload ]
[ "$(sed -n '1p' "$MNT/r2/c/sub/deep/file")" = nested ]
[ "$(sed -n '1p' "$MNT/r2/c/moving/payload")" = payload ]
[ "$(sed -n '1p' "$MNT/r2/c/moving/delta-file")" = delta ]
[ "$(sed -n '1p' "$MNT/r2/cross-file")" = cross-file ]
[ -e "$MNT/r1/a/pending2/delta.00000000000000000000" ]
[ "$(sed -n '1p' "$MNT/r1/destination")" = old ]
[ ! -e "$MNT/r1/a/sub" ]
[ -d "$MNT/r1/b/cross.dst" ]
[ ! -e "$MNT/r1/a/cross.src" ]
[ -d "$MNT/r1/a/same.dst" ]
[ ! -e "$MNT/r1/a/same.src" ]

sudo umount "$MNT"
sudo rmmod ext5
sudo rmmod jbd3
rm "$IMAGE" "$TMP/after-cross.csv" "$TMP/pending-first.out" \
	"$TMP/pending-second.out" "$TMP/pending-final.out"
rmdir "$MNT" "$TMP"
trap - EXIT
echo "SplineFS rename smoke: PASS"
