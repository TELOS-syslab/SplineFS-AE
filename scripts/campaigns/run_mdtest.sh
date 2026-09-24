#!/bin/bash
# run_mdtest.sh -- Fig. 13: mdtest create, stat, read and remove rates
# across file counts and memcg caps.  DESTRUCTIVE: formats DEV.
#
#   sudo env DEV=/dev/<disposable> CONFIRM_DESTROY=/dev/<disposable> \
#       bash scripts/campaigns/run_mdtest.sh
#
# Options:  COUNTS, CAPS, REPS, FSES, QUICK=1
#           PROMOTE=force (default): promote between create and stat, outside
#           the timers, as the paper's figure did; auto: the policy decides
set -euo pipefail
AE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$AE_ROOT/scripts/lib/wrapper.sh"

REPS=${REPS:-3}
COUNTS=${COUNTS:-"100000 200000 400000 800000 1000000"}
CAPS=${CAPS:-"536870912 1073741824 2147483648 4294967296 8589934592"}
FSES=${FSES:-"splinefs ext4"}
PROMOTE=${PROMOTE:-force}

if [ "${QUICK:-0}" = 1 ]; then
    ae_warn_quick
    REPS=1; COUNTS="100000"; CAPS="2147483648"
fi

ae_begin mdtest
ae_record_config "REPS=$REPS COUNTS='$COUNTS' CAPS='$CAPS' FSES='$FSES' PROMOTE=$PROMOTE"

ae_step "mdtest create / stat / read / remove across counts and caps"
OUT="$AE_OUT/mdtest.csv"
env DEV="$DEV" CONFIRM_DESTROY="$CONFIRM_DESTROY" \
    REPS="$REPS" COUNTS="$COUNTS" CAPS="$CAPS" FSES="$FSES" PROMOTE="$PROMOTE" \
    OUT="$OUT" RAW="$AE_OUT/raw" \
    bash "$AE_ROOT/utils/evaluation/mdtest/sweep.sh"
ae_have "$OUT"
ae_end
