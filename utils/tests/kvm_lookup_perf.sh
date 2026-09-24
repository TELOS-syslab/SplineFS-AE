#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$(dirname "$0")/kernel_root.sh"
ext5_require_kernel_root
FILES=${FILES:-200000}
LOOKUPS=${LOOKUPS:-$FILES}
REPS=${REPS:-5}
MODE=${MODE:-positive-unique}
HOST_CPU=${HOST_CPU:-2}
OUT_DIR=${OUT_DIR:-$ROOT/results/$(date +%F)/kvm-lookup-current}
TMP=$(mktemp -d /tmp/splinefs-kvm-perf.XXXXXX)
INITRD=$TMP/initramfs.cpio.gz
EXT4_IMAGE=$TMP/ext4.img
EXT5_IMAGE=$TMP/ext5.img
GUEST_LOG=$OUT_DIR/guest.log

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
sudo -n test -r /dev/kvm
sudo -n test -w /dev/kvm
[ -s "$KERNEL_ROOT/arch/x86/boot/bzImage" ]
[ -s "$ROOT/src/ext5/ext5.ko" ]
[ -s "$ROOT/src/jbd3/jbd3.ko" ]
ext5_require_module_match
case "$MODE" in
positive-unique) [ "$LOOKUPS" -le "$FILES" ] ;;
negative-unique) ;;
*) echo "MODE must be positive-unique or negative-unique" >&2; exit 2 ;;
esac
[ "$FILES" -ge 8192 ] || {
	echo "FILES must meet the adaptive cold size floor (8192)" >&2
	exit 2
}
mkdir -p "$OUT_DIR" "$TMP/root"/{bin,dev,ext4,spline,proc,sys}

cp /bin/busybox "$TMP/root/bin/busybox"
for applet in sh mount umount insmod rmmod mkdir cat test sync awk poweroff \
	sleep; do
	ln -s busybox "$TMP/root/bin/$applet"
done
cp "$ROOT/src/ext5/ext5.ko" "$TMP/root/ext5.ko"
cp "$ROOT/src/jbd3/jbd3.ko" "$TMP/root/jbd3.ko"
cp "$ROOT/utils/tests/qemu_perf_init.sh" "$TMP/root/init"
chmod +x "$TMP/root/init"
gcc -static -O2 -std=gnu11 -Wall -Wextra -Werror \
	-o "$TMP/root/lookup_bench" \
	"$ROOT/utils/src/benchmarks/lookup_bench.c"
gcc -static -O2 -o "$TMP/root/ext5_li_wait" \
	"$ROOT/utils/src/fsctl/ext5_li_wait.c"
gcc -static -O2 -o "$TMP/root/ext5_li_info" \
	"$ROOT/utils/src/fsctl/ext5_li_info.c"
(cd "$TMP/root" && find . -print0 | cpio --null -o --format=newc 2>/dev/null |
	gzip -1 > "$INITRD")

truncate -s 4G "$EXT4_IMAGE" "$EXT5_IMAGE"
mkfs.ext4 -q -F -E lazy_itable_init=0 -O ^has_journal "$EXT4_IMAGE"
mkfs.ext4 -q -F -E lazy_itable_init=0 -O ^has_journal "$EXT5_IMAGE"

uname -a > "$OUT_DIR/host-uname.txt"
lscpu > "$OUT_DIR/host-lscpu.txt"
qemu-system-x86_64 --version > "$OUT_DIR/qemu-version.txt"
sha256sum "$ROOT/src/ext5/ext5.ko" > "$OUT_DIR/module.sha256"
sha256sum "$KERNEL_ROOT/arch/x86/boot/bzImage" > "$OUT_DIR/kernel.sha256"
sha256sum "$ROOT/utils/src/benchmarks/lookup_bench.c" \
	"$ROOT/utils/tests/qemu_perf_init.sh" \
	"$ROOT/utils/tests/kvm_lookup_perf.sh" > "$OUT_DIR/harness.sha256"
printf 'files=%s\nlookups=%s\nreps=%s\nmode=%s\nhost_cpu=%s\nkernel=%s\n' \
	"$FILES" "$LOOKUPS" "$REPS" "$MODE" "$HOST_CPU" "$KERNEL_ROOT" \
	> "$OUT_DIR/config.txt"

timeout 900 sudo -n taskset -c "$HOST_CPU" qemu-system-x86_64 \
	-machine accel=kvm -cpu host -m 2048 -smp 1 -nographic -no-reboot \
	-kernel "$KERNEL_ROOT/arch/x86/boot/bzImage" -initrd "$INITRD" \
	-append "console=ttyS0 rdinit=/init panic=-1 nmi_watchdog=0 spline_files=$FILES spline_lookups=$LOOKUPS spline_reps=$REPS spline_mode=$MODE" \
	-drive "file=$EXT4_IMAGE,format=raw,if=virtio,cache=none,aio=native" \
	-drive "file=$EXT5_IMAGE,format=raw,if=virtio,cache=none,aio=native" \
	> "$GUEST_LOG" 2>&1 || true

if ! grep -q '^KVM_PERF_PASS' "$GUEST_LOG"; then
	cat "$GUEST_LOG" >&2
	exit 1
fi
if grep -Eq 'falling back to tcg|BUG:|Oops:|WARNING:|KASAN:|general protection fault|KVM_PERF_FAIL|EXT5-fs error' \
		"$GUEST_LOG"; then
	cat "$GUEST_LOG" >&2
	exit 1
fi

printf 'rep,fs,cycles_per_op,instructions_per_op,ns_per_op\n' \
	> "$OUT_DIR/results.csv"
awk -F= '
{ sub(/\r$/, "") }
/^KVM_PERF_BEGIN / {
    split($0, fields, " ");
    split(fields[2], r, "="); split(fields[3], f, "=");
    rep=r[2]; fs=f[2]; active=1; next
}
active && $1=="cycles_per_op" { cycles=$2; next }
active && $1=="instructions_per_op" { instructions=$2; next }
active && $1=="ns_per_op" { ns=$2; next }
/^KVM_PERF_END / {
    printf "%s,%s,%s,%s,%s\n", rep,fs,cycles,instructions,ns;
    active=0
}
' "$GUEST_LOG" >> "$OUT_DIR/results.csv"

awk -F, -v reps="$REPS" '
NR>1 {
    if ($3 <= 0 || $4 <= 0 || $5 <= 0) exit 2;
    n[$2]++; c[$2]+=$3; i[$2]+=$4; t[$2]+=$5
}
END {
    if (n["ext4"] != reps || n["splinefs"] != reps) exit 3;
    ec=c["ext4"]/n["ext4"]; ei=i["ext4"]/n["ext4"];
    et=t["ext4"]/n["ext4"];
    sc=c["splinefs"]/n["splinefs"]; si=i["splinefs"]/n["splinefs"];
    st=t["splinefs"]/n["splinefs"];
    printf "ext4 mean cycles=%.3f instructions=%.3f ns=%.3f n=%d\n", ec,ei,et,n["ext4"];
    printf "splinefs mean cycles=%.3f instructions=%.3f ns=%.3f n=%d\n", sc,si,st,n["splinefs"];
    printf "splinefs/ext4 cycles=%.4f instructions=%.4f ns=%.4f\n", sc/ec,si/ei,st/et
}' "$OUT_DIR/results.csv" > "$OUT_DIR/summary.txt"
cat "$OUT_DIR/summary.txt"
echo "results: $OUT_DIR"
