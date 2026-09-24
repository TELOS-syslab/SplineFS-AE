#!/bin/bash
# upstream_diff.sh -- what SplineFS changed in ext4 and jbd2.
#
#   bash scripts/upstream_diff.sh           new files, and lines added and
#                                           removed per changed file
#   bash scripts/upstream_diff.sh --full    the complete diff
#
# Compares src/ with fs/ext4, fs/jbd2 and include/ of Linux 6.16.5 after
# renaming ext4 to ext5 and jbd2 to jbd3.  Uses the kernel source in
# deploy/kernel/, and downloads it there first if it is missing (150 MB).
set -euo pipefail
AE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FULL=0
[ "${1:-}" = --full ] && FULL=1
find_tree() {
    ls -d "$AE_ROOT"/deploy/kernel/linux-6.16.5-vm "$AE_ROOT"/deploy/kernel/linux-6.16.5 \
        2>/dev/null | head -1 || true
}
K=$(find_tree)
if [ -z "$K" ]; then
    bash "$AE_ROOT/scripts/internal/fetch_kernel.sh" --source-only >&2
    K=$(find_tree)
fi
[ -n "$K" ] || { echo "no Linux 6.16.5 source under deploy/kernel/" >&2; exit 1; }
renamed() { sed 's/ext4/ext5/g; s/EXT4/EXT5/g; s/Ext4/Ext5/g; s/jbd2/jbd3/g; s/JBD2/JBD3/g; s/Jbd2/Jbd3/g' "$1"; }

upstream_of() {
    local rel=${1#src/} base
    case "$rel" in
    ext5/*) base="fs/ext4/$(basename "$rel" | sed 's/ext5/ext4/; s/jbd3/jbd2/')" ;;
    jbd3/*) base="fs/jbd2/$(basename "$rel")" ;;
    include/*) base=$(echo "include/${rel#include/}" | sed 's/ext5/ext4/g; s/jbd3/jbd2/g') ;;
    esac
    [ "$base" = include/linux/journal-head-jbd2.h ] && base=include/linux/journal-head.h
    [ -f "$K/$base" ] && echo "$K/$base"
}

cd "$AE_ROOT"
new=0 added=0 removed=0
for f in $(git ls-files 'src/*.c' 'src/*.h'); do
    up=$(upstream_of "$f" || true)
    if [ -z "$up" ]; then
        n=$(wc -l < "$f")
        new=$((new + n))
        [ "$FULL" = 1 ] || printf 'new      %6d lines  %s\n' "$n" "$f"
        continue
    fi
    if [ "$FULL" = 1 ]; then
        diff -u --label "upstream/${up#$K/}" --label "$f" <(renamed "$up") "$f" || true
        continue
    fi
    a=$(diff <(renamed "$up") "$f" | grep -c '^>' || true)
    r=$(diff <(renamed "$up") "$f" | grep -c '^<' || true)
    added=$((added + a)); removed=$((removed + r))
    [ $((a + r)) -eq 0 ] || printf 'changed  +%-5d -%-5d  %s\n' "$a" "$r" "$f"
done
[ "$FULL" = 1 ] || printf '\n%d lines in new files; +%d / -%d in files derived from Linux 6.16.5.\nFiles not listed are identical to upstream after renaming.\n' \
    "$new" "$added" "$removed"
