#!/bin/bash
# run_cache_pressure.sh -- Fig. 9b/9c: cold lookups under a memcg cap.
# DESTRUCTIVE: formats DEV.
#
#   sudo env DEV=/dev/<disposable> CONFIRM_DESTROY=/dev/<disposable> \
#       bash scripts/campaigns/run_cache_pressure.sh
#
# Options:  MODE=positive|negative|both (default both)
#           REPS=<n>        repetitions per point, default 3
#           CAPS="..."      memcg caps in bytes
#           FSES="..."      default "splinefs naive ext4"; naive is one model
#                           per directory, as in Fig. 9a
#           LIST_FILE=...   the GUFI path list, by default the staged
#                           yellusers_2M.txt; empty for a synthetic tree
#           QUICK=1         tiny shape, mechanics only
set -euo pipefail

AE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$AE_ROOT/scripts/lib/wrapper.sh"

if [ -z "${LIST_FILE+x}" ]; then
    _cand="$AE_ROOT/datasets/staged/gufi/yellusers_2M.txt"
    [ -s "$_cand" ] && LIST_FILE="$_cand" || LIST_FILE=""
fi

MODE=${MODE:-both}
case "$MODE" in positive|negative|both) ;; *)
    echo "ERROR: MODE must be positive, negative, or both (got '$MODE')" >&2
    exit 1 ;;
esac
REPS=${REPS:-3}
CAPS=${CAPS:-"268435456 536870912 1073741824 2147483648"}
FSES=${FSES:-"splinefs naive ext4"}

if [ "${QUICK:-0}" = 1 ]; then
    ae_warn_quick
    export K=200 M=200 LOOKUPS=50000 SETTLE_OPS=50000 REPS=1
    CAPS="536870912"
    LIST_FILE=""
fi

ae_begin cache_pressure
ae_record_config "MODE=$MODE REPS=$REPS CAPS=$CAPS FSES='$FSES' LIST_FILE=${LIST_FILE:-none}"

if [ -n "$LIST_FILE" ]; then
    printf '\n  Workload: GUFI path list %s\n            (the tree Fig. 9b and 9c were measured on)\n' "$LIST_FILE"
else
    printf '\n  Workload: synthetic hardlink-pool tree.\n'
    printf '            Fig. 9b and 9c used a GUFI path list; without it the\n'
    printf '            256 MB parity point will not reproduce. See datasets/MANIFEST.md.\n'
fi

run_one() {
    # Assign separately: `local m=$1 out=${m}` reads an unset m.
    local mode=$1
    local out="$AE_OUT/${mode}.csv"
    ae_step "cache pressure, $mode lookups"
    # Negative lookups use the Bloom prefilter, off by default.
    local margs=""
    [ "$mode" = negative ] && margs="li_bloom_enabled=Y"
    env DEV="$DEV" CONFIRM_DESTROY="$CONFIRM_DESTROY" \
        REPS="$REPS" CAPS="$CAPS" FSES="$FSES" LOOKUP_MODE="$mode" LIST_FILE="$LIST_FILE" \
        MODULE_ARGS="$margs" OUT="$out" RAW="$AE_OUT/raw-$mode" \
        bash "$AE_ROOT/utils/evaluation/cache_pressure/run.sh"
    ae_have "$out"
}

case "$MODE" in
    positive) run_one positive ;;
    negative) run_one negative ;;
    both)     run_one positive; run_one negative ;;
esac

ae_end
