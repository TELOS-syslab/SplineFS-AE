#!/bin/bash
# run_locality_sweep.sh -- Fig. 14: adaptive selection as churn spreads.
# DESTRUCTIVE: formats DEV.
#
# Shape: 128 regions of 64 directories of 128 names (about 1 M names); 600 K
# warmup and 400 K measured lookups, one create-unlink pair per 16 lookups,
# at a 512 MB cap.
#
#   sudo env DEV=/dev/<disposable> CONFIRM_DESTROY=/dev/<disposable> \
#       bash scripts/campaigns/run_locality_sweep.sh
# Options: REPS, FRACTIONS, WARMUP, MEASURE.
set -euo pipefail
AE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$AE_ROOT/scripts/lib/wrapper.sh"

REPS=${REPS:-3}
FRACTIONS=${FRACTIONS:-"0 5 10 25 50 75 100"}
REGIONS=${REGIONS:-128} LEAVES=${LEAVES:-64} ENTRIES=${ENTRIES:-128}
WARMUP=${WARMUP:-600000} MEASURE=${MEASURE:-400000} GAP=${GAP:-16}
CAP=${CAP:-536870912}

if [ "${QUICK:-0}" = 1 ]; then
    ae_warn_quick
    REPS=1; FRACTIONS="0 50 100"; REGIONS=8; LEAVES=8; ENTRIES=64
    WARMUP=2000; MEASURE=1500
fi

# The bench needs warmup + measure <= regions x leaves x entries.
tree=$((REGIONS * LEAVES * ENTRIES))
if [ $((WARMUP + MEASURE)) -gt "$tree" ]; then
    ae_die "WARMUP+MEASURE=$((WARMUP + MEASURE)) exceeds the tree, REGIONS x LEAVES x ENTRIES = $tree; shrink the operation counts with the tree"
fi

ae_begin locality_sweep
ae_record_config "REPS=$REPS FRACTIONS='$FRACTIONS' REGIONS=$REGIONS LEAVES=$LEAVES ENTRIES=$ENTRIES WARMUP=$WARMUP MEASURE=$MEASURE GAP=$GAP CAP=$CAP"

ae_step "churn sweep under a $((CAP/1024/1024)) MB cap"
OUT="$AE_OUT/locality_sweep.csv"
env DEV="$DEV" CONFIRM_DESTROY="$CONFIRM_DESTROY" \
    REPS="$REPS" FRACTIONS="$FRACTIONS" \
    REGIONS="$REGIONS" LEAVES="$LEAVES" ENTRIES="$ENTRIES" \
    WARMUP="$WARMUP" MEASURE="$MEASURE" GAP="$GAP" CAP="$CAP" \
    RUN_ID="$(basename "$AE_OUT")" \
    OUT="$OUT" RAW="$AE_OUT/raw" \
    bash "$AE_ROOT/utils/evaluation/locality_sweep/run_sweep.sh"

python3 "$AE_ROOT/utils/evaluation/locality_sweep/analyze_sweep.py" \
    "$OUT" "$AE_OUT/summary.md" || true
ae_have "$OUT"
ae_end
