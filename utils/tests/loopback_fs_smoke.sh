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
	if [ -n "${MNT:-}" ] && mountpoint -q "$MNT"; then
		sudo umount "$MNT"
	fi
	sudo rmmod ext5 2>/dev/null || true
	sudo rmmod jbd3 2>/dev/null || true
	if [ -n "${IMAGE:-}" ] && [ -f "$IMAGE" ]; then
		rm "$IMAGE"
	fi
	if [ -n "${MNT:-}" ] && [ -d "$MNT" ]; then
		rmdir "$MNT" 2>/dev/null || true
	fi
	if [ -n "${TMP:-}" ] && [ -d "$TMP" ]; then
		rmdir "$TMP" 2>/dev/null || true
	fi
}
trap cleanup EXIT

sudo -n true
! findmnt -rn -t ext5 | grep -q . || {
	echo "refusing to run while an ext5 filesystem is mounted" >&2
	exit 1
}

TMP=$(mktemp -d /tmp/splinefs-fs-smoke.XXXXXX)
IMAGE=$TMP/fs.img
MNT=$TMP/mnt
mkdir "$MNT"
truncate -s 768M "$IMAGE"
mkfs.ext4 -q -F -O ^has_journal "$IMAGE"

sudo insmod "$ROOT/src/jbd3/jbd3.ko"
sudo insmod "$ROOT/src/ext5/ext5.ko" li_policy_mode=0 \
	li_debug_controls=Y li_stats_enabled=Y
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
sudo chown "$(id -u):$(id -g)" "$MNT"

mkdir -p "$MNT/tree/a" "$MNT/tree/b" "$MNT/tree/dead"
for i in $(seq 0 511); do
	touch "$MNT/tree/a/file.$i" "$MNT/tree/b/item.$i"
done
"$ROOT/utils/bin/ext5_promote_dir" "$MNT/tree"
"$ROOT/utils/bin/ext5_li_info" "$MNT/tree" | grep -q 'spline_epsilon: 8'
stat "$MNT/tree/a/file.311" >/dev/null

# Core stable-mode namespace operations.
ln "$MNT/tree/a/file.311" "$MNT/tree/a/hardlink.311"
[ "$(stat -c %i "$MNT/tree/a/file.311")" = \
  "$(stat -c %i "$MNT/tree/a/hardlink.311")" ]
ln -s file.311 "$MNT/tree/a/symlink.311"
[ "$(readlink "$MNT/tree/a/symlink.311")" = file.311 ]
mkdir "$MNT/tree/a/newdir"
touch "$MNT/tree/a/newdir/inside"
rm "$MNT/tree/a/newdir/inside"
rmdir "$MNT/tree/a/newdir"
rm "$MNT/tree/a/hardlink.311" "$MNT/tree/a/symlink.311"
# Remove an originally promoted empty interior.  Its now-dead PLID must not
# make later compaction or full demotion require a nonexistent inode.
rmdir "$MNT/tree/dead"

sudo umount "$MNT"
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
stat "$MNT/tree/b/item.407" >/dev/null
touch "$MNT/tree/a/post.remount"
rm "$MNT/tree/b/item.19"
"$ROOT/utils/bin/ext5_compact" "$MNT/tree"
stat "$MNT/tree/a/post.remount" >/dev/null
[ ! -e "$MNT/tree/b/item.19" ]

printf old > "$MNT/tree/a/source"
printf target > "$MNT/tree/a/destination"
mv -f "$MNT/tree/a/source" "$MNT/tree/a/destination"
[ "$(sed -n '1p' "$MNT/tree/a/destination")" = old ]

"$ROOT/utils/bin/ext5_demote_dir" "$MNT/tree"
stat "$MNT/tree/a/file.311" >/dev/null
BOUNDS=$(sudo awk '$1=="rs_bound_violations" {print $2}' \
	/sys/kernel/debug/ext5/li_stats)
[ "$BOUNDS" -eq 0 ]

sudo umount "$MNT"
sudo rmmod ext5
sudo rmmod jbd3
if sudo mount -t ext4 -o loop,noatime "$IMAGE" "$MNT" 2>/dev/null; then
	sudo umount "$MNT"
	echo "stock ext4 accepted a v2-formatted filesystem" >&2
	exit 1
fi
rm "$IMAGE"
rmdir "$MNT" "$TMP"
trap - EXIT
echo "SplineFS loopback filesystem smoke: PASS"
