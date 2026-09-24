#!/usr/bin/env bash
# fetch_data.sh -- stage the data the paper's runs used in datasets/staged/,
# for evaluation on your own machine.
#
#   bash datasets/fetch_data.sh DIR [PREFIX...]
#
# DIR is a copy of our netdisk folder (link and password posted in HotCRP);
# files missing from DIR are skipped.  PREFIX selects entries of data.list,
# such as gufi/ or traces/; without one, everything is staged (23.5 GB).
# Each file is checked against data.list, and archives are unpacked where the
# campaigns look.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
FROM=$(cd "${1:?usage: fetch_data.sh DIR [PREFIX...], DIR a copy of the netdisk folder}" && pwd)
shift
mkdir -p "$HERE/staged"
cd "$HERE/staged"

while read -r sum path act; do
    case "$sum" in ''|\#*) continue ;; esac
    if [ $# -gt 0 ]; then
        hit=0
        for p in "$@"; do case "$path" in "$p"*) hit=1 ;; esac; done
        [ "$hit" = 1 ] || continue
    fi
    target=${act#*:}
    stamp=".fetched/$sum"
    if [ -e "$stamp" ]; then
        echo "have $path"
        continue
    fi
    mkdir -p "$(dirname "$path")" .fetched
    if [ ! -f "$FROM/$path" ]; then
        echo "skip $path (not in $FROM)"
        continue
    fi
    echo "copying $path"
    cp "$FROM/$path" "$path.part"
    echo "$sum  $path.part" | sha256sum -c --quiet - ||
        { echo "checksum mismatch on $path; download it again" >&2; rm -f "$path.part"; exit 1; }
    mv "$path.part" "$path"
    case "$act" in
    untar:*)
        echo "  unpacking into $target"
        rm -rf "$target"
        mkdir -p "$target"
        tar -xf "$path" -C "$target"
        rm -f "$path" ;;
    gunzip:*)
        echo "  decompressing to $target"
        mkdir -p "$(dirname "$target")"
        gzip -dc "$path" > "$target.part"
        mv "$target.part" "$target"
        rm -f "$path" ;;
    esac
    touch "$stamp"
done < "$HERE/data.list"
