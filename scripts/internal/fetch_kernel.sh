#!/bin/bash
# fetch_kernel.sh -- download, configure and build vanilla Linux 6.16.5.
#
# SplineFS is an out-of-tree module that needs no kernel patch, so a
# kernel.org 6.16.5 is all it needs.  Two builds, each in its own tree:
#
#   --profile vm      deploy/kernel/linux-6.16.5-vm  (default)  the evaluation
#                     VM: no lock debugging, plus virtio, 9p, KVM guest support,
#                     and xfs, btrfs and f2fs for the baselines
#   --profile debug   deploy/kernel/linux-6.16.5     the correctness tests:
#                     lockdep, spinlock and atomic-sleep debugging on
#
#   bash scripts/internal/fetch_kernel.sh [--profile vm|debug] [--source-only|--config-only]
#   JOBS=16 bash scripts/internal/fetch_kernel.sh
#
# Idempotent.  About 8 minutes at -j32.
set -euo pipefail

AE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VERSION=${EXT5_KERNEL_VERSION:-6.16.5}
# From https://cdn.kernel.org/pub/linux/kernel/v6.x/sha256sums.asc
SHA256=${KERNEL_SHA256:-76bffbae7eab2a1de1ed05692bef709f43b02a52fe95ae655cacf0fa252213f3}
URL=${KERNEL_URL:-https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$VERSION.tar.xz}
BUILD="$AE_ROOT/deploy/kernel"
TARBALL="$BUILD/linux-$VERSION.tar.xz"
JOBS=${JOBS:-$(nproc)}
PROFILE=vm
CONFIG_ONLY=0
SOURCE_ONLY=0
while [ $# -gt 0 ]; do
    case "$1" in
    --profile) PROFILE=$2; shift 2 ;;
    --profile=*) PROFILE=${1#--profile=}; shift ;;
    --config-only) CONFIG_ONLY=1; shift ;;
    --source-only) SOURCE_ONLY=1; shift ;;
    *) echo "usage: $0 [--profile vm|debug] [--source-only|--config-only]" >&2; exit 1 ;;
    esac
done
case "$PROFILE" in
vm)    TREE="$BUILD/linux-$VERSION-vm" ;;
debug) TREE="$BUILD/linux-$VERSION" ;;
*) echo "unknown profile '$PROFILE'; use vm or debug" >&2; exit 1 ;;
esac

say() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }
die() { printf 'ERROR: %s\n' "$1" >&2; exit 1; }

tools="curl tar xz"
[ "$SOURCE_ONLY" = 1 ] || tools="$tools make gcc flex bison bc"
for t in $tools; do
    command -v "$t" >/dev/null 2>&1 ||
        die "$t is required; on Debian/Ubuntu: apt install build-essential flex bison bc libelf-dev libssl-dev"
done
mkdir -p "$BUILD"

say "1/4  source ($PROFILE profile)"
if [ -s "$TARBALL" ] && echo "$SHA256  $TARBALL" | sha256sum -c - >/dev/null 2>&1; then
    echo "  already have a verified $TARBALL"
else
    echo "  downloading $URL"
    curl -fL --retry 3 --progress-bar -o "$TARBALL.part" "$URL"
    mv "$TARBALL.part" "$TARBALL"
    echo "$SHA256  $TARBALL" | sha256sum -c - ||
        die "checksum mismatch on $TARBALL; delete it and retry"
fi
if [ -f "$TREE/Makefile" ]; then
    echo "  already extracted at $TREE"
else
    echo "  extracting to $TREE"
    tmp=$(mktemp -d "$BUILD/extract.XXXXXX")
    tar -C "$tmp" -xf "$TARBALL"
    mv "$tmp/linux-$VERSION" "$TREE"
    rmdir "$tmp"
fi
[ "$SOURCE_ONLY" -eq 0 ] || exit 0

say "2/4  configure"
make -C "$TREE" defconfig >/dev/null
cfg() { "$TREE/scripts/config" --file "$TREE/.config" "$@" || true; }
# ext5 compiles the whole ext4 object list, including crypto.c and verity.c,
# so the features behind them must be on.
for opt in EXT4_FS JBD2 FS_ENCRYPTION FS_ENCRYPTION_ALGS FS_VERITY \
           EXT4_FS_POSIX_ACL EXT4_FS_SECURITY QUOTA QFMT_V2 QUOTACTL UNICODE \
           VIRTIO VIRTIO_PCI VIRTIO_BLK VIRTIO_CONSOLE BLK_DEV_LOOP \
           MODULES MODULE_UNLOAD MODULE_FORCE_UNLOAD MODULE_SRCVERSION_ALL \
           MEMCG CGROUPS DEBUG_FS SERIAL_8250 SERIAL_8250_CONSOLE DEBUG_INFO_NONE; do
    cfg --enable "$opt"
done
cfg --disable MODULE_SIG_FORCE
cfg --disable SYSTEM_TRUSTED_KEYRING
if [ "$PROFILE" = debug ]; then
    for opt in BLK_DEV_INITRD DEBUG_KERNEL DEBUG_ATOMIC_SLEEP DEBUG_SPINLOCK PROVE_LOCKING; do
        cfg --enable "$opt"
    done
else
    for opt in NET_9P NET_9P_VIRTIO 9P_FS 9P_FS_POSIX_ACL VIRTIO_NET \
               HYPERVISOR_GUEST PARAVIRT KVM_GUEST PARAVIRT_SPINLOCKS \
               PERF_EVENTS DEVTMPFS DEVTMPFS_MOUNT TMPFS TMPFS_POSIX_ACL \
               AUTOFS_FS FHANDLE INOTIFY_USER XFS_FS BTRFS_FS F2FS_FS; do
        cfg --enable "$opt"
    done
    for opt in PROVE_LOCKING DEBUG_LOCK_ALLOC DEBUG_SPINLOCK DEBUG_ATOMIC_SLEEP \
               DEBUG_MUTEXES DEBUG_RWSEMS LOCK_STAT; do
        cfg --disable "$opt"
    done
fi
make -C "$TREE" olddefconfig >/dev/null
echo "  configured: $PROFILE profile"
[ "$CONFIG_ONLY" -eq 0 ] || { echo "stopping after configure as asked"; exit 0; }

say "3/4  build bzImage and modules (-j$JOBS)"
# `modules` writes Module.symvers, which out-of-tree module builds need.
make -C "$TREE" -j"$JOBS" bzImage modules

say "4/4  verify"
[ -s "$TREE/arch/x86/boot/bzImage" ] || die "no bzImage was produced"
[ -s "$TREE/Module.symvers" ] || die "no Module.symvers"
if [ "$PROFILE" = vm ] && grep -q '^CONFIG_PROVE_LOCKING=y' "$TREE/.config"; then
    die "the vm profile ended up with lockdep on; see $TREE/.config"
fi
printf '  bzImage  %s\n' "$TREE/arch/x86/boot/bzImage"
