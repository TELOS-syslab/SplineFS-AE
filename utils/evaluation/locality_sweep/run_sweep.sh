#!/usr/bin/env bash
# Fig. 14 driver: churned-region sweep.  DESTRUCTIVE: formats DEV for every
# data point.  The adaptive arm runs the module's own online policy.

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
DEV=${DEV:-}
MNT=${MNT:-/mnt/splinefs-locality-sweep}
JBD3_KO=${JBD3_KO:-$ROOT/src/jbd3/jbd3.ko}
EXT5_KO=${EXT5_KO:-$ROOT/src/ext5/ext5.ko}

# Nonzero pins the adaptive arm's quiet-ops threshold; 0 lets it adapt.
POLICY_QUIET_OPS=${POLICY_QUIET_OPS:-0}
POLICY_SIZE_FLOOR=${POLICY_SIZE_FLOOR:-0}
POLICY_BUILDCOST_PERMILLE=${POLICY_BUILDCOST_PERMILLE:-0}
BENCH="$HERE/locality_sweep_bench"
STATS=/sys/kernel/debug/ext5/li_stats
CG=/sys/fs/cgroup/splinefs_locality_sweep

REGIONS=${REGIONS:-128}
LEAVES=${LEAVES:-64}
ENTRIES=${ENTRIES:-128}
POOL=${POOL:-1024}
WARMUP=${WARMUP:-600000}
MEASURE=${MEASURE:-400000}
GAP=${GAP:-16}
CAP=${CAP:-536870912}
REPS=${REPS:-3}
FRACTIONS=${FRACTIONS:-"0 1 5 10 25 50 100"}
ADAPT_SETTLE=${ADAPT_SETTLE:-30}
FS_BLOCKS=${FS_BLOCKS:-8388608}

RUN_ID=${RUN_ID:-$(date +%Y%m%d-%H%M%S)}
RAW=${RAW:-$HERE/raw/sweep-$RUN_ID}
OUT=${OUT:-$HERE/locality_sweep.csv}

say()
{
	echo "[locality-sweep $(date +%H:%M:%S)] $*" >&2
}

die()
{
	echo "ERROR: $*" >&2
	exit 1
}

teardown()
{
	cd /
	echo max > "$CG/memory.max" 2>/dev/null || true
	sync
	if mountpoint -q "$MNT"; then
		umount "$MNT" 2>/dev/null || umount -l "$MNT" 2>/dev/null || true
	fi
	rmmod ext5 2>/dev/null || true
	rmmod jbd3 2>/dev/null || true
}

trap teardown EXIT

check_environment()
{
	[ "$EUID" -eq 0 ] || die "run as root"
	ulimit -n 65536
	[ -n "$DEV" ] || die "set DEV to a disposable block device"
	[ "${CONFIRM_DESTROY:-}" = "$DEV" ] ||
		die "set CONFIRM_DESTROY=$DEV; this script reformats the device"
	[ -b "$DEV" ] || die "$DEV is not a block device"
	case "$DEV" in
	/dev/sd*)  die "refusing /dev/sd* device $DEV: system disks live there" ;;
	/dev/dm-*|/dev/md*|/dev/mapper/*) die "refusing $DEV" ;;
	esac
	! findmnt -rn -S "$DEV" >/dev/null || die "$DEV is mounted"
	! lsblk -rno MOUNTPOINT "$DEV" 2>/dev/null | grep -q . ||
		die "$DEV holds a mounted partition"
	! grep -Fq "$DEV" /etc/fstab || die "$DEV appears in /etc/fstab"
	[ -s "$JBD3_KO" ] || die "missing $JBD3_KO"
	[ -s "$EXT5_KO" ] || die "missing $EXT5_KO"
	mkdir -p "$MNT" "$RAW" "$CG"
	make -C "$HERE" locality_sweep_bench
	mountpoint -q /sys/kernel/debug ||
		mount -t debugfs none /sys/kernel/debug
	grep -qw memory /sys/fs/cgroup/cgroup.subtree_control ||
		echo +memory > /sys/fs/cgroup/cgroup.subtree_control
	echo 0 > "$CG/memory.swap.max" 2>/dev/null || true

	cat > "$RAW/config.txt" <<EOF
device=$DEV
kernel=$(uname -r)
regions=$REGIONS
leaves_per_group=$LEAVES
entries_per_leaf=$ENTRIES
total_leaf_dirs=$((REGIONS * LEAVES))
total_entries=$((REGIONS * LEAVES * ENTRIES))
warmup=$WARMUP
measure=$MEASURE
gap=$GAP
cap=$CAP
reps=$REPS
fractions=$FRACTIONS
adaptive_settle=$ADAPT_SETTLE
ext5_srcversion=$(modinfo -F srcversion "$EXT5_KO")
EOF
}

mount_case()
{
	local fs=$1

	teardown
	local have=$(( $(blockdev --getsize64 "$DEV") / 1024 ))   # KiB, as mke2fs reads FS_BLOCKS
	mkfs.ext4 -q -F -E nodiscard,lazy_itable_init=1 \
		-O ^has_journal "$DEV" "$(( FS_BLOCKS < have ? FS_BLOCKS : have ))"
	case "$fs" in
	ext4)
		mount -t ext4 "$DEV" "$MNT"
		;;
	policy_off)
		insmod "$JBD3_KO"
		insmod "$EXT5_KO" li_stats_enabled=Y li_policy_mode=0
		mount -t ext5 "$DEV" "$MNT"
		;;
	adaptive)
		insmod "$JBD3_KO"
		insmod "$EXT5_KO" li_stats_enabled=Y li_policy_mode=1 \
			li_quiet_ops="$POLICY_QUIET_OPS" \
			li_size_floor_entries="$POLICY_SIZE_FLOOR" \
			li_buildcost_permille="$POLICY_BUILDCOST_PERMILLE"
		mount -t ext5 "$DEV" "$MNT"
		;;
	*)
		die "unknown filesystem: $fs"
		;;
	esac
	mountpoint -q "$MNT" || die "no filesystem mounted at $MNT (DEV=$DEV)"
}

output_value()
{
	local key=$1 file=$2
	awk -v key="$key" '
		{
			for (i = 1; i <= NF; i++) {
				split($i, pair, "=")
				if (pair[1] == key) {
					print pair[2]
					exit
				}
			}
		}' "$file"
}

stat_value()
{
	local key=$1
	awk -v key="$key" '$1 == key { print $2 }' "$STATS"
}

run_in_cgroup()
{
	echo max > "$CG/memory.max"
	echo "$CAP" > "$CG/memory.max"
	(
		echo "$BASHPID" > "$CG/cgroup.procs"
		exec "$@"
	)
	echo max > "$CG/memory.max"
}

active_groups_for()
{
	local percent=$1
	local active=$(((REGIONS * percent + 50) / 100))

	if ((percent > 0 && active == 0)); then
		active=1
	fi
	echo "$active"
}

append_result()
{
	local percent=$1 active=$2 fs=$3 rep=$4 prefix=$5
	local roots=NA interiors=NA mutable=NA promoted_fraction=NA
	local promote_runs=NA stable_lookups=NA mode_summary=

	if [ "$fs" = adaptive ]; then
		"$BENCH" modes "$MNT/tree" "$REGIONS" "$LEAVES" \
			> "$prefix.modes.csv" 2> "$prefix.modes.summary"
		cp "$STATS" "$prefix.stats"
		mode_summary=$(cat "$prefix.modes.summary")
		roots=$(echo "$mode_summary" |
			sed -n 's/.*roots=\([0-9]*\).*/\1/p')
		interiors=$(echo "$mode_summary" |
			sed -n 's/.*interiors=\([0-9]*\).*/\1/p')
		mutable=$(echo "$mode_summary" |
			sed -n 's/.*mutable=\([0-9]*\).*/\1/p')
		promoted_fraction=$(awk -v r="$roots" -v i="$interiors" \
			-v total="$((1 + REGIONS + REGIONS * LEAVES))" \
			'BEGIN { printf "%.9f", (r + i) / total }')
		promote_runs=$(stat_value auto_promote_run)
		stable_lookups=$(stat_value stable_lookups)
	fi

	printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
		"$percent" "$active" "$fs" "$rep" \
		"$(output_value lookups "$prefix.bench.out")" \
		"$(output_value mutations "$prefix.bench.out")" \
		"$(output_value errors "$prefix.bench.out")" \
		"$(output_value seconds "$prefix.bench.out")" \
		"$(output_value ops_per_sec "$prefix.bench.out")" \
		"$(output_value lookup_us "$prefix.bench.out")" \
		"$roots" "$interiors" "$mutable" "$promoted_fraction" \
		"$promote_runs" "$stable_lookups" >> "$OUT"
}

run_one()
{
	local percent=$1 active=$2 fs=$3 rep=$4
	local settle=0
	local prefix="$RAW/p${percent}.${fs}.r${rep}"

	say "churn=${percent}% active=$active fs=$fs rep=$rep"
	mount_case "$fs"
	dmesg -c >/dev/null 2>&1 || true
	"$BENCH" prepare "$MNT/tree" "$REGIONS" "$LEAVES" "$ENTRIES" \
		"$POOL" > "$prefix.prepare.log" 2>&1
	if [ "$fs" = adaptive ]; then
		if [ "$POLICY_QUIET_OPS" -ne 0 ]; then
			echo "$POLICY_QUIET_OPS" \
				> /sys/module/ext5/parameters/li_quiet_ops
		fi
		echo reset > "$STATS"
		settle=$ADAPT_SETTLE
	fi
	run_in_cgroup "$BENCH" run "$MNT/tree" "$REGIONS" "$LEAVES" \
		"$ENTRIES" "$WARMUP" "$MEASURE" "$GAP" "$active" \
		"$settle" "$((rep * 1000 + percent))" \
		> "$prefix.bench.out" 2> "$prefix.bench.err"
	append_result "$percent" "$active" "$fs" "$rep" "$prefix"
	dmesg > "$prefix.dmesg" 2>/dev/null || true
	if grep -Eq 'BUG:|Oops|kernel panic|general protection fault' \
		"$prefix.dmesg"; then
		die "kernel error in $prefix.dmesg"
	fi
	teardown
}

main()
{
	local rep percent active index=0 order fs

	check_environment
	echo "churn_percent,active_groups,fs,rep,lookups,mutations,errors,seconds,ops_per_sec,lookup_us,roots,interiors,mutable,promoted_fraction,promote_runs,stable_lookups" > "$OUT"
	for rep in $(seq 1 "$REPS"); do
		for percent in $FRACTIONS; do
			active=$(active_groups_for "$percent")
			case $(((rep + index) % 3)) in
			0) order="adaptive policy_off ext4" ;;
			1) order="policy_off ext4 adaptive" ;;
			2) order="ext4 adaptive policy_off" ;;
			esac
			for fs in $order; do
				run_one "$percent" "$active" "$fs" "$rep"
			done
			((index += 1))
		done
	done
	python3 "$HERE/analyze_sweep.py" "$OUT" "$HERE/locality_sweep_summary.md"
	say "results: $OUT"
	say "raw logs: $RAW"
}

main "$@"
