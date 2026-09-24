#!/bin/bash
# wrapper.sh -- shared code of scripts/campaigns/run_*.sh: device checks,
# run directories, configuration records.  Sourced; needs AE_ROOT.

: "${AE_ROOT:?wrapper.sh needs AE_ROOT}"

AE_EXPERIMENT=""
AE_OUT=""
AE_START=0
AE_ARTIFACTS=()

ae_die()  { printf '\nERROR: %s\n' "$*" >&2; exit 1; }

# ae_try_read <seconds> <path> -- read a sysfs node that may never answer;
# prints the value or "unavailable".  A blocked reader is abandoned, and the
# node is marked in /run (or a per-user fallback) so it is not probed again.
_ae_wedged_dir() {
    if [ -n "${AE_WEDGED_DIR:-}" ]; then echo "$AE_WEDGED_DIR"; return; fi
    if mkdir -p /run/splinefs-ae 2>/dev/null && [ -w /run/splinefs-ae ]; then
        echo /run/splinefs-ae
    else
        echo "${XDG_RUNTIME_DIR:-/tmp}/splinefs-ae-$(id -u)"
    fi
}
_AE_WEDGED_DIR=$(_ae_wedged_dir)

ae_try_read() {
    local secs=$1 path=$2 tmp pid i=0
    local marker="$_AE_WEDGED_DIR/$(echo "$path" | tr / _).wedged"
    if [ -e "$marker" ]; then
        echo unavailable
        return 1
    fi
    tmp=$(mktemp)
    ( cat "$path" > "$tmp" 2>/dev/null ) &
    pid=$!
    while kill -0 "$pid" 2>/dev/null && [ "$i" -lt $((secs * 10)) ]; do
        sleep 0.1; i=$((i + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
        disown "$pid" 2>/dev/null || true
        rm -f "$tmp"
        mkdir -p "$_AE_WEDGED_DIR" 2>/dev/null &&
            : > "$marker" 2>/dev/null
        echo unavailable
        return 1
    fi
    cat "$tmp" 2>/dev/null || echo unavailable
    rm -f "$tmp"
}
ae_step() { printf '\n-- %s\n' "$*"; }
ae_warn_quick() {
    cat >&2 <<'MSG'

  QUICK=1: reduced shape for a mechanics check only.
  The result is NOT evidence and must not be reported as a paper number.

MSG
}

# ae_require_device -- refuse anything that is not clearly disposable.
ae_require_device() {
    [ "$(id -u)" -eq 0 ] || ae_die "run as root (sudo env DEV=... CONFIRM_DESTROY=... bash $0)"
    [ -n "${DEV:-}" ] || ae_die "set DEV to a disposable block device"
    [ "${CONFIRM_DESTROY:-}" = "$DEV" ] || ae_die "set CONFIRM_DESTROY=$DEV to confirm it will be erased"
    [ -b "$DEV" ] || ae_die "$DEV is not a block device"
    case "$DEV" in
        /dev/sd*) ae_die "refusing /dev/sd* device $DEV: system disks live there" ;;
        /dev/dm-*|/dev/md*|/dev/mapper/*) ae_die "refusing $DEV" ;;
    esac
    lsblk -rno MOUNTPOINT "$DEV" 2>/dev/null | grep -q . \
        && ae_die "$DEV or one of its partitions is mounted"
    grep -Fq "$DEV" /etc/fstab 2>/dev/null && ae_die "$DEV appears in /etc/fstab"
    return 0
}

ae_check_clean() {
    lsmod | grep -qE '^(ext5|jbd3) ' && ae_die "ext5/jbd3 already loaded; unload first"
    mount | grep -q 'type ext5' && ae_die "an ext5 filesystem is mounted"
    return 0
}

# Campaign -> the figure panel it writes; runs are filed by panel.
ae_panel_for() {
    case "$1" in
    footprint)      echo F9a ;;
    cache_pressure) echo F9b ;;   # also writes F9c; see ae_publish_panels
    realworld)      echo F10 ;;
    applookup)      echo F11 ;;
    ablation)       echo F12a ;;
    lookup_cpu)     echo F12b ;;
    mdtest)         echo F13 ;;
    locality_sweep) echo F14 ;;
    *)              echo "$1" ;;
    esac
}

ae_panel_siblings() {
    case "$1" in
    cache_pressure) echo F9c ;;
    *)              echo "" ;;
    esac
}

ae_begin() {
    AE_EXPERIMENT=$1
    AE_START=$(date +%s)
    AE_PANEL=$(ae_panel_for "$AE_EXPERIMENT")
    AE_STAMP=$(date +%Y%m%d-%H%M%S)
    AE_OUT="${AE_RESULTS:-$AE_ROOT/results}/$AE_PANEL/$AE_STAMP"
    ae_require_device
    ae_check_clean
    [ -f "$AE_ROOT/src/ext5/ext5.ko" ] || ae_die "ext5.ko not built; run 'make all'"
    mkdir -p "$AE_OUT"
    # Log to the run's own run.log too; callers find the run by AE_RUN_DIR.
    exec > >(tee -a "$AE_OUT/run.log") 2>&1
    printf 'AE_RUN_DIR=%s\n' "$AE_OUT"
    printf 'experiment : %s\noutput     : %s\ndevice     : %s\n' \
        "$AE_EXPERIMENT" "$AE_OUT" "$DEV"
}

ae_record_config() {
    local ko="$AE_ROOT/src/ext5/ext5.ko"
    {
        printf 'experiment=%s\n' "$AE_EXPERIMENT"
        # Checks rank runs by tier, so a reduced run never stands in for a full one.
        printf 'tier=%s\n' "${AE_TIER:-}"
        printf 'platform=%s\n' "${AE_PLATFORM:-host}"
        printf 'started=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        printf 'host=%s\n' "$(hostname)"
        printf 'kernel=%s\n' "$(uname -r)"
        printf 'srcversion=%s\n' "$(modinfo -F srcversion "$ko")"
        printf 'ko_sha256=%s\n' "$(sha256sum "$ko" | cut -d' ' -f1)"
        printf 'device=%s\n' "$DEV"
        # Userspace binaries too: a rebuild changes what a later run measured.
        local b
        for b in "$AE_ROOT"/utils/bin/*; do
            [ -f "$b" ] && printf 'bin_%s=%s\n' "$(basename "$b")" \
                "$(sha256sum "$b" | cut -c1-16)"
        done
        printf 'device_model=%s\n' "$(lsblk -dno MODEL "$DEV" 2>/dev/null | xargs || echo unknown)"
        printf 'cpu_governor=%s\n' \
            "$(ae_try_read 5 /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
        printf 'no_turbo=%s\n' \
            "$(ae_try_read 5 /sys/devices/system/cpu/intel_pstate/no_turbo)"
        # Turbo state from IA32_MISC_ENABLE bit 38 (Turbo Mode Disable).
        printf 'turbo_msr_1a0_bit38=%s\n' "$(
            _v=$(rdmsr -p 0 0x1a0 2>/dev/null)
            if [ -n "$_v" ]; then
                python3 -c "print('off' if (int('$_v',16)>>38)&1 else 'on')" 2>/dev/null || echo unknown
            else
                echo "unknown (needs root and msr-tools)"
            fi)"
        # Above base_frequency proves turbo is on; at or below proves nothing.
        printf 'max_observed_freq=%s\nbase_frequency=%s\n' "$(
            _m=0
            for _f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq; do
                _c=$(cat "$_f" 2>/dev/null) || continue
                [ -n "$_c" ] && [ "$_c" -gt "$_m" ] && _m=$_c
            done
            echo "$_m")" \
            "$(cat /sys/devices/system/cpu/cpu0/cpufreq/base_frequency 2>/dev/null || echo 0)"
        printf 'nr_cpus=%s\n' "$(nproc)"
        printf 'mem_gb=%s\n' "$(free -g | awk '/^Mem:/{print $2}')"
        [ -n "${1:-}" ] && printf 'options=%s\n' "$1"
    } > "$AE_OUT/config.txt"
    # Warn when the CPU state differs from the paper's.  A guest cannot see it.
    local gov turbo
    gov=$(awk -F= '$1=="cpu_governor" {print $2}' "$AE_OUT/config.txt")
    turbo=$(awk -F= '$1=="turbo_msr_1a0_bit38" {print $2}' "$AE_OUT/config.txt")
    if [ "${AE_PLATFORM:-host}" != qemu ] &&
       { [ "$gov" != performance ] || [ "$turbo" != off ]; }; then
        cat >&2 <<MSG

  NOTE: governor='$gov', turbo='$turbo' (MSR 0x1A0 bit 38).
  The paper's setup uses the performance governor with turbo disabled.
  Ratios between filesystems are less sensitive to this than absolute rates.
  To match: sudo bash scripts/internal/host_prep.sh apply

MSG
    fi
}

ae_have() { AE_ARTIFACTS+=("$1"); }

ae_end() {
    local secs=$(( $(date +%s) - AE_START ))
    printf '\n== %s finished in %dm%02ds ==\n' "$AE_EXPERIMENT" $((secs/60)) $((secs%60))
    # Fold the driver's CSVs into the panel CSVs the checker and figures read.
    python3 "$AE_ROOT/scripts/lib/publish.py" "$AE_EXPERIMENT" "$AE_OUT" ||
        printf 'WARNING: could not publish panel CSVs for %s\n' "$AE_EXPERIMENT" >&2
    # Hand the run back to the invoking user.
    if [ -n "${SUDO_UID:-}" ]; then
        chown -R "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" \
            "${AE_RESULTS:-$AE_ROOT/results}/$AE_PANEL" 2>/dev/null || true
        for extra in $(ae_panel_siblings "$AE_EXPERIMENT"); do
            chown -R "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" \
                "${AE_RESULTS:-$AE_ROOT/results}/$extra" 2>/dev/null || true
        done
    fi
    printf 'config : %s\nlog    : %s\n' \
        "$AE_OUT/config.txt" "$AE_OUT/run.log"
    local a
    for a in "${AE_ARTIFACTS[@]:-}"; do
        [ -n "$a" ] && [ -f "$a" ] && printf 'result : %s (%s rows)\n' "$a" "$(( $(wc -l < "$a") - 1 ))"
    done
    if [ "${AE_PLATFORM:-host}" = qemu ]; then
        printf '\nCompare with the paper: make figures, on the host\n'
    else
        printf '\nCompare with the paper: make -C %s figures\n' "$AE_ROOT"
    fi
}
