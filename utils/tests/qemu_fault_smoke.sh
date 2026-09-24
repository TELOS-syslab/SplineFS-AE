#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$(dirname "$0")/kernel_root.sh"
ext5_require_kernel_root
TMP=$(mktemp -d /tmp/splinefs-qemu-fault.XXXXXX)
INITRD=$TMP/initramfs.cpio.gz
IMAGE=$TMP/fs.img
IMAGE2=$TMP/fs2.img

cleanup()
{
	set +e
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
for applet in sh mount umount insmod rmmod mkdir touch cat test sync rm rmdir \
	mv ln awk poweroff sleep; do
	ln -s busybox "$TMP/root/bin/$applet"
done
cp "$ROOT/src/ext5/ext5.ko" "$TMP/root/ext5.ko"
cp "$ROOT/src/jbd3/jbd3.ko" "$TMP/root/jbd3.ko"
cp "$ROOT/utils/tests/qemu_fault_init.sh" "$TMP/root/init"
chmod +x "$TMP/root/init"

gcc -static -O2 -std=gnu11 -Wall -Wextra -Werror \
	-o "$TMP/root/stable_fault" "$ROOT/utils/tests/stable_fault.c"
gcc -static -O2 -o "$TMP/root/ext5_promote_dir" \
	"$ROOT/utils/src/fsctl/ext5_promote_dir.c"
gcc -static -O2 -o "$TMP/root/ext5_compact" \
	"$ROOT/utils/src/fsctl/ext5_compact.c"
gcc -static -O2 -o "$TMP/root/ext5_demote_dir" \
	"$ROOT/utils/src/fsctl/ext5_demote_dir.c"
gcc -static -O2 -o "$TMP/root/ext5_li_wait" \
	"$ROOT/utils/src/fsctl/ext5_li_wait.c"
gcc -static -O2 -o "$TMP/root/ext5_li_info" \
	"$ROOT/utils/src/fsctl/ext5_li_info.c"
gcc -static -O2 -std=gnu11 -Wall -Wextra -Werror \
	-o "$TMP/root/inode_pool_test" "$ROOT/utils/tests/inode_pool_test.c"
gcc -static -O2 -std=gnu11 -Wall -Wextra -Werror \
	-o "$TMP/root/rename_capacity" "$ROOT/utils/tests/rename_capacity.c"

(cd "$TMP/root" && find . -print0 | cpio --null -o --format=newc 2>/dev/null |
	gzip -1 > "$INITRD")

run_case()
{
	local mode=$1 output=$TMP/$1.log cpus=2
	local mkfs_args=(-q -F -E lazy_itable_init=0)

	case "$mode" in
	delta-capacity*) cpus=1 ;;
	inode-pool-1k*) mkfs_args+=(-b 1024) ;;
	inode-pool-2k*) mkfs_args+=(-b 2048) ;;
	inode-pool-4k*) mkfs_args+=(-b 4096) ;;
	esac
	case "$mode" in
	*-journal) ;;
	*) mkfs_args+=(-O ^has_journal) ;;
	esac

	truncate -s 512M "$IMAGE"
	truncate -s 512M "$IMAGE2"
	mkfs.ext4 "${mkfs_args[@]}" "$IMAGE"
	mkfs.ext4 -q -F -E lazy_itable_init=0 -O ^has_journal "$IMAGE2"
	timeout 180 qemu-system-x86_64 \
		-accel kvm -accel tcg -m 1024 -smp "$cpus" -nographic -no-reboot \
		-kernel "$KERNEL_ROOT/arch/x86/boot/bzImage" -initrd "$INITRD" \
		-append "console=ttyS0 rdinit=/init panic=-1 spline_fault=$mode" \
		-drive "file=$IMAGE,format=raw,if=virtio" \
		-drive "file=$IMAGE2,format=raw,if=virtio" > "$output" 2>&1 || true
	if ! grep -q "^QEMU_FAULT_PASS $mode" "$output"; then
		cat "$output" >&2
		exit 1
	fi
	if grep -Eq 'BUG:|Oops:|WARNING:|KASAN:|general protection fault|QEMU_FAULT_FAIL|EXT5-fs error' \
			"$output"; then
		cat "$output" >&2
		exit 1
	fi
	echo "SplineFS QEMU fault case ($mode): PASS"
}

if [ -n "${CASE:-}" ]; then
	run_case "$CASE"
	exit 0
fi

run_case descriptor-header
run_case descriptor-base
run_case delta-tail
run_case delta-sequence
run_case delta-capacity
run_case delta-capacity-journal
run_case op-clock
run_case mount-clock
run_case nested-promote
run_case directory-only
for blocksize in 1k 2k 4k; do
	run_case "inode-pool-$blocksize"
	run_case "inode-pool-$blocksize-journal"
done
