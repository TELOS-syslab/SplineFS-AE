#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# A promoted child need not have a cached dentry when an ancestor absorbs it.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
JOURNAL=${JOURNAL:-0}
BLOCK_SIZE=${BLOCK_SIZE:-4096}
sudo -n true
if findmnt -rn -t ext5 | grep -q . || lsmod | grep -qE '^(ext5|jbd3) '; then
	echo "refusing to run while ext5/jbd3 is in use" >&2
	exit 1
fi
TMP=$(mktemp -d /tmp/splinefs-nested-promote.XXXXXX)
IMAGE=$TMP/fs.img
MNT=$TMP/mnt
cleanup()
{
	local status=$?

	set +e
	if mountpoint -q "$MNT"; then sudo umount "$MNT"; fi
	if mountpoint -q "$MNT"; then
		echo "nested promotion artifacts retained: $TMP" >&2
		return
	fi
	sudo rmmod ext5 2>/dev/null
	sudo rmmod jbd3 2>/dev/null
	if [ "$status" -ne 0 ]; then
		echo "nested promotion artifacts retained: $TMP" >&2
		return
	fi
	rm -f "$IMAGE" "$TMP/e2fsck.log"
	rmdir "$MNT" "$TMP"
}
trap cleanup EXIT
mkdir "$MNT"
truncate -s 512M "$IMAGE"
if [ "$JOURNAL" -eq 1 ]; then
	mkfs.ext4 -q -F -b "$BLOCK_SIZE" -E lazy_itable_init=0 "$IMAGE"
else
	mkfs.ext4 -q -F -b "$BLOCK_SIZE" -E lazy_itable_init=0 -O ^has_journal "$IMAGE"
fi
sudo insmod "$ROOT/src/jbd3/jbd3.ko"
sudo insmod "$ROOT/src/ext5/ext5.ko" li_policy_mode=0 li_debug_controls=Y
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
sudo chown "$(id -u):$(id -g)" "$MNT"
mkdir -p "$MNT/tree/learned" "$MNT/tree/mutable"
for i in $(seq 0 127); do
	touch "$MNT/tree/learned/file.$i" "$MNT/tree/mutable/file.$i"
done
"$ROOT/utils/bin/ext5_promote_dir" "$MNT/tree/learned"

# Remount discards every child dentry. Do not traverse the children again
# before the parent ioctl: collection loads the child by inode number only.
sudo umount "$MNT"
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
"$ROOT/utils/bin/ext5_promote_dir" "$MNT/tree"
"$ROOT/utils/bin/ext5_li_info" "$MNT/tree" >/dev/null
for dir in learned mutable; do
	for i in $(seq 0 127); do
		stat "$MNT/tree/$dir/file.$i" >/dev/null
	done
done
"$ROOT/utils/bin/ext5_demote_dir" "$MNT/tree"
sudo umount "$MNT"
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
stat "$MNT/tree/learned/file.127" "$MNT/tree/mutable/file.127" >/dev/null
sudo umount "$MNT"
# In particular, validate the reconstructed '..' inode numbers independently
# of the VFS dentry cache. The whole tree is conventional after demotion.
"$ROOT/utils/tests/clear_lidir_feature" "$IMAGE"
if ! e2fsck -fn "$IMAGE" > "$TMP/e2fsck.log" 2>&1; then
	cat "$TMP/e2fsck.log" >&2
	exit 1
fi
echo "SplineFS nested promotion after dentry reclaim: PASS"
