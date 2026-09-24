# Datasets

The real namespaces and application inputs the paper uses.  Campaigns look
for them under `datasets/staged/`, use what is there, and skip the rest; the
synthetic workloads need nothing.

## 1. Getting them

On our machine, over SSH, everything is staged, and `~/datasets` links to it.

On your own machine, `fetch_data.sh` stages the files the paper's runs
used, checks each against `data.list`, and unpacks archives where the
campaigns look for them.  The files are in a PKU netdisk folder,
<https://disk.pku.edu.cn/link/AABD734EEC0CE7474886EDD2B995AC7CBE> (password posted in HotCRP); download the
whole folder into one directory that keeps its layout.

```sh
bash datasets/fetch_data.sh ~/splinefs-ae-data                     # everything
bash datasets/fetch_data.sh ~/splinefs-ae-data gufi/yellusers_     # a subset, by prefix
```

No evaluation mode uses the inputs of `realworld` with `BIG=1` or of the
full *yellusers* tree (10.4 GB), so they are not distributed:
`fetch_gufi.sh --all` rebuilds them, and they are staged on our machine.

The VM cannot follow symbolic links out of the shared tree, so stage real
files and directories.

The other scripts rebuild inputs from their original sources instead:
`fetch_gufi.sh` (the GUFI lists, from the GUFI authors' FTP server; set
`GUFI_PROXY` to an HTTP proxy where it is unreachable), `stage_linux.sh` (the
Linux inputs, from Linux 6.16.5 rather than the paper's 6.6.1), and
`make_apks.sh`, `make_corpus.sh`, `make_repos.sh` and `build_rag_chunks.py`
(from F-Droid and GitHub as they are today: the paper's shape, not its exact
content).

## 2. Contents

| Staged path | Size | Used by |
|---|---|---|
| `gufi/yellusers_2M.txt` | 0.25 GB | footprint (*yellusers*), cache_pressure |
| `gufi/yellusers_sample.txt` | 0.19 GB | realworld (*yellusers*) |
| `gufi/yellusers.txt` | 1.9 GB | realworld, the full tree (not distributed) |
| `gufi/{yellprojs,anony,scr4,ttscratch}_sample.txt` | 1.7-2.8 GB each | realworld with `BIG=1` (not distributed) |
| `realapp_data/{apks,corpus,repos_large,linux-src}` | 2.5, 8.8, 6.9, 1.3 GB | footprint |
| `realapp_data/container_rootfs.tar` | 2.3 GB | applookup, A1 |
| `realapp_data/imagenet_subset_1400x250.tar` | 45.5 GB | applookup, A2 (a stand-in; see section 3) |
| `realapp_data/linux_src.tar` | 1.4 GB | applookup, A3 |
| `realapp_data/lake_100000.tar` | 0.2 GB | applookup, A5 |
| `realapp_data/rag_chunks.tar` | 0.35 GB | applookup, A6 |
| `traces/realapp_kernel_{prep,du_scan}.trace` | 0.05 GB | realworld (kernel `du`) |
| `traces/realapp_imagenet_md_{prep,scan}.trace` | 0.19 GB | realworld (ImageNet metadata, full mode) |

A4 builds its own databases.  The three *yellusers* lists are prefixes of the
14.9 M-entry `yellusers.txt`: Fig. 9 uses the first 2,000,000 entries,
Fig. 10 the first 1,485,000 (10%).  The kernel traces are a recorded `du` of
a Linux tree; the ImageNet traces create 2,000 class directories of 500
one-byte files each, the shape of an ImageNet training set, and then `stat`
every file in random order (`utils/src/replay`).

## 3. Sources and terms

The datasets are not part of this artifact, and its licenses do not cover
them.  We provide copies to evaluators for this evaluation only; each keeps
its own terms.

- **GUFI traces**: anonymized metadata scans of LANL filesystems, released by
  Los Alamos National Laboratory as LA-UR-21-21017 and described in
  <https://github.com/mar-file-system/GUFI-Filesystem-Traces>.  That
  repository states no license; use the traces under LANL's terms.
  `fetch_gufi.sh` downloads `GUFITraces.tar.bz2` from the FTP address its
  README gives.  The repository's Git LFS copy is a different archive, and
  its LFS quota has run out before (issue #1 there), so the script does not
  use it.
- **ImageNet**: not distributed.  A2 times only the ImageFolder walk
  (`scandir` and `stat` of every file), so our server provides a stand-in
  with the paper's subset's directory names, file names and sizes, and
  zero-filled contents.  The subset itself comes from ImageNet-1K
  (<https://image-net.org>), licensed for non-commercial research.
- **F-Droid packages, GitHub repositories and the documentation corpus**:
  each under its own license.
- **Container root filesystem** (A1): public Docker images, whose packages
  keep their own licenses.
- **Linux source**: GPL-2.0, from kernel.org.
