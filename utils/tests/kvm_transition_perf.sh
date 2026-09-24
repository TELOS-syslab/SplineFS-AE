#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$(dirname "$0")/kernel_root.sh"
ext5_require_kernel_root
FILES=${FILES:-200000}
PROBES=${PROBES:-1000000}
REPS=${REPS:-3}
HOST_CPUS=${HOST_CPUS:-2,3}
OUT_DIR=${OUT_DIR:-$ROOT/results/$(date +%F)/kvm-transition-current}
TMP=$(mktemp -d /tmp/splinefs-kvm-transition.XXXXXX)
INITRD=$TMP/initramfs.cpio.gz
IMAGE=$TMP/fs.img
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
[ "$FILES" -ge 8192 ]
mkdir -p "$OUT_DIR" "$TMP/root"/{bin,dev,mnt,proc,sys,tmp}

cp /bin/busybox "$TMP/root/bin/busybox"
for applet in sh mount umount insmod rmmod mkdir touch stat cat test sync awk \
	taskset poweroff sleep; do
	ln -s busybox "$TMP/root/bin/$applet"
done
cp "$ROOT/src/ext5/ext5.ko" "$TMP/root/ext5.ko"
cp "$ROOT/src/jbd3/jbd3.ko" "$TMP/root/jbd3.ko"
cp "$ROOT/utils/tests/qemu_transition_init.sh" "$TMP/root/init"
chmod +x "$TMP/root/init"
gcc -static -O2 -std=gnu11 -Wall -Wextra -Werror \
	-o "$TMP/root/lookup_bench" \
	"$ROOT/utils/src/benchmarks/lookup_bench.c"
gcc -static -O2 -std=gnu11 -Wall -Wextra -Werror \
	-o "$TMP/root/transition_probe" \
	"$ROOT/utils/tests/transition_probe.c"
gcc -static -O2 -o "$TMP/root/ext5_promote_dir" \
	"$ROOT/utils/src/fsctl/ext5_promote_dir.c"
gcc -static -O2 -o "$TMP/root/ext5_compact" \
	"$ROOT/utils/src/fsctl/ext5_compact.c"
gcc -static -O2 -o "$TMP/root/ext5_demote_dir" \
	"$ROOT/utils/src/fsctl/ext5_demote_dir.c"
(cd "$TMP/root" && find . -print0 | cpio --null -o --format=newc 2>/dev/null |
	gzip -1 > "$INITRD")

truncate -s 4G "$IMAGE"
mkfs.ext4 -q -F -E lazy_itable_init=0 -O ^has_journal "$IMAGE"
uname -a > "$OUT_DIR/host-uname.txt"
lscpu > "$OUT_DIR/host-lscpu.txt"
qemu-system-x86_64 --version > "$OUT_DIR/qemu-version.txt"
sha256sum "$ROOT/src/ext5/ext5.ko" > "$OUT_DIR/module.sha256"
sha256sum "$KERNEL_ROOT/arch/x86/boot/bzImage" > "$OUT_DIR/kernel.sha256"
sha256sum "$ROOT/utils/tests/transition_probe.c" \
	"$ROOT/utils/tests/qemu_transition_init.sh" \
	"$ROOT/utils/tests/kvm_transition_perf.sh" > "$OUT_DIR/harness.sha256"
printf 'files=%s\nprobes=%s\nreps=%s\nhost_cpus=%s\nkernel=%s\n' \
	"$FILES" "$PROBES" "$REPS" "$HOST_CPUS" "$KERNEL_ROOT" \
	> "$OUT_DIR/config.txt"

timeout 900 sudo -n taskset -c "$HOST_CPUS" qemu-system-x86_64 \
	-machine accel=kvm -cpu host -m 2048 -smp 2 -nographic -no-reboot \
	-kernel "$KERNEL_ROOT/arch/x86/boot/bzImage" -initrd "$INITRD" \
	-append "console=ttyS0 rdinit=/init panic=-1 nmi_watchdog=0 spline_files=$FILES spline_probes=$PROBES spline_reps=$REPS" \
	-drive "file=$IMAGE,format=raw,if=virtio,cache=none,aio=native" \
	> "$GUEST_LOG" 2>&1 || true

if ! grep -q '^KVM_TRANSITION_PASS' "$GUEST_LOG"; then
	cat "$GUEST_LOG" >&2
	exit 1
fi
if grep -Eq 'falling back to tcg|BUG:|Oops:|WARNING:|KASAN:|general protection fault|KVM_TRANSITION_FAIL|EXT5-fs error' \
		"$GUEST_LOG"; then
	cat "$GUEST_LOG" >&2
	exit 1
fi

printf 'rep,transition,attempts,p50_ns,p99_ns,p999_ns,max_ns,elapsed_sec\n' \
	> "$OUT_DIR/results.csv"
awk -F= '
{ sub(/\r$/, "") }
/^KVM_TRANSITION_BEGIN / {
    split($0, fields, " ");
    split(fields[2], r, "="); split(fields[3], t, "=");
    rep=r[2]; transition=t[2]; active=1; next
}
active && $1=="p50_ns" { p50=$2; next }
active && $1=="p99_ns" { p99=$2; next }
active && $1=="p999_ns" { p999=$2; next }
active && $1=="max_ns" { max=$2; next }
active && $1=="elapsed_sec" { elapsed=$2; next }
active && $1=="attempts" { attempts=$2; next }
/^KVM_TRANSITION_END / {
    printf "%s,%s,%s,%s,%s,%s,%s,%s\n", rep,transition,attempts,p50,p99,p999,max,elapsed;
    active=0
}
' "$GUEST_LOG" >> "$OUT_DIR/results.csv"

awk -F, -v reps="$REPS" '
NR>1 {n[$2]++; p99[$2]+=$5; attempts[$2]+=$3; if ($7>max[$2]) max[$2]=$7}
END {
    for (t in n) {
        if (n[t] != reps) exit 2;
        printf "%s mean_attempts=%.2f mean_p99_ns=%.1f max_ns=%d n=%d\n", t,attempts[t]/n[t],p99[t]/n[t],max[t],n[t]
    }
}' "$OUT_DIR/results.csv" | sort > "$OUT_DIR/summary.txt"
cat "$OUT_DIR/summary.txt"
echo "results: $OUT_DIR"
