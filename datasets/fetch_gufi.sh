#!/usr/bin/env bash
# fetch_gufi.sh -- download the LANL GUFI metadata corpus (10.7 GB,
# LA-UR-21-21017) and derive the path lists the experiments use.
#
#   bash fetch_gufi.sh [--all] [DEST]      default DEST: ./staged/gufi
#   GUFI_PROXY=http://host:port            an HTTP proxy, if the FTP server is
#                                          unreachable directly
#
# Writes one "path d|f" line per entry:
#   yellusers.txt         14,851,827 entries (the full tree)
#   yellusers_2M.txt      first 2,000,000 lines (footprint, cache_pressure)
#   yellusers_sample.txt  first 1,485,000 lines (realworld)
# --all also writes {yellprojs,anony,scr4,ttscratch}_sample.txt, 16,500,000
# lines each, for realworld with BIG=1.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ALL=0
[ "${1:-}" = --all ] && { ALL=1; shift; }
DEST=${1:-$HERE/staged/gufi}
URL=${GUFI_URL:-ftp://hpc-ftp.lanl.gov/data/storage/GUFI/GUFITraces.tar.bz2}
SHA256=9f0e8153e0deba2d3286e85209c3518f4aa9f775be73b114b20bbdf3ba912c25
TARBALL=$DEST/GUFITraces.tar.bz2

mkdir -p "$DEST"
if [ -s "$TARBALL" ]; then
    echo "already present: $TARBALL"
else
    echo "downloading $URL (10.7 GB)"
    echo "If the URL has moved, set GUFI_URL or copy the archive to $TARBALL."
    proxy=()
    [ -z "${GUFI_PROXY:-}" ] || proxy=(-x "$GUFI_PROXY" --proxytunnel)
    curl -fL --retry 3 -C - "${proxy[@]}" -o "$TARBALL.part" "$URL"
    mv "$TARBALL.part" "$TARBALL"
fi
sum=$(sha256sum "$TARBALL" | cut -d' ' -f1)
[ "$sum" = "$SHA256" ] ||
    echo "warning: sha256 $sum differs from the paper's copy ($SHA256)" >&2

# Stream one snapshot out of the archive.
snapshot() {
    echo "converting $1 (decompressing the archive, 20-40 minutes)" >&2
    tar -xjOf "$TARBALL" --wildcards "*$1"
}

snapshot yellusers | awk -F'|' '{print $1, ($2 == "f" ? "f" : "d")}' \
    > "$DEST/yellusers.txt.part"
mv "$DEST/yellusers.txt.part" "$DEST/yellusers.txt"
head -n 2000000 "$DEST/yellusers.txt" > "$DEST/yellusers_2M.txt"
head -n 1485000 "$DEST/yellusers.txt" > "$DEST/yellusers_sample.txt"

if [ "$ALL" = 1 ]; then
    snapshot yellprojs |
        awk -F'|' 'n < 16500000 {print $1, ($2 == "f" ? "f" : "d"); n++}' \
        > "$DEST/yellprojs_sample.txt"
    for s in anony scr4 ttscratch; do
        snapshot "$s" |
            awk -F'|' '$1 == "" || $2 == "l" {next} n < 16500000 {print "/" $1, $2; n++}' \
            > "$DEST/${s}_sample.txt"
    done
fi
wc -l "$DEST"/*.txt
