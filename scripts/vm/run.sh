#!/bin/bash
# run.sh -- run the evaluation inside the VM that scripts/vm/build.sh built.
#
#   bash scripts/vm/run.sh                          minor tier on a 64 GB disk image
#   TIER=medium bash scripts/vm/run.sh              another tier; REPS=3 for three repetitions
#   CAMPAIGNS="footprint mdtest" bash scripts/vm/run.sh
#   sudo VM_DISK=/dev/nvme0n1p4 CONFIRM_DESTROY=/dev/nvme0n1p4 bash scripts/vm/run.sh
#   bash scripts/vm/run.sh --shell                  boot to a root shell instead
#
# Settings (environment):
#   VM_DISK    test disk: an image file (default deploy/vm/disk.img, created
#              sparse, VM_DISK_GB=64) or a spare block device, which is
#              reformatted and needs CONFIRM_DESTROY set to the same path
#   VM_MEM     guest memory in MiB (default 3/4 of host memory, at most 16384)
#   VM_CPUS    guest CPUs (default min(8, nproc))
#   TIER, REPS, CAMPAIGNS  as for scripts/run_queue.sh (default TIER=minor)
#   MAX_CAP    largest memory cap in bytes (default guest memory - 2 GiB)
#   AE_RESULTS where results go (default results/)
#
# Results are laid out as for a host run, recorded with platform=qemu.
set -euo pipefail
AE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VM="$AE_ROOT/deploy/vm"
KERNEL="$AE_ROOT/deploy/kernel/linux-6.16.5-vm/arch/x86/boot/bzImage"
SHELL_MODE=0
[ "${1:-}" = --shell ] && SHELL_MODE=1
die() { printf 'ERROR: %s\n' "$1" >&2; exit 1; }

command -v qemu-system-x86_64 >/dev/null || die "qemu-system-x86_64 not found"
[ -r /dev/kvm ] && [ -w /dev/kvm ] || die "KVM is unavailable: /dev/kvm is missing or not accessible.
  Join the kvm group (sudo usermod -aG kvm \$USER, then log in again) or run
  with sudo.  Without KVM the guest is emulated and its numbers mean nothing."
for f in "$KERNEL" "$VM/module/ext5/ext5.ko" "$VM/rootfs.img"; do
    [ -s "$f" ] || die "$f is missing; run bash scripts/vm/build.sh first"
done

host_mem=$(awk '/MemTotal/ { print int($2 / 1024) }' /proc/meminfo)
def_mem=$(( host_mem * 3 / 4 ))
[ "$def_mem" -le 16384 ] || def_mem=16384
VM_MEM=${VM_MEM:-$def_mem}
VM_CPUS=${VM_CPUS:-$(( $(nproc) < 8 ? $(nproc) : 8 ))}
MAX_CAP=${MAX_CAP:-$(( (VM_MEM - 2048) * 1024 * 1024 ))}
TIER=${TIER:-minor}
RESULTS=$(mkdir -p "${AE_RESULTS:-$AE_ROOT/results}/logs" && cd "${AE_RESULTS:-$AE_ROOT/results}" && pwd)

VM_DISK=${VM_DISK:-$VM/disk.img}
if [ -b "$VM_DISK" ]; then
    [ "${CONFIRM_DESTROY:-}" = "$VM_DISK" ] ||
        die "VM_DISK is a block device; set CONFIRM_DESTROY=$VM_DISK to let the run reformat it"
    real=$(readlink -f "$VM_DISK")
    lsblk -nro MOUNTPOINTS "$real" 2>/dev/null | grep -q . &&
        die "$VM_DISK (or a partition on it) is mounted"
    rootdev=$(readlink -f "$(findmnt -no SOURCE /)")
    for d in "$rootdev" "/dev/$(lsblk -nro PKNAME "$rootdev" 2>/dev/null | head -1)"; do
        [ "$d" != "$real" ] || die "$VM_DISK holds the root filesystem"
    done
    [ -w "$VM_DISK" ] || die "no write access to $VM_DISK; run with sudo"
    cache="cache=none,aio=native"
else
    [ -e "$VM_DISK" ] || { mkdir -p "$(dirname "$VM_DISK")"; truncate -s "${VM_DISK_GB:-64}G" "$VM_DISK"; }
    probe="$VM_DISK.odirect-probe"
    if dd if=/dev/zero of="$probe" bs=4096 count=1 oflag=direct status=none 2>/dev/null; then
        cache="cache=none,aio=native"
    else
        echo "WARNING: $(dirname "$VM_DISK") does not support O_DIRECT, so the guest disk" >&2
        echo "  is cached by the host and cold-cache results will look faster than they are." >&2
        cache="cache=writeback"
    fi
    rm -f "$probe"
fi

append="root=/dev/vda rw console=ttyS0 quiet"
if [ "$SHELL_MODE" = 0 ]; then
    append="$append splinefs.run=1 splinefs.tier=$TIER splinefs.max_cap=$MAX_CAP"
    [ -z "${REPS:-}" ] || append="$append splinefs.reps=$REPS"
    [ -z "${CAMPAIGNS:-}" ] || append="$append splinefs.campaigns=$(echo $CAMPAIGNS | tr ' ' ',')"
fi
shares=(-virtfs "local,path=$AE_ROOT,mount_tag=hostae,security_model=none,readonly=on"
        -virtfs "local,path=$RESULTS,mount_tag=results,security_model=none")
if [ -d "$AE_ROOT/datasets/staged" ]; then
    shares+=(-virtfs "local,path=$(readlink -f "$AE_ROOT/datasets/staged"),mount_tag=datasets,security_model=none,readonly=on")
fi

log="$RESULTS/logs/vm-console-$(date +%Y%m%d-%H%M%S).log"
printf 'VM: %s CPUs, %s MiB, test disk %s (%s)\n' "$VM_CPUS" "$VM_MEM" "$VM_DISK" "$cache"
[ "$SHELL_MODE" = 1 ] && echo "shell: root on this console, or ssh -p ${VM_SSH_PORT:-2222} -i $VM/id_ed25519 root@localhost; 'poweroff' to leave" ||
    printf 'tier %s, largest cap %s MiB; console log %s\n' "$TIER" "$(( MAX_CAP >> 20 ))" "$log"
qemu-system-x86_64 -enable-kvm -cpu host -smp "$VM_CPUS" -m "$VM_MEM" \
    -kernel "$KERNEL" -append "$append" \
    -drive "file=$VM/rootfs.img,format=raw,if=virtio,snapshot=on" \
    -drive "file=$VM_DISK,format=raw,if=virtio,$cache" \
    "${shares[@]}" \
    -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:${VM_SSH_PORT:-2222}-:22" \
    -device virtio-net-pci,netdev=net0 \
    -nographic -no-reboot 2>&1 | tee "$log"

if [ -n "${SUDO_UID:-}" ]; then
    chown -R "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$RESULTS"
fi
[ "$SHELL_MODE" = 1 ] && exit 0
grep -q "SPLINEFS-VM-DONE rc=0" "$log" || die "the VM run did not finish cleanly; see $log"
echo "done.  Compare the trends with: python3 scripts/check.py --trend --results $RESULTS"
