#!/bin/busybox sh
# SPDX-License-Identifier: GPL-2.0

fail()
{
	echo "QEMU_JOURNAL_FAIL: $*"
	sync
	poweroff -f
	sleep 5
}

finish_lifecycle()
{
	test "$(cat /mnt/tree/b/target)" = source || fail recovery-contents
	test "$(cat /mnt/tree/a/moving/payload)" = payload ||
		fail recovery-race-payload
	test -e /mnt/tree/a/new || fail recovery-new
	test -e /mnt/tree/b/moved.17 || fail recovery-moved
	test ! -e /mnt/tree/a/base.17 || fail recovery-old
	test -d /mnt/tree/b/cross.dst || fail recovery-cross-replace
	test ! -e /mnt/tree/a/cross.src || fail recovery-cross-source
	test -d /mnt/tree/a/same.dst || fail recovery-same-replace
	test ! -e /mnt/tree/a/same.src || fail recovery-same-source
	/ext5_compact /mnt/tree || fail compact
	/ext5_demote_dir /mnt/tree || fail demote
	test -e /mnt/tree/a/new || fail mutable-new
	test -e /mnt/tree/b/moved.17 || fail mutable-moved

	# Reparent a low PLID under a later one. Both compaction and demotion
	# must follow directory links, including grandchildren, regardless of IDs.
	for mode in compact demote rehome; do
		branch=/mnt/order-$mode
		mkdir -p "$branch/a/deep" "$branch/b" || fail order-mkdir
		echo kept > "$branch/a/deep/kept" || fail order-prepare
		/ext5_promote_dir "$branch" || fail order-promote
		mv "$branch/a" "$branch/b/a" || fail order-reparent
		case "$mode" in
		compact) /ext5_compact "$branch" || fail order-compact ;;
		demote) /ext5_demote_dir "$branch" || fail order-demote ;;
		rehome)
			mkdir /mnt/order-destination || fail rehome-destination
			touch /mnt/order-destination/base || fail rehome-base
			/ext5_promote_dir /mnt/order-destination || fail rehome-promote
			mv "$branch/b" /mnt/order-destination/b || fail rehome-rename
			/ext5_li_wait /mnt || fail rehome-wait
			;;
		esac
	done
	mkdir -p /mnt/root-source/model/child /mnt/root-destination || fail root-mkdir
	echo root-kept > /mnt/root-source/model/child/kept || fail root-prepare
	/ext5_promote_dir /mnt/root-source/model || fail root-promote
	mv /mnt/root-source/model /mnt/root-destination/model || fail root-rename
	test ! -e /mnt/root-source/model || fail root-old-path
	sync
	umount /mnt || fail umount-finish
	mount -t ext5 -o noatime /dev/vda /mnt || fail namespace-remount
	test "$(cat /mnt/order-compact/b/a/deep/kept)" = kept || fail compact-reparent-remount
	test "$(cat /mnt/order-demote/b/a/deep/kept)" = kept || fail demote-reparent-remount
	test "$(cat /mnt/order-destination/b/a/deep/kept)" = kept || fail rehome-reparent-remount
	test "$(cat /mnt/root-destination/model/child/kept)" = root-kept || fail root-remount
	/ext5_demote_dir /mnt/order-compact || fail order-demote-after-compact
	/ext5_demote_dir /mnt/root-destination/model || fail moved-root-demote
	# A subsequent conventional rename validates the reconstructed '..'.
	mv /mnt/root-destination/model /mnt/root-source/model || fail moved-root-dotdot
	sync
	umount /mnt || fail namespace-final-unmount
	rmmod ext5 || fail rmmod-ext5
	rmmod jbd3 || fail rmmod-jbd3
	echo QEMU_JOURNAL_PASS
	poweroff -f
	sleep 5
}

mount -t proc proc /proc || fail proc
mount -t sysfs sysfs /sys || fail sysfs
mount -t devtmpfs devtmpfs /dev || fail devtmpfs
mkdir -p /mnt
insmod /jbd3.ko || fail jbd3
insmod /ext5.ko li_policy_mode=0 li_debug_controls=Y li_stats_enabled=Y || fail ext5
mount -t ext5 -o noatime /dev/vda /mnt || fail mount

CMDLINE=$(cat /proc/cmdline)
case "$CMDLINE" in
*spline_phase=recover*)
	finish_lifecycle
	;;
*spline_phase=clean*)
	PHASE=clean
	;;
*)
	PHASE=crash
	;;
esac

mkdir -p /mnt/tree/a /mnt/tree/b || fail mkdir
mkdir /mnt/tree/a/moving || fail moving-mkdir
printf payload > /mnt/tree/a/moving/payload || fail moving-payload
mkdir /mnt/tree/a/emptystable /mnt/tree/a/deletestable ||
	fail empty-test-mkdir
touch /mnt/tree/a/deletestable/child || fail delete-test-child
mkdir /mnt/tree/a/cross.src /mnt/tree/b/cross.dst ||
	fail cross-replace-mkdir
mkdir /mnt/tree/a/same.src /mnt/tree/a/same.dst ||
	fail same-replace-mkdir
touch /mnt/tree/source /mnt/tree/destination || fail race-exchange-files
i=0
while [ "$i" -lt 256 ]; do
	touch "/mnt/tree/a/base.$i" "/mnt/tree/b/base.$i" || fail prepare
	i=$((i + 1))
done
/ext5_promote_dir /mnt/tree || fail promote
/rename_race /mnt/tree 250 || fail rename-race

touch /mnt/tree/a/emptystable/child || fail empty-delta-child
if rmdir /mnt/tree/a/emptystable 2>/dev/null; then
	fail nonempty-delta-rmdir
fi
rm /mnt/tree/a/emptystable/child || fail empty-delta-unlink
rmdir /mnt/tree/a/emptystable || fail empty-delta-rmdir
rm /mnt/tree/a/deletestable/child || fail base-child-unlink
rmdir /mnt/tree/a/deletestable || fail empty-deleted-base-rmdir

a_links=$(stat -c %h /mnt/tree/a) || fail cross-a-links-before
b_links=$(stat -c %h /mnt/tree/b) || fail cross-b-links-before
mv -fT /mnt/tree/a/cross.src /mnt/tree/b/cross.dst ||
	fail cross-dir-replace
test "$(stat -c %h /mnt/tree/a)" -eq $((a_links - 1)) ||
	fail cross-a-links-after
test "$(stat -c %h /mnt/tree/b)" -eq "$b_links" ||
	fail cross-b-links-after

a_links=$(stat -c %h /mnt/tree/a) || fail same-a-links-before
mv -fT /mnt/tree/a/same.src /mnt/tree/a/same.dst ||
	fail same-dir-replace
test "$(stat -c %h /mnt/tree/a)" -eq $((a_links - 1)) ||
	fail same-a-links-after

echo payload > /mnt/tree/a/new || fail create
ln /mnt/tree/a/new /mnt/tree/a/link || fail link
rm /mnt/tree/a/link || fail unlink
mkdir /mnt/tree/a/empty || fail mkdir-stable
rmdir /mnt/tree/a/empty || fail rmdir-stable
echo source > /mnt/tree/a/source || fail source
echo target > /mnt/tree/b/target || fail target
mv -f /mnt/tree/a/source /mnt/tree/b/target || fail replace
mv /mnt/tree/a/base.17 /mnt/tree/b/moved.17 || fail rename
test "$(cat /mnt/tree/b/target)" = source || fail contents
test ! -e /mnt/tree/a/base.17 || fail old-name
test -e /mnt/tree/b/moved.17 || fail new-name

sync
if [ "$PHASE" = clean ]; then
	umount /mnt || fail umount-clean
	mount -t ext5 -o noatime /dev/vda /mnt || fail remount-clean
	finish_lifecycle
fi

echo QEMU_CRASH_READY
echo 1 > /proc/sys/kernel/sysrq
echo c > /proc/sysrq-trigger
fail panic-returned
sleep 5
