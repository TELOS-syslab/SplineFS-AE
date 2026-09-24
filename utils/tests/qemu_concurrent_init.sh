#!/bin/busybox sh
# SPDX-License-Identifier: GPL-2.0

fail()
{
	echo "QEMU_CONCURRENT_FAIL: $*"
	sync
	poweroff -f
	sleep 5
}

mount -t proc proc /proc || fail proc
mount -t sysfs sysfs /sys || fail sysfs
mount -t devtmpfs devtmpfs /dev || fail devtmpfs
mkdir -p /mnt /sys/kernel/debug
mount -t debugfs debugfs /sys/kernel/debug || fail debugfs
insmod /jbd3.ko || fail jbd3
insmod /ext5.ko li_policy_mode=0 li_debug_controls=Y \
	li_stats_enabled=Y li_delta_trigger_bytes=8388608 || fail ext5
mount -t ext5 -o noatime /dev/vda /mnt || fail mount

mkdir /mnt/tree || fail tree
d=0
while [ "$d" -lt 16 ]; do
	mkdir "/mnt/tree/d$d" || fail prepare-dir
	i=0
	while [ "$i" -lt 128 ]; do
		touch "/mnt/tree/d$d/base.$i" || fail prepare-file
		i=$((i + 1))
	done
	d=$((d + 1))
done

# The ancestor must subsume independently promoted children while mutations
# race its snapshots. It must take no transition mutex under a snapshot lock.
/ext5_promote_dir /mnt/tree/d7 || fail nested-seed-d7
/ext5_promote_dir /mnt/tree/d9 || fail nested-seed-d9

# Promotion may publish only a completely version-validated snapshot. Traffic
# can force EAGAIN, but must never expose a partial mode transition.
/namespace_stress /mnt/tree 4 300 &
stress_pid=$!
promoted=0
attempt=0
while [ "$attempt" -lt 64 ]; do
	if /ext5_promote_dir /mnt/tree >/dev/null 2>&1; then
		promoted=1
		break
	fi
	attempt=$((attempt + 1))
done
wait "$stress_pid" || fail promotion-traffic
if [ "$promoted" -eq 0 ]; then
	/ext5_promote_dir /mnt/tree || fail promotion-after-traffic
fi
/ext5_li_info /mnt/tree >/dev/null || fail promoted-info

# Give compaction a real nonempty delta, then keep appending while its private
# snapshot/model/stage pipeline runs. The helper retries only bounded EAGAIN.
i=0
while [ "$i" -lt 128 ]; do
	touch "/mnt/tree/d0/seed.$i" || fail compact-seed
	i=$((i + 1))
done
/namespace_stress /mnt/tree 4 500 &
stress_pid=$!
/ext5_compact /mnt/tree >/dev/null || fail concurrent-compact
wait "$stress_pid" || fail compaction-traffic
/ext5_li_wait /mnt || fail compact-wait
/ext5_li_info /mnt/tree >/dev/null || fail compact-info
stat /mnt/tree/d7/base.91 >/dev/null || fail compact-base

# Demotion builds conventional shadows from a pinned generation and exact
# delta cut. A racing mutation may reject the first build without publication;
# after traffic stops, a retry must complete and retain every permanent name.
/namespace_stress /mnt/tree 4 300 &
stress_pid=$!
/ext5_demote_dir /mnt/tree >/dev/null 2>&1
demote_status=$?
if [ "$demote_status" -ne 0 ] && [ "$demote_status" -ne 3 ]; then
	fail demote-status-$demote_status
fi
wait "$stress_pid" || fail demotion-traffic
/ext5_li_wait /mnt || fail demote-wait
if /ext5_li_info /mnt/tree >/dev/null 2>&1; then
	/ext5_demote_dir /mnt/tree >/dev/null || fail demote-retry
fi
stat /mnt/tree/d7/base.91 >/dev/null || fail demoted-base
stat /mnt/tree/d0/seed.17 >/dev/null || fail demoted-seed

# Exercise the resulting namespace through another complete generation and a
# clean remount so a race cannot hide an in-memory-only success.
/ext5_promote_dir /mnt/tree >/dev/null || fail repromote
/namespace_stress /mnt/tree 4 500 || fail stable-traffic
/ext5_li_wait /mnt || fail final-wait
stat /mnt/tree/d7/base.91 >/dev/null || fail final-base
test "$(awk '$1 == "rs_bound_violations" { print $2 }' \
	/sys/kernel/debug/ext5/li_stats)" -eq 0 || fail spline-bound

sync
umount /mnt || fail umount
mount -t ext5 -o noatime /dev/vda /mnt || fail remount
stat /mnt/tree/d7/base.91 >/dev/null || fail remount-base
stat /mnt/tree/d0/seed.17 >/dev/null || fail remount-seed

# Two long-delay promotions, one inode evicted, then unmount: no promotion
# timer may outlive teardown.
/ext5_demote_dir /mnt/tree >/dev/null || fail teardown-demote
echo 1 > /sys/module/ext5/parameters/li_policy_mode || fail teardown-policy
echo 1 > /sys/module/ext5/parameters/li_size_floor_entries || fail teardown-floor
echo 1 > /sys/module/ext5/parameters/li_quiet_ops || fail teardown-quiet
echo 1 > /sys/module/ext5/parameters/li_buildcost_permille || fail teardown-cost
echo 5000 > /sys/module/ext5/parameters/li_promote_idle_ms || fail teardown-idle
baseline=$(awk '$1 == "auto_promote_scheduled" { print $2 }' \
	/sys/kernel/debug/ext5/li_stats)
mkdir /mnt/victim || fail teardown-victim-mkdir
touch /mnt/victim/seed || fail teardown-victim-seed
touch /mnt/tree/teardown-arm || fail teardown-arm
for dir in victim tree; do
	i=0
	while [ "$i" -lt 64 ]; do
		stat "/mnt/$dir/teardown-miss.$i" >/dev/null 2>&1
		i=$((i + 1))
	done
done
test "$(awk '$1 == "auto_promote_scheduled" { print $2 }' \
	/sys/kernel/debug/ext5/li_stats)" -ge $((baseline + 2)) || \
	fail teardown-not-scheduled
rm /mnt/victim/seed || fail teardown-victim-unlink
rmdir /mnt/victim || fail teardown-victim-rmdir
sync
echo 2 > /proc/sys/vm/drop_caches
umount /mnt || fail final-umount
rmmod ext5 || fail rmmod-ext5
rmmod jbd3 || fail rmmod-jbd3
# Wait past the timers' due time so the log check sees any late callback.
sleep 6
echo QEMU_CONCURRENT_PASS
poweroff -f
sleep 5
