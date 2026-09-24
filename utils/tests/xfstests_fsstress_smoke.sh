#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Optional upstream xfstests fsstress wrapper. FSSTRESS must name the externally
# built binary; this script never installs or vendors xfstests.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
FSSTRESS=${FSSTRESS:?set FSSTRESS to upstream xfstests ltp/fsstress}
DURATION=${DURATION:-30}
PROCS=${PROCS:-4}
BASE_FILES=${BASE_FILES:-8192}
LOG_OUT=${LOG_OUT:-}
FSSTRESS_MODE=${FSSTRESS_MODE:-namespace}
TMP=
IMAGE=
MNT=
START_MARKER=

cleanup()
{
	set +e
	cd /
	if [ -n "${MNT:-}" ] && mountpoint -q "$MNT"; then sudo umount "$MNT"; fi
	sudo rmmod ext5 2>/dev/null || true
	sudo rmmod jbd3 2>/dev/null || true
	if [ -n "${TMP:-}" ] && [ -f "$TMP/fsstress.log" ]; then
		[ -z "$LOG_OUT" ] || cp "$TMP/fsstress.log" "$LOG_OUT"
		rm -f "$TMP/fsstress.log"
	fi
	[ -z "${IMAGE:-}" ] || [ ! -f "$IMAGE" ] || rm "$IMAGE"
	[ -z "${MNT:-}" ] || [ ! -d "$MNT" ] || rmdir "$MNT" 2>/dev/null || true
	[ -z "${TMP:-}" ] || [ ! -d "$TMP" ] || rmdir "$TMP" 2>/dev/null || true
}
trap cleanup EXIT

[ -x "$FSSTRESS" ]
sudo -n true
! findmnt -rn -t ext5 | grep -q .
TMP=$(mktemp -d /tmp/splinefs-fsstress.XXXXXX)
IMAGE=$TMP/fs.img
MNT=$TMP/mnt
mkdir "$MNT"
truncate -s 8G "$IMAGE"
mkfs.ext4 -q -F -E lazy_itable_init=0 -O ^has_journal "$IMAGE"
sudo insmod "$ROOT/src/jbd3/jbd3.ko"
sudo insmod "$ROOT/src/ext5/ext5.ko" li_policy_mode=0 \
	li_debug_controls=Y li_stats_enabled=Y
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
sudo chown "$(id -u):$(id -g)" "$MNT"
START_MARKER=$(date +%s)

"$ROOT/utils/bin/lookup_bench" prepare "$MNT/tree" "$BASE_FILES"
"$ROOT/utils/bin/ext5_promote_dir" "$MNT/tree"
if [ "$FSSTRESS_MODE" = namespace ]; then
	"$FSSTRESS" -d "$MNT/tree" -p "$PROCS" --duration="$DURATION" \
		-s 314159 -z \
		-f creat=4 -f unlink=4 -f mkdir=2 -f rmdir=2 -f link=2 \
		-f rename=3 -f rnoreplace=1 -f stat=8 -f getdents=4 \
		-f symlink=1 -f readlink=1 -f mknod=1 -f setxattr=1 \
		-f removefattr=1 > "$TMP/fsstress.log" 2>&1
elif [ "$FSSTRESS_MODE" = data ]; then
	"$FSSTRESS" -d "$MNT/tree" -p "$PROCS" --duration="$DURATION" \
		-s 314159 > "$TMP/fsstress.log" 2>&1
else
	echo "FSSTRESS_MODE must be namespace or data" >&2
	exit 2
fi
"$ROOT/utils/bin/ext5_li_wait" "$MNT"
find "$MNT/tree" -xdev -printf . >/dev/null

sync
sudo umount "$MNT"
sudo mount -t ext5 -o loop,noatime "$IMAGE" "$MNT"
stat "$MNT/tree/item.00000000000000000017" >/dev/null
find "$MNT/tree" -xdev -printf . >/dev/null
sudo umount "$MNT"

bad=$(sudo dmesg --color=never --since "@$START_MARKER" 2>/dev/null | \
	grep -Eai 'ext5.*(error|corrupt|warning)|BUG:|Oops:|KASAN:|general protection fault|use-after-free|refcount|lockdep' || true)
[ -z "$bad" ] || { printf '%s\n' "$bad" >&2; exit 1; }
sudo rmmod ext5
sudo rmmod jbd3
[ -z "$LOG_OUT" ] || cp "$TMP/fsstress.log" "$LOG_OUT"
rm -f "$TMP/fsstress.log"
rm "$IMAGE"
rmdir "$MNT" "$TMP"
trap - EXIT
echo "SplineFS upstream fsstress lifecycle (${FSSTRESS_MODE}, ${PROCS} processes, ${DURATION}s): PASS"
