CC ?= gcc-13
CFLAGS ?= -O2 -std=gnu11 -Wall -Wextra -Werror

.PHONY: all test fs-test qemu-test kvm-perf kvm-transition clean

all: radix_spline_test format_test namespace_stress policy_calibrate rename_race \
	cross_root_file_race transition_probe stable_fault clear_lidir_feature \
	readdir_cookie inode_pool_test rename_capacity

radix_spline_test: radix_spline_test.c ../../src/ext5/radix_spline.c \
		../../src/ext5/radix_spline.h
	$(CC) $(CFLAGS) -I../../src/ext5 -o $@ \
		radix_spline_test.c ../../src/ext5/radix_spline.c

format_test: format_test.c ../../src/ext5/lidir_format.h
	$(CC) $(CFLAGS) -I../../src/ext5 -o $@ format_test.c

namespace_stress: namespace_stress.c
	$(CC) $(CFLAGS) -pthread -o $@ namespace_stress.c

policy_calibrate: policy_calibrate.c
	$(CC) $(CFLAGS) -o $@ policy_calibrate.c

rename_race: rename_race.c
	$(CC) $(CFLAGS) -pthread -o $@ rename_race.c

rename_capacity: rename_capacity.c
	$(CC) $(CFLAGS) -o $@ rename_capacity.c

cross_root_file_race: cross_root_file_race.c
	$(CC) $(CFLAGS) -pthread -o $@ cross_root_file_race.c

transition_probe: transition_probe.c
	$(CC) $(CFLAGS) -o $@ transition_probe.c

stable_fault: stable_fault.c ../../src/ext5/lidir_format.h
	$(CC) $(CFLAGS) -o $@ stable_fault.c

clear_lidir_feature: clear_lidir_feature.c
	$(CC) $(CFLAGS) -o $@ clear_lidir_feature.c

readdir_cookie: readdir_cookie.c
	$(CC) $(CFLAGS) -o $@ readdir_cookie.c

inode_pool_test: inode_pool_test.c
	$(CC) $(CFLAGS) -o $@ inode_pool_test.c

test: radix_spline_test format_test
	./radix_spline_test
	./format_test

fs-test: all
	./loopback_fs_smoke.sh
	./journal_fs_smoke.sh
	./rename_smoke.sh
	./policy_smoke.sh
	./idle_promote_smoke.sh
	./delayed_unmount_smoke.sh
	./adaptive_parameters_smoke.sh
	./adaptive_mount_isolation_smoke.sh
	./boundary_smoke.sh
	./policy_reclaim_smoke.sh
	./full_churn_smoke.sh
	./cache_reclaim_smoke.sh
	bash ./nested_promote_reclaim_smoke.sh
	./concurrent_fs_stress.sh
	./delta_tail_cancel_smoke.sh
	./nojournal_churn_smoke.sh
	./compaction_tail_smoke.sh
	./repeated_bounce_smoke.sh
	./bulk_demote_smoke.sh
	DIR_INDEX=0 FILES=1024 ./bulk_demote_smoke.sh
	./shadow_enospc_smoke.sh
	./automatic_demote_smoke.sh
	./idle_demote_smoke.sh
	./branch_demote_smoke.sh

qemu-test: all
	JOURNAL=Y ./qemu_journal_smoke.sh
	JOURNAL=N ./qemu_journal_smoke.sh
	./qemu_fault_smoke.sh
	./qemu_concurrent_smoke.sh

kvm-perf: all
	./kvm_lookup_perf.sh

kvm-transition: all
	./kvm_transition_perf.sh

clean:
	rm -f radix_spline_test format_test namespace_stress policy_calibrate \
		rename_race transition_probe stable_fault clear_lidir_feature \
		cross_root_file_race readdir_cookie inode_pool_test rename_capacity
