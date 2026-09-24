#!/bin/bash
# env_check.sh -- report host preconditions for the SplineFS artifact.
#
# Read-only.  Prints what is present, what is missing, and what would block a
# destructive experiment.  Exits 0 if the non-destructive path is usable, 1 if
# a hard requirement is absent.
set -uo pipefail

AE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
fail=0
warn=0

hdr()  { printf '\n== %s ==\n' "$1"; }
ok()   { printf '  [ ok ]   %s\n' "$1"; }
bad()  { printf '  [FAIL]   %s\n' "$1"; fail=1; }
note() { printf '  [note]   %s\n' "$1"; warn=1; }

hdr "Artifact tree"
ok "root $AE_ROOT"
if [ -f "$AE_ROOT/src/ext5/ext5.ko" ]; then
    ok "ext5.ko built, srcversion $(modinfo -F srcversion "$AE_ROOT/src/ext5/ext5.ko" 2>/dev/null)"
    ok "ext5.ko sha256 $(sha256sum "$AE_ROOT/src/ext5/ext5.ko" | cut -d' ' -f1)"
else
    note "ext5.ko not built yet; run 'make all'"
fi

hdr "Kernel"
running=$(uname -r)
printf '  running kernel: %s\n' "$running"
case "$running" in
    6.16.5*) ok "matches the artifact's target 6.16.5" ;;
    *)       bad "artifact targets 6.16.5; out-of-tree modules will not load on $running" ;;
esac
if [ -d "/lib/modules/$running/build" ]; then
    ok "kernel build tree at $(readlink -f "/lib/modules/$running/build")"
else
    bad "no kernel build tree at /lib/modules/$running/build"
fi

hdr "Toolchain"
for t in gcc-13 make python3; do
    if command -v "$t" >/dev/null 2>&1; then ok "$t: $(command -v "$t")"
    else bad "$t not found"; fi
done
for t in perf qemu-system-x86_64 mdtest; do
    if command -v "$t" >/dev/null 2>&1; then ok "$t: $(command -v "$t")"
    else note "$t not found (needed by some experiments)"; fi
done

hdr "Privileges"
if sudo -n true 2>/dev/null; then ok "passwordless sudo"
else bad "passwordless sudo required (insmod, mkfs, mount, drop_caches)"; fi

hdr "Kernel interfaces"
if [ -d /sys/fs/cgroup/cgroup.controls ] || grep -q cgroup2 /proc/filesystems; then
    ok "cgroup v2 available"
    grep -q memory /sys/fs/cgroup/cgroup.controllers 2>/dev/null \
        && ok "memory controller enabled" \
        || note "memory controller not in /sys/fs/cgroup/cgroup.controllers"
else
    bad "cgroup v2 required for the memcg cap sweeps"
fi
mountpoint -q /sys/kernel/debug && ok "debugfs mounted" \
    || note "debugfs not mounted; li_stats diagnostics unavailable"
[ -e /dev/kvm ] && ok "/dev/kvm present" \
    || note "/dev/kvm absent; qemu-test and kvm-perf unavailable"

hdr "Contamination"
if lsmod | grep -qE '^(ext5|jbd3) '; then
    bad "ext5 or jbd3 already loaded; unload before measuring"
else
    ok "no ext5/jbd3 module loaded"
fi
if mount | grep -q 'type ext5'; then
    bad "an ext5 filesystem is mounted: $(mount | grep 'type ext5' | head -1)"
else
    ok "no ext5 mount"
fi

hdr "CPU state"
# intel_pstate's no_turbo can block forever: read it in the background and
# abandon it.  Nodes known to block are marked in /run (or a per-user fallback).
_ae_wedged_dir() {
    if [ -n "${AE_WEDGED_DIR:-}" ]; then echo "$AE_WEDGED_DIR"; return; fi
    if mkdir -p /run/splinefs-ae 2>/dev/null && [ -w /run/splinefs-ae ]; then
        echo /run/splinefs-ae
    else
        echo "${XDG_RUNTIME_DIR:-/tmp}/splinefs-ae-$(id -u)"
    fi
}
WEDGED_DIR=$(_ae_wedged_dir)
try_read() {                            # try_read <seconds> <path>
    local tmp pid i=0
    local marker="$WEDGED_DIR/$(echo "$2" | tr / _).wedged"
    [ -e "$marker" ] && { echo unavailable; return 1; }
    tmp=$(mktemp)
    ( cat "$2" > "$tmp" 2>/dev/null ) &
    pid=$!
    while kill -0 "$pid" 2>/dev/null && [ "$i" -lt $(($1 * 10)) ]; do
        sleep 0.1; i=$((i + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
        disown "$pid" 2>/dev/null || true; rm -f "$tmp"
        mkdir -p "$WEDGED_DIR" 2>/dev/null && : > "$marker" 2>/dev/null
        echo unavailable; return 1
    fi
    cat "$tmp" 2>/dev/null; rm -f "$tmp"
}
gov=$(try_read 5 /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)
printf '  governor: %s\n' "$gov"
[ "$gov" = performance ] || note "governor is '$gov'; the paper uses performance"
turbo=$(try_read 5 /sys/devices/system/cpu/intel_pstate/no_turbo)
case "$turbo" in
1) ok "turbo disabled" ;;
0) note "turbo enabled; the paper disables it (scripts/internal/host_prep.sh apply)" ;;
*) note "intel_pstate no_turbo did not respond in 5s; it is wedged, and turbo cannot be changed until reboot" ;;
esac
for svc in power-profiles-daemon tuned thermald; do
    systemctl is-active --quiet "$svc" 2>/dev/null &&
        note "$svc is running and competes with a fixed governor"
done
printf '  cores: %s   NUMA nodes: %s   RAM: %s GB\n' \
    "$(nproc)" "$(ls -d /sys/devices/system/node/node[0-9]* 2>/dev/null | wc -l)" \
    "$(free -g | awk '/^Mem:/{print $2}')"

hdr "Benchmark device"
DEV=${DEV:-}
if [ -z "$DEV" ]; then
    note "DEV unset; destructive experiments need DEV=<disposable partition>"
    printf '  candidate disposable devices (nothing mounted on them or under them):\n'
    lsblk -rno NAME,SIZE,TYPE "$@" 2>/dev/null | awk '$3=="disk"||$3=="part"{print $1, $2}' |
    while read -r name size; do
        dev="/dev/$name"
        case "$name" in sd*|pmem*|loop*|dm-*) continue ;; esac
        # Skip anything mounted, holding a mounted child, or listed in fstab.
        lsblk -rno MOUNTPOINT "$dev" 2>/dev/null | grep -q . && continue
        grep -q "^$dev[[:space:]]" /etc/fstab 2>/dev/null && continue
        printf '    %-20s %s\n' "$dev" "$size"
    done
    printf '  /dev/sd* is never offered: those are this host'"'"'s system disks.\n'
else
    if [ ! -b "$DEV" ]; then bad "$DEV is not a block device"
    elif case "$DEV" in /dev/sd*) true ;; *) false ;; esac; then
        bad "$DEV is a /dev/sd* device; this host keeps / and /home there. Refusing."
    elif lsblk -rno MOUNTPOINT "$DEV" 2>/dev/null | grep -q .; then
        bad "$DEV or one of its partitions is mounted; refusing"
    elif grep -q "^$DEV[[:space:]]" /etc/fstab 2>/dev/null; then
        bad "$DEV appears in /etc/fstab; refusing"
    else ok "$DEV usable, $(lsblk -dno SIZE "$DEV" | tr -d ' ')"; fi
fi

hdr "Verdict"
if [ "$fail" -ne 0 ]; then
    printf '  blocked: fix the [FAIL] items above\n'; exit 1
fi
[ "$warn" -ne 0 ] && printf '  usable, with the [note] caveats above\n' \
                  || printf '  all checks passed\n'
exit 0
