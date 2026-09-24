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
TMP=$(mktemp -d /tmp/splinefs-policy-reclaim.XXXXXX)
IMAGE=$TMP/fs.img
MNT=$TMP/mnt
mkdir "$MNT"
truncate -s 256M "$IMAGE"
mkfs.ext4 -q -F -O ^has_journal "$IMAGE"
sudo insmod "$ROOT/src/jbd3/jbd3.ko"
sudo insmod "$ROOT/src/ext5/ext5.ko" li_policy_mode=1 \
	li_size_floor_entries=64 li_quiet_ops=100 \
	li_buildcost_permille=1 li_stats_enabled=Y
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
sudo chown "$(id -u):$(id -g)" "$MNT"

mkdir "$MNT/tree"
for i in $(seq 0 127); do touch "$MNT/tree/base.$i"; done

# Reclaim dentries/inodes after the size crossing. Mount-lifetime policy
# records must restore the candidate's count, armed state, and last mutation.
sync
echo 2 | sudo tee /proc/sys/vm/drop_caches >/dev/null
LOOKUP_BENCH_MISS_BASE=100000 "$ROOT/utils/bin/lookup_bench" run \
	"$MNT/tree" negative-unique 400 128 > /dev/null
"$ROOT/utils/bin/ext5_li_wait" "$MNT"
"$ROOT/utils/bin/ext5_li_info" "$MNT/tree" >/dev/null
[ "$(sudo awk '$1=="auto_promote_run" {print $2}' \
	/sys/kernel/debug/ext5/li_stats)" -eq 1 ]

sudo umount "$MNT"
sudo rmmod ext5
sudo rmmod jbd3
rm "$IMAGE"
rmdir "$MNT" "$TMP"
trap - EXIT
echo "SplineFS mutable policy evidence survives inode reclaim: PASS"
