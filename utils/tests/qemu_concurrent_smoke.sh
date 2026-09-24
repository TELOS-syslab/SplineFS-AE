#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$(dirname "$0")/kernel_root.sh"
ext5_require_kernel_root
TMP=$(mktemp -d /tmp/splinefs-qemu-concurrent.XXXXXX)
INITRD=$TMP/initramfs.cpio.gz
IMAGE=$TMP/fs.img
OUTPUT=$TMP/guest.log

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
for applet in sh mount umount insmod rmmod mkdir touch stat test sync awk \
	poweroff sleep; do
	ln -s busybox "$TMP/root/bin/$applet"
done
cp "$ROOT/src/ext5/ext5.ko" "$TMP/root/ext5.ko"
cp "$ROOT/src/jbd3/jbd3.ko" "$TMP/root/jbd3.ko"
cp "$ROOT/utils/tests/qemu_concurrent_init.sh" "$TMP/root/init"
chmod +x "$TMP/root/init"

gcc -static -O2 -pthread -o "$TMP/root/namespace_stress" \
	"$ROOT/utils/tests/namespace_stress.c"
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

(cd "$TMP/root" && find . -print0 | cpio --null -o --format=newc 2>/dev/null |
	gzip -1 > "$INITRD")
truncate -s 1G "$IMAGE"
mkfs.ext4 -q -F -E lazy_itable_init=0 "$IMAGE"

timeout 240 qemu-system-x86_64 \
	-accel kvm -accel tcg -m 1536 -smp 4 -nographic -no-reboot \
	-kernel "$KERNEL_ROOT/arch/x86/boot/bzImage" -initrd "$INITRD" \
	-append "console=ttyS0 rdinit=/init panic=-1" \
	-drive "file=$IMAGE,format=raw,if=virtio" > "$OUTPUT" 2>&1 || true
if ! grep -q '^QEMU_CONCURRENT_PASS' "$OUTPUT"; then
	cat "$OUTPUT" >&2
	exit 1
fi
if grep -Eq 'BUG:|Oops:|WARNING:|KASAN:|general protection fault|QEMU_CONCURRENT_FAIL|EXT5-fs error' \
		"$OUTPUT"; then
	cat "$OUTPUT" >&2
	exit 1
fi
echo "SplineFS QEMU concurrent promotion/compaction/demotion: PASS"
