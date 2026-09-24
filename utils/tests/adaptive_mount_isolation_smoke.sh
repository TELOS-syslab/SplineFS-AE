#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TMP=
MNT_A=
MNT_B=
IMAGE_A=
IMAGE_B=

cleanup()
{
	set +e
	[ -z "${MNT_A:-}" ] || ! mountpoint -q "$MNT_A" || sudo umount "$MNT_A"
	[ -z "${MNT_B:-}" ] || ! mountpoint -q "$MNT_B" || sudo umount "$MNT_B"
	sudo rmmod ext5 2>/dev/null || true
	sudo rmmod jbd3 2>/dev/null || true
	[ -z "${IMAGE_A:-}" ] || [ ! -f "$IMAGE_A" ] || rm "$IMAGE_A"
	[ -z "${IMAGE_B:-}" ] || [ ! -f "$IMAGE_B" ] || rm "$IMAGE_B"
	[ -z "${MNT_A:-}" ] || [ ! -d "$MNT_A" ] || rmdir "$MNT_A" 2>/dev/null || true
	[ -z "${MNT_B:-}" ] || [ ! -d "$MNT_B" ] || rmdir "$MNT_B" 2>/dev/null || true
	[ -z "${TMP:-}" ] || [ ! -d "$TMP" ] || rmdir "$TMP" 2>/dev/null || true
}
trap cleanup EXIT

sudo -n true
! findmnt -rn -t ext5 | grep -q .
TMP=$(mktemp -d /tmp/splinefs-adaptive-mounts.XXXXXX)
MNT_A=$TMP/a
MNT_B=$TMP/b
IMAGE_A=$TMP/a.img
IMAGE_B=$TMP/b.img
mkdir "$MNT_A" "$MNT_B"
truncate -s 512M "$IMAGE_A"
truncate -s 512M "$IMAGE_B"
mkfs.ext4 -q -F -O ^has_journal "$IMAGE_A"
mkfs.ext4 -q -F -O ^has_journal "$IMAGE_B"
sudo insmod "$ROOT/src/jbd3/jbd3.ko"
sudo insmod "$ROOT/src/ext5/ext5.ko" li_policy_mode=1 \
	li_debug_controls=Y li_size_floor_entries=1000000 li_stats_enabled=Y
sudo mount -t ext5 -o loop,noatime "$IMAGE_A" "$MNT_A"
sudo mount -t ext5 -o loop,noatime "$IMAGE_B" "$MNT_B"
sudo chown "$(id -u):$(id -g)" "$MNT_A" "$MNT_B"
SYS_A=/sys/fs/ext5/$(basename "$(findmnt -n -o SOURCE --target "$MNT_A")")
SYS_B=/sys/fs/ext5/$(basename "$(findmnt -n -o SOURCE --target "$MNT_B")")

mkdir "$MNT_A/tree" "$MNT_B/tree"
for i in $(seq 0 1023); do
	touch "$MNT_A/tree/base.$i" "$MNT_B/tree/base.$i"
done

b_samples=$(cat "$SYS_B/li_rmd_samples")
b_quiet=$(cat "$SYS_B/li_quiet_ops_effective")
"$ROOT/utils/tests/policy_calibrate" "$MNT_A/tree" 256 128
[ "$(cat "$SYS_A/li_rmd_samples")" -ge 256 ]
[ "$(cat "$SYS_A/li_quiet_ops_effective")" -lt 10000 ]
[ "$(cat "$SYS_B/li_rmd_samples")" -eq "$b_samples" ]
[ "$(cat "$SYS_B/li_quiet_ops_effective")" -eq "$b_quiet" ]

echo 0 | sudo tee /sys/module/ext5/parameters/li_size_floor_entries >/dev/null
echo 0 | sudo tee /sys/module/ext5/parameters/li_buildcost_permille >/dev/null
"$ROOT/utils/bin/ext5_promote_dir" "$MNT_A/tree"
# A has built, so its estimators come from its own measurements while B's
# are still the seeds.  A's build coefficient can still equal its seed (a
# quarter-weight EWMA), so it is not asserted.
[ "$(cat "$SYS_A/li_size_floor_entries_effective")" -ne 8192 ]
[ "$(cat "$SYS_A/li_size_floor_entries_effective")" \
	-ne "$(cat "$SYS_B/li_size_floor_entries_effective")" ]
[ "$(cat "$SYS_B/li_size_floor_entries_effective")" -eq 8192 ]
[ "$(cat "$SYS_B/li_buildcost_permille_effective")" -eq 16 ]

# A nonzero operator override intentionally applies to every mount; restoring
# zero reveals each mount's independent learned value again.
echo 777 | sudo tee /sys/module/ext5/parameters/li_quiet_ops >/dev/null
[ "$(cat "$SYS_A/li_quiet_ops_effective")" -eq 777 ]
[ "$(cat "$SYS_B/li_quiet_ops_effective")" -eq 777 ]
echo 0 | sudo tee /sys/module/ext5/parameters/li_quiet_ops >/dev/null
[ "$(cat "$SYS_A/li_quiet_ops_effective")" -lt 10000 ]
[ "$(cat "$SYS_B/li_quiet_ops_effective")" -eq "$b_quiet" ]

sudo umount "$MNT_A"
sudo umount "$MNT_B"
sudo rmmod ext5
sudo rmmod jbd3
rm "$IMAGE_A" "$IMAGE_B"
rmdir "$MNT_A" "$MNT_B" "$TMP"
trap - EXIT
echo "SplineFS per-filesystem adaptive T* isolation: PASS"
