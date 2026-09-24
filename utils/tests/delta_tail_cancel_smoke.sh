#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Tail cancellation must remove transient names without resurrecting base names.
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

info_value()
{
	local key=$1

	"$ROOT/utils/bin/ext5_li_info" "$MNT/tree" |
		awk -v key="$key" '$1==key":" {print $2}'
}

sudo -n true
! findmnt -rn -t ext5 | grep -q .
TMP=$(mktemp -d /tmp/splinefs-delta-tail-cancel.XXXXXX)
IMAGE=$TMP/fs.img
MNT=$TMP/mnt
mkdir "$MNT"
truncate -s 1G "$IMAGE"
mkfs.ext4 -q -F -E lazy_itable_init=0 "$IMAGE"
sudo insmod "$ROOT/src/jbd3/jbd3.ko"
sudo insmod "$ROOT/src/ext5/ext5.ko" li_policy_mode=0 \
	li_debug_controls=Y li_stats_enabled=Y li_demote_refills=0
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
sudo chown "$(id -u):$(id -g)" "$MNT"

"$ROOT/utils/bin/lookup_bench" prepare "$MNT/tree" 4096
printf base > "$MNT/tree/base-name"
printf source > "$MNT/tree/rename-source"
printf target > "$MNT/tree/rename-target"
"$ROOT/utils/bin/ext5_promote_dir" "$MNT/tree"

# The common create/unlink pair is a physical-tail inverse.  It leaves no live
# override, no log occupancy, and no compaction work to perform.
: > "$MNT/tree/transient"
rm "$MNT/tree/transient"
[ "$(info_value delta_used)" -eq 0 ]
[ "$(info_value delta_records)" -eq 0 ]

# Cancellation never jumps over another record.  Interleaved inverses remain
# ordinary tombstones and are folded by normal compaction.
: > "$MNT/tree/interleave-a"
: > "$MNT/tree/interleave-b"
rm "$MNT/tree/interleave-a" "$MNT/tree/interleave-b"
[ "$(info_value delta_records)" -eq 2 ]
"$ROOT/utils/bin/ext5_compact" "$MNT/tree"
[ "$(info_value delta_used)" -eq 0 ]

# Recreating a deleted base name has prior delta state, so the following unlink
# must retain a tombstone instead of exposing the immutable base entry again.
rm "$MNT/tree/base-name"
: > "$MNT/tree/base-name"
rm "$MNT/tree/base-name"
[ ! -e "$MNT/tree/base-name" ]
[ "$(info_value delta_records)" -eq 1 ]

# A rename replacement is also non-cancellable: deleting the new target later
# must not resurrect the replaced inode from the base generation.
mv -f "$MNT/tree/rename-source" "$MNT/tree/rename-target"
[ "$(cat "$MNT/tree/rename-target")" = source ]
rm "$MNT/tree/rename-target"
[ ! -e "$MNT/tree/rename-target" ]

sudo umount "$MNT"
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
[ ! -e "$MNT/tree/transient" ]
[ ! -e "$MNT/tree/interleave-a" ]
[ ! -e "$MNT/tree/interleave-b" ]
[ ! -e "$MNT/tree/base-name" ]
[ ! -e "$MNT/tree/rename-source" ]
[ ! -e "$MNT/tree/rename-target" ]
stat "$MNT/tree/item.00000000000000000000" >/dev/null

sudo umount "$MNT"
sudo rmmod ext5
sudo rmmod jbd3
rm "$IMAGE"
rmdir "$MNT" "$TMP"
trap - EXIT
echo "SplineFS delta tail cancellation: PASS"
