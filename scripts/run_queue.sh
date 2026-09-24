#!/bin/bash
# run_queue.sh -- run the evaluation campaigns.  DESTRUCTIVE: formats DEV.
#
#   sudo env DEV=/dev/X CONFIRM_DESTROY=/dev/X bash scripts/run_queue.sh
#       on a terminal, a menu: pick a mode and campaigns, or resume
#   ... run_queue.sh --tier minor|medium|full      no menu
#   ... run_queue.sh --tier minor footprint mdtest  those campaigns only
#   ... run_queue.sh --reps 3      repetitions per cell (default 1)
#   ... run_queue.sh --resume      rerun what the last queue did not finish
#   ... run_queue.sh --dry-run     print the plan only
#
# Ctrl-C pauses the queue; --resume continues it.
set -uo pipefail
AE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RESULTS="${AE_RESULTS:-$AE_ROOT/results}"
LOGS="$RESULTS/logs"
LEDGER="$LOGS/ledger.txt"

DRY=0
RESUME=0
TIER=${TIER:-}
REPS_OPT=${REPS:-}
ALL="lookup_cpu footprint cache_pressure realworld ablation locality_sweep mdtest applookup"
ARGS=()          # bash 4.3 errors on ${ARGS[*]} for an empty array under set -u
while [ $# -gt 0 ]; do
    case "$1" in
    --dry-run) DRY=1; shift ;;
    --resume)  RESUME=1; shift ;;
    --tier) TIER=$2; shift 2 ;;
    --tier=*) TIER=${1#--tier=}; shift ;;
    --reps) REPS_OPT=$2; shift 2 ;;
    --reps=*) REPS_OPT=${1#--reps=}; shift ;;
    -h|--help) sed -n '2,/^[^#]/{/^#/p}' "$0"; exit 0 ;;
    *) ARGS+=("$1"); shift ;;
    esac
done

# --- what the last queue left unfinished ----------------------------------
# Anything in the last queue's plan without a DONE is unfinished.
LAST_TIER="" LAST_REPS="" LAST_PLAN="" LAST_LEFT="" LAST_WHY=""
if [ -f "$LEDGER" ]; then
    hdr=$(grep -n '^=== queue started' "$LEDGER" | tail -1)
    if [ -n "$hdr" ]; then
        line=${hdr%%:*}
        LAST_TIER=$(sed -n "${line}p" "$LEDGER" | sed -n 's/.* tier=\([^ ]*\).*/\1/p')
        LAST_REPS=$(sed -n "${line}p" "$LEDGER" | sed -n 's/.* reps=\([0-9]*\).*/\1/p')
        LAST_PLAN=$(sed -n "${line}p" "$LEDGER" | sed -n 's/.* plan=\([^ ]*\).*/\1/p' | tr ',' ' ')
        done_set=$(tail -n +"$line" "$LEDGER" | awk '$3=="DONE"||$3=="SKIP"{print $2}')
        for c in $LAST_PLAN; do
            echo " $done_set " | grep -q " $c " && continue
            LAST_LEFT="$LAST_LEFT $c"
            why=$(tail -n +"$line" "$LEDGER" | awk -v c="$c" '$2==c && ($3=="FAIL"||$3=="PAUSED"){s=$3" "$4} END{print s}')
            LAST_WHY="$LAST_WHY
    $(printf '%-16s %s' "$c" "${why:-not reached}")"
        done
        LAST_LEFT=${LAST_LEFT# }
    fi
fi

# --- menu -----------------------------------------------------------------
# Prompts go to stderr; stdout carries only the chosen names.
pick_campaigns() {
    local i=1 c sel out=""
    echo >&2
    for c in $ALL; do printf '    %d) %s\n' "$i" "$c" >&2; i=$((i + 1)); done
    printf '\n  Campaigns, by number (e.g. "1 3 5"), or "all": ' >&2
    read -r sel
    [ "$sel" = all ] && { echo "$ALL"; return; }
    for n in $sel; do
        c=$(echo "$ALL" | tr ' ' '\n' | sed -n "${n}p")
        [ -n "$c" ] && out="$out $c"
    done
    echo "${out# }"
}

if [ -z "$TIER" ] && [ "${#ARGS[@]}" -eq 0 ] && [ "$RESUME" = 0 ] && [ -t 0 ]; then
    printf '\n  SplineFS artifact evaluation\n'
    if [ -n "$LAST_LEFT" ]; then
        printf '\n  The last queue (tier %s) stopped with %d unfinished:%s\n' \
            "$LAST_TIER" "$(echo $LAST_LEFT | wc -w)" "$LAST_WHY"
        printf '\n    r) resume it\n'
    fi
    cat <<'MENU'

    1) minor    ~30 min   a few workloads per campaign: checks that everything works
    2) medium   ~50 min   every claim, fewer workloads than the paper  (recommended)
    3) full     ~2.2 h    every campaign and workload
    4) pick campaigns, then a tier

  Times are for 1 repetition, the default; --reps 3 takes three times as long.
    q) quit

MENU
    [ -n "$REPS_OPT" ] &&
        printf '  Repetitions: %s for every campaign, from --reps or REPS.\n\n' "$REPS_OPT"
    printf '  Choice: '
    read -r choice
    case "$choice" in
    r|R) [ -n "$LAST_LEFT" ] || { echo "  nothing to resume"; exit 1; }; RESUME=1 ;;
    1) TIER=minor ;;
    2) TIER=medium ;;
    3) TIER=full ;;
    4) picked=$(pick_campaigns)
       [ -n "$picked" ] || { echo "  no campaigns chosen"; exit 1; }
       read -r -a ARGS <<< "$picked"
       printf '  Tier for these (minor/medium/full) [medium]: '
       read -r TIER; TIER=${TIER:-medium} ;;
    q|Q|"") exit 0 ;;
    *) echo "  unknown choice '$choice'"; exit 1 ;;
    esac
fi

if [ "$RESUME" = 1 ]; then
    [ -n "$LAST_PLAN" ] || { echo "ERROR: the last queue did not record its plan, so it cannot be resumed; start a new one" >&2; exit 1; }
    [ -n "$LAST_LEFT" ] || { echo "  The last queue (tier $LAST_TIER) finished everything; nothing to resume."; exit 0; }
    TIER=$LAST_TIER
    REPS_OPT=${REPS_OPT:-$LAST_REPS}
    read -r -a ARGS <<< "$LAST_LEFT"
fi
TIER=${TIER:-medium}

REPS_Q=${REPS_OPT:-1}
case "$REPS_Q" in
''|*[!0-9]*|0) echo "ERROR: --reps must be a positive integer, not '$REPS_Q'" >&2; exit 1 ;;
esac

# --- tiers -----------------------------------------------------------------
case "$TIER" in
minor)
    QUEUE="$ALL"
    SETTLES="that every driver, the analyser and the filesystem still work.
            A few workloads per campaign, each run exactly as medium runs
            it, so a regression shows against medium's same cell." ;;
medium)
    QUEUE="$ALL"
    SETTLES="every claim, on a reduced set of workloads.  Narrower than
            the paper." ;;
full)
    QUEUE="$ALL"
    SETTLES="everything in section 5 this artifact can measure, with the
            drivers' own workloads." ;;
*)
    echo "ERROR: unknown tier '$TIER'; use minor, medium or full" >&2
    exit 1 ;;
esac
if [ "${#ARGS[@]}" -gt 0 ]; then
    for c in "${ARGS[@]}"; do
        echo " $ALL " | grep -q " $c " || { echo "ERROR: unknown campaign '$c'; one of: $ALL" >&2; exit 1; }
    done
    QUEUE="${ARGS[*]}"
    [ "$RESUME" = 1 ] && SETTLES="the rest of the last queue, at tier $TIER." ||
        SETTLES="the campaigns you named, at tier $TIER."
fi

# Seconds per repetition, for the plan printout only.
secs_per_rep() {
    case "$TIER:$1" in
    minor:lookup_cpu)      echo 15 ;;   minor:footprint)      echo 268 ;;
    minor:cache_pressure)  echo 433 ;;  minor:realworld)      echo 153 ;;
    minor:ablation)        echo 497 ;;  minor:locality_sweep) echo 282 ;;
    minor:mdtest)          echo 51 ;;   minor:applookup)      echo 38 ;;
    medium:cache_pressure) echo 460 ;;  medium:locality_sweep) echo 360 ;;
    medium:mdtest)         echo 320 ;;  medium:applookup)     echo 180 ;;
    full:cache_pressure)   echo 5400 ;; full:realworld)       echo 780 ;;
    full:applookup)        echo 10500 ;;
    *:lookup_cpu)          echo 24 ;;   *:footprint)          echo 820 ;;
    *:realworld)           echo 400 ;;  *:cache_pressure)     echo 1100 ;;
    *:ablation)            echo 520 ;;  *:locality_sweep)     echo 600 ;;
    *:mdtest)              echo 2020 ;; *:applookup)          echo 2260 ;;
    *) echo 0 ;;
    esac
}
runtime_of() { echo $(( ($(secs_per_rep "$1") * REPS_Q + 59) / 60 )); }

# Settings per tier and campaign.  minor runs a subset of each campaign's
# workloads, on SplineFS and ext4 only; medium runs fewer workloads than full.
# APPS must come last: it reaches run_applookup.sh as positional arguments.
tier_env() {
    local r=""
    case "$TIER:$1" in
    minor:lookup_cpu)     r="FSES='splinefs ext4'" ;;
    minor:footprint)      r="SHAPES='100:10000 100000:10' LISTS= REALDIRS=" ;;
    minor:cache_pressure) r="CAPS='536870912 2147483648' FSES='splinefs ext4'" ;;
    minor:realworld)      r="FSES='splinefs ext4' IMAGENET=0" ;;
    minor:locality_sweep) r="FRACTIONS='0 50 100'" ;;
    minor:mdtest)         r="COUNTS=100000 CAPS='2147483648 8589934592'" ;;
    minor:applookup)      r="FSES='splinefs ext4' APPS=A3" ;;

    medium:cache_pressure) r="CAPS='536870912 2147483648' FSES='splinefs ext4'" ;;
    medium:realworld)      r="IMAGENET=0" ;;
    medium:mdtest)         r="COUNTS='100000 1000000' CAPS='2147483648 8589934592'" ;;
    medium:applookup)      r="APPS='A3 A1'" ;;
    medium:locality_sweep) r="FRACTIONS='0 10 50 100'" ;;
    esac
    echo "REPS=$REPS_Q${r:+ $r}"
}

# MAX_CAP (bytes, set by scripts/vm/run.sh) drops caps the machine cannot hold.
DEFAULT_CAPS_cache_pressure="268435456 536870912 1073741824 2147483648"
DEFAULT_CAPS_ablation="268435456 536870912 1073741824 2147483648 4294967296 8589934592"
DEFAULT_CAPS_mdtest="536870912 1073741824 2147483648 4294967296 8589934592"
DEFAULT_CAPS_realworld="2147483648"
DEFAULT_CAPS_applookup="2147483648"
cap_limit() {
    local exp=$1 extra=$2 caps="" keep="" dropped="" c var
    if [ -z "${MAX_CAP:-}" ] || [ "$exp" = lookup_cpu ] || [ "$exp" = footprint ] ||
       [ "$exp" = locality_sweep ]; then
        echo "$extra"; return
    fi
    if [[ $extra =~ CAPS=\'([^\']*)\' ]]; then caps=${BASH_REMATCH[1]}
    elif [[ $extra =~ CAPS=([0-9]+) ]]; then caps=${BASH_REMATCH[1]}
    else var="DEFAULT_CAPS_$exp"; caps=${!var:-}; fi
    [ -n "$caps" ] || { echo "$extra"; return; }
    for c in $caps; do
        if [ "$c" -le "$MAX_CAP" ]; then keep="$keep $c"; else dropped="$dropped $c"; fi
    done
    [ -n "$keep" ] || keep=" $(echo $caps | tr ' ' '\n' | sort -n | head -1)"
    [ -z "$dropped" ] || printf '  %s: caps above MAX_CAP dropped:%s\n' "$exp" "$dropped" >&2
    extra=$(echo "$extra" | sed -E "s/CAPS='[^']*'//; s/CAPS=[0-9]+//; s/  +/ /g")
    local head=${extra%% APPS=*}
    echo "${head% } CAPS='${keep# }'$( [[ $extra == *APPS=* ]] && echo " APPS=${extra#*APPS=}")"
}

# --- plan ------------------------------------------------------------------
total=0
for exp in $QUEUE; do total=$((total + $(secs_per_rep "$exp") * REPS_Q)); done
total=$(( (total + 59) / 60 ))
printf '\n  SplineFS AE -- tier %s\n\n' "$TIER"
printf '  Settles:  %s\n' "$SETTLES"
printf '  Reps:     %s per cell\n' "$REPS_Q"
how="measured per repetition on the paper machine"
printf '  Cost:     about %d min, %s h (%s)\n' "$total" \
    "$(awk -v m="$total" 'BEGIN { printf "%.1f", m / 60 }')" "$how"
printf '  Device:   %s  (formatted repeatedly, all data lost)\n' "${DEV:-<unset>}"
printf '  Output:   %s\n\n' "$RESULTS"
printf '  %-16s %6s  %s\n' CAMPAIGN MIN SETTINGS
printf '  %-16s %6s  %s\n' ---------------- ------ --------
for exp in $QUEUE; do
    note=$(cap_limit "$exp" "$(tier_env "$exp")")
    [ -f "$AE_ROOT/scripts/campaigns/run_$exp.sh" ] || note="MISSING scripts/campaigns/run_$exp.sh"
    printf '  %-16s %6s  %s\n' "$exp" "$(runtime_of "$exp")" "$note"
done
echo

[ "$DRY" = 1 ] && { echo "  --dry-run: nothing was run."; echo; exit 0; }

# --- preflight -------------------------------------------------------------
fail=0
say_bad() { printf '  FAIL  %s\n' "$1" >&2; fail=1; }
[ -n "${DEV:-}" ] || say_bad "DEV is unset"
[ -n "${CONFIRM_DESTROY:-}" ] || say_bad "CONFIRM_DESTROY is unset"
[ "${DEV:-x}" = "${CONFIRM_DESTROY:-y}" ] ||
    say_bad "CONFIRM_DESTROY must equal DEV; refusing to format ${DEV:-?}"
[ -b "${DEV:-/nonexistent}" ] || say_bad "DEV ${DEV:-} is not a block device"
[ "$(id -u)" = 0 ] || say_bad "must run as root (campaigns insmod and mkfs)"
[ -s "$AE_ROOT/src/ext5/ext5.ko" ] || say_bad "src/ext5/ext5.ko missing; run make -C src"
if [ -s "$AE_ROOT/src/ext5/ext5.ko" ]; then
    want=$(uname -r)
    got=$(modinfo -F vermagic "$AE_ROOT/src/ext5/ext5.ko" 2>/dev/null | awk '{print $1}')
    [ "$got" = "$want" ] ||
        say_bad "ext5.ko is for kernel '$got' but this host runs '$want'; rebuild with make -C src"
fi
for b in multidir_lookup_bench ext5_li_walk; do
    [ -x "$AE_ROOT/utils/bin/$b" ] || say_bad "utils/bin/$b missing; run make -C utils"
done
grep -qs "^${DEV:-/nonexistent} " /proc/mounts &&
    say_bad "$DEV is mounted; unmount it first"

[ "$fail" = 0 ] || { echo; echo "  Preflight failed; nothing was run." >&2; exit 1; }
echo "  Preflight OK."
echo

# --- run -------------------------------------------------------------------
mkdir -p "$LOGS"
printf '=== queue started %s tier=%s reps=%s plan=%s ===\n' \
    "$(date -Is)" "$TIER" "$REPS_Q" "$(echo $QUEUE | tr ' ' ',')" >> "$LEDGER"

# Ctrl-C or a kill pauses: stop the campaign, release the device, log PAUSED.
current=""
child=""
# Signal every descendant, deepest first, so none keeps the device open.
kill_tree() {
    local p=$1 sig=$2 c
    for c in $(ps -o pid= --ppid "$p" 2>/dev/null); do kill_tree "$c" "$sig"; done
    kill -"$sig" "$p" 2>/dev/null
}
release_device() {
    for m in /mnt/splinefs-eval /mnt/splinefs-realworld /mnt/splinefs-app \
             /mnt/splinefs-overhead /mnt/splinefs-mdtest \
             /mnt/splinefs-footprint /mnt/splinefs-locality-sweep; do
        mountpoint -q "$m" && { umount "$m" 2>/dev/null || umount -l "$m"; }
    done
    rmmod ext5 2>/dev/null; rmmod jbd3 2>/dev/null; sync
}
on_pause() {
    trap - INT TERM HUP
    echo
    if [ -n "$current" ]; then
        [ -n "$child" ] && { kill_tree "$child" TERM; sleep 3; kill_tree "$child" KILL; }
        printf '%s  %-16s PAUSED\n' "$(date -Is)" "$current" | tee -a "$LEDGER"
    fi
    release_device
    printf '=== queue paused %s ===\n' "$(date -Is)" >> "$LEDGER"
    printf '\n  Paused.  Resume with: run_queue.sh --resume\n'
    exit 130
}
trap on_pause INT TERM HUP
failed=0
for exp in $QUEUE; do
    current=$exp
    script="$AE_ROOT/scripts/campaigns/run_$exp.sh"
    if [ ! -f "$script" ]; then
        printf '%s  %-16s SKIP  no scripts/campaigns/run_%s.sh\n' \
            "$(date -Is)" "$exp" "$exp" | tee -a "$LEDGER"
        continue
    fi
    log=$(mktemp "${TMPDIR:-/tmp}/ae-$exp.XXXXXX")
    extra=$(cap_limit "$exp" "$(tier_env "$exp")")
    printf '%s  %-16s START  tier=%s %s\n' \
        "$(date -Is)" "$exp" "$TIER" "$extra" | tee -a "$LEDGER"
    start=$(date +%s)
    # eval keeps quoted override values such as FSES='splinefs ext4' intact.
    apps=""
    envs="$extra"
    case "$extra" in
    *APPS=*)
        apps=${extra#*APPS=}
        envs=${extra%%APPS=*} ;;
    esac
    # In the background, so a trapped signal is handled at once.
    eval env DEV=\"\$DEV\" CONFIRM_DESTROY=\"\$CONFIRM_DESTROY\" AE_TIER=\"\$TIER\" \
        AE_RESULTS=\"\$RESULTS\" $envs bash \"\$script\" $apps > "$log" 2>&1 &
    child=$!
    wait "$child"
    rc=$?
    child=""
    mins=$(( ($(date +%s) - start) / 60 ))
    # Keep this log only if the campaign died before creating its run directory.
    run_dir=$(sed -n 's/^AE_RUN_DIR=//p' "$log" | tail -1)
    # A run with an empty panel CSV failed, whatever its exit code.  Exit 3
    # means the campaign's inputs are not staged: logged SKIP.
    if [ $rc -eq 0 ] && [ -n "$run_dir" ] && [ -d "$run_dir" ]; then
        rows=$(cat "$run_dir"/*.csv 2>/dev/null | grep -cv '^$')
        if [ "${rows:-0}" -le 1 ]; then
            rc=65
            printf 'ERROR: %s exited 0 but produced no measurements\n' "$exp" >&2
        fi
    fi
    if [ -n "$run_dir" ] && [ -d "$run_dir" ]; then
        rm -f "$log"
        log="$run_dir/run.log"
    else
        mv "$log" "$LOGS/$exp.failed.log"
        log="$LOGS/$exp.failed.log"
    fi
    printf '%s  %-16s %s  rc=%d  %dm  %s\n' \
        "$(date -Is)" "$exp" \
        "$(case $rc in 0) echo DONE ;; 3) echo SKIP ;; *) echo FAIL ;; esac)" \
        "$rc" "$mins" "$log" | tee -a "$LEDGER"
    case $rc in 0|3) ;; *) failed=$((failed + 1)) ;; esac

    # Leave the device and the module in a known state.
    release_device
done
printf '=== queue finished %s ===\n' "$(date -Is)" >> "$LEDGER"
echo
echo "  ledger: $LEDGER"
echo "  next:   python3 $AE_ROOT/scripts/check.py --results $RESULTS"
if [ "$failed" -gt 0 ]; then
    printf '  %d campaign(s) failed; see the ledger\n' "$failed"
    exit 1
fi
