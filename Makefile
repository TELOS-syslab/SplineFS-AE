.PHONY: help all test fs-test qemu-test clean \
        vm-build vm-fetch vm-run vm-shell vm-tests queue figures trend check

.DEFAULT_GOAL := help

help:
	@echo 'SplineFS artifact.  See README.md.'
	@echo
	@echo 'On your machine, in a VM (needs KVM; the build needs sudo and network):'
	@echo '  make vm-build        6.16.5 kernel, SplineFS, Ubuntu 24.04 image (~20 min)'
	@echo '  make vm-fetch FROM=D or copy them prebuilt from D, the netdisk folder (1.4 GB)'
	@echo '  make vm-run          run TIER=minor|medium|full [REPS=N] inside the VM'
	@echo '  make vm-tests        the QEMU crash and concurrency suite, lockdep kernel'
	@echo
	@echo 'On the evaluation machine: DESTRUCTIVE, needs root and a spare device:'
	@echo '  sudo make queue DEV=/dev/X TIER=medium [REPS=N] [DRY=1]'
	@echo
	@echo 'Results:'
	@echo '  make figures         figures and results/SUMMARY.md'
	@echo '  make trend           which of the paper'"'"'s trends a run shows'
	@echo
	@echo 'Build and test:  make all | test | fs-test | qemu-test | clean'

vm-build:
	bash scripts/vm/build.sh

vm-fetch:
	@test -n "$(FROM)" || { echo 'set FROM to your copy of the netdisk folder (README section 2)'; exit 1; }
	bash scripts/vm/fetch.sh "$(FROM)"

vm-run:
	env TIER="$(or $(TIER),minor)" REPS="$(REPS)" CAMPAIGNS="$(CAMPAIGNS)" \
	    VM_DISK="$(VM_DISK)" CONFIRM_DESTROY="$(CONFIRM_DESTROY)" \
	    VM_MEM="$(VM_MEM)" VM_CPUS="$(VM_CPUS)" MAX_CAP="$(MAX_CAP)" \
	    bash scripts/vm/run.sh

vm-shell:
	bash scripts/vm/run.sh --shell

vm-tests:
	bash scripts/vm/build.sh --tests

# DEV is passed as both DEV and CONFIRM_DESTROY, which the drivers require
# to match.
queue:
	@test -n "$(DEV)" || { echo 'set DEV=/dev/<disposable>'; exit 1; }
	env DEV="$(DEV)" CONFIRM_DESTROY="$(DEV)" \
	    bash scripts/run_queue.sh $(if $(DRY),--dry-run,) --tier "$(or $(TIER),medium)" \
	    $(if $(REPS),--reps "$(REPS)",)

figures:
	python3 scripts/check.py

trend:
	python3 scripts/check.py --trend

check:
	bash scripts/internal/env_check.sh

all:
	$(MAKE) -C src -j$${JOBS:-8}
	$(MAKE) -C utils -j$${JOBS:-8}

test: all
	$(MAKE) -C utils/tests test

fs-test: all
	$(MAKE) -C utils/tests fs-test

qemu-test: all
	$(MAKE) -C utils/tests qemu-test

clean:
	$(MAKE) -C src clean
	$(MAKE) -C utils clean
