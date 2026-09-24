#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Exercise the bottom-up HTree builder with enough 1 KiB blocks to require an
# indirect index level, then let stock e2fsck validate the demoted layout.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
FILES=${FILES:-50000}
DIR_INDEX=${DIR_INDEX:-1}
TMP=
IMAGE=
MNT=

cleanup()
{
	set +e
	[ -z "${MNT:-}" ] || ! mountpoint -q "$MNT" || sudo umount "$MNT"
	sudo rmmod ext5 2>/dev/null || true
	sudo rmmod jbd3 2>/dev/null || true
	[ -z "${IMAGE:-}" ] || [ ! -f "$IMAGE" ] || rm "$IMAGE"
	[ -z "${MNT:-}" ] || [ ! -d "$MNT" ] || rmdir "$MNT" 2>/dev/null || true
	[ -z "${TMP:-}" ] || [ ! -d "$TMP" ] || rmdir "$TMP" 2>/dev/null || true
}
trap cleanup EXIT

sudo -n true
! findmnt -rn -t ext5 | grep -q .
TMP=$(mktemp -d /tmp/splinefs-bulk-demote.XXXXXX)
IMAGE=$TMP/fs.img
MNT=$TMP/mnt
mkdir "$MNT"
truncate -s 2G "$IMAGE"
if [ "$DIR_INDEX" -eq 1 ]; then
	mkfs.ext4 -q -F -b 1024 -E lazy_itable_init=0 -O ^has_journal "$IMAGE"
else
	mkfs.ext4 -q -F -b 1024 -E lazy_itable_init=0 \
		-O ^has_journal,^dir_index "$IMAGE"
fi
sudo insmod "$ROOT/src/jbd3/jbd3.ko"
sudo insmod "$ROOT/src/ext5/ext5.ko" li_policy_mode=0 \
	li_debug_controls=Y li_stats_enabled=Y
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
sudo chown "$(id -u):$(id -g)" "$MNT"

tree=$MNT/tree
"$ROOT/utils/bin/lookup_bench" prepare "$tree" "$FILES"
"$ROOT/utils/bin/ext5_promote_dir" "$tree"
touch "$tree/delta.extra"
"$ROOT/utils/tests/readdir_cookie" "$tree" \
	"$ROOT/utils/bin/ext5_compact"
# Leave mixed live delta state for demotion itself: one base DELETE, one base
# replacement, and one delta-only INSERT. This exercises both sides of the
# monotonic base/delta merge without an intervening compaction.
rm "$tree/item.00000000000000000017" "$tree/delta.extra"
printf replacement > "$tree/item.00000000000000000017"
printf live-delta > "$tree/delta.live"
"$ROOT/utils/bin/ext5_demote_dir" "$tree"
"$ROOT/utils/bin/lookup_bench" run "$tree" positive-unique \
	"$FILES" "$FILES" 1701 >/dev/null
"$ROOT/utils/bin/lookup_bench" run "$tree" negative-unique \
	"$FILES" "$FILES" 1702 >/dev/null
[ "$(find "$tree" -mindepth 1 -maxdepth 1 -type f | wc -l)" \
	-eq $((FILES + 1)) ]
[ "$(sed -n '1p' "$tree/item.00000000000000000017")" = replacement ]
[ ! -e "$tree/delta.extra" ]
[ "$(sed -n '1p' "$tree/delta.live")" = live-delta ]

sync
sudo umount "$MNT"
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
stat "$tree/item.00000000000000000017" "$tree/delta.live" >/dev/null
[ "$(sed -n '1p' "$tree/item.00000000000000000017")" = replacement ]
[ ! -e "$tree/delta.extra" ]
[ "$(find "$tree" -mindepth 1 -maxdepth 1 -type f | wc -l)" \
	-eq $((FILES + 1)) ]
sudo umount "$MNT"
sudo rmmod ext5
sudo rmmod jbd3

# Full demotion left only conventional directories.  Remove the format guard
# on this disposable image so stock e2fsck can inspect their exact structure.
"$ROOT/utils/tests/clear_lidir_feature" "$IMAGE"
if ! e2fsck -fn "$IMAGE" > "$TMP/e2fsck.out" 2>&1; then
	cat "$TMP/e2fsck.out" >&2
	exit 1
fi

rm "$IMAGE" "$TMP/e2fsck.out"
rmdir "$MNT" "$TMP"
trap - EXIT
echo "SplineFS bulk demotion HTree/e2fsck: PASS"
