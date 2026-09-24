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
! findmnt -rn -t ext5 | grep -q .
TMP=$(mktemp -d /tmp/splinefs-journal.XXXXXX)
IMAGE=$TMP/fs.img
MNT=$TMP/mnt
mkdir "$MNT"
truncate -s 768M "$IMAGE"
mkfs.ext4 -q -F -E lazy_itable_init=0 "$IMAGE"
sudo insmod "$ROOT/src/jbd3/jbd3.ko"
sudo insmod "$ROOT/src/ext5/ext5.ko" li_policy_mode=0 \
	li_debug_controls=Y li_stats_enabled=Y
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
sudo chown "$(id -u):$(id -g)" "$MNT"

mkdir -p "$MNT/tree/a" "$MNT/tree/b"
for i in $(seq 0 255); do
	touch "$MNT/tree/a/base.$i" "$MNT/tree/b/base.$i"
done
"$ROOT/utils/bin/ext5_promote_dir" "$MNT/tree"

# Each operation uses the host transaction for both namespace metadata and its
# stable delta record.
printf payload > "$MNT/tree/a/new"
ln "$MNT/tree/a/new" "$MNT/tree/a/link"
rm "$MNT/tree/a/link"
mkdir "$MNT/tree/a/empty"
rmdir "$MNT/tree/a/empty"
printf source > "$MNT/tree/a/source"
printf target > "$MNT/tree/b/target"
mv -f "$MNT/tree/a/source" "$MNT/tree/b/target"
[ "$(cat "$MNT/tree/b/target")" = source ]
mv "$MNT/tree/a/base.17" "$MNT/tree/b/moved.17"
[ ! -e "$MNT/tree/a/base.17" ]
stat "$MNT/tree/b/moved.17" >/dev/null

sync
sudo umount "$MNT"
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
[ "$(cat "$MNT/tree/b/target")" = source ]
stat "$MNT/tree/a/new" "$MNT/tree/b/moved.17" >/dev/null
[ ! -e "$MNT/tree/a/base.17" ]

"$ROOT/utils/bin/ext5_compact" "$MNT/tree"
stat "$MNT/tree/a/new" "$MNT/tree/b/moved.17" >/dev/null
"$ROOT/utils/bin/ext5_demote_dir" "$MNT/tree"
stat "$MNT/tree/a/new" "$MNT/tree/b/moved.17" >/dev/null

sudo umount "$MNT"
sudo rmmod ext5
sudo rmmod jbd3
rm "$IMAGE"
rmdir "$MNT" "$TMP"
trap - EXIT
echo "SplineFS journal-coupled delta/namespace lifecycle: PASS"
