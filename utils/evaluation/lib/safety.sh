#!/usr/bin/env bash
# Shared safety and cleanup helpers for the destructive drivers.

eval_die()
{
	echo "ERROR: $*" >&2
	exit 1
}

# The requested filesystem size, capped at the device's; both in KiB.
eval_fs_blocks()
{
	local have
	have=$(( $(blockdev --getsize64 "$DEV") / 1024 ))
	if [ "$1" -le "$have" ]; then echo "$1"; else echo "$have"; fi
}

# Stop if the test filesystem is not mounted.
eval_require_mounted()
{
	mountpoint -q "$1" || eval_die "no filesystem mounted at $1 (DEV=$DEV)"
}

eval_require_device()
{
	[ "${EUID:-$(id -u)}" -eq 0 ] || eval_die "run as root"
	[ -n "${DEV:-}" ] || eval_die "set DEV to the disposable block device"
	[ "${CONFIRM_DESTROY:-}" = "$DEV" ] ||
		eval_die "set CONFIRM_DESTROY=$DEV"
	[ -b "$DEV" ] || eval_die "$DEV is not a block device"
	! findmnt -rn -S "$DEV" | grep -q . || eval_die "$DEV is mounted"
	# A whole disk counts as mounted when any of its partitions is.
	! lsblk -rno MOUNTPOINT "$DEV" 2>/dev/null | grep -q . ||
		eval_die "$DEV holds a mounted partition"
	! grep -Fq "$DEV" /etc/fstab || eval_die "$DEV appears in /etc/fstab"
	# Refuse every SCSI/SATA node.
	case "$DEV" in
	/dev/sd*)
		eval_die "refusing /dev/sd* device $DEV: system disks live there" ;;
	/dev/nvme0n1|/dev/nvme0n1p*|/dev/dm-*|/dev/md*|/dev/mapper/*)
		eval_die "refusing common system device $DEV" ;;
	esac
	# Refuse a device that backs any mount, bind mounts included.
	! grep -q " $(readlink -f "$DEV") " /proc/self/mountinfo 2>/dev/null ||
		eval_die "$DEV appears in /proc/self/mountinfo"
}

eval_drop_caches()
{
	sync
	echo 3 > /proc/sys/vm/drop_caches
}

# eval_assert_splinefs_run <tree> -- the policy promoted somewhere under
# <tree>, with no manual transitions and no bound violations.
# EVAL_EXPECT_NO_PROMOTION=1 requires that nothing promoted instead.
eval_assert_splinefs_run()
{
	local root=$1
	local stats=/sys/kernel/debug/ext5/li_stats
	local manual bounds roots found

	# EINVAL here just means nothing is promoted.
	"$AE_UTILS/bin/ext5_li_wait" "$root" >/dev/null 2>&1 || true
	if [ "${EVAL_EXPECT_NO_PROMOTION:-0}" = 1 ]; then
		found=$("$AE_UTILS/bin/ext5_li_walk" --csv "$root" 2>/dev/null |
			awk -F, '$2=="ROOT" && !seen {print $1; seen=1}') || found=""
		[ -z "$found" ] ||
			eval_die "promotion is disabled for this arm but $found is promoted"
		manual=$(awk '$1=="manual_operations" {print $2}' "$stats")
		bounds=$(awk '$1=="rs_bound_violations" {print $2}' "$stats")
		[ "${manual:-1}" -eq 0 ] || eval_die "manual transition contaminated run"
		[ "${bounds:-1}" -eq 0 ] || eval_die "RadixSpline bound violation"
		EVAL_PROMOTED_ROOT=$root
		export EVAL_PROMOTED_ROOT
		return 0
	fi
	# No early awk exit: SIGPIPE under pipefail would fail the assignment.
	found=$("$AE_UTILS/bin/ext5_li_walk" --csv "$root" 2>/dev/null |
		awk -F, '$2=="ROOT" && !seen {print $1; seen=1}') || found=""
	[ -n "$found" ] ||
		eval_die "adaptive policy promoted nothing under $root"
	EVAL_PROMOTED_ROOT=$found
	export EVAL_PROMOTED_ROOT
	# The policy may demote the root between the walk and this read: retry, then warn.
	if ! "$AE_UTILS/bin/ext5_li_info" "$found" >/dev/null 2>&1; then
		found=$("$AE_UTILS/bin/ext5_li_walk" --csv "$root" 2>/dev/null |
			awk -F, '$2=="ROOT" && !seen {print $1; seen=1}') || found=""
		if [ -n "$found" ] &&
		   "$AE_UTILS/bin/ext5_li_info" "$found" >/dev/null 2>&1; then
			EVAL_PROMOTED_ROOT=$found
			export EVAL_PROMOTED_ROOT
		else
			echo "WARN: the promoted root was demoted before its descriptor" >&2
			echo "      could be read; the policy did promote, and live_roots" >&2
			echo "      below remains the gate." >&2
		fi
	fi
	manual=$(awk '$1=="manual_operations" {print $2}' "$stats")
	bounds=$(awk '$1=="rs_bound_violations" {print $2}' "$stats")
	roots=$(awk '$1=="live_roots" {print $2}' "$stats")
	[ "${manual:-1}" -eq 0 ] || eval_die "manual transition contaminated run"
	[ "${bounds:-1}" -eq 0 ] || eval_die "RadixSpline bound violation"
	[ "${roots:-0}" -gt 0 ] || eval_die "no autonomously promoted root"
}

