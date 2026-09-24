#!/bin/bash
# run_footprint.sh -- Fig. 9a: directory metadata across namespaces.
# DESTRUCTIVE: formats DEV.
#
#   sudo env DEV=/dev/<disposable> CONFIRM_DESTROY=/dev/<disposable> \
#       bash scripts/campaigns/run_footprint.sh
#
# Options:  REPS=<n>        default 3
#           SHAPES="K:M .." synthetic shapes
#           REALDIRS=...    "label:/path" pairs; default: the staged real trees
#           LISTS=...       "label:/list" pairs; default: yellusers_2M.txt
#           PROMOTE=force|auto
#           QUICK=1         tiny shape, mechanics only
set -euo pipefail
AE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$AE_ROOT/scripts/lib/wrapper.sh"

REPS=${REPS:-3}
# Fig. 9a measures a promoted subtree, so promotion is forced by default.
PROMOTE=${PROMOTE:-force}
SHAPES=${SHAPES:-"1:1000000 10:100000 100:10000 1000:1000 10000:100 100000:10"}

# ${VAR+x} tells unset from empty: REALDIRS= and LISTS= leave real trees out.
# By default, whatever real trees are staged.
REAL_CHOSEN=${REALDIRS+x}${LISTS+x}
if [ -z "${REALDIRS+x}" ]; then
    REALDIRS=""
    D="$AE_ROOT/datasets/staged/realapp_data"
    for pair in "apks:$D/apks" "corpus:$D/corpus" \
                "kernel:$D/linux-src" "repos:$D/repos_large"; do
        [ -d "${pair##*:}" ] && REALDIRS="$REALDIRS ${pair}"
    done
    REALDIRS=${REALDIRS# }
fi

if [ -z "${LISTS+x}" ]; then
    _g="$AE_ROOT/datasets/staged/gufi/yellusers_2M.txt"
    [ -s "$_g" ] && LISTS="yellusers:$_g" || LISTS=""
fi

# QUICK comes last so it overrides every default above.
if [ "${QUICK:-0}" = 1 ]; then
    ae_warn_quick
    REPS=1; SHAPES="10:2000 100:200"; REALDIRS=""; LISTS=""
fi

ae_begin footprint
ae_record_config "REPS=$REPS SHAPES='$SHAPES' REALDIRS='$REALDIRS' LISTS='$LISTS' PROMOTE=$PROMOTE"

if [ -z "$REALDIRS$LISTS" ]; then
    if [ -n "$REAL_CHOSEN" ]; then
        printf '\n  Real trees are left out (REALDIRS and LISTS are empty); synthetic shapes only.\n'
    else
        printf '\n  No real trees staged; synthetic shapes only.  See datasets/MANIFEST.md.\n'
    fi
fi

ae_step "directory metadata across shapes and real trees"
OUT="$AE_OUT/footprint.csv"
env DEV="$DEV" CONFIRM_DESTROY="$CONFIRM_DESTROY" \
    REPS="$REPS" SHAPES="$SHAPES" REALDIRS="$REALDIRS" LISTS="$LISTS" \
    PROMOTE="$PROMOTE" \
    OUT="$OUT" RAW="$AE_OUT/raw" \
    bash "$AE_ROOT/utils/evaluation/footprint/run.sh"
ae_have "$OUT"
ae_end
