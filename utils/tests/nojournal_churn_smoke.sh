#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Exercise inverse-tail cancellation and the safe no-journal allocation cursor
# across more temporary inodes than one block group can hold.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PAIRS=${PAIRS:-20000}
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

info_value()
{
	local key=$1

	"$ROOT/utils/bin/ext5_li_info" "$MNT/tree" |
		awk -v key="$key" '$1==key":" {print $2}'
}

sudo -n true
! findmnt -rn -t ext5 | grep -q .
TMP=$(mktemp -d /tmp/splinefs-nojournal-churn.XXXXXX)
IMAGE=$TMP/fs.img
MNT=$TMP/mnt
mkdir "$MNT"
truncate -s 2G "$IMAGE"
mkfs.ext4 -q -F -E lazy_itable_init=0 -O ^has_journal "$IMAGE"
sudo insmod "$ROOT/src/jbd3/jbd3.ko"
sudo insmod "$ROOT/src/ext5/ext5.ko" li_policy_mode=0 \
	li_debug_controls=Y li_stats_enabled=N li_demote_refills=0
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
sudo chown "$(id -u):$(id -g)" "$MNT"

"$ROOT/utils/bin/lookup_bench" prepare "$MNT/tree" 4096
"$ROOT/utils/bin/ext5_promote_dir" "$MNT/tree"
"$ROOT/utils/bin/delta_bench" temp "$MNT/tree" 0 "$PAIRS" >/dev/null
"$ROOT/utils/bin/ext5_li_wait" "$MNT/tree"
[ "$(info_value delta_used)" -eq 0 ]
[ "$(info_value delta_records)" -eq 0 ]
[ "$(sudo awk '$1=="compaction_publications" {print $2}' \
	/sys/kernel/debug/ext5/li_stats)" -eq 0 ]

sudo umount "$MNT"
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
stat "$MNT/tree/item.00000000000000000000" >/dev/null
[ ! -e "$MNT/tree/temp.000000000000000000000" ]
[ "$(info_value delta_used)" -eq 0 ]
"$ROOT/utils/bin/ext5_demote_dir" "$MNT/tree"
stat "$MNT/tree/item.00000000000000000000" >/dev/null

sudo umount "$MNT"
sudo rmmod ext5
sudo rmmod jbd3
rm "$IMAGE"
rmdir "$MNT" "$TMP"
trap - EXIT
echo "SplineFS no-journal transient churn: PASS"
