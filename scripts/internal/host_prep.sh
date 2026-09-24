#!/bin/bash
# host_prep.sh -- set the CPU state of the paper's setup: performance
# governor, turbo off.
#
#   sudo bash scripts/internal/host_prep.sh apply     before measuring
#   sudo bash scripts/internal/host_prep.sh restore   afterwards
#   bash scripts/internal/host_prep.sh status         read-only
#
# apply saves the previous state under /run/splinefs-ae for restore.  The
# change is host-wide.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Saved state is cleared on reboot, when it stops describing the machine.
if mkdir -p /run/splinefs-ae 2>/dev/null && [ -w /run/splinefs-ae ]; then
	STATE=/run/splinefs-ae/host_state
else
	STATE="${XDG_RUNTIME_DIR:-$HERE}/splinefs-ae-host_state"
fi
TURBO=/sys/devices/system/cpu/intel_pstate/no_turbo

# intel_pstate's no_turbo node can block forever; never read it without a bound.
TMO=${TMO:-5}

# A task in uninterruptible sleep ignores SIGKILL, so timeout would block too:
# run the access in the background and abandon it if it does not return.
# Nodes known to block are marked in /run (or a per-user fallback).
_ae_wedged_dir() {
    if [ -n "${AE_WEDGED_DIR:-}" ]; then echo "$AE_WEDGED_DIR"; return; fi
    if mkdir -p /run/splinefs-ae 2>/dev/null && [ -w /run/splinefs-ae ]; then
        echo /run/splinefs-ae
    else
        echo "${XDG_RUNTIME_DIR:-/tmp}/splinefs-ae-$(id -u)"
    fi
}
WEDGED_DIR=$(_ae_wedged_dir)
_try() {                                # _try <seconds> <command...>
    local secs=$1; shift
    local tmp pid i=0
    local marker="$WEDGED_DIR/$(echo "$*" | tr -c 'A-Za-z0-9' _).wedged"
    [ -e "$marker" ] && return 1
    tmp=$(mktemp)
    ( "$@" > "$tmp" 2>/dev/null ) &
    pid=$!
    while kill -0 "$pid" 2>/dev/null && [ "$i" -lt $((secs * 10)) ]; do
        sleep 0.1; i=$((i + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
        disown "$pid" 2>/dev/null || true
        rm -f "$tmp"
        mkdir -p "$WEDGED_DIR" 2>/dev/null && : > "$marker" 2>/dev/null
        return 1
    fi
    cat "$tmp" 2>/dev/null
    rm -f "$tmp"
    return 0
}

rd() { _try "$TMO" cat "$1" || echo unavailable; }
wr() { _try "$TMO" bash -c "echo '$2' > '$1'" >/dev/null; }

# Power daemons fight a manual governor change; stop them for the campaign.
POWER_SERVICES="power-profiles-daemon tuned thermald"

governors() { ls /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null; }

# Turbo is controlled through IA32_MISC_ENABLE bit 38 (Turbo Mode Disable).
# Writing intel_pstate/no_turbo crashes this host's kernel.
MSR_MISC_ENABLE=0x1a0
MSR_TURBO_DISABLE_BIT=38

msr_ok() { command -v rdmsr >/dev/null 2>&1 && command -v wrmsr >/dev/null 2>&1; }

turbo_state_by_msr() {
    local v
    msr_ok || { echo unknown; return; }
    v=$(_try "$TMO" rdmsr -p 0 $MSR_MISC_ENABLE 2>/dev/null) || { echo unknown; return; }
    [ -n "$v" ] && [ "$v" != unavailable ] || { echo unknown; return; }
    python3 - "$v" <<'PYEOF' 2>/dev/null || echo unknown
import sys
v = int(sys.argv[1], 16)
print("off" if (v >> 38) & 1 else "on")
PYEOF
}

# Sets or clears bit 38 on every CPU, keeping the other bits.
turbo_set_by_msr() {
    local want=$1 v new
    msr_ok || return 1
    v=$(rdmsr -p 0 $MSR_MISC_ENABLE 2>/dev/null) || return 1
    new=$(python3 - "$v" "$want" <<'PYEOF'
import sys
v = int(sys.argv[1], 16)
print(hex(v | (1 << 38) if sys.argv[2] == "off" else v & ~(1 << 38)))
PYEOF
) || return 1
    wrmsr -a $MSR_MISC_ENABLE "$new" || return 1
}

# Any core above base_frequency proves turbo is on; an idle core proves
# nothing.  Returns on or unknown, never off.
turbo_state_by_freq() {
    local base cur mx=0 f
    base=$(cat /sys/devices/system/cpu/cpu0/cpufreq/base_frequency 2>/dev/null)
    [ -n "$base" ] || { echo unknown; return; }
    for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq; do
        cur=$(cat "$f" 2>/dev/null) || continue
        [ -n "$cur" ] && [ "$cur" -gt "$mx" ] && mx=$cur
    done
    [ "$mx" -gt 0 ] || { echo unknown; return; }
    if [ "$mx" -gt $((base + base / 50)) ]; then echo on; else echo unknown; fi
}

status() {
    local g t
    g=$(rd /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)
    t=$(rd $TURBO)
    case "$(turbo_state_by_msr)" in
    off) t=1 ;;
    on)  t=0 ;;
    esac
    printf 'governor : %s\n' "$g"
    printf 'no_turbo : %s (MSR 0x1A0 bit 38: %s)\n' "$t" "$(turbo_state_by_msr)"
    if [ "$t" = unavailable ]; then
        printf '           the intel_pstate no_turbo node did not respond within %ss.\n' "$TMO"
        case "$(turbo_state_by_freq)" in
        on)
            printf '           a core is running above base_frequency: turbo is ON.\n' ;;
        *)
            printf '           turbo state is UNKNOWN. No core is above base_frequency\n'
            printf '           right now, but idle cores prove nothing either way.\n'
            if msr_ok && [ "$(id -u)" -ne 0 ]; then
                printf '           Re-run as root for an authoritative reading: MSR 0x1A0\n'
                printf '           answers definitively and cannot hang, but needs root.\n'
            fi ;;
        esac
        printf '           Measurements taken in this state are not comparable to\n'
        printf '           the paper. Reboot to recover a writable no_turbo node.\n'
    fi
    printf 'distinct governors across CPUs: %s\n' \
        "$(_try "$TMO" bash -c 'cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor' 2>/dev/null | sort -u | tr '\n' ' ' || echo 'unreadable')"
    for svc in $POWER_SERVICES; do
        systemctl is-active --quiet "$svc" 2>/dev/null &&
            printf 'running  : %s (competes with a manual governor)\n' "$svc"
    done
    if [ "$g" = performance ] && [ "$t" = 1 ]; then
        printf '\nmatches the paper: yes\n'
    else
        printf '\nmatches the paper: no (run: sudo bash %s apply)\n' "$0"
    fi
}

need_root() { [ "$(id -u)" -eq 0 ] || { echo "run as root" >&2; exit 1; }; }

case "${1:-status}" in
status) status ;;
apply)
    need_root
    [ -f "$STATE" ] && { echo "state already saved in $STATE; restore first" >&2; exit 1; }
    {
        printf 'governor=%s\n' "$(rd /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
        printf 'no_turbo=%s\n' "$(rd $TURBO)"
        for svc in $POWER_SERVICES; do
            systemctl is-active --quiet "$svc" 2>/dev/null &&
                printf 'stopped_service=%s\n' "$svc"
        done
    } > "$STATE"
    # Disable turbo before changing the governor.
    if msr_ok; then
        if turbo_set_by_msr off && [ "$(turbo_state_by_msr)" = off ]; then
            echo "turbo disabled via MSR 0x1A0 bit 38"
        else
            echo "WARNING: could not disable turbo through the MSR." >&2
        fi
    elif [ "$(rd $TURBO)" = unavailable ]; then
        echo "WARNING: no_turbo is already unreadable; leaving turbo alone." >&2
        echo "         The paper disables turbo; this host cannot until reboot." >&2
    else
        echo "WARNING: msr-tools not installed; falling back to the sysfs node," >&2
        echo "         which is known to Oops on this host kernel." >&2
        wr $TURBO 1
        # Verify by frequency: reading the node back can block.
        case "$(turbo_state_by_freq)" in
        on)
            echo "WARNING: turbo is still ON after the write; a core is running" >&2
            echo "         above base_frequency. Do not measure in this state." >&2 ;;
        *)
            echo "turbo write issued; state not confirmable by frequency alone" ;;
        esac
    fi
    for svc in $POWER_SERVICES; do
        if systemctl is-active --quiet "$svc" 2>/dev/null; then
            echo "stopping $svc for the campaign"
            timeout 20 systemctl stop "$svc" || echo "  could not stop $svc" >&2
        fi
    done
    for f in $(governors); do wr "$f" performance; done
    echo "applied; previous state saved in $STATE"
    status ;;
restore)
    need_root
    [ -f "$STATE" ] || { echo "no saved state at $STATE" >&2; exit 1; }
    governor=unknown; no_turbo=unavailable
    . "$STATE"
    if [ "$governor" != unknown ]; then
        for f in $(governors); do wr "$f" "$governor"; done
    fi
    if msr_ok; then
        turbo_set_by_msr on && echo "turbo re-enabled via MSR 0x1A0 bit 38"
    elif [ "$no_turbo" != unavailable ]; then
        wr $TURBO "$no_turbo"
    fi
    for svc in $(awk -F= '$1=="stopped_service" {print $2}' "$STATE"); do
        echo "restarting $svc"
        timeout 20 systemctl start "$svc" || echo "  could not start $svc" >&2
    done
    rm -f "$STATE"
    echo "restored"
    status ;;
*) echo "usage: $0 {status|apply|restore}" >&2; exit 1 ;;
esac
