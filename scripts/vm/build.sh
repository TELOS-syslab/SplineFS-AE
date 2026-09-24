#!/bin/bash
# build.sh -- build the evaluation VM: kernel, SplineFS module, root image.
#
#   bash scripts/vm/build.sh            what scripts/vm/run.sh needs
#   bash scripts/vm/build.sh --tests    also the lockdep kernel and the QEMU
#                               correctness suite (make qemu-test)
#
# Needs gcc, make, flex, bison, bc, libelf-dev, libssl-dev, curl, rsync,
# qemu-system-x86, debootstrap, network access, and sudo for the root image.
# Outputs, not in git: the kernel in deploy/kernel/linux-6.16.5-vm, the module
# in deploy/vm/module, the root image and its SSH key in deploy/vm.
# About 20 minutes on 8 cores, most of it the kernel and debootstrap.
set -euo pipefail
AE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VM="$AE_ROOT/deploy/vm"
KTREE="$AE_ROOT/deploy/kernel/linux-6.16.5-vm"
TESTS=0
[ "${1:-}" = --tests ] && TESTS=1
say() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }
die() { printf 'ERROR: %s\n' "$1" >&2; exit 1; }
for t in rsync qemu-system-x86_64 debootstrap sudo; do
    command -v "$t" >/dev/null || die "$t is required"
done
mkdir -p "$VM"

say "1/3  kernel"
bash "$AE_ROOT/scripts/internal/fetch_kernel.sh" --profile vm

say "2/3  SplineFS module for the VM kernel"
rsync -a --delete --exclude='*.o' --exclude='*.ko' --exclude='*.mod' \
    --exclude='*.mod.c' --exclude='.*.cmd' --exclude='modules.order' \
    --exclude='Module.symvers' "$AE_ROOT/src/" "$VM/module/"
make -s -C "$VM/module" KDIR="$KTREE" -j"$(nproc)"
ko="$VM/module/ext5/ext5.ko"
[ -s "$ko" ] || die "the module build produced no ext5.ko"
printf '  %s\n' "$ko"

say "3/3  Ubuntu 24.04 root image"
if [ -s "$VM/rootfs.img" ] && [ "${REBUILD_ROOTFS:-0}" != 1 ]; then
    echo "  already built: $VM/rootfs.img (REBUILD_ROOTFS=1 rebuilds it)"
else
    [ -s "$VM/id_ed25519" ] ||
        ssh-keygen -q -t ed25519 -N '' -C splinefs-vm -f "$VM/id_ed25519"
    # sudo drops the environment; keep the proxy settings debootstrap, apt
    # and the IOR download need behind a proxy.
    proxy=()
    for v in http_proxy https_proxy no_proxy HTTP_PROXY HTTPS_PROXY NO_PROXY; do
        [ -z "${!v:-}" ] || proxy+=("$v=${!v}")
    done
    sudo env "${proxy[@]}" VM_MIRROR="${VM_MIRROR:-}" VM_ROOTFS_GB="${VM_ROOTFS_GB:-16}" \
        bash "$AE_ROOT/scripts/vm/guest/mkrootfs.sh" "$VM/rootfs.img" "$VM/id_ed25519.pub"
    sudo chown "$(id -u):$(id -g)" "$VM/rootfs.img"
fi

if [ "$TESTS" = 1 ]; then
    say "correctness suite (lockdep kernel)"
    bash "$AE_ROOT/scripts/internal/fetch_kernel.sh" --profile debug
    make -C "$AE_ROOT" qemu-test KDIR="$AE_ROOT/deploy/kernel/linux-6.16.5" \
        KERNEL_ROOT="$AE_ROOT/deploy/kernel/linux-6.16.5"
fi
say "done"
echo "  next: bash scripts/vm/run.sh"
