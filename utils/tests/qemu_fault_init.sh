#!/bin/busybox sh
# SPDX-License-Identifier: GPL-2.0

fail()
{
	echo "QEMU_FAULT_FAIL: $*"
	sync
	poweroff -f
	sleep 5
}

mount -t proc proc /proc || fail proc
mount -t sysfs sysfs /sys || fail sysfs
mount -t devtmpfs devtmpfs /dev || fail devtmpfs
mkdir -p /mnt /mnt2
insmod /jbd3.ko || fail jbd3
DELTA_MODE=0

case "$(cat /proc/cmdline)" in
*spline_fault=descriptor-header*)
	MODE=descriptor-header
	;;
*spline_fault=descriptor-base*)
	MODE=descriptor-base
	;;
*spline_fault=delta-tail*)
	MODE=delta-tail
	DELTA_MODE=1
	;;
*spline_fault=delta-sequence*)
	MODE=delta-sequence
	DELTA_MODE=1
	;;
*spline_fault=op-clock*)
	MODE=op-clock
	;;
*spline_fault=mount-clock*)
	MODE=mount-clock
	;;
*spline_fault=nested-promote*)
	MODE=nested-promote
	;;
*spline_fault=directory-only*)
	MODE=directory-only
	;;
*spline_fault=inode-pool-*|*spline_fault=delta-capacity*)
	for arg in $(cat /proc/cmdline); do
		case "$arg" in spline_fault=*) MODE=${arg#spline_fault=} ;; esac
	done
	;;
*)
	fail bad-mode
	;;
esac

if [ "$MODE" = op-clock ]; then
	insmod /ext5.ko li_policy_mode=1 li_debug_controls=Y \
		li_quiet_ops=1 li_size_floor_entries=1000000 \
		li_stats_enabled=Y || fail ext5
	mkdir -p /sys/kernel/debug
	mount -t debugfs debugfs /sys/kernel/debug || fail debugfs
elif [ "$MODE" = mount-clock ]; then
	insmod /ext5.ko li_policy_mode=1 li_quiet_ops=32 \
		li_size_floor_entries=16 li_buildcost_permille=1 \
		li_promote_idle_ms=0 li_stats_enabled=Y || fail ext5
	mkdir -p /sys/kernel/debug
	mount -t debugfs debugfs /sys/kernel/debug || fail debugfs
elif [ "$MODE" = directory-only ]; then
	insmod /ext5.ko li_policy_mode=2 li_debug_controls=Y \
		li_size_floor_entries=16 li_quiet_ops=32 li_buildcost_permille=1 \
		li_promote_idle_ms=60000 || fail ext5
elif [ "${MODE#delta-capacity}" != "$MODE" ]; then
	insmod /ext5.ko li_policy_mode=0 li_debug_controls=Y \
		li_delta_trigger_bytes=8388608 || fail ext5
else
	insmod /ext5.ko li_policy_mode=0 li_debug_controls=Y || fail ext5
fi
mount -t ext5 -o noatime /dev/vda /mnt || fail mount

case "$MODE" in
delta-capacity*)
	mkdir /mnt/capacity /mnt/other || fail capacity-mkdir
	echo source > /mnt/capacity/s || fail capacity-source
	echo target > /mnt/capacity/t || fail capacity-target
	echo other > /mnt/other/t || fail capacity-other
	echo other-source > /mnt/other/s || fail capacity-other-source
	/ext5_promote_dir /mnt/capacity || fail capacity-promote
	/ext5_promote_dir /mnt/other || fail capacity-other-promote
	/rename_capacity fill /mnt/capacity || fail capacity-fill
	/rename_capacity fill /mnt/other || fail capacity-other-fill
	touch /mnt/other/x || fail capacity-other-last
	test "$(/ext5_li_info /mnt/capacity | awk '$1=="delta_used:" {print $2}')" -eq 4024 || fail capacity-cut
	/stable_fault /mnt/capacity /mnt /dev/vda delta-capacity || fail capacity-fixture
	mount -t ext5 -o noatime /dev/vda /mnt || fail capacity-remount
	/stable_fault /mnt/other /mnt /dev/vda delta-capacity || fail capacity-other-fixture
	mount -t ext5 -o noatime /dev/vda /mnt || fail capacity-other-remount
	/rename_capacity rename /mnt/capacity || fail capacity-rename
	/rename_capacity rename /mnt/other /mnt/capacity || fail capacity-cross-root
	sync
	umount /mnt || fail capacity-unmount
	mount -t ext5 -o noatime /dev/vda /mnt || fail capacity-final-remount
	test "$(cat /mnt/capacity/s)" = source || fail capacity-lost-source
	test "$(cat /mnt/capacity/t)" = target || fail capacity-replaced-target
	test "$(cat /mnt/other/t)" = other || fail capacity-cross-replaced-target
	test "$(cat /mnt/other/s)" = other-source || fail capacity-cross-lost-source
	umount /mnt || fail capacity-final-unmount
	rmmod ext5 || fail capacity-unload
	rmmod jbd3 || fail capacity-unload-jbd3
	echo "QEMU_FAULT_PASS $MODE"
	poweroff -f
	sleep 5
	;;
inode-pool-*)
	mkdir /mnt/tree || fail pool-mkdir
	/inode_pool_test /mnt/tree 128 22 || fail pool-mutable
	i=0
	while [ "$i" -lt 64 ]; do
		touch "/mnt/tree/base.$i" || fail pool-base
		i=$((i + 1))
	done
	/ext5_promote_dir /mnt/tree || fail pool-promote
	# Exercise the descriptor's odd slot as well as its initial even slot.
	for pass in 1 2; do
		touch "/mnt/tree/precompact.$pass" || fail pool-precompact-tail
		/ext5_compact /mnt/tree || fail pool-precompact
	done
	/inode_pool_test /mnt/tree 0 22 || fail pool-zero
	/inode_pool_test /mnt/tree 128 0 || fail pool-reserve
	/inode_pool_test /mnt/tree 128 17 || fail pool-duplicate
	i=0
	while [ "$i" -lt 8 ]; do
		touch "/mnt/tree/pool.$i" || fail pool-create-first
		i=$((i + 1))
	done
	sync
	echo 2 > /proc/sys/vm/drop_caches
	test -e /mnt/tree/base.63 || fail pool-reparse
	while [ "$i" -lt 16 ]; do
		touch "/mnt/tree/pool.$i" || fail pool-create-second
		i=$((i + 1))
	done
	/ext5_compact /mnt/tree || fail pool-compact
	/stable_fault /mnt/tree /mnt /dev/vda pool-check 16 || fail pool-cursor
	mount -t ext5 -o noatime /dev/vda /mnt || fail pool-remount
	# The reservation ioctl is deliberately the first access after remount.
	/inode_pool_test /mnt/tree 128 17 || fail pool-persisted-duplicate
	touch /mnt/tree/pool.after-remount || fail pool-post-remount
	/ext5_compact /mnt/tree || fail pool-post-remount-compact
	/stable_fault /mnt/tree /mnt /dev/vda pool-check 17 || fail pool-post-remount-cursor
	mount -t ext5 -o noatime /dev/vda /mnt || fail pool-final-remount
	/ext5_demote_dir /mnt/tree || fail pool-demote
	/inode_pool_test /mnt/tree 128 22 || fail pool-demoted
	i=0
	while [ "$i" -lt 16 ]; do
		test -e "/mnt/tree/pool.$i" || fail pool-final-name
		i=$((i + 1))
	done
	test -e /mnt/tree/base.63 || fail pool-final-base
	test -e /mnt/tree/pool.after-remount || fail pool-final-tail
	umount /mnt || fail pool-final-umount
	rmmod ext5 || fail rmmod-ext5
	rmmod jbd3 || fail rmmod-jbd3
	echo "QEMU_FAULT_PASS $MODE"
	poweroff -f
	sleep 5
	;;
esac

if [ "$MODE" = directory-only ]; then
	mkdir -p /mnt/tree/child || fail directory-only-mkdir
	i=0
	while [ "$i" -lt 64 ]; do
		touch "/mnt/tree/child/base.$i" "/mnt/tree/base.$i" || fail directory-only-base
		i=$((i + 1))
	done
	/ext5_promote_dir /mnt/tree/child || fail directory-only-child
	echo 2 > /sys/module/ext5/parameters/li_policy_mode
	echo 16 > /sys/module/ext5/parameters/li_size_floor_entries
	echo 32 > /sys/module/ext5/parameters/li_quiet_ops
	echo 1 > /sys/module/ext5/parameters/li_buildcost_permille
	echo 0 > /sys/module/ext5/parameters/li_promote_idle_ms
	i=0
	while [ "$i" -lt 128 ]; do
		test ! -e "/mnt/tree/missing.$i" || fail directory-only-miss
		i=$((i + 1))
	done
	/ext5_li_wait /mnt || fail directory-only-wait
	# Mode 2 builds one model per directory. Promoting a parent must leave
	# the learned child as an independent ROOT rather than an INTERIOR.
	test "$(/ext5_li_info /mnt/tree | awk '$1=="parent_count:" {print $2}')" -eq 1 || fail directory-only-parent-scope
	test "$(/ext5_li_info /mnt/tree/child | awk '$1=="parent_count:" {print $2}')" -eq 1 || fail directory-only-child-scope
	test -e /mnt/tree/child/base.63 || fail directory-only-lookup
	umount /mnt || fail directory-only-umount
	mount -t ext5 -o noatime /dev/vda /mnt || fail directory-only-remount
	test -e /mnt/tree/child/base.63 || fail directory-only-remount-lookup
	umount /mnt || fail directory-only-cleanup
	rmmod ext5 || fail rmmod-ext5
	rmmod jbd3 || fail rmmod-jbd3
	echo "QEMU_FAULT_PASS $MODE"
	poweroff -f
	sleep 5
fi

if [ "$MODE" = nested-promote ]; then
	for state in warm reclaimed remounted; do
		tree=/mnt/tree-$state
		mkdir -p "$tree/child/deep" "$tree/mid/learned" || fail nested-mkdir
		touch "$tree/child/deep/base" "$tree/mid/learned/base" || fail nested-base
		/ext5_promote_dir "$tree/child" || fail nested-child-promote
		/ext5_promote_dir "$tree/mid/learned" || fail nested-deep-promote
		sync
		case "$state" in
		reclaimed) echo 2 > /proc/sys/vm/drop_caches ;;
		remounted)
			umount /mnt || fail nested-umount
			mount -t ext5 -o noatime /dev/vda /mnt || fail nested-remount
			;;
		esac
		# No child pathname is accessed after reclaim/remount. The walk must
		# recover each parent from its snapshot, at either depth, without a
		# child alias. Two cycles also exercise re-used collection buffers.
		/ext5_promote_dir "$tree" || fail nested-parent-promote-$state
		test -e "$tree/child/deep/base" || fail nested-base-after-promote
		test -e "$tree/mid/learned/base" || fail nested-deep-after-promote
		touch "$tree/child/tail" || fail nested-tail
		/ext5_compact "$tree" || fail nested-compact
		/ext5_demote_dir "$tree" || fail nested-parent-demote
		/ext5_promote_dir "$tree" || fail nested-repromote
		/ext5_demote_dir "$tree" || fail nested-redemote
		sync
		umount /mnt || fail nested-final-umount
		mount -t ext5 -o noatime /dev/vda /mnt || fail nested-final-remount
		test -e "$tree/child/deep/base" || fail nested-base-after-demote
		test -e "$tree/mid/learned/base" || fail nested-deep-after-demote
		test -e "$tree/child/tail" || fail nested-tail-after-demote
	done
	umount /mnt || fail nested-cleanup
	rmmod ext5 || fail rmmod-ext5
	rmmod jbd3 || fail rmmod-jbd3
	echo "QEMU_FAULT_PASS $MODE"
	poweroff -f
	sleep 5
fi

if [ "$MODE" = mount-clock ]; then
	mount -t ext5 -o noatime /dev/vdb /mnt2 || fail mount-second
	mkdir /mnt/a /mnt2/b || fail clock-roots
	i=0
	while [ "$i" -lt 32 ]; do
		touch "/mnt/a/base.$i" || fail clock-prepare-a
		touch "/mnt2/b/base.$i" || fail clock-prepare-b
		i=$((i + 1))
	done

	# Activity on B must not increase A's RMD.
	i=0
	while [ "$i" -lt 128 ]; do
		test ! -e "/mnt2/b/missing.$i" || fail clock-missing-b
		i=$((i + 1))
	done
	test ! -e /mnt/a/one-local-miss || fail clock-local-probe
	/ext5_li_wait /mnt || fail clock-isolation-wait
	if /ext5_li_info /mnt/a >/dev/null 2>&1; then
		fail cross-mount-clock-aged-a
	fi

	# The same amount of activity on A must cross its own T*(R).
	i=0
	while [ "$i" -lt 128 ]; do
		test ! -e "/mnt/a/missing.$i" || fail clock-missing-a
		i=$((i + 1))
	done
	/ext5_li_wait /mnt || fail clock-promotion-wait
	/ext5_li_info /mnt/a >/dev/null || fail clock-a-not-promoted
	test "$(awk '$1 == "manual_operations" { print $2 }' \
		/sys/kernel/debug/ext5/li_stats)" -eq 0 || fail clock-manual

	sync
	umount /mnt2 || fail clock-umount-second
	umount /mnt || fail clock-umount-first
	rmmod ext5 || fail rmmod-ext5
	rmmod jbd3 || fail rmmod-jbd3
	echo "QEMU_FAULT_PASS $MODE"
	poweroff -f
	sleep 5
fi

mkdir /mnt/tree || fail mkdir
i=0
while [ "$i" -lt 256 ]; do
	touch "/mnt/tree/base.$i" || fail prepare
	i=$((i + 1))
done
/ext5_promote_dir /mnt/tree || fail promote

if [ "$MODE" = op-clock ]; then
	opclock()
	{
		awk '$1 == "policy_op_clock" { print $2 }' \
			/sys/kernel/debug/ext5/li_stats
	}
	assert_one_tick()
	{
		before=$1
		after=$(opclock)
		test $((after - before)) -eq 1 ||
			fail "op-clock-$2 before=$before after=$after"
	}

	test ! -e /mnt/tree/clock.file || fail clock-file-prewarm
	before=$(opclock)
	touch /mnt/tree/clock.file || fail clock-create
	assert_one_tick "$before" create
	before=$(opclock)
	rm /mnt/tree/clock.file || fail clock-unlink
	assert_one_tick "$before" unlink

	test ! -e /mnt/tree/clock.dir || fail clock-dir-prewarm
	before=$(opclock)
	mkdir /mnt/tree/clock.dir || fail clock-mkdir
	assert_one_tick "$before" mkdir
	before=$(opclock)
	rmdir /mnt/tree/clock.dir || fail clock-rmdir
	assert_one_tick "$before" rmdir

	touch /mnt/tree/rename.source || fail rename-source
	test ! -e /mnt/tree/rename.target || fail rename-target-prewarm
	before=$(opclock)
	mv /mnt/tree/rename.source /mnt/tree/rename.target || fail clock-rename
	assert_one_tick "$before" rename
	test ! -e /mnt/tree/rename.link || fail link-target-prewarm
	before=$(opclock)
	ln /mnt/tree/rename.target /mnt/tree/rename.link || fail clock-link
	assert_one_tick "$before" link

	sync
	umount /mnt || fail clock-umount
	rmmod ext5 || fail rmmod-ext5
	rmmod jbd3 || fail rmmod-jbd3
	echo "QEMU_FAULT_PASS $MODE"
	poweroff -f
	sleep 5
fi

if [ "$DELTA_MODE" -eq 1 ]; then
	echo prefix > /mnt/tree/prefix || fail prefix-create
	echo tail > /mnt/tree/tail || fail tail-create
fi

# stable_fault synchronizes, obtains FIEMAP while mounted, cleanly unmounts,
# and only then changes the disposable block device.
/stable_fault /mnt/tree /mnt /dev/vda "$MODE" || fail inject
mount -t ext5 -o noatime /dev/vda /mnt || fail remount-fault
test -e /mnt/tree/base.17 || fail base-after-fault

if [ "$DELTA_MODE" -eq 1 ]; then
	test "$(cat /mnt/tree/prefix)" = prefix || fail prefix-after-fault
	test ! -e /mnt/tree/tail || fail torn-tail-visible
	echo after > /mnt/tree/after || fail append-after-tail
	sync
	umount /mnt || fail tail-umount
	mount -t ext5 -o noatime /dev/vda /mnt || fail tail-remount
	test "$(cat /mnt/tree/prefix)" = prefix || fail prefix-after-remount
	test "$(cat /mnt/tree/after)" = after || fail append-after-remount
	test ! -e /mnt/tree/tail || fail torn-tail-after-remount
fi

/ext5_compact /mnt/tree || fail compact
/ext5_demote_dir /mnt/tree || fail demote
test -e /mnt/tree/base.17 || fail base-after-demote
if [ "$DELTA_MODE" -eq 1 ]; then
	test "$(cat /mnt/tree/prefix)" = prefix || fail prefix-after-demote
	test "$(cat /mnt/tree/after)" = after || fail append-after-demote
	test ! -e /mnt/tree/tail || fail torn-tail-after-demote
fi
sync
umount /mnt || fail final-umount
rmmod ext5 || fail rmmod-ext5
rmmod jbd3 || fail rmmod-jbd3
echo "QEMU_FAULT_PASS $MODE"
poweroff -f
sleep 5
