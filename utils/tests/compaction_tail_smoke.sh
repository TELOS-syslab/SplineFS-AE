#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TMP=
IMAGE=
MNT=
compact_pid=

cleanup()
{
	set +e
	if [ -n "${compact_pid:-}" ]; then
		kill "$compact_pid" 2>/dev/null || true
		wait "$compact_pid" 2>/dev/null || true
	fi
	if [ -n "${MNT:-}" ] && mountpoint -q "$MNT"; then sudo umount "$MNT"; fi
	sudo rmmod ext5 2>/dev/null || true
	sudo rmmod jbd3 2>/dev/null || true
	[ -z "${IMAGE:-}" ] || [ ! -f "$IMAGE" ] || rm "$IMAGE"
	[ -z "${TMP:-}" ] || [ ! -d "$TMP" ] || rm -f "$TMP"/*.out "$TMP"/*.err
	[ -z "${MNT:-}" ] || [ ! -d "$MNT" ] || rmdir "$MNT" 2>/dev/null || true
	[ -z "${TMP:-}" ] || [ ! -d "$TMP" ] || rmdir "$TMP" 2>/dev/null || true
}
trap cleanup EXIT

delta_used()
{
	"$ROOT/utils/bin/ext5_li_info" "$MNT/tree" |
		awk '$1=="delta_used:" {print $2}'
}

verify_tail()
{
	local i

	for i in $(seq 0 127); do
		stat "$MNT/tree/tail.$i" >/dev/null
	done
}

sudo -n true
! findmnt -rn -t ext5 | grep -q .
TMP=$(mktemp -d /tmp/splinefs-compact-tail.XXXXXX)
IMAGE=$TMP/fs.img
MNT=$TMP/mnt
mkdir "$MNT"
truncate -s 2G "$IMAGE"
mkfs.ext4 -q -F -E lazy_itable_init=0 -O ^has_journal "$IMAGE"
sudo insmod "$ROOT/src/jbd3/jbd3.ko"
sudo insmod "$ROOT/src/ext5/ext5.ko" li_policy_mode=0 \
	li_debug_controls=Y li_delta_trigger_bytes=8388608 li_stats_enabled=Y
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
sudo chown "$(id -u):$(id -g)" "$MNT"

"$ROOT/utils/bin/lookup_bench" prepare "$MNT/tree" 50000
"$ROOT/utils/bin/ext5_promote_dir" "$MNT/tree"
touch "$MNT/tree/seed"

# The initial snapshot includes seed. Persistent creates begin only after the
# compactor starts, so a successful first publication must carry a raw tail.
"$ROOT/utils/bin/ext5_compact" "$MNT/tree" \
	> "$TMP/compact.out" 2> "$TMP/compact.err" &
compact_pid=$!
sleep 0.01
for i in $(seq 0 127); do : > "$MNT/tree/tail.$i"; done
wait "$compact_pid"
compact_pid=
[ "$(delta_used)" -gt 0 ]
verify_tail

sudo umount "$MNT"
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
[ "$(delta_used)" -gt 0 ]
verify_tail

# With no concurrent appender, the next generation folds the carried tail and
# publishes an empty delta.
"$ROOT/utils/bin/ext5_compact" "$MNT/tree"
[ "$(delta_used)" -eq 0 ]
verify_tail

sudo umount "$MNT"
sudo rmmod ext5
sudo rmmod jbd3
rm "$IMAGE" "$TMP/compact.out" "$TMP/compact.err"
rmdir "$MNT" "$TMP"
trap - EXIT
echo "SplineFS off-lock compaction tail handoff: PASS"
