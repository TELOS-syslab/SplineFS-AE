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
	[ -z "${TMP:-}" ] || [ ! -d "$TMP" ] || rm -f "$TMP"/*.csv
	[ -z "${MNT:-}" ] || [ ! -d "$MNT" ] || rmdir "$MNT" 2>/dev/null || true
	[ -z "${TMP:-}" ] || [ ! -d "$TMP" ] || rmdir "$TMP" 2>/dev/null || true
}
trap cleanup EXIT

sudo -n true
! findmnt -rn -t ext5 | grep -q . || exit 1
TMP=$(mktemp -d /tmp/splinefs-branch-demote.XXXXXX)
IMAGE=$TMP/fs.img
MNT=$TMP/mnt
mkdir "$MNT"
truncate -s 1G "$IMAGE"
mkfs.ext4 -q -F -O ^has_journal "$IMAGE"
sudo insmod "$ROOT/src/jbd3/jbd3.ko"
sudo insmod "$ROOT/src/ext5/ext5.ko" li_policy_mode=1 \
	li_debug_controls=Y li_demote_refills=2 \
	li_demote_idle_ms=50 \
	li_delta_trigger_bytes=65536 li_quiet_ops=5000 \
	li_buildcost_permille=1 li_stats_enabled=Y
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
sudo chown "$(id -u):$(id -g)" "$MNT"

mkdir -p "$MNT/tree/hot/deep" "$MNT/tree/quiet"
for i in $(seq 0 127); do
	touch "$MNT/tree/hot/base.$i" "$MNT/tree/quiet/base.$i"
done
touch "$MNT/tree/hot/deep/kept"
"$ROOT/utils/bin/ext5_promote_dir" "$MNT/tree"

# Two consecutive rapid-growth strikes localized to hot select its PLID rather
# than tearing down the whole stable root.
"$ROOT/utils/bin/delta_bench" fill "$MNT/tree/hot" 0 1100 >/dev/null
for attempt in $(seq 1 100); do
	"$ROOT/utils/bin/ext5_li_wait" "$MNT"
	[ "$(sudo awk '$1=="compaction_publications" {print $2}' \
		/sys/kernel/debug/ext5/li_stats)" -ge 1 ] && break
	sleep 0.01
done
"$ROOT/utils/bin/delta_bench" fill "$MNT/tree/hot" 1100 1100 >/dev/null
for attempt in $(seq 1 100); do
	"$ROOT/utils/bin/ext5_li_wait" "$MNT"
	[ "$(sudo awk '$1=="branch_demotion_publications" {print $2}' \
		/sys/kernel/debug/ext5/li_stats)" -ge 1 ] && break
	sleep 0.01
done
"$ROOT/utils/bin/ext5_li_walk" --csv "$MNT/tree" > "$TMP/after.csv"
awk -F, -v p="$MNT/tree" '$1==p && $2=="ROOT" {ok=1} END {exit !ok}' \
	"$TMP/after.csv"
awk -F, -v p="$MNT/tree/hot" '$1==p && $2=="MUTABLE" {ok=1} END {exit !ok}' \
	"$TMP/after.csv"
awk -F, -v p="$MNT/tree/hot/deep" '$1==p && $2=="MUTABLE" {ok=1} END {exit !ok}' \
	"$TMP/after.csv"
awk -F, -v p="$MNT/tree/quiet" '$1==p && $2=="INTERIOR" {ok=1} END {exit !ok}' \
	"$TMP/after.csv"
[ "$(sudo awk '$1=="live_roots" {print $2}' /sys/kernel/debug/ext5/li_stats)" -eq 1 ]
[ "$(sudo awk '$1=="live_interior_dirs" {print $2}' /sys/kernel/debug/ext5/li_stats)" -eq 1 ]
stat "$MNT/tree/hot/delta.00000000000000002199" "$MNT/tree/hot/deep/kept" \
	"$MNT/tree/quiet/base.91" >/dev/null

# The next compaction retains the mutable branch boundary slot but removes its
# unreachable old PLID ranges from the learned base.
"$ROOT/utils/bin/ext5_compact" "$MNT/tree"
entries=$("$ROOT/utils/bin/ext5_li_info" "$MNT/tree" |
	awk '$1=="entry_count:" {print $2}')
[ "$entries" -lt 256 ]

sudo umount "$MNT"
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
stat "$MNT/tree/hot/delta.00000000000000002199" "$MNT/tree/hot/deep/kept" \
	"$MNT/tree/quiet/base.91" >/dev/null
"$ROOT/utils/bin/ext5_demote_dir" "$MNT/tree"
stat "$MNT/tree/hot/delta.00000000000000002199" "$MNT/tree/hot/deep/kept" \
	"$MNT/tree/quiet/base.91" >/dev/null
[ "$(sudo awk '$1=="live_roots" {print $2}' /sys/kernel/debug/ext5/li_stats)" -eq 0 ]
[ "$(sudo awk '$1=="live_interior_dirs" {print $2}' /sys/kernel/debug/ext5/li_stats)" -eq 0 ]

sudo umount "$MNT"
sudo rmmod ext5
sudo rmmod jbd3
rm "$IMAGE" "$TMP/after.csv"
rmdir "$MNT" "$TMP"
trap - EXIT
echo "SplineFS lazy branch demotion/dead-PLID compaction: PASS"
