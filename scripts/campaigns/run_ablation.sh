#!/bin/bash
# run_ablation.sh -- Fig. 12a: what promotion and subtree sharing each buy.
# DESTRUCTIVE: formats DEV.
#
# The arms are policy modes of the same module:
#   mutable        li_policy_mode=0   promotion off
#   +promote       li_policy_mode=2   one model per directory
#   +subtree model li_policy_mode=3   one model shared across the subtree
#
# Workload: 2,000 directories of 1,000 entries, hardlinked to a small inode
# pool so only directory metadata varies.
#
#   sudo env DEV=/dev/<disposable> CONFIRM_DESTROY=/dev/<disposable> \
#       bash scripts/campaigns/run_ablation.sh
# Options: REPS, CAPS, K, M, SETTLE_OPS.
set -euo pipefail
AE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$AE_ROOT/scripts/lib/wrapper.sh"

REPS=${REPS:-3}
# Many small directories, the regime Fig. 12a is about.  The size floor is
# set below M so the per-directory arm can promote a 1,000-entry directory.
K=${K:-2000} M=${M:-1000} POOL=${POOL:-4096}
SIZE_FLOOR=${SIZE_FLOOR:-512}
# Untimed lookups before timing; the shared model can need over a million.
SETTLE_OPS=${SETTLE_OPS:-1500000}
CAPS=${CAPS:-"268435456 536870912 1073741824 2147483648 4294967296 8589934592"}

ae_begin ablation
ae_record_config "REPS=$REPS K=$K M=$M POOL=$POOL CAPS=$CAPS SIZE_FLOOR=$SIZE_FLOOR SETTLE_OPS=$SETTLE_OPS"

for arm in "mutable:0" "promote:2" "subtree:3"; do
    name=${arm%%:*}; mode=${arm##*:}
    ae_step "arm '$name' (li_policy_mode=$mode)"
    OUT="$AE_OUT/$name.csv"
    # The promotion-off arm is correct only if nothing promoted.
    expect_none=0
    [ "$mode" = 0 ] && expect_none=1
    require_root=0
    [ "$mode" = 3 ] && require_root=1
    env DEV="$DEV" CONFIRM_DESTROY="$CONFIRM_DESTROY" \
        REPS="$REPS" K="$K" M="$M" POOL="$POOL" CAPS="$CAPS" \
        SETTLE_OPS="$SETTLE_OPS" EVAL_REQUIRE_TREE_ROOT="$require_root" \
        EVAL_REQUIRE_PARENT_COUNT="$((require_root ? K + 2 : 0))" \
        EVAL_EXPECT_NO_PROMOTION="$expect_none" \
        MODULE_ARGS="li_policy_mode=$mode li_size_floor_entries=$SIZE_FLOOR" \
        OUT="$OUT" RAW="$AE_OUT/raw-$name" \
        bash "$AE_ROOT/utils/evaluation/cache_pressure/run.sh"
    ae_have "$OUT"
done
ae_end
