#!/bin/bash
# run_realworld.sh -- Fig. 10: cold scans of real namespaces at a 2 GB cap,
# against ext4, xfs, btrfs and f2fs.  DESTRUCTIVE: formats DEV.
#
#   sudo env DEV=/dev/<disposable> CONFIRM_DESTROY=/dev/<disposable> \
#       bash scripts/campaigns/run_realworld.sh
#
# By default: the GUFI yellusers path list, and the recorded kernel `du` and
# ImageNet metadata scans.
# Options:  TREES="label:/path/to/pathlist ..."   path-list namespaces
#           TRACES="label:prep:scan:recroot ..." recorded-stream namespaces
#           IMAGENET=0                           leave out the ImageNet row
#           BIG=1                                add the four large GUFI trees
#           PROMOTE=force|auto, REPS, CAPS, FSES, QUICK=1
set -euo pipefail
AE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$AE_ROOT/scripts/lib/wrapper.sh"

REPS=${REPS:-3}
# force places one model at the wide fan-out point, as the paper's figure did.
PROMOTE=${PROMOTE:-force}
CAPS=${CAPS:-2147483648}
FSES=${FSES:-"splinefs ext4 xfs btrfs f2fs"}
G="$AE_ROOT/datasets/staged/gufi"
TR="$AE_ROOT/datasets/staged/traces"

# The four other GUFI trees (16.5 M entries each) take hours; BIG=1 adds them.
BIG=${BIG:-0}
if [ -z "${TREES+x}" ]; then
    TREES=""
    pairs="yellusers:$G/yellusers_sample.txt"
    if [ "$BIG" = 1 ]; then
        pairs="$pairs yellprojs:$G/yellprojs_sample.txt"
        pairs="$pairs anony:$G/anony_sample.txt"
        pairs="$pairs scr4:$G/scr4_sample.txt"
        pairs="$pairs ttscratch:$G/ttscratch_sample.txt"
        echo "BIG=1: including the four 16.5 M-entry trees; expect many hours." >&2
    fi
    for pair in $pairs; do
        [ -s "${pair##*:}" ] && TREES="$TREES $pair"
    done
    TREES=${TREES# }
    if [ "$BIG" != 1 ]; then
        for extra in yellprojs anony scr4 ttscratch; do
            if [ -s "$G/${extra}_sample.txt" ]; then
                echo "note: $G/${extra}_sample.txt is staged but deferred;" \
                     "set BIG=1 to include the 16.5 M-entry trees." >&2
                break
            fi
        done
    fi
fi

if [ -z "${TRACES+x}" ]; then
    TRACES=""
    # label:prepare:scan:recorded-root
    rows="kernel_du:$TR/realapp_kernel_prep.trace:$TR/realapp_kernel_du_scan.trace:/tmp/cp_kernel_dst"
    [ "${IMAGENET:-1}" = 0 ] ||
        rows="$rows imagenet_md:$TR/realapp_imagenet_md_prep.trace:$TR/realapp_imagenet_md_scan.trace:/tmp/realapp_mltrain_md_big"
    for t in $rows; do
        pt=${t#*:}; pt=${pt%%:*}
        [ -s "$pt" ] && TRACES="$TRACES $t"
    done
    TRACES=${TRACES# }
fi

if [ "${QUICK:-0}" = 1 ]; then
    ae_warn_quick
    REPS=1; FSES="splinefs ext4"
    TREES="yellusers:$G/yellusers_2M.txt"; TRACES=""
fi

# Exit 3 tells run_queue.sh to log SKIP rather than FAIL.
[ -n "$TREES$TRACES" ] ||
    { echo "No namespace is staged; skipping.  See datasets/MANIFEST.md" >&2; exit 3; }

ae_begin realworld
ae_record_config "REPS=$REPS CAPS=$CAPS FSES='$FSES' TREES='$TREES' TRACES='$TRACES' PROMOTE=$PROMOTE"

ae_step "cold whole-tree scans, recorded order replayed on every filesystem"
OUT="$AE_OUT/realworld.csv"
env DEV="$DEV" CONFIRM_DESTROY="$CONFIRM_DESTROY" \
    REPS="$REPS" CAPS="$CAPS" FSES="$FSES" TREES="$TREES" TRACES="$TRACES" \
    PROMOTE="$PROMOTE" \
    OUT="$OUT" RAW="$AE_OUT/raw" \
    bash "$AE_ROOT/utils/evaluation/realworld/run.sh"
ae_have "$OUT"
ae_end
