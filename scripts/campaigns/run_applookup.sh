#!/bin/bash
# run_applookup.sh -- Fig. 11: cold, metadata-bound stages of real
# applications, timed on their real datasets.  DESTRUCTIVE: formats DEV.
#
#   A1 container    python:3.12 + node:20 rootfs scan at startup
#   A2 DataLoader   ImageNet subset, dataset indexing before training
#   A3 tar          archive pass over the Linux kernel tree
#   A4 SQLite       per-app databases and caches, opened at launch
#   A5 DuckDB       partitioned Parquet lake, scanned at query start
#   A6 LlamaIndex   chunk store, loaded at startup
#
#   sudo env DEV=/dev/<disposable> CONFIRM_DESTROY=/dev/<disposable> \
#       bash scripts/campaigns/run_applookup.sh [A1 A3 ...]
#
# With no argument every staged application runs; one whose dataset is
# missing is skipped.  Options: REPS, RUN_REPEATS (cold samples per
# repetition, default 10), CAPS, FSES, PY.
set -euo pipefail
AE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$AE_ROOT/scripts/lib/wrapper.sh"

REPS=${REPS:-3}
RUN_REPEATS=${RUN_REPEATS:-10}
CAPS=${CAPS:-2147483648}
FSES=${FSES:-"splinefs ext4 xfs btrfs f2fs"}
DATA="$AE_ROOT/datasets/staged/realapp_data"
APPS_DIR="$AE_ROOT/utils/evaluation/application/apps"
APPS=${*:-"A1 A2 A3 A4 A5 A6"}

# A2, A4, A5 and A6 need a Python with torch, pyarrow, duckdb and llama-index.
pick_python() {
    local cand home
    # PY, then the Python of utils/evaluation/application/install_python.sh.
    for cand in "${PY:-}" /opt/splinefs-ae-py/bin/python; do
        [ -n "$cand" ] && [ -x "$(command -v "$cand")" ] || continue
        if "$cand" -c 'import pyarrow, duckdb, llama_index.core' 2>/dev/null; then
            echo "$cand"; return 0
        fi
    done
    # Under sudo, also search the invoking user's home.
    for home in "$HOME" "$(getent passwd "${SUDO_USER:-${USER:-}}" 2>/dev/null | cut -d: -f6)"; do
        [ -n "$home" ] || continue
        for cand in "$home"/{anaconda3,miniconda3,miniforge3,mambaforge}/envs/*/bin/python \
                    "$home"/{anaconda3,miniconda3,miniforge3,mambaforge}/bin/python; do
            [ -x "$cand" ] || continue
            if "$cand" -c 'import pyarrow, duckdb, llama_index.core' 2>/dev/null; then
                echo "$cand"; return 0
            fi
        done
    done
    for cand in python3 python \
                /opt/conda/envs/*/bin/python /opt/conda/bin/python; do
        [ -n "$cand" ] || continue
        command -v "$cand" >/dev/null 2>&1 || [ -x "$cand" ] || continue
        if "$cand" -c 'import pyarrow, duckdb, llama_index.core' 2>/dev/null; then
            echo "$cand"; return 0
        fi
    done
    return 1
}
PY=$(pick_python) || {
    PY=python3
    kept=""
    for a in $APPS; do
        case "$a" in A2|A4|A5|A6) ;; *) kept="$kept $a" ;; esac
    done
    [ "${kept# }" = "$APPS" ] ||
        echo "No Python with pyarrow, duckdb and llama_index.core: running only${kept:- nothing}; run utils/evaluation/application/install_python.sh or set PY for A2, A4, A5, A6." >&2
    APPS=${kept# }
}
# Exit 3 tells run_queue.sh to log SKIP rather than FAIL.
[ -n "$APPS" ] || exit 3
echo "using python: $PY"

# A4 with 12,000 apps (252,000 files), A5 with a 100,000-file lake.
NAPPS=${NAPPS:-12000}; ROWS=${ROWS:-200}; CACHE=${CACHE:-20}
LAKE_FILES=${LAKE_FILES:-100000}

ae_begin applookup
export SWAP_MAX=${SWAP_MAX:-max}
ae_record_config "REPS=$REPS RUN_REPEATS=$RUN_REPEATS CAPS=$CAPS FSES='$FSES' APPS='$APPS' PY=$PY NAPPS=$NAPPS ROWS=$ROWS CACHE=$CACHE LAKE_FILES=$LAKE_FILES SWAP_MAX=$SWAP_MAX swap_kb=$(awk '$1=="SwapTotal:" {print $2}' /proc/meminfo)"

# stage <id> <label> <needs> <STAGE_CMD> <ADAPT_CMD> <RUN_CMD>
stage() {
    local id=$1 label=$2 needs=$3 stage_cmd=$4 adapt_cmd=$5 run_cmd=$6
    if [ -n "$needs" ] && [ ! -e "$needs" ]; then
        printf '\n  skip %s (%s): missing %s\n' "$id" "$label" "$needs"
        printf '       see datasets/MANIFEST.md to stage or rebuild it\n'
        return 0
    fi
    ae_step "$id $label"
    AE_RAN_STAGE=1
    local out="$AE_OUT/${id}_${label}.csv"
    # A failed stage is recorded and the next application runs.
    if env DEV="$DEV" CONFIRM_DESTROY="$CONFIRM_DESTROY" \
        REPS="$REPS" RUN_REPEATS="$RUN_REPEATS" CAPS="$CAPS" FSES="$FSES" \
        WORKLOAD="$label" \
        STAGE_CMD="$stage_cmd" ADAPT_CMD="$adapt_cmd" RUN_CMD="$run_cmd" \
        OUT="$out" RAW="$AE_OUT/raw-$id" \
        bash "$AE_ROOT/utils/evaluation/application/run_stage.sh"; then
        ae_have "$out"
    else
        printf '\n  %s (%s) FAILED, continuing with the next application.\n' \
            "$id" "$label"
        printf '     A stage killed at the memcg cap is a property of that\n'
        printf '     workload at that cap, not of the filesystem; see\n'
        printf '     %s\n' "$AE_OUT/raw-$id"
        AE_FAILED_STAGES="${AE_FAILED_STAGES:-} $id"
    fi
}

for app in $APPS; do
case "$app" in
A1) # Container rootfs scan at startup.
    stage A1 container "$DATA/container_rootfs.tar" \
        'tar -xf '"$DATA"'/container_rootfs.tar -C "$TREE"' \
        'find "$TREE" -mindepth 1 -printf ""' \
        'find "$TREE" -mindepth 1 -printf ""' ;;
A2) # PyTorch DataLoader indexing the dataset before training.
    stage A2 dataloader "$DATA/imagenet_subset_1400x250.tar" \
        'tar -xf '"$DATA"'/imagenet_subset_1400x250.tar -C "$TREE"' \
        "$PY $APPS_DIR/dataloader_index.py \"\$TREE\"" \
        "$PY $APPS_DIR/dataloader_index.py \"\$TREE\"" ;;
A3) # GNU tar archive pass over the kernel tree.
    stage A3 tar "$DATA/linux_src.tar" \
        'tar -xf '"$DATA"'/linux_src.tar -C "$TREE"' \
        'tar -cf /dev/null -C "$TREE" .' \
        'tar -cf /dev/null -C "$TREE" .' ;;
A4) # Real SQLite databases opened at launch across many app directories.
    stage A4 sqlite "" \
        "$PY $APPS_DIR/mobile_sqlite.py build \"\$TREE\" $NAPPS $ROWS $CACHE" \
        "$PY $APPS_DIR/mobile_sqlite.py run \"\$TREE\"" \
        "$PY $APPS_DIR/mobile_sqlite.py run \"\$TREE\"" ;;
A5) # DuckDB cold scan over a partitioned Parquet lake.
    stage A5 duckdb "$DATA/lake_${LAKE_FILES}.tar" \
        'tar -xf '"$DATA"'/lake_'"$LAKE_FILES"'.tar -C "$TREE"' \
        "$PY $APPS_DIR/duckdb_lake.py run \"\$TREE/lake\"" \
        "$PY $APPS_DIR/duckdb_lake.py run \"\$TREE/lake\"" ;;
A6) # LlamaIndex SimpleDirectoryReader loading the chunk store.
    stage A6 llamaindex "$DATA/rag_chunks.tar" \
        'mkdir -p "$TREE/store" && tar -xf '"$DATA"'/rag_chunks.tar -C "$TREE/store"' \
        "$PY $APPS_DIR/rag_llamaindex.py \"\$TREE/store\"" \
        "$PY $APPS_DIR/rag_llamaindex.py \"\$TREE/store\"" ;;
*) ae_die "unknown application '$app'; expected A1..A6" ;;
esac
done
[ -n "${AE_RAN_STAGE:-}" ] ||
    { echo "No application input is staged; skipping.  See datasets/MANIFEST.md" >&2; exit 3; }

[ -z "${AE_FAILED_STAGES:-}" ] ||
    printf '\nStages that failed and were skipped:%s\n' "$AE_FAILED_STAGES"
ae_end
