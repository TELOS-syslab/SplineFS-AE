#!/bin/bash
# fetch.sh -- get the evaluation VM that scripts/vm/build.sh would build: the
# 6.16.5 kernel, SplineFS built for it, and the Ubuntu 24.04 image (1.4 GB).
#
#   bash scripts/vm/fetch.sh DIR|FILE
#
# Takes vm/splinefs-vm.tar.gz from DIR, a copy of our netdisk folder (link
# and password posted in HotCRP), or the package FILE itself.
# The image holds the public half of the SSH key in deploy/vm/id_ed25519,
# which the package includes; the VM's SSH port is bound to 127.0.0.1 only.
set -euo pipefail
AE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
read -r SUM NAME < "$AE_ROOT/scripts/vm/prebuilt.sha256"
OUT="$AE_ROOT/deploy/$(basename "$NAME")"
mkdir -p "$AE_ROOT/deploy"
SRC=${1:?usage: fetch.sh DIR|FILE, DIR a copy of the netdisk folder}
[ -d "$SRC" ] && SRC="$SRC/$NAME"
[ -f "$SRC" ] || { echo "no package at $SRC" >&2; exit 1; }
echo "copying $SRC"
cp "$SRC" "$OUT.part"
echo "$SUM  $OUT.part" | sha256sum -c --quiet - ||
    { echo "checksum mismatch: $SRC does not match scripts/vm/prebuilt.sha256" >&2
      rm -f "$OUT.part"; exit 1; }
tar -xSzf "$OUT.part" -C "$AE_ROOT/deploy"
rm -f "$OUT.part" "$OUT"
echo "ready: kernel, module and image in deploy/; next: make vm-run"
