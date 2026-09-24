#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# A delayed transition timer must not retain an inode or outlive unmount/module
# teardown.  The host counterpart of the QEMU post-unload test.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TMP=
IMAGE=
MNT=
DMESG_BEFORE=0

cleanup()
{
	set +e
	if [ -n "${MNT:-}" ] && mountpoint -q "$MNT"; then sudo umount "$MNT"; fi
	sudo rmmod ext5 2>/dev/null || true
	sudo rmmod jbd3 2>/dev/null || true
	[ -z "${IMAGE:-}" ] || [ ! -f "$IMAGE" ] || rm "$IMAGE"
	[ -z "${MNT:-}" ] || [ ! -d "$MNT" ] || rmdir "$MNT" 2>/dev/null || true
	[ -z "${TMP:-}" ] || [ ! -d "$TMP" ] || find "$TMP" -depth -delete
}
trap cleanup EXIT

sudo -n true
! findmnt -rn -t ext5 | grep -q .
TMP=$(mktemp -d /tmp/splinefs-delayed-unmount.XXXXXX)
IMAGE=$TMP/fs.img
MNT=$TMP/mnt
mkdir "$MNT"
truncate -s 1G "$IMAGE"
mkfs.ext4 -q -F -E lazy_itable_init=0 -O ^has_journal "$IMAGE"
DMESG_BEFORE=$(sudo dmesg --color=never | wc -l)
sudo insmod "$ROOT/src/jbd3/jbd3.ko"
sudo insmod "$ROOT/src/ext5/ext5.ko" li_policy_mode=1 \
	li_stats_enabled=Y li_size_floor_entries=1 li_quiet_ops=1 \
	li_buildcost_permille=1 li_promote_idle_ms=5000
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
sudo chown "$(id -u):$(id -g)" "$MNT"
baseline=$(sudo awk '$1=="auto_promote_scheduled" {print $2}' \
	/sys/kernel/debug/ext5/li_stats)
for dir in victim tree; do
	mkdir "$MNT/$dir"
	touch "$MNT/$dir/seed"
	for i in $(seq 0 63); do
		stat "$MNT/$dir/teardown-miss.$i" >/dev/null 2>&1 || true
	done
done
[ "$(sudo awk '$1=="auto_promote_scheduled" {print $2}' \
	/sys/kernel/debug/ext5/li_stats)" -ge $((baseline + 2)) ]

# The first timer dies with an ordinary directory inode while the filesystem
# remains live. The second dies through the unmount path below.
rm "$MNT/victim/seed"
rmdir "$MNT/victim"
sync
echo 2 | sudo tee /proc/sys/vm/drop_caches >/dev/null

sudo umount "$MNT"
sudo rmmod ext5
sudo rmmod jbd3
sleep 6
sudo dmesg --color=never | tail -n +$((DMESG_BEFORE + 1)) > "$TMP/dmesg.new"
# grep, not rg: rg is not on sudo's secure_path, so this check silently did
# nothing (command-not-found made the `if` false) and the test passed
# regardless of what the kernel had logged.
if grep -Ei 'VFS: Busy inodes|workqueue: cannot queue|kmem_cache_destroy ext5_inode_cache|BUG:|Oops:|WARNING:|general protection fault|use-after-free' \
	"$TMP/dmesg.new"; then
	echo "delayed-unmount kernel diagnostic" >&2
	exit 1
fi

rm "$IMAGE" "$TMP/dmesg.new"
rmdir "$MNT" "$TMP"
trap - EXIT
echo "SplineFS delayed transition inode/unmount teardown: PASS"
