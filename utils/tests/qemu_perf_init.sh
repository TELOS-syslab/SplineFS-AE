#!/bin/busybox sh
# SPDX-License-Identifier: GPL-2.0

fail()
{
	echo "KVM_PERF_FAIL: $*"
	sync
	poweroff -f
	sleep 5
}

FILES=200000
LOOKUPS=200000
REPS=5
MODE=positive-unique

mount -t proc proc /proc || fail proc
mount -t sysfs sysfs /sys || fail sysfs
mount -t devtmpfs devtmpfs /dev || fail devtmpfs
for arg in $(cat /proc/cmdline 2>/dev/null); do
	case "$arg" in
	spline_files=*) FILES=${arg#*=} ;;
	spline_lookups=*) LOOKUPS=${arg#*=} ;;
	spline_reps=*) REPS=${arg#*=} ;;
	spline_mode=*) MODE=${arg#*=} ;;
	esac
done
mkdir -p /ext4 /spline /sys/kernel/debug
mount -t debugfs debugfs /sys/kernel/debug || fail debugfs
insmod /jbd3.ko || fail jbd3
insmod /ext5.ko li_policy_mode=1 li_stats_enabled=N || fail ext5
mount -t ext4 -o noatime /dev/vda /ext4 || fail mount-ext4
mount -t ext5 -o noatime /dev/vdb /spline || fail mount-ext5

/lookup_bench prepare /ext4/lookup "$FILES" || fail prepare-ext4
/lookup_bench prepare /spline/lookup "$FILES" || fail prepare-spline

log=0
value=$FILES
while [ "$value" -gt 1 ]; do
	value=$((value / 2))
	log=$((log + 1))
done
threshold=$(((16 * FILES * log + 999) / 1000))
if [ "$threshold" -lt 10000 ]; then
	threshold=10000
fi
SETTLE_OPS=$((threshold + threshold / 8 + 65536))
LOOKUP_BENCH_MISS_BASE=$((FILES * 8)) \
	/lookup_bench run /spline/lookup negative-unique \
	"$SETTLE_OPS" "$FILES" 17 >/dev/null || fail settle
/ext5_li_wait /spline || fail settle-wait
/ext5_li_info /spline/lookup >/dev/null || fail autonomous-promotion
manual=$(awk '$1 == "manual_operations" { print $2 }' \
	/sys/kernel/debug/ext5/li_stats)
bounds=$(awk '$1 == "rs_bound_violations" { print $2 }' \
	/sys/kernel/debug/ext5/li_stats)
test "$manual" -eq 0 || fail manual-contamination
test "$bounds" -eq 0 || fail initial-bound

# Fault every positive namespace page once. Subsequent drop_caches=2 removes
# dentries/inodes while retaining page-cache data, matching the host-loop CPU
# methodology without mixing physical I/O into the counter interval.
LOOKUP_BENCH_MISS_BASE=$((FILES * 16)) \
	/lookup_bench run /ext4/lookup "$MODE" "$LOOKUPS" "$FILES" 101 \
	>/dev/null || fail warm-ext4
LOOKUP_BENCH_MISS_BASE=$((FILES * 16)) \
	/lookup_bench run /spline/lookup "$MODE" "$LOOKUPS" "$FILES" 101 \
	>/dev/null || fail warm-spline
sync

run_one()
{
	fs=$1
	rep=$2
	case "$fs" in
	ext4) dir=/ext4/lookup ;;
	splinefs) dir=/spline/lookup ;;
	*) fail bad-fs ;;
	esac
	echo 2 > /proc/sys/vm/drop_caches || fail drop-caches
	echo "KVM_PERF_BEGIN rep=$rep fs=$fs"
	LOOKUP_BENCH_PERF=1 \
	LOOKUP_BENCH_MISS_BASE=$((FILES * 16 + rep * LOOKUPS)) \
		/lookup_bench run "$dir" "$MODE" \
		"$LOOKUPS" "$FILES" $((rep + 1000)) || fail perf-$fs-$rep
	echo "KVM_PERF_END rep=$rep fs=$fs"
}

rep=1
while [ "$rep" -le "$REPS" ]; do
	if [ $((rep % 2)) -eq 1 ]; then
		run_one splinefs "$rep"
		run_one ext4 "$rep"
	else
		run_one ext4 "$rep"
		run_one splinefs "$rep"
	fi
	rep=$((rep + 1))
done

manual=$(awk '$1 == "manual_operations" { print $2 }' \
	/sys/kernel/debug/ext5/li_stats)
bounds=$(awk '$1 == "rs_bound_violations" { print $2 }' \
	/sys/kernel/debug/ext5/li_stats)
test "$manual" -eq 0 || fail final-manual-contamination
test "$bounds" -eq 0 || fail final-bound
echo "KVM_PERF_STATS files=$FILES lookups=$LOOKUPS mode=$MODE settle=$SETTLE_OPS manual=$manual bounds=$bounds"

sync
umount /spline || fail umount-ext5
umount /ext4 || fail umount-ext4
rmmod ext5 || fail rmmod-ext5
rmmod jbd3 || fail rmmod-jbd3
echo KVM_PERF_PASS
poweroff -f
sleep 5
