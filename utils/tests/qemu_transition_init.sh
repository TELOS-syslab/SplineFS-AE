#!/bin/busybox sh
# SPDX-License-Identifier: GPL-2.0

fail()
{
	echo "KVM_TRANSITION_FAIL: $*"
	sync
	poweroff -f
	sleep 5
}

FILES=200000
PROBES=1000000
REPS=3
mount -t proc proc /proc || fail proc
mount -t sysfs sysfs /sys || fail sysfs
mount -t devtmpfs devtmpfs /dev || fail devtmpfs
for arg in $(cat /proc/cmdline 2>/dev/null); do
	case "$arg" in
	spline_files=*) FILES=${arg#*=} ;;
	spline_probes=*) PROBES=${arg#*=} ;;
	spline_reps=*) REPS=${arg#*=} ;;
	esac
done

mkdir -p /mnt /sys/kernel/debug
mount -t debugfs debugfs /sys/kernel/debug || fail debugfs
insmod /jbd3.ko || fail jbd3
insmod /ext5.ko li_policy_mode=0 li_debug_controls=Y \
	li_stats_enabled=N li_delta_trigger_bytes=8388608 || fail ext5
mount -t ext5 -o noatime /dev/vda /mnt || fail mount
/lookup_bench prepare /mnt/tree "$FILES" || fail prepare

run_transition()
{
	name=$1
	rep=$2
	command=$3
	output=/tmp/$name.$rep.lookup
	attempts=1

	# The preceding probe drops dentries/inodes and can evict the parsed learned
	# generation. Reparse/fault once outside the latency sample so max measures
	# transition overlap/publication rather than one-time cold reconstruction.
	test ! -e "/mnt/tree/prewarm.$name.$rep" || fail prewarm-$name-r$rep
	taskset -c 0 /transition_probe /mnt/tree "$PROBES" \
		"$name-r$rep" > "$output" &
	probe_pid=$!
	sleep 0.05
	if [ "$name" = demotion ]; then
		attempts=0
		while [ "$attempts" -lt 64 ]; do
			attempts=$((attempts + 1))
			taskset -c 1 "$command" /mnt/tree >/dev/null 2>&1
			status=$?
			if [ "$status" -eq 0 ]; then
				break
			fi
			if [ "$status" -ne 3 ]; then
				fail "$name-r$rep-status-$status"
			fi
			sleep 0.01
		done
		test "$status" -eq 0 || fail "$name-r$rep-retries"
	else
		taskset -c 1 "$command" /mnt/tree >/dev/null || fail "$name-r$rep"
	fi
	wait "$probe_pid" || fail "$name-probe-r$rep"
	echo "KVM_TRANSITION_BEGIN rep=$rep transition=$name"
	echo "attempts=$attempts"
	cat "$output"
	echo "KVM_TRANSITION_END rep=$rep transition=$name"
	# Unique misses leave a large negative-dentry set. Reset dentries/inodes
	# between probes (not page cache) so later cycles do not measure cumulative
	# synthetic cache pressure from earlier transition names.
	echo 2 > /proc/sys/vm/drop_caches || fail drop-caches-$name-r$rep
}

rep=1
while [ "$rep" -le "$REPS" ]; do
	run_transition promotion "$rep" /ext5_promote_dir

	# A nonempty delta makes compaction rebuild and publish a full generation.
	i=0
	while [ "$i" -lt 2048 ]; do
		touch "/mnt/tree/seed.$rep.$i" || fail seed-$rep
		i=$((i + 1))
	done
	run_transition compaction "$rep" /ext5_compact
	run_transition demotion "$rep" /ext5_demote_dir
	stat "/mnt/tree/item.00000000000000000017" >/dev/null || fail base-$rep
	stat "/mnt/tree/seed.$rep.17" >/dev/null || fail seed-check-$rep
	rep=$((rep + 1))
done

manual=$(awk '$1 == "manual_operations" { print $2 }' \
	/sys/kernel/debug/ext5/li_stats)
bounds=$(awk '$1 == "rs_bound_violations" { print $2 }' \
	/sys/kernel/debug/ext5/li_stats)
test "$manual" -ge $((REPS * 3)) || fail manual-count
test "$bounds" -eq 0 || fail spline-bound
echo "KVM_TRANSITION_STATS files=$FILES probes=$PROBES reps=$REPS manual=$manual bounds=$bounds"

sync
umount /mnt || fail umount
mount -t ext5 -o noatime /dev/vda /mnt || fail remount
stat /mnt/tree/item.00000000000000000017 >/dev/null || fail remount-base
stat "/mnt/tree/seed.$REPS.17" >/dev/null || fail remount-seed
umount /mnt || fail final-umount
rmmod ext5 || fail rmmod-ext5
rmmod jbd3 || fail rmmod-jbd3
echo KVM_TRANSITION_PASS
poweroff -f
sleep 5
