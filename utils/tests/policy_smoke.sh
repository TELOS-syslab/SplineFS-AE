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
	[ -z "${IMAGE:-}" ] || [ ! -f "$IMAGE" ] || rm "$IMAGE"
	[ -z "${MNT:-}" ] || [ ! -d "$MNT" ] || rmdir "$MNT" 2>/dev/null || true
	[ -z "${TMP:-}" ] || [ ! -d "$TMP" ] || rmdir "$TMP" 2>/dev/null || true
}
trap cleanup EXIT

sudo -n true
! findmnt -rn -t ext5 | grep -q . || {
	echo "refusing to run while an ext5 filesystem is mounted" >&2
	exit 1
}

TMP=$(mktemp -d /tmp/splinefs-policy-smoke.XXXXXX)
IMAGE=$TMP/fs.img
MNT=$TMP/mnt
mkdir "$MNT"
truncate -s 512M "$IMAGE"
mkfs.ext4 -q -F -O ^has_journal "$IMAGE"

sudo insmod "$ROOT/src/jbd3/jbd3.ko"
sudo insmod "$ROOT/src/ext5/ext5.ko" li_policy_mode=1 \
	li_size_floor_entries=128 li_quiet_ops=100 \
	li_buildcost_permille=100 li_stats_enabled=Y
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
sudo chown "$(id -u):$(id -g)" "$MNT"

mkdir -p "$MNT/tree/d0" "$MNT/tree/d1" "$MNT/tree/d2" "$MNT/tree/d3"
for d in 0 1 2 3; do
	for i in $(seq 0 63); do
		touch "$MNT/tree/d$d/f.$i"
	done
done
for i in $(seq 0 399); do
	stat "$MNT/tree/d$((i % 4))/missing.$i" >/dev/null 2>&1 || true
done
"$ROOT/utils/bin/ext5_li_wait" "$MNT"
"$ROOT/utils/bin/ext5_li_info" "$MNT/tree" >/dev/null

RUNS=$(sudo awk '$1=="auto_promote_run" {print $2}' \
	/sys/kernel/debug/ext5/li_stats)
CALLS=$(sudo awk '$1=="promote_calls" {print $2}' \
	/sys/kernel/debug/ext5/li_stats)
MANUAL=$(sudo awk '$1=="manual_operations" {print $2}' \
	/sys/kernel/debug/ext5/li_stats)
ROOTS=$(sudo awk '$1=="live_roots" {print $2}' \
	/sys/kernel/debug/ext5/li_stats)
[ "$RUNS" -eq 1 ]
[ "$CALLS" -eq 1 ]
[ "$MANUAL" -eq 0 ]
[ "$ROOTS" -eq 1 ]

sudo umount "$MNT"
sudo rmmod ext5
sudo rmmod jbd3
rm "$IMAGE"
rmdir "$MNT" "$TMP"
trap - EXIT
echo "SplineFS autonomous policy smoke: PASS"
