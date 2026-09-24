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
TMP=$(mktemp -d /tmp/splinefs-churn.XXXXXX)
IMAGE=$TMP/fs.img
MNT=$TMP/mnt
mkdir "$MNT"
truncate -s 384M "$IMAGE"
mkfs.ext4 -q -F -O ^has_journal "$IMAGE"
sudo insmod "$ROOT/src/jbd3/jbd3.ko"
sudo insmod "$ROOT/src/ext5/ext5.ko" li_policy_mode=1 \
	li_size_floor_entries=64 li_quiet_ops=50 li_buildcost_permille=100 \
	li_stats_enabled=Y
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
sudo chown "$(id -u):$(id -g)" "$MNT"
mkdir -p "$MNT/tree/d0" "$MNT/tree/d1"
for d in 0 1; do for i in $(seq 0 39); do touch "$MNT/tree/d$d/base.$i"; done; done

for i in $(seq 0 299); do
	d=$((i % 2))
	touch "$MNT/tree/d$d/churn.$i"
	rm "$MNT/tree/d$d/churn.$i"
	stat "$MNT/tree/d$d/missing.$i" >/dev/null 2>&1 || true
done
"$ROOT/utils/bin/ext5_li_wait" "$MNT"
ROOTS=$(sudo awk '$1=="live_roots" {print $2}' /sys/kernel/debug/ext5/li_stats)
MANUAL=$(sudo awk '$1=="manual_operations" {print $2}' /sys/kernel/debug/ext5/li_stats)
[ "$ROOTS" -eq 0 ]
[ "$MANUAL" -eq 0 ]
if "$ROOT/utils/bin/ext5_li_info" "$MNT/tree" >/dev/null 2>&1; then
	echo "continuously churned tree unexpectedly promoted" >&2
	exit 1
fi
stat "$MNT/tree/d1/base.23" >/dev/null

sudo umount "$MNT"
sudo rmmod ext5
sudo rmmod jbd3
rm "$IMAGE"
rmdir "$MNT" "$TMP"
trap - EXIT
echo "SplineFS full-churn fallback smoke: PASS"
