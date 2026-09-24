# SplineFS artifact

Artifact for *SplineFS: Adaptive Learned Directory Indexing for
Memory-Constrained Filesystems* (ATC '26). Badges requested: Available,
Functional, Reproduced.

SplineFS extends ext4 with one learned index per quiet directory subtree.
This artifact provides the `ext5` kernel module for Linux 6.16.5, its tests,
and the paper's experiment drivers.

Choose one evaluation route: a VM on your machine (section 2) shows the
performance trends on your hardware; the authors' machine (section 3) runs
the experiments on the paper's hardware.

## 1. Contents

```text
.
├── src/                  ext5 filesystem and jbd3 journal modules
├── utils/
│   ├── src/              benchmarks and control tools
│   ├── evaluation/       workload drivers
│   └── tests/            correctness tests
├── scripts/
│   ├── campaigns/        one wrapper per experiment
│   ├── vm/               VM build, download, and run scripts
│   ├── run_queue.sh      schedules experiments
│   └── check.py          compares results and draws figures
├── docs/                 claims and evaluation details
├── datasets/             input manifest and download scripts
├── results/              measurement output
└── deploy/               generated kernel, module, and VM files (not in Git)
```

`ext5` and `jbd3` are ext4 and jbd2 of Linux 6.16.5, renamed so they load
beside them; `bash scripts/upstream_diff.sh` lists every change we made.

See [CLAIMS.md](docs/CLAIMS.md) for the claim-to-experiment mapping and
[MANIFEST.md](datasets/MANIFEST.md) for datasets and their terms.

## 2. Run in a VM

You need x86-64 Linux with KVM, sudo, at least 16 GiB of RAM, and about
170 GB of free disk space (the 25 GB download can be deleted after the fetch
steps); `make vm-run` needs access to `/dev/kvm` (be in the `kvm` group, or
run it with `sudo`).

The prebuilt VM and the data are in a PKU netdisk folder, [https://disk.pku.edu.cn/link/AABD734EEC0CE7474886EDD2B995AC7CBE](https://disk.pku.edu.cn/link/AABD734EEC0CE7474886EDD2B995AC7CBE); its
password is posted in HotCRP. Download the whole folder (25 GB), keeping
its layout, into a directory such as `~/splinefs-ae-data`. Then:

```sh
make vm-fetch FROM=~/splinefs-ae-data
bash datasets/fetch_data.sh ~/splinefs-ae-data
make vm-run
make trend
```

`vm-fetch` unpacks the prebuilt kernel, module, and Ubuntu VM image;
`make vm-build` builds them instead ([the build script](scripts/vm/build.sh)
lists its dependencies). `fetch_data.sh` stages the real namespaces and
application inputs; without them, Fig. 10 and Fig. 11 are skipped, and
Fig. 9 runs on synthetic trees only. `vm-fetch` and `fetch_data.sh` check
every file against its published checksum. `make vm-tests` runs the kernel
correctness suite.

`make vm-run` runs the `minor` mode (section 4) and writes to `results/`.
For a broader run, use `make vm-run TIER=medium REPS=3`.

| Setting | Default | Purpose |
|---|---|---|
| `VM_DISK` | `deploy/vm/disk.img` | Test disk; created as a 64 GiB sparse image |
| `CONFIRM_DESTROY` | unset | Must equal `VM_DISK` when it names a block device |
| `VM_MEM` | 3/4 of host RAM, at most 16 GiB | Guest memory in MiB |
| `VM_CPUS` | host CPU count, at most 8 | Guest CPUs |
| `TIER` | `minor` | Evaluation mode: `minor`, `medium` or `full` (section 4) |
| `REPS` | `1` | Repetitions of each measurement |
| `CAMPAIGNS` | all | Experiments to run, by campaign name (section 5), space-separated |
| `MAX_CAP` | guest memory minus 2 GiB | Largest memory-cgroup cap an experiment may use, in bytes; larger caps are skipped |

Every campaign reformats its test disk. A block device given as `VM_DISK`
must be disposable: its contents will be destroyed. The runner requires
matching `CONFIRM_DESTROY` and refuses mounted devices. Running
`scripts/run_queue.sh` directly on a host, without the VM, likewise requires
`DEV` and `CONFIRM_DESTROY` to name the same disposable device.

## 3. Run on the authors' machine

Post an SSH public key in HotCRP. We will provide an account number `N`.
Replace `N` in the first command, and run the queue inside `tmux` so that a
dropped connection does not interrupt it:

```sh
ssh -J ae-jumpN@chunk9.top -p 6022 ae-reviewerN@localhost
tmux                                      # after a disconnect: tmux attach
sudo splinefs-ae queue --tier medium
make -C ~/splinefs-ae figures
```

The queue writes to `~/results` (section 6). Use `sudo splinefs-ae status`
to check whether the shared test device is free, and
`sudo splinefs-ae queue --resume` after an interrupted run.

This machine has two Intel Xeon Gold 5218 CPUs (32 cores), 192 GB of DRAM,
and an Intel D7-P5510 NVMe SSD. It runs Ubuntu 24.04.1, Linux 6.16.5,
GCC 13.3, and Python 3.13, with turbo and power saving disabled
(`scripts/internal/host_prep.sh`).

## 4. Evaluation modes

`TIER=` in the VM, or `--tier` on the authors' machine, selects the mode.

| Mode | Time on the authors' machine | Coverage |
|---|---:|---|
| `minor` | about 30 min | A few workloads per experiment; checks that the artifact runs |
| `medium` | about 50 min | Every claim, with fewer workloads than the paper |
| `full` | about 6 h | All workloads available in the artifact |

Times are for one repetition, the default. Use `REPS=3` in the VM or
`--reps 3` on the authors' machine for three repetitions; this takes about
three times as long. Run `scripts/run_queue.sh --dry-run --tier medium` to
see the workload plan without running it.

## 5. Experiments

| Paper | Campaign | What it shows | Minutes: minor / medium / full |
|---|---|---|---|
| Fig. 9a | `footprint` | directory metadata of SplineFS against ext4 and a per-directory index | 6 / 12 / 12 |
| Fig. 9b, 9c | `cache_pressure` | positive and negative lookups under memory caps | 7 / 8 / 90 |
| Fig. 10 | `realworld` | cold scans of real namespaces | 3 / 7 / 13 |
| Fig. 11 | `applookup` | stages of real applications | 1 / 3 / 175 |
| Fig. 12a | `ablation` | no promotion, per-directory, and subtree models | 8 / 9 / 9 |
| Fig. 12b | `lookup_cpu` | cycles and instructions per lookup | 1 / 1 / 1 |
| Fig. 13 | `mdtest` | create, stat, read and remove rates | 1 / 6 / 34 |
| Fig. 14 | `locality_sweep` | promoted coverage and throughput as churn spreads | 5 / 6 / 11 |

To run only Fig. 9a, use `make vm-run TIER=medium CAMPAIGNS=footprint` in
the VM or `sudo splinefs-ae queue --tier medium footprint` on the authors'
machine. Each campaign's settings are listed at the top of its script in
`scripts/campaigns/`.

## 6. Results

Each run writes `results/<panel>/<timestamp>/`, where `<panel>` is the
figure panel, such as `F9a`: the panel's CSV, `config.txt` (parameters and
CPU state), raw output and `run.log`. On the authors' machine,
the same files go under `~/results/`.

`make figures` writes `SUMMARY.md` in the result directory, comparing the
measurements with the paper, and draws the figures beside their CSVs if
matplotlib is installed. VM results depend on your hardware, so `make trend`
writes `TREND.md` instead: for each figure, whether SplineFS's advantage over
ext4 points the way the paper reports.

## 7. License

The kernel modules in `src/` are GPL-2.0 (derived from ext4 and jbd2); see
`LICENSE`. Everything else is MIT; see `LICENSE.tools`. The datasets are
third-party and not part of this artifact; [MANIFEST.md](datasets/MANIFEST.md)
gives their sources and terms.
