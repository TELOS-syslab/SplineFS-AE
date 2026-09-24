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
TMP=$(mktemp -d /tmp/splinefs-shrink.XXXXXX)
IMAGE=$TMP/fs.img
MNT=$TMP/mnt
mkdir "$MNT"
truncate -s 512M "$IMAGE"
mkfs.ext4 -q -F -O ^has_journal "$IMAGE"
sudo insmod "$ROOT/src/jbd3/jbd3.ko"
sudo insmod "$ROOT/src/ext5/ext5.ko" li_policy_mode=0 \
	li_debug_controls=Y li_bloom_enabled=Y li_stats_enabled=Y
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
sudo chown "$(id -u):$(id -g)" "$MNT"
mkdir "$MNT/tree"
for i in $(seq 0 1023); do touch "$MNT/tree/f.$i"; done
"$ROOT/utils/bin/ext5_promote_dir" "$MNT/tree"
"$ROOT/utils/bin/ext5_li_info" "$MNT/tree" >/dev/null
[ ! -e "$MNT/tree/definitely.missing" ]
before=$(sudo awk '$1=="stable_blob_parses" {print $2}' \
	/sys/kernel/debug/ext5/li_stats)
sudo sh -c 'echo 2 > /proc/sys/vm/drop_caches'
stat "$MNT/tree/f.777" >/dev/null
after=$(sudo awk '$1=="stable_blob_parses" {print $2}' \
	/sys/kernel/debug/ext5/li_stats)
[ "$after" -gt "$before" ]

sudo umount "$MNT"
sudo rmmod ext5
sudo rmmod jbd3
rm "$IMAGE"
rmdir "$MNT" "$TMP"
trap - EXIT
echo "SplineFS navigation-cache reclaim smoke: PASS"
