# Claims

The paper's claims, the experiment that measures each, and the paper's
value.  `make figures` compares each measured value with these (the numbers
are in `docs/expected.csv`) and writes `results/SUMMARY.md`.  Experiments are
campaigns of `scripts/run_queue.sh`.

## 1. Headline

| # | Claim | Campaign | Paper |
|---|---|---|---|
| H1 | Directory metadata up to 19.7x smaller than ext4 | `footprint` | 19.7x on GUFI *yellusers* |
| H2 | Cold scans up to 3.6x faster on real namespaces | `realworld` | 3.65x on *yellusers* (10% sample), 2 GB cap |
| H3 | Application stages up to 2.1x faster | `applookup` | 2.10x on the `tar` pass |

## 2. Evaluation (§5)

| # | Claim | Paper | Campaign | Paper's value |
|---|---|---|---|---|
| F9a | Metadata over five namespaces, up to 19.7x below ext4; a per-directory index is largest | Fig. 9a | `footprint` | 1.8 GB against 89 MB on *yellusers* |
| F9b | Positive cold lookups 3.0-4.2x ext4 from 512 MB to 2 GB; parity at 256 MB; a per-directory index stays below ext4 | Fig. 9b | `cache_pressure` | 4.22x, 3.01x, 2.98x at 512 MB, 1 GB, 2 GB; 1.03x at 256 MB |
| F9c | Negative lookups 3.2x ext4 at 2 GB; a per-directory index stays below ext4 | Fig. 9c | `cache_pressure` | 3.19x |
| F10 | Real namespace scans win every row | Fig. 10 | `realworld` | 1.57x (kernel `du`) to 3.65x (*yellusers*) |
| F10full | The full 11.6 M-file *yellusers* tree | §5.4 | `realworld` with `TREES=yellusers:datasets/staged/gufi/yellusers.txt` | 2.03x |
| F11 | Application stages up to 2.1x over ext4, 3.0x over xfs, btrfs, f2fs | Fig. 11 | `applookup` | `tar` 2.10x ext4, 3.04x xfs |
| F12a | No promotion tracks ext4; per-directory models 1.3-1.4x; one model per subtree 1.8x | Fig. 12a | `ablation` | 0.96-1.04x, 1.34-1.41x, up to 1.83x |
| F12b | A learned lookup costs 0.69x the cycles and 0.54x the instructions of htree | Fig. 12b | `lookup_cpu` | 0.69x, 0.54x |
| F13 | mdtest: stat and read up to 1.32x; create 0.95-1.05x; remove 0.80-1.11x | Fig. 13 | `mdtest` | the 5 x 5 grid |
| F14 | Promoted coverage follows the quiet fraction | Fig. 14 | `locality_sweep` | 100, 95.3, 89.8, 75.0, 50.0% at 0, 5, 10, 25, 50% churn; 1.44x to 1.09x |
| F14par | With every region churned, SplineFS matches promotion off | §5.5 | `locality_sweep` | 1.003x promotion off, 1.007x ext4 |

Fig. 10 and Fig. 11 per row, as SplineFS's speedup over each filesystem:

| Fig. 10 namespace | ext4 | xfs | btrfs | f2fs |
|---|---|---|---|---|
| kernel `du` | 1.57x | 2.62x | 2.52x | 14.05x |
| ImageNet metadata | 1.64x | 2.31x | 2.57x | 22.78x |
| *yellusers* 10% | 3.65x | 1.99x | 2.19x | 3.63x |
| *yellprojs* 10% | 3.03x | 1.71x | 1.81x | 2.69x |
| *anony* 10% | 2.02x | 1.14x | 1.14x | 1.76x |
| *scr4* 10% | 1.95x | 1.10x | 1.00x | 1.78x |
| *ttscratch* 10% | 2.02x | 1.19x | 1.21x | 1.79x |
| *yellusers* full | 2.03x | 1.60x | 1.39x | |

| Fig. 11 stage | ext4 | xfs | btrfs | f2fs |
|---|---|---|---|---|
| A1 container rootfs scan | 1.73x | 1.60x | 1.35x | 1.25x |
| A2 DataLoader indexing | 1.12x | 2.13x | 1.81x | 1.48x |
| A3 `tar` | 2.10x | 3.04x | 2.54x | 2.76x |
| A4 SQLite open | 1.31x | 1.45x | 1.55x | 1.78x |
| A5 DuckDB lake scan | 1.14x | 1.08x | 1.62x | 1.53x |
| A6 LlamaIndex chunk load | 1.21x | 1.18x | 0.83x | 1.22x |

## 3. Design (§4) in the code

| # | Claim | Where |
|---|---|---|
| D1 | One RadixSpline per promoted subtree over sorted `(PLID, hash)` keys | `promote_make_key()`, `sfs_li_radix_sort()`, `sfs_rs_build()`; format in `lidir_format.h` |
| D2 | Keyed HalfSipHash key, full SipHash tiebreak, exact names decide | `promote_add_slot()`, `ext5_stable_inode_by_name()` |
| D3 | A lookup checks the delta before the base, so negatives stay exact | `ext5_stable_inode_by_name()` |
| D4 | The base holds a Bloom filter, radix table, spline, slots, parent ranges and names | `struct lidir_disk_descriptor` in `lidir_format.h` |
| D5 | `T*(R) = max(T_v, c |R| log |R|)` | `ext5_li_t_star()` |
| D6 | One operation clock per filesystem, ticked by dcache-miss lookups and mutations | `ext5_li_op_tick()`, called from `ext5_lookup()` |
| D7 | A mutation stamps every ancestor | `ext5_subtree_count_walk()` |
| D8 | The highest quiet ancestor that qualifies is promoted | `ext5_li_maybe_promote_on_lookup()` |
| D9 | Compaction stages a new generation, validates its delta cut, then publishes its descriptor and in-memory view together | `ext5_stable_compact()` |
| D10 | Insert-dominated refills demote; delete-dominated ones compact | `li_demote_refills`, the compaction worker in `dir_stable.c` |
| D11 | A churning branch demotes on its own; compaction drops its dead PLIDs | `ext5_stable_dominant_churn_plid()`, `compact_drop_dead_plids()` |
| D12 | A learned rename prepares both delta records before changing either name | `ext5_stable_rename_links()`, `ext5_rename()` |
| D13 | A create or unlink logs its delta record in the inode's own transaction | `ext5_stable_add_link()`, `ext5_stable_append_delta()` |
| D14 | Delta records carry a CRC and a sequence number; mount replays the valid prefix | `ext5_stable_append_delta()`, `ext5_stable_replay_delta()` |
| D15 | `jbd3` is `jbd2` renamed | `scripts/upstream_diff.sh` lists no `jbd3` source file |
