#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Repeatedly cross both representation boundaries and verify exact namespace
# set equality. This catches stale delta replay and mutable-count handoff bugs
# that a single promote/compact/demote cycle cannot expose.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(mktemp -d /tmp/splinefs-repeat-bounce.XXXXXX)

cleanup()
{
	set +e
	find "$OUT" -maxdepth 1 -type f -delete
	rmdir "$OUT" 2>/dev/null || true
}
trap cleanup EXIT

BASE_FILES=8192 TV_OPS=10000 QUIET_OPS=11024 \
	PROMOTION_RETRY_OPS=1024 BUILD_PERMILLE=16 COMPACT_IDLE_MS=0 \
	DEMOTE_IDLE_MS=0 PROMOTE_IDLE_MS=0 REFILL_RECORDS=1100 CYCLES=4 \
	JOURNAL=0 IMAGE_SIZE=8G RESET_AFTER_DEMOTE=1 VERIFY_EACH_RESET=1 \
	SCENARIOS=adversarial OUT_DIR="$OUT" \
	"$ROOT/utils/evaluation/reviewer/flipflop_loop.sh" >/dev/null

[ "$(awk '$1=="promotion_publications" {print $2}' \
	"$OUT/adversarial.final.stats")" -eq 5 ]
[ "$(awk '$1=="compaction_publications" {print $2}' \
	"$OUT/adversarial.final.stats")" -eq 4 ]
[ "$(awk '$1=="full_demotion_publications" {print $2}' \
	"$OUT/adversarial.final.stats")" -eq 4 ]
[ "$(awk -F= '$1=="verified_absent" {print $2}' \
	"$OUT/adversarial.normalization-absent.out")" -eq 8800 ]
[ "$(awk -F= '$1=="verified_base" {print $2}' \
	"$OUT/adversarial.base-readdir.out")" -eq 8192 ]
[ "$(awk -F, 'END {print $2}' "$OUT/transition_cycles.csv")" -eq 8192 ]

trap - EXIT
cleanup
echo "SplineFS repeated boundary/log-terminator lifecycle: PASS"
