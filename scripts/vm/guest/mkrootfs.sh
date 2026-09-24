#!/bin/bash
# mkrootfs.sh IMAGE PUBKEY -- build the Ubuntu 24.04 root image of the
# evaluation VM.  Run as root (debootstrap, loop mount, chroot); needs network.
#
#   VM_MIRROR     Ubuntu mirror (default http://archive.ubuntu.com/ubuntu)
#   VM_ROOTFS_GB  image size in GiB (default 16, sparse)
set -euo pipefail
IMG=$1
PUBKEY=$2
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MIRROR=${VM_MIRROR:-http://archive.ubuntu.com/ubuntu}
SIZE_GB=${VM_ROOTFS_GB:-16}
PKGS=systemd-sysv,udev,kmod,iproute2,openssh-server,ca-certificates,wget,rsync,git
PKGS=$PKGS,e2fsprogs,xfsprogs,btrfs-progs,f2fs-tools,util-linux,procps,psmisc,msr-tools
PKGS=$PKGS,python3,python3-venv,build-essential,autoconf,automake,libtool,pkg-config
PKGS=$PKGS,openmpi-bin,libopenmpi-dev

[ "$(id -u)" = 0 ] || { echo "mkrootfs.sh must run as root" >&2; exit 1; }
command -v debootstrap >/dev/null || { echo "install debootstrap first" >&2; exit 1; }
[ -s "$PUBKEY" ] || { echo "no public key at $PUBKEY" >&2; exit 1; }

MNT=$(mktemp -d)
cleanup() {
    for m in dev/pts dev sys proc; do
        mountpoint -q "$MNT/$m" && umount "$MNT/$m"
    done
    mountpoint -q "$MNT" && umount "$MNT"
    rmdir "$MNT"
}
trap cleanup EXIT

rm -f "$IMG.part"
truncate -s "${SIZE_GB}G" "$IMG.part"
mkfs.ext4 -q -F -L splinefs-root "$IMG.part"
mount -o loop "$IMG.part" "$MNT"

echo "debootstrap noble from $MIRROR (several minutes)"
debootstrap --arch amd64 --variant=minbase --components=main,universe \
    noble "$MNT" "$MIRROR"

mount -t proc proc "$MNT/proc"
mount -t sysfs sys "$MNT/sys"
mount --bind /dev "$MNT/dev"
mount --bind /dev/pts "$MNT/dev/pts"
cp /etc/resolv.conf "$MNT/etc/resolv.conf"

install -D -m 0755 "$HERE/splinefs-ae-run" "$MNT/usr/local/sbin/splinefs-ae-run"
install -D -m 0644 "$HERE/splinefs-ae.service" "$MNT/etc/systemd/system/splinefs-ae.service"
install -D -m 0755 "$HERE/../../../utils/evaluation/application/install_python.sh" \
    "$MNT/tmp/install_python.sh"
install -d -m 0700 "$MNT/root/.ssh"
install -m 0600 "$PUBKEY" "$MNT/root/.ssh/authorized_keys"
MIRROR="$MIRROR" PKGS="${PKGS//,/ }" chroot "$MNT" /bin/bash -s < "$HERE/setup-chroot.sh"

rm -f "$MNT/etc/resolv.conf"
cleanup
trap - EXIT
mv "$IMG.part" "$IMG"
echo "root image ready: $IMG"
