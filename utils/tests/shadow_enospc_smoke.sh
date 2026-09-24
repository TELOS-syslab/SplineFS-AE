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
	[ -z "${TMP:-}" ] || [ ! -d "$TMP" ] || rm -f "$TMP"/*.err
	[ -z "${MNT:-}" ] || [ ! -d "$MNT" ] || rmdir "$MNT" 2>/dev/null || true
	[ -z "${TMP:-}" ] || [ ! -d "$TMP" ] || rmdir "$TMP" 2>/dev/null || true
}
trap cleanup EXIT

mode_is()
{
	local expected=$1

	"$ROOT/utils/bin/ext5_li_walk" --csv "$MNT/tree" |
		awk -F, -v p="$MNT/tree" -v m="$expected" \
			'$1==p && $2==m {ok=1} END {exit !ok}'
}

fill_for_shadow_failure()
{
	local avail_k fill_k

	avail_k=$(df -Pk "$MNT" | awk 'NR==2 {print $4}')
	fill_k=$((avail_k - 64))
	[ "$fill_k" -gt 0 ]
	fallocate -l "${fill_k}K" "$MNT/fill"
	sync
}

sudo -n true
! findmnt -rn -t ext5 | grep -q .
TMP=$(mktemp -d /tmp/splinefs-shadow-enospc.XXXXXX)
IMAGE=$TMP/fs.img
MNT=$TMP/mnt
mkdir "$MNT"
truncate -s 512M "$IMAGE"
mkfs.ext4 -q -F -E lazy_itable_init=0 -O ^has_journal "$IMAGE"
sudo insmod "$ROOT/src/jbd3/jbd3.ko"
sudo insmod "$ROOT/src/ext5/ext5.ko" li_policy_mode=0 \
	li_debug_controls=Y li_stats_enabled=Y
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
sudo chown "$(id -u):$(id -g)" "$MNT"

"$ROOT/utils/bin/lookup_bench" prepare "$MNT/tree" 8192
fill_for_shadow_failure
free_before=$(df -Pi "$MNT" | awk 'NR==2 {print $4}')
if "$ROOT/utils/bin/ext5_promote_dir" "$MNT/tree" \
		> /dev/null 2> "$TMP/promote.err"; then
	echo "promotion unexpectedly succeeded with only 64KiB free" >&2
	exit 1
fi
grep -q 'No space left on device' "$TMP/promote.err"
sync
free_after=$(df -Pi "$MNT" | awk 'NR==2 {print $4}')
[ "$free_after" -eq "$free_before" ]
mode_is MUTABLE
stat "$MNT/tree/item.00000000000000008191" >/dev/null

rm "$MNT/fill"
"$ROOT/utils/bin/ext5_promote_dir" "$MNT/tree"
mode_is ROOT
touch "$MNT/tree/compact.seed"
fill_for_shadow_failure
if "$ROOT/utils/bin/ext5_compact" "$MNT/tree" \
		> /dev/null 2> "$TMP/compact.err"; then
	echo "compaction unexpectedly succeeded with only 64KiB free" >&2
	exit 1
fi
grep -q 'No space left on device' "$TMP/compact.err"
mode_is ROOT
stat "$MNT/tree/compact.seed" >/dev/null
rm "$MNT/fill"
"$ROOT/utils/bin/ext5_compact" "$MNT/tree"
mode_is ROOT

fill_for_shadow_failure
free_before=$(df -Pi "$MNT" | awk 'NR==2 {print $4}')
if "$ROOT/utils/bin/ext5_demote_dir" "$MNT/tree" \
		> /dev/null 2> "$TMP/demote.err"; then
	echo "demotion unexpectedly succeeded with only 64KiB free" >&2
	exit 1
fi
grep -q 'No space left on device' "$TMP/demote.err"
sync
free_after=$(df -Pi "$MNT" | awk 'NR==2 {print $4}')
[ "$free_after" -eq "$free_before" ]
mode_is ROOT
stat "$MNT/tree/item.00000000000000008191" >/dev/null

rm "$MNT/fill"
"$ROOT/utils/bin/ext5_demote_dir" "$MNT/tree"
mode_is MUTABLE
sudo umount "$MNT"
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
stat "$MNT/tree/item.00000000000000008191" >/dev/null
mode_is MUTABLE

sudo umount "$MNT"
sudo rmmod ext5
sudo rmmod jbd3
rm "$IMAGE" "$TMP/promote.err" "$TMP/compact.err" "$TMP/demote.err"
rmdir "$MNT" "$TMP"
trap - EXIT
echo "SplineFS shadow ENOSPC abort/reclaim: PASS"
