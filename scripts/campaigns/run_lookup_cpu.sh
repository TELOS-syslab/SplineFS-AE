#!/bin/bash
# run_lookup_cpu.sh -- Fig. 12b: CPU cycles and instructions per lookup
# after a dcache miss.  DESTRUCTIVE: formats DEV.
#
#   sudo env DEV=/dev/<disposable> CONFIRM_DESTROY=/dev/<disposable> \
#       bash scripts/campaigns/run_lookup_cpu.sh
#
# Options:  REPS=<n>     default 5
#           FILES=<n>    directory size, default 200000
#           LI_STATS=1   the module's per-lookup counters (they add overhead)
set -euo pipefail
AE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$AE_ROOT/scripts/lib/wrapper.sh"

REPS=${REPS:-5}
FILES=${FILES:-200000}
FSES=${FSES:-"splinefs ext4 xfs btrfs"}
LI_STATS=${LI_STATS:-0}
STATS=N; [ "$LI_STATS" = 1 ] && STATS=Y

ae_begin lookup_cpu
ae_record_config "REPS=$REPS FILES=$FILES FSES='$FSES' LI_STATS=$LI_STATS"

if [ "$LI_STATS" = 1 ]; then
    cat >&2 <<'MSG'
  LI_STATS=1: per-lookup counters are on; F12b cycles and instructions come
  from a run with LI_STATS=0.
MSG
fi

ae_step "warm page cache, cold dcache, CPU-pinned lookup loop"
OUT="$AE_OUT/index_cpu.csv"
env DEV="$DEV" CONFIRM_DESTROY="$CONFIRM_DESTROY" \
    REPS="$REPS" FILES="$FILES" LOOKUPS="$FILES" FSES="$FSES" \
    STATS_ENABLED="$STATS" OUT="$OUT" RAW="$AE_OUT/raw" \
    bash "$AE_ROOT/utils/evaluation/overhead/run.sh"
ae_have "$OUT"
ae_end
