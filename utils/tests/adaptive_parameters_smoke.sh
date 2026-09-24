#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TMP=
IMAGE=
MNT=
POLICY=

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

value()
{
	cat "$POLICY/$1"
}

sudo -n true
! findmnt -rn -t ext5 | grep -q . || exit 1
TMP=$(mktemp -d /tmp/splinefs-adaptive.XXXXXX)
IMAGE=$TMP/fs.img
MNT=$TMP/mnt
mkdir "$MNT"
truncate -s 512M "$IMAGE"
mkfs.ext4 -q -F -O ^has_journal "$IMAGE"
sudo insmod "$ROOT/src/jbd3/jbd3.ko"
sudo insmod "$ROOT/src/ext5/ext5.ko" li_policy_mode=1 \
	li_debug_controls=Y li_size_floor_entries=1000000 li_stats_enabled=Y
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
sudo chown "$(id -u):$(id -g)" "$MNT"
DEV=$(basename "$(findmnt -n -o SOURCE --target "$MNT")")
POLICY=/sys/fs/ext5/$DEV
mkdir "$MNT/tree"
for i in $(seq 0 1023); do touch "$MNT/tree/base.$i"; done
[ "$(value li_buildcost_permille_effective)" -eq 16 ]

# Equal burst and quiet-gap samples produce a non-default MCR-derived T_v.
"$ROOT/utils/tests/policy_calibrate" "$MNT/tree" 256 128
samples=$(value li_rmd_samples)
learned_quiet=$(value li_quiet_ops_effective)
[ "$samples" -ge 256 ]
[ "$learned_quiet" -lt 10000 ]
[ "$learned_quiet" -ge 16 ]

# Runtime override takes effect without remount; zero restores adaptive mode.
echo 777 | sudo tee /sys/module/ext5/parameters/li_quiet_ops >/dev/null
[ "$(value li_quiet_ops_effective)" -eq 777 ]
echo 0 | sudo tee /sys/module/ext5/parameters/li_quiet_ops >/dev/null
[ "$(value li_quiet_ops_effective)" -eq "$learned_quiet" ]

# Let the debug promotion supply measured footprint/build-cost observations.
echo 0 | sudo tee /sys/module/ext5/parameters/li_size_floor_entries >/dev/null
echo 0 | sudo tee /sys/module/ext5/parameters/li_buildcost_permille >/dev/null
before_floor=$(value li_size_floor_entries_effective)
"$ROOT/utils/bin/ext5_promote_dir" "$MNT/tree"
after_floor=$(value li_size_floor_entries_effective)
[ "$after_floor" -ne "$before_floor" ]

echo 4096 | sudo tee /sys/module/ext5/parameters/li_size_floor_entries >/dev/null
echo 2750 | sudo tee /sys/module/ext5/parameters/li_buildcost_permille >/dev/null
[ "$(value li_size_floor_entries_effective)" -eq 4096 ]
[ "$(value li_buildcost_permille_effective)" -eq 2750 ]
# c=2.750, N=4096, floor(log2 N)=12 -> 135168 operations.
[ "$(value li_tstar_at_floor)" -eq 135168 ]

sudo umount "$MNT"
sudo rmmod ext5
sudo rmmod jbd3
rm "$IMAGE"
rmdir "$MNT" "$TMP"
trap - EXIT
echo "SplineFS adaptive/configurable T* parameters: PASS"
