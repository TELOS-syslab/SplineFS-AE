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
TMP=$(mktemp -d /tmp/splinefs-demote.XXXXXX)
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
mkdir "$MNT/tree"
for i in $(seq 0 255); do touch "$MNT/tree/base.$i"; done
"$ROOT/utils/bin/ext5_promote_dir" "$MNT/tree"
"$ROOT/utils/bin/delta_bench" fill "$MNT/tree" 0 1100 >/dev/null
for attempt in $(seq 1 100); do
	"$ROOT/utils/bin/ext5_li_wait" "$MNT"
	[ "$(sudo awk '$1=="compaction_publications" {print $2}' \
		/sys/kernel/debug/ext5/li_stats)" -ge 1 ] && break
	sleep 0.01
done
[ "$(sudo awk '$1=="compaction_publications" {print $2}' \
	/sys/kernel/debug/ext5/li_stats)" -eq 1 ]
"$ROOT/utils/bin/delta_bench" fill "$MNT/tree" 1100 1100 >/dev/null
for attempt in $(seq 1 100); do
	"$ROOT/utils/bin/ext5_li_wait" "$MNT"
	[ "$(sudo awk '$1=="full_demotion_publications" {print $2}' \
		/sys/kernel/debug/ext5/li_stats)" -ge 1 ] && break
	sleep 0.01
done
roots=$(sudo awk '$1=="live_roots" {print $2}' \
	/sys/kernel/debug/ext5/li_stats)
rapid=$(sudo awk '$1=="rapid_insert_refills" {print $2}' \
	/sys/kernel/debug/ext5/li_stats)
if [ "$roots" -ne 0 ] || [ "$rapid" -ne 2 ]; then
	sudo cat /sys/kernel/debug/ext5/li_stats >&2
	"$ROOT/utils/bin/ext5_li_info" "$MNT/tree" >&2 || true
	exit 1
fi
stat "$MNT/tree/delta.00000000000000002199" >/dev/null

sudo umount "$MNT"
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
stat "$MNT/tree/delta.00000000000000002199" >/dev/null
sudo umount "$MNT"
sudo rmmod ext5
sudo rmmod jbd3
rm "$IMAGE"
rmdir "$MNT" "$TMP"
trap - EXIT
echo "SplineFS automatic churn demotion smoke: PASS"
