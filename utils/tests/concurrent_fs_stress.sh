#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TMP=
IMAGE=
MNT=
stress_pid=

cleanup()
{
	set +e
	if [ -n "${stress_pid:-}" ]; then
		kill "$stress_pid" 2>/dev/null || true
		wait "$stress_pid" 2>/dev/null || true
	fi
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

TMP=$(mktemp -d /tmp/splinefs-concurrent.XXXXXX)
IMAGE=$TMP/fs.img
MNT=$TMP/mnt
mkdir "$MNT"
truncate -s 1G "$IMAGE"
mkfs.ext4 -q -F -O ^has_journal "$IMAGE"

sudo insmod "$ROOT/src/jbd3/jbd3.ko"
sudo insmod "$ROOT/src/ext5/ext5.ko" li_policy_mode=0 \
	li_debug_controls=Y li_demote_refills=2 li_stats_enabled=Y
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
sudo chown "$(id -u):$(id -g)" "$MNT"

mkdir "$MNT/tree"
for d in $(seq 0 15); do
	mkdir "$MNT/tree/d$d"
	for i in $(seq 0 127); do
		touch "$MNT/tree/d$d/base.$i"
	done
done

# Promotion builds from an unlocked snapshot and must either publish a fully
# validated generation or return EAGAIN without changing the mutable tree.
"$ROOT/utils/tests/namespace_stress" "$MNT/tree" 8 1000 &
stress_pid=$!
promoted=0
for attempt in $(seq 1 64); do
	if "$ROOT/utils/bin/ext5_promote_dir" "$MNT/tree" >/dev/null 2>&1; then
		promoted=1
		break
	fi
done
wait "$stress_pid"
stress_pid=
if [ "$promoted" -eq 0 ]; then
	"$ROOT/utils/bin/ext5_promote_dir" "$MNT/tree"
fi

# Demotion builds from a pinned base/private-delta snapshot, validates under
# every stable parent lock, swaps shadows, and drains old readers after unlock.
# Concurrent mutations may invalidate/retry a build, but none may disappear.
"$ROOT/utils/tests/namespace_stress" "$MNT/tree" 8 1000 &
stress_pid=$!
set +e
"$ROOT/utils/bin/ext5_demote_dir" "$MNT/tree" >/dev/null 2>&1
demote_status=$?
set -e
wait "$stress_pid"
stress_pid=
"$ROOT/utils/bin/ext5_li_wait" "$MNT"
if [ "$demote_status" -ne 0 ]; then
	[ "$demote_status" -eq 3 ]
	if "$ROOT/utils/bin/ext5_li_info" "$MNT/tree" >/dev/null 2>&1; then
		"$ROOT/utils/bin/ext5_demote_dir" "$MNT/tree"
	fi
fi
"$ROOT/utils/bin/ext5_promote_dir" "$MNT/tree"

# Exercise ordinary stable mutations and background compaction after both
# transition races.
"$ROOT/utils/tests/namespace_stress" "$MNT/tree" 8 2000
"$ROOT/utils/bin/ext5_li_wait" "$MNT"
stat "$MNT/tree/d7/base.91" >/dev/null
[ "$(sudo awk '$1=="rs_bound_violations" {print $2}' \
	/sys/kernel/debug/ext5/li_stats)" -eq 0 ]

sudo umount "$MNT"
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
stat "$MNT/tree/d7/base.91" >/dev/null

sudo umount "$MNT"
sudo rmmod ext5
sudo rmmod jbd3
rm "$IMAGE"
rmdir "$MNT" "$TMP"
trap - EXIT
echo "SplineFS concurrent promote/demote/compaction stress: PASS"
