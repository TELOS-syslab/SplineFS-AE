#!/usr/bin/env bash
# stage_linux.sh -- stage the inputs derived from a Linux source tree.
#
#   bash datasets/stage_linux.sh
#
# Writes, from the Linux 6.16.5 tarball in deploy/kernel/ (downloaded first
# if missing):
#   staged/realapp_data/linux-src      the source tree (footprint, kernel)
#   staged/realapp_data/linux_src.tar  the tree as one uncompressed tar
#                                      (applookup, A3 tar)
# The paper used Linux 6.6.1; any recent tree has the same shape.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
AE_ROOT=$(cd "$HERE/.." && pwd)
TARBALL=$AE_ROOT/deploy/kernel/linux-6.16.5.tar.xz
OUT=$HERE/staged/realapp_data

[ -s "$TARBALL" ] || bash "$AE_ROOT/scripts/internal/fetch_kernel.sh" --source-only
mkdir -p "$OUT"
if [ ! -d "$OUT/linux-src" ]; then
    tmp=$(mktemp -d "$OUT/.extract.XXXXXX")
    tar -xJf "$TARBALL" -C "$tmp"
    mv "$tmp/linux-6.16.5" "$OUT/linux-src"
    rmdir "$tmp"
fi
[ -s "$OUT/linux_src.tar" ] || {
    xz -dc "$TARBALL" > "$OUT/linux_src.tar.part"
    mv "$OUT/linux_src.tar.part" "$OUT/linux_src.tar"
}
du -sh "$OUT/linux-src" "$OUT/linux_src.tar"
