#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$(dirname "$0")/kernel_root.sh"
ext5_require_kernel_root
JOURNAL=${JOURNAL:-Y}
TMP=$(mktemp -d /tmp/splinefs-qemu-journal.XXXXXX)
INITRD=$TMP/initramfs.cpio.gz
IMAGE=$TMP/fs.img
CRASH_OUTPUT=$TMP/crash.log
FINISH_OUTPUT=$TMP/finish.log

cleanup()
{
	local status=$?

	set +e
	if [ "${KEEP_TMP:-0}" -eq 1 ] || [ "$status" -ne 0 ]; then
		echo "QEMU journal artifacts retained: $TMP" >&2
		return
	fi
	if [ -d "$TMP" ]; then
		find "$TMP" -depth -delete
	fi
}
trap cleanup EXIT

command -v qemu-system-x86_64 >/dev/null
command -v busybox >/dev/null
command -v cpio >/dev/null
[ -s "$KERNEL_ROOT/arch/x86/boot/bzImage" ]
[ -s "$ROOT/src/ext5/ext5.ko" ]
[ -s "$ROOT/src/jbd3/jbd3.ko" ]
ext5_require_module_match

mkdir -p "$TMP/root"/{bin,dev,mnt,proc,sys}
cp /bin/busybox "$TMP/root/bin/busybox"
for applet in sh mount umount insmod rmmod mkdir touch ln rm rmdir mv cat \
	stat test sync poweroff sleep; do
	ln -s busybox "$TMP/root/bin/$applet"
done
cp "$ROOT/src/ext5/ext5.ko" "$TMP/root/ext5.ko"
cp "$ROOT/src/jbd3/jbd3.ko" "$TMP/root/jbd3.ko"
cp "$ROOT/utils/tests/qemu_journal_init.sh" "$TMP/root/init"
chmod +x "$TMP/root/init"

gcc -static -O2 -o "$TMP/root/ext5_promote_dir" \
	"$ROOT/utils/src/fsctl/ext5_promote_dir.c"
gcc -static -O2 -o "$TMP/root/ext5_compact" \
	"$ROOT/utils/src/fsctl/ext5_compact.c"
gcc -static -O2 -o "$TMP/root/ext5_demote_dir" \
	"$ROOT/utils/src/fsctl/ext5_demote_dir.c"
gcc -static -O2 -o "$TMP/root/ext5_li_wait" \
	"$ROOT/utils/src/fsctl/ext5_li_wait.c"
gcc -static -O2 -pthread -o "$TMP/root/rename_race" \
	"$ROOT/utils/tests/rename_race.c"

(cd "$TMP/root" && find . -print0 | cpio --null -o --format=newc 2>/dev/null |
	gzip -1 > "$INITRD")
truncate -s 1G "$IMAGE"
if [ "$JOURNAL" = Y ]; then
	mkfs.ext4 -q -F -E lazy_itable_init=0 "$IMAGE"
else
	mkfs.ext4 -q -F -E lazy_itable_init=0 -O ^has_journal "$IMAGE"
fi

run_guest()
{
	local phase=$1 output=$2

	timeout 180 qemu-system-x86_64 \
		-accel kvm -accel tcg -m 1024 -smp 2 -nographic -no-reboot \
		-kernel "$KERNEL_ROOT/arch/x86/boot/bzImage" -initrd "$INITRD" \
		-append "console=ttyS0 rdinit=/init panic=-1 spline_phase=$phase" \
		-drive "file=$IMAGE,format=raw,if=virtio" > "$output" 2>&1 || true
}

check_guest()
{
	local output=$1 marker=$2

	if ! grep -q "^$marker" "$output"; then
		cat "$output" >&2
		exit 1
	fi
	if grep -Eq 'BUG:|Oops:|WARNING:|KASAN:|general protection fault|QEMU_JOURNAL_FAIL' \
			"$output"; then
		cat "$output" >&2
		exit 1
	fi
}

if [ "$JOURNAL" = Y ]; then
	run_guest crash "$CRASH_OUTPUT"
	check_guest "$CRASH_OUTPUT" QEMU_CRASH_READY
	run_guest recover "$FINISH_OUTPUT"
	check_guest "$FINISH_OUTPUT" QEMU_JOURNAL_PASS
	echo "SplineFS QEMU journaled panic/recovery lifecycle: PASS"
else
	run_guest clean "$FINISH_OUTPUT"
	check_guest "$FINISH_OUTPUT" QEMU_JOURNAL_PASS
	echo "SplineFS QEMU no-journal clean remount lifecycle: PASS"
fi
