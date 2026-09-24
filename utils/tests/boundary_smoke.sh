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
TMP=$(mktemp -d /tmp/splinefs-boundary.XXXXXX)
IMAGE=$TMP/fs.img
MNT=$TMP/mnt
mkdir "$MNT"
truncate -s 512M "$IMAGE"
mkfs.ext4 -q -F -O ^has_journal "$IMAGE"
sudo insmod "$ROOT/src/jbd3/jbd3.ko"
sudo insmod "$ROOT/src/ext5/ext5.ko" li_policy_mode=1 \
	li_size_floor_entries=64 li_quiet_ops=100 \
	li_buildcost_permille=1 li_stats_enabled=Y
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
sudo chown "$(id -u):$(id -g)" "$MNT"

mkdir -p "$MNT/tree/a" "$MNT/tree/hot"
for i in $(seq 0 79); do touch "$MNT/tree/a/f.$i"; done

# Keep the common ancestor active while A accumulates a quiet interval.  A and
# the common ancestor are both size-armed; only A may qualify in this phase.
for i in $(seq 0 199); do
	stat "$MNT/tree/a/quiet-miss.$i" >/dev/null 2>&1 || true
	touch "$MNT/tree/hot/churn.$i"
	rm "$MNT/tree/hot/churn.$i"
done
"$ROOT/utils/bin/ext5_li_wait" "$MNT"
"$ROOT/utils/bin/ext5_li_walk" --csv "$MNT/tree" > "$TMP/first.csv"
awk -F, -v p="$MNT/tree/a" '$1==p && $2=="ROOT" {ok=1} END {exit !ok}' \
	"$TMP/first.csv"
awk -F, -v p="$MNT/tree" '$1==p && $2=="MUTABLE" {ok=1} END {exit !ok}' \
	"$TMP/first.csv"

# Once the hot branch also goes quiet, dcache-miss lookups through the stable
# child evaluate the mutable ancestor.  The highest quiet boundary absorbs A.
for i in $(seq 0 399); do
	stat "$MNT/tree/a/whole-quiet-miss.$i" >/dev/null 2>&1 || true
done
"$ROOT/utils/bin/ext5_li_wait" "$MNT"
"$ROOT/utils/bin/ext5_li_walk" --csv "$MNT/tree" > "$TMP/second.csv"
awk -F, -v p="$MNT/tree" '$1==p && $2=="ROOT" {ok=1} END {exit !ok}' \
	"$TMP/second.csv"
awk -F, -v p="$MNT/tree/a" '$1==p && $2=="INTERIOR" {ok=1} END {exit !ok}' \
	"$TMP/second.csv"
[ "$(awk -F, '$2=="ROOT" {n++} END {print n+0}' "$TMP/second.csv")" -eq 1 ]
[ "$(sudo awk '$1=="manual_operations" {print $2}' \
	/sys/kernel/debug/ext5/li_stats)" -eq 0 ]

sudo umount "$MNT"
sudo rmmod ext5
sudo rmmod jbd3
rm "$IMAGE" "$TMP/first.csv" "$TMP/second.csv"
rmdir "$MNT" "$TMP"
trap - EXIT
echo "SplineFS adaptive boundary climb/absorption: PASS"
