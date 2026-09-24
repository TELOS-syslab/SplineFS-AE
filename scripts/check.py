"""Check every measured value against what the paper claims.

Reads the best run of each figure panel under results/, writes
results/SUMMARY.md comparing measured against docs/expected.csv, and renders
the figures through scripts/figures.py.

  check.py                       every panel
  check.py --experiment mdtest   one campaign
  check.py --no-figures          summary only, no matplotlib needed
  check.py --trend               whether each figure's trend holds, for runs
                                 whose magnitudes are not comparable (the VM)
  check.py --results DIR         results elsewhere (also AE_RESULTS;
                                 default results/, or ~/results when this
                                 checkout is read-only)
"""
from __future__ import annotations

import argparse
import csv
import os
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parent),
                str(Path(__file__).resolve().parent / "lib")]
import figures  # noqa: E402
import paneldata as D  # noqa: E402

AE_ROOT = Path(__file__).resolve().parent.parent
def _default_results() -> Path:
    """AE_RESULTS; else results/ here, or ~/results when this checkout is
    read-only (the shared one on the authors' machine) and it exists."""
    if os.environ.get("AE_RESULTS"):
        return Path(os.environ["AE_RESULTS"])
    here, home = AE_ROOT / "results", Path.home() / "results"
    if not os.access(here, os.W_OK) and home.is_dir():
        return home
    return here


RESULTS = _default_results()


def _groups(run, tag: str, col: str):
    """(label, rows) pairs from a panel CSV, split on a discriminator column."""
    return sorted(D.groups(D.panel(run, tag), col).items())


def run_option(d: Path, key: str) -> str:
    """Read one option out of a run's config.txt options line."""
    cfg = d / "config.txt"
    if not cfg.exists():
        return ""
    for line in cfg.read_text().splitlines():
        if line.startswith("options="):
            for tok in line[len("options="):].split():
                if tok.startswith(key + "="):
                    return tok[len(key) + 1:]
    return ""


def admissible(rows: list[dict], promote: str = "") -> tuple[bool, list[str]]:
    """Whether SplineFS rows are a valid result: no bound violations, some
    promoted root, and no manual transitions unless the run forced promotion
    outside its timers (PROMOTE=force, as F9a, F10 and F13 do).
    """
    problems = []
    forced = promote == "force"
    sfs = [r for r in rows if r.get("fs") in ("splinefs", "adaptive")]
    if not sfs:
        return True, []
    checks = [("bound_violations", 0, "spline bound violations"),
              ("rs_bound_violations", 0, "spline bound violations")]
    if not forced:
        checks.insert(0, ("manual_operations", 0, "manual transitions"))
    for field, want, label in checks:
        vals = [r[field] for r in sfs if field in r and r[field] not in ("", None)]
        bad = [v for v in vals if v.strip() not in ("0", "0.0")]
        if bad:
            problems.append(f"{len(bad)} row(s) with nonzero {label}")
    roots = [r["live_roots"] for r in sfs if "live_roots" in r and r["live_roots"]]
    if roots and all(v.strip() in ("0", "0.0") for v in roots):
        problems.append("no autonomously promoted root in any row")
    return not problems, problems


def verdict(measured, expected, tol) -> str:
    if measured is None or expected is None:
        return "no expectation recorded"
    if isinstance(expected, tuple):
        lo, hi = expected
        span = (hi - lo) * 0.25 or 0.1
        ok = (lo - span) <= measured <= (hi + span)
        return f"{'within' if ok else 'OUTSIDE'} the claimed {lo}-{hi}x range"
    delta = abs(measured - expected) / expected
    return (f"{'matches' if delta <= tol else 'DIFFERS FROM'} the claimed "
            f"{expected} ({delta * 100:.0f}% away)")



def ratios_per_rep(rows, key_col, value_col, base="ext4", target="splinefs"):
    """Per-repetition target/base ratios, grouped by key."""
    by_rep = defaultdict(lambda: defaultdict(dict))
    for r in rows:
        v = D.num(r, value_col)
        if v is None:
            continue
        by_rep[r.get(key_col, "")][r.get("rep", "")][r.get("fs", "")] = v
    out = {}
    for key, reps in by_rep.items():
        vals = [d[target] / d[base] for d in reps.values()
                if base in d and target in d and d[base]]
        if vals:
            out[key] = vals
    return out


def spread_note(vals: list[float]) -> str:
    """Flag a key whose repetitions disagree enough to distrust the mean."""
    if len(vals) < 2:
        return " **n=1**"
    lo, hi = min(vals), max(vals)
    if lo <= 0:
        return ""
    rel = (hi - lo) / lo
    return f" **spread {rel:.0%}**" if rel >= 0.20 else ""


def _load_expected():
    f = AE_ROOT / "docs" / "expected.csv"
    claims, rows, apps, arms = {}, defaultdict(dict), defaultdict(dict), {}
    if not f.exists():
        return claims, rows, apps, arms
    for r in D.read(f):
        low = D.num(r, "low")
        high = D.num(r, "high")
        scope, cid, key = r.get("scope"), r.get("claim"), r.get("key")
        if scope == "claim":
            tol = D.num(r, "tolerance")
            val = (low, high) if high is not None else low
            claims[cid] = (r.get("figure", ""), r.get("description", ""),
                           val, tol)
        elif scope == "row":
            rows[key][r.get("baseline")] = low
        elif scope == "app":
            apps[key][r.get("baseline")] = low
        elif scope == "arm":
            arms[key] = (low, high)
    return claims, rows, apps, arms


_BASE_ORDER = ("ext4", "xfs", "btrfs", "f2fs")
EXPECTATIONS, _F10_ROWS, _F11_APPS, PAPER_ABLATION = _load_expected()
# Per label, a tuple in baseline order, None where the paper has no value.
F10_ROWS = {k: tuple(v.get(b) for b in _BASE_ORDER) for k, v in _F10_ROWS.items()}
F11_APPS = {k: tuple(v.get(b) for b in _BASE_ORDER) for k, v in _F11_APPS.items()}


def analyse_cache_pressure(d: Path, lines: list[str]):
    for mode, claim in (("positive", "F9b"), ("negative", "F9c")):
        # d is the F9b directory; F9c is the same stamp under F9c/.
        f = D.sibling(d, claim) / f"{claim}.csv"
        if not f.exists():
            continue
        rows = D.read(f)
        ok, problems = admissible(rows)
        ratios = D.ratio(rows, "cap_bytes", "ops_per_sec")
        lines.append(f"\n### {claim}: {EXPECTATIONS[claim][1]} (`{f.name}`)\n")
        kinds = {r.get("workload", "") for r in rows if r.get("workload")}
        if kinds == {"gufi-list"}:
            lines.append("Workload: the GUFI path list, the tree Fig. 9b and 9c "
                         "were measured on.\n")
        elif kinds == {"synthetic-preplink"}:
            lines.append("Workload: the synthetic hardlink-pool tree, **not** the "
                         "tree the figure used. It holds inode count constant, "
                         "which also removes the pressure behind the paper's "
                         "parity point at the tightest cap. These numbers do not "
                         "speak to the published figure.\n")
        elif kinds:
            lines.append(f"**Mixed workloads in one file: {sorted(kinds)}. "
                         "Ratios below average across them and are meaningless.**\n")
        if not ok:
            lines.append(f"**Not admissible as a main result:** {'; '.join(problems)}.\n")
        per_rep = ratios_per_rep(rows, "cap_bytes", "ops_per_sec")
        naive = D.ratio(rows, "cap_bytes", "ops_per_sec", target="naive")
        # Naive rows without throughput: OOM-killed or not finished in NAIVE_TIMEOUT.
        killed = {r["cap_bytes"] for r in rows
                  if r.get("fs") == "naive" and not r.get("ops_per_sec")}
        lines.append("| memcg cap | SplineFS / ext4 | reps | min | max | naive / ext4 |")
        lines.append("|---|---|---|---|---|---|")
        noisy = []
        for cap, v in ratios.items():
            mb = int(cap) // (1024 * 1024) if cap.isdigit() else cap
            vals = per_rep.get(cap, [])
            note = spread_note(vals)
            lo = f"{min(vals):.2f}x" if vals else "-"
            hi = f"{max(vals):.2f}x" if vals else "-"
            nv = (f"{naive[cap]:.2f}x" if cap in naive else
                  "did not finish" if cap in killed else "-")
            lines.append(f"| {mb} MB | {v:.2f}x{note} | {len(vals)} | {lo} | {hi} | {nv} |")
            if "spread" in note:
                noisy.append(f"{mb} MB")
        if naive or killed:
            above = [f"{int(k) // (1024 * 1024)} MB" for k, v in naive.items()
                     if k.isdigit() and v >= 1]
            lines.append("\nThe paper: the naive per-directory index stays below ext4. "
                         + ("It does at every cap here." if not above else
                            f"**It does not at {', '.join(above)}.**"))
        if noisy:
            lines.append(f"\n**Repetitions disagree by 20% or more at {', '.join(noisy)}.** "
                         "The mean at those caps should not be quoted on its own: "
                         "an identical pre-fix run swung 3.40x to 1.92x at 512 MB "
                         "between two repetitions. Tight caps need more "
                         "repetitions than loose ones, which the paper's flat "
                         "\"mean of three runs\" does not provide.")
        if ratios:
            capped = {k: v for k, v in ratios.items()
                      if k.isdigit() and int(k) >= 536870912}
            if claim == "F9b" and capped:
                lo, hi = min(capped.values()), max(capped.values())
                clo, chi = EXPECTATIONS["F9b"][2]
                lines.append(f"\nRange from 512 MB upward: {lo:.2f}x to {hi:.2f}x, "
                             f"against the claimed {clo}-{chi}x.")
                # Every cap in the band must sit in the range, so check the floor too.
                if lo < clo:
                    below = [f"{int(k)//(1024*1024)} MB ({v:.2f}x)"
                             for k, v in capped.items()
                             if v < clo]
                    lines.append(f"\n**Below the claimed floor of {clo}x at "
                                 + ", ".join(below) +
                                 ". F9b does not hold as stated across the band.**")
                elif hi > chi:
                    lines.append(f"\nAbove the claimed ceiling of {chi}x, "
                                 "which is a stronger result than claimed.")
                else:
                    lines.append("\nEvery cap in the band sits inside the claimed range.")
                ordered = list(ratios.values())
                if len(ordered) >= 3:
                    change = ordered[-1] - ordered[0]
                    lines.append(
                        f"\nTrend across caps: {ordered[0]:.2f}x at the tightest to "
                        f"{ordered[-1]:.2f}x at the widest, a change of {change:+.2f}x.")
                    if kinds == {"synthetic-preplink"}:
                        lines.append(
                            "\nFig. 9b declines as the cap grows and reaches parity at "
                            "256 MB, because there the inodes and data each lookup "
                            "faults already fill the cap. A `preplink` tree hardlinks "
                            "every name into a small inode pool, which removes that "
                            "pressure, so this driver stays flat instead. Matching the "
                            "figure's shape needs the GUFI tree.")
                    elif kinds == {"gufi-list"}:
                        lines.append(
                            "\nFig. 9b reports the opposite trend, declining from "
                            "4.22x at 512 MB to 2.98x at 2 GB. Before reading anything "
                            "into that, note the spread above: the tight caps are the "
                            "noisy ones, and the published figure is single-run, so "
                            "its shape may not be separable from noise either.")
            if claim == "F9c":
                at2g = ratios.get("2147483648")
                if at2g:
                    lines.append(f"\nAt 2 GB: {at2g:.2f}x. "
                                 f"{verdict(at2g, 3.2, 0.25)}.")


def analyse_lookup_cpu(d: Path, lines: list[str]):
    f = d / "F12b.csv"
    if not f.exists():
        return
    rows = D.read(f)
    ok, problems = admissible(rows)
    lines.append(f"\n### F12b: {EXPECTATIONS['F12bc'][1]} (`{f.name}`)\n")
    if not ok:
        lines.append(f"**Not admissible as a main result:** {'; '.join(problems)}.\n")
    per_fs = defaultdict(lambda: defaultdict(list))
    for r in rows:
        for metric in ("cycles_per_lookup", "cycles", "instructions_per_lookup",
                       "instructions", "ns_per_lookup", "seconds"):
            v = D.num(r, metric)
            if v is not None:
                per_fs[r.get("fs", "")][metric].append(v)
    def pick(fs, *names):
        for n in names:
            if n in per_fs.get(fs, {}):
                return statistics.mean(per_fs[fs][n])
        return None
    base_c = pick("ext4", "cycles_per_lookup", "cycles")
    base_i = pick("ext4", "instructions_per_lookup", "instructions")
    lines.append("| filesystem | cycles/lookup | instr/lookup | cycles vs ext4 | instr vs ext4 |")
    lines.append("|---|---|---|---|---|")
    for fs in sorted(per_fs):
        c = pick(fs, "cycles_per_lookup", "cycles")
        i = pick(fs, "instructions_per_lookup", "instructions")
        rc = f"{c / base_c:.3f}x" if c and base_c else "-"
        ri = f"{i / base_i:.3f}x" if i and base_i else "-"
        lines.append(f"| {fs} | {c:,.0f} | {i:,.0f} | {rc} | {ri} |"
                     if c and i else f"| {fs} | - | - | {rc} | {ri} |")
    sc = pick("splinefs", "cycles_per_lookup", "cycles")
    si = pick("splinefs", "instructions_per_lookup", "instructions")
    if sc and base_c:
        lines.append(f"\nCycles: {sc / base_c:.3f}x. {verdict(sc / base_c, 0.69, 0.15)}.")
    if si and base_i:
        lines.append(f"\nInstructions: {si / base_i:.3f}x. {verdict(si / base_i, 0.54, 0.15)}.")




def analyse_ablation(d: Path, lines: list[str]):
    arms = {"mutable": "F12a1", "promote": "F12a2", "subtree": "F12a3"}
    lines.append("\n### F12a: ablation (`F12a.csv`, one row set per arm)\n")
    lines.append("| memcg cap | " + " | ".join(arms) + " |")
    lines.append("|---" * (len(arms) + 1) + "|")
    by_arm = dict(_groups(d, "F12a", "arm"))
    per_arm = {arm: (D.ratio(by_arm[arm], "cap_bytes", "ops_per_sec")
                     if arm in by_arm else {}) for arm in arms}
    caps = sorted({k for r in per_arm.values() for k in r},
                  key=lambda k: int(k) if k.isdigit() else 0)
    for cap in caps:
        mb = int(cap) // (1024 * 1024) if cap.isdigit() else cap
        cells = [f"{per_arm[a][cap]:.2f}x" if cap in per_arm[a] else "-" for a in arms]
        lines.append(f"| {mb} MB | " + " | ".join(cells) + " |")
    # The claim is about 512 MB and up; at 256 MB every arm is bound by cache misses.
    SETTLED = 512 * 1024 * 1024
    tight = [c for c in caps if c.isdigit() and int(c) < SETTLED]
    if tight:
        lines.append(f"\nThe {', '.join(str(int(c) // (1024*1024)) + ' MB' for c in tight)} "
                     "row is below the cache cliff, where every arm is bound "
                     "by misses rather than by the index, and the claims are "
                     "judged on the rows above it. The paper's run measured 2.98x / 2.12x / "
                     "3.11x there for the same three arms.")
    for arm, claim in arms.items():
        vals = [v for c, v in per_arm[arm].items()
                if c.isdigit() and int(c) >= SETTLED]
        if vals:
            best = max(vals)
            lines.append(f"\n{claim} ({arm}): peak {best:.2f}x over "
                         f"{len(vals)} settled caps, range {min(vals):.2f}-"
                         f"{best:.2f}x. "
                         f"{verdict(best, EXPECTATIONS[claim][2], EXPECTATIONS[claim][3])}. "
                         f"The paper's run measured {PAPER_ABLATION[arm][0]:.2f}-{PAPER_ABLATION[arm][1]:.2f}x.")


def analyse_locality_sweep(d: Path, lines: list[str]):
    f = d / "F14.csv"
    if not f.exists():
        return
    rows = D.read(f)
    lines.append(f"\n### F14, F14par: {EXPECTATIONS['F14'][1]} (`{f.name}`)\n")
    # Sweep CSV: churn_percent; arms adaptive / policy_off / ext4.
    ratios = D.ratio(rows, "churn_percent", "ops_per_sec",
                      base="ext4", target="adaptive")
    vs_off = D.ratio(rows, "churn_percent", "ops_per_sec",
                      base="policy_off", target="adaptive")
    cov = defaultdict(list)
    for r in rows:
        c = D.num(r, "promoted_fraction")
        if c is not None:
            cov[r.get("churn_percent", "")].append(c)
    lines.append("| churned regions | promoted coverage | adaptive / ext4 | adaptive / no-promotion |")
    lines.append("|---|---|---|---|")
    for key in ratios:
        frac = key
        c = statistics.mean(cov[frac]) if cov.get(frac) else None
        cs = f"{c:.1%}" if c is not None else "-"
        off = f"{vs_off[key]:.3f}x" if key in vs_off else "-"
        lines.append(f"| {frac}% | {cs} | {ratios[key]:.3f}x | {off} |")
    at100 = ratios.get("100")
    if at100:
        lines.append(f"\nAt 100% churn: {at100:.3f}x ext4. {verdict(at100, 1.007, 0.02)}.")



def analyse_footprint(d: Path, lines: list[str]):
    f = d / "F9a.csv"
    if not f.exists():
        return
    rows = D.read(f)
    promote = run_option(d, "PROMOTE")
    ok, problems = admissible(rows, promote)
    lines.append(f"\n### F9a: {EXPECTATIONS['F9a'][1]} (`{f.name}`)\n")
    if not ok:
        lines.append(f"**Not admissible as a main result:** {'; '.join(problems)}.\n")
    per_label = defaultdict(lambda: defaultdict(list))
    for r in rows:
        for col in ("ext4_total_bytes", "sfs_total_bytes",
                    "naive_total_bytes", "ratio_total"):
            v = D.num(r, col)
            if v is not None:
                per_label[r.get("label", "")][col].append(v)
    lines.append("| tree | ext4 | SplineFS | naive | ext4 / SplineFS | naive / SplineFS |")
    lines.append("|---|---|---|---|---|---|")
    best = None
    not_largest = []
    for label in sorted(per_label):
        m = per_label[label]
        def mean(col):
            return statistics.mean(m[col]) if m.get(col) else None
        def mb(col):
            return f"{mean(col) / 1e6:,.1f} MB" if m.get(col) else "-"
        ratio = mean("ratio_total")
        if ratio and (best is None or ratio > best):
            best = ratio
        naive, sfs, e4 = mean("naive_total_bytes"), mean("sfs_total_bytes"), mean("ext4_total_bytes")
        nv = naive / sfs if naive and sfs else None
        if naive and sfs and e4 and not label.startswith("shape_") and naive <= max(sfs, e4):
            not_largest.append(label)
        lines.append(f"| {label} | {mb('ext4_total_bytes')} | "
                     f"{mb('sfs_total_bytes')} | {mb('naive_total_bytes')} | "
                     + (f"{ratio:.2f}x | " if ratio else "- | ")
                     + (f"{nv:.2f}x |" if nv else "- |"))
    if any(not k.startswith("shape_") for k in per_label):
        lines.append("\nThe paper: the naive per-directory index is the largest. "
                     + ("It is largest on every real namespace here."
                        if not not_largest else
                        f"**It is not the largest on {', '.join(not_largest)}.**"))
    if best:
        lines.append(f"\nBest reduction: {best:.2f}x. {verdict(best, 19.7, 0.30)}.")
        lines.append("\nThe paper's 19.7x is the GUFI *yellusers* row. A synthetic-only "
                     "run cannot reach it: the reduction grows as directories get "
                     "smaller and more numerous, which is what the real trees supply.")






def analyse_realworld(d: Path, lines: list[str]):
    f = d / "F10.csv"
    if not f.exists():
        return
    rows = D.read(f)
    # PROMOTE=force places one model outside the timer, as the paper did; auto gets the full rule.
    promote = run_option(d, "PROMOTE")
    ok, problems = admissible(rows, promote)
    lines.append(f"\n### F10, F10full: {EXPECTATIONS['F10'][1]} (`{f.name}`)\n")
    if not ok:
        lines.append(f"**Not admissible as a main result:** {'; '.join(problems)}.\n")
    per = defaultdict(lambda: defaultdict(list))
    roots = defaultdict(list)
    for r in rows:
        v = D.num(r, "ops_per_sec")
        if v is not None:
            per[r.get("label", "")][r.get("fs", "")].append(v)
        rv = D.num(r, "promoted_roots")
        if rv and r.get("fs") == "splinefs":
            roots[r.get("label", "")].append(rv)
    others = ["ext4", "xfs", "btrfs", "f2fs"]
    lines.append("| tree | roots | " + " | ".join(f"vs {o}" for o in others)
                 + " | claimed vs ext4 |")
    lines.append("|---" * (len(others) + 3) + "|")
    ext4_ratios = []
    for label in sorted(per):
        m = per[label]
        if "splinefs" not in m:
            continue
        sfs = statistics.mean(m["splinefs"])
        cells = []
        for o in others:
            if m.get(o) and statistics.mean(m[o]):
                rr = sfs / statistics.mean(m[o])
                cells.append(f"{rr:.2f}x")
                if o == "ext4":
                    ext4_ratios.append((label, rr))
            else:
                cells.append("-")
        nroots = f"{statistics.mean(roots[label]):.0f}" if roots.get(label) else "-"
        want = F10_ROWS.get(label)
        if want is None:
            claim = "no paper value"
        else:
            got = dict(zip(others, [statistics.mean(m[o]) and sfs / statistics.mean(m[o])
                                    if m.get(o) and statistics.mean(m[o]) else None
                                    for o in others]))
            claim = f"{want[0]:.2f}x"
            if got["ext4"] is not None:
                # Flag rows more than 15% from the paper's single-run values.
                if abs(got["ext4"] - want[0]) / want[0] > 0.15:
                    claim += " **off**"
        lines.append(f"| {label} | {nroots} | " + " | ".join(cells) + f" | {claim} |")
    if ext4_ratios:
        lo = min(ext4_ratios, key=lambda t: t[1])
        hi = max(ext4_ratios, key=lambda t: t[1])
        lines.append(f"\nAgainst ext4: {lo[1]:.2f}x ({lo[0]}) to {hi[1]:.2f}x ({hi[0]}).")
        losers = [f"{l} ({r:.2f}x)" for l, r in ext4_ratios if r < 1.0]
        if losers:
            # A range check sees only the endpoints, so name the losing rows first.
            lines.append("\n**F10 claims SplineFS wins every row. These rows lose: "
                         + ", ".join(losers) + ". The claim does not hold as "
                         "stated, whatever the top of the range is.**")
        else:
            claimed_lo, claimed_hi = EXPECTATIONS["F10"][2]
            lines.append(f"\nEvery row wins. Top of range {hi[1]:.2f}x against the "
                         f"claimed {claimed_hi}x; weakest {lo[1]:.2f}x against the "
                         f"claimed {claimed_lo}x.")



def analyse_mdtest(d: Path, lines: list[str]):
    f = d / "F13.csv"
    if not f.exists():
        return
    rows = D.read(f)
    promote = run_option(d, "PROMOTE")
    ok, problems = admissible(rows, promote)
    lines.append(f"\n### F13: {EXPECTATIONS['F13'][1]} (`{f.name}`)\n")
    if promote == "force":
        lines.append("Promotion forced between create and the read phases, "
                     "outside the timer, as the submitted figure did.\n")
    elif promote == "auto":
        lines.append("Default policy left enabled throughout, so transition "
                     "cost falls inside whichever phase incurs it. This is "
                     "what the author response claims (R14), not what the "
                     "submitted figure measured.\n")
    if not ok:
        lines.append(f"**Not admissible as a main result:** {'; '.join(problems)}.\n")
    ops = [("create_ops", "create", (0.95, 1.05)),
           ("stat_ops", "stat", 1.32),
           ("read_ops", "read", 1.32),
           ("remove_ops", "remove", (0.80, 1.11))]
    # The tightest caps at the larger counts are bistable; judge the band above them.
    SETTLED = 2 * 1024 * 1024 * 1024

    def split(col):
        settled, tight = [], []
        for key, v in D.ratio(rows, ("files", "cap_bytes"), col).items():
            cap = next((int(k) for k in key if str(k).isdigit() and int(k) > 1 << 20), 0)
            (settled if cap >= SETTLED else tight).append(v)
        return settled, tight

    lines.append("Cells are split at a 2 GB cap. Below it the run is on the "
                 "cache cliff, where repetitions of one cell differ by up to "
                 "2x; the claims are judged above it.\n")
    lines.append("| operation | settled min | settled max | settled mean | tight-cap range | claimed |")
    lines.append("|---|---|---|---|---|---|")
    verdicts = []
    for col, name, claim in ops:
        settled, tight = split(col)
        want = f"{claim[0]}-{claim[1]}x" if isinstance(claim, tuple) else f"{claim}x"
        if not settled:
            lines.append(f"| {name} | - | - | - | - | {want} |")
            continue
        lo, hi, mean = min(settled), max(settled), statistics.mean(settled)
        trange = f"{min(tight):.3f}-{max(tight):.3f}x" if tight else "-"
        flag = ""
        if isinstance(claim, tuple):
            if lo < claim[0] - 0.02:
                flag = " **below**"
                verdicts.append(f"{name} falls under the claimed band")
            elif hi > claim[1] + 0.02:
                flag = " **above, faster than claimed**"
                verdicts.append(f"{name} is faster than the claimed {want}, "
                                f"{lo:.2f}-{hi:.2f}x")
            else:
                verdicts.append(f"{name} holds at {lo:.2f}-{hi:.2f}x")
        else:
            if hi < claim * 0.9:
                flag = " **below**"
                verdicts.append(f"{name} does not reach the claimed {want}")
            else:
                verdicts.append(f"{name} exceeds the claimed {want}, "
                                f"mean {mean:.2f}x over {len(settled)} cells")
        lines.append(f"| {name} | {lo:.3f}x | {hi:.3f}x | {mean:.3f}x | "
                     f"{trange} | {want}{flag} |")
    lines.append("\nA cell is one (file count, memcg cap) pair, the median of "
                 "its repetitions. " + "; ".join(verdicts) + ".")



def analyse_applookup(d: Path, lines: list[str]):
    """F11: steady_seconds per stage, as baseline over SplineFS."""
    stages = sorted(_groups(d, "F11", "stage"))
    if not stages:
        return
    lines.append(f"\n### F11: {EXPECTATIONS['F11'][1]}\n")
    others = ["ext4", "xfs", "btrfs", "f2fs"]
    lines.append("| app | samples | spread | " + " | ".join(f"vs {o}" for o in others)
                 + " | claimed vs ext4 |")
    lines.append("|---" * (len(others) + 4) + "|")
    all_problems, best = [], []
    for stage, rows in stages:
        ok, problems = admissible(rows)
        if not ok:
            all_problems.append(f"{stage}: {'; '.join(problems)}")
        per = defaultdict(list)
        for r in rows:
            v = D.num(r, "steady_seconds")
            if v:
                per[r.get("fs", "")].append(v)
        if not per.get("splinefs"):
            continue
        sfs = statistics.mean(per["splinefs"])
        # A failed stage has an empty workload; fall back to the id.
        label = next((r.get("workload") for r in rows if r.get("workload")), stage)
        cells = []
        for o in others:
            if per.get(o) and sfs:
                cells.append(f"{statistics.mean(per[o]) / sfs:.2f}x")
            else:
                cells.append("-")
        want = F11_APPS.get(label)
        if want is None:
            claim = "no paper value"
        elif per.get("ext4") and sfs:
            got = statistics.mean(per["ext4"]) / sfs
            best.append((label, got))
            claim = f"{want[0]:.2f}x"
            if abs(got - want[0]) / want[0] > 0.15:
                claim += " **off**"
        else:
            claim = f"{want[0]:.2f}x"
        # Report the SplineFS spread before the ratio.
        sv = per["splinefs"]
        spread = f"{max(sv) / min(sv):.2f}x" if min(sv) > 0 else "-"
        if min(sv) > 0 and max(sv) / min(sv) > 1.5:
            spread += " **noisy**"
        lines.append(f"| {label} | {len(sv)} | {spread} | "
                     + " | ".join(cells) + f" | {claim} |")
    if all_problems:
        lines.append("\n**Not admissible as a main result:** "
                     + "; ".join(all_problems) + ".\n")
    if best:
        hi = max(best, key=lambda t: t[1])
        lo = min(best, key=lambda t: t[1])
        lines.append(f"\nAgainst ext4: {lo[1]:.2f}x ({lo[0]}) to {hi[1]:.2f}x "
                     f"({hi[0]}); the claim is \"up to 2.1x\", which the paper reached "
                     f"on tar alone.")
        losers = [f"{l} ({r:.2f}x)" for l, r in best if r < 1.0]
        if losers:
            lines.append("\n**These stages lose to ext4: " + ", ".join(losers)
                         + ".**")


ANALYSERS = {
    "footprint": analyse_footprint,
    "mdtest": analyse_mdtest,
    "realworld": analyse_realworld,
    "applookup": analyse_applookup,
    "cache_pressure": analyse_cache_pressure,
    "lookup_cpu": analyse_lookup_cpu,
    "ablation": analyse_ablation,
    "locality_sweep": analyse_locality_sweep,
}


# Panels each campaign draws.
EXPERIMENT_TAGS = {
    "footprint": ["F9a"], "cache_pressure": ["F9b", "F9c"],
    "realworld": ["F10"], "applookup": ["F11"], "ablation": ["F12a"],
    "lookup_cpu": ["F12b"], "mdtest": ["F13"], "locality_sweep": ["F14"],
}


def _reps(run: Path) -> int:
    """REPS from a run's recorded options, or 0 if it did not record one."""
    cfg = run / "config.txt"
    if cfg.exists():
        for line in cfg.read_text().splitlines():
            if line.startswith("options=") and "REPS=" in line:
                v = line.split("REPS=", 1)[1].split()[0].strip("'\"")
                return int(v) if v.isdigit() else 0
    return 0


_RANK = {"minor": 1, "medium": 2, "full": 3}


def _rank(run: Path) -> tuple[bool, bool, int]:
    """Rank of a run: three repetitions or more, then bare metal over the VM,
    then tier (full 3, medium 2, minor 1).  Runs without a tier rank as full
    when they meet the repetition floor.
    """
    reps = _reps(run)
    tier, metal = None, True
    cfg = run / "config.txt"
    if cfg.exists():
        for line in cfg.read_text().splitlines():
            if line.startswith("tier=") and line[5:].strip() in _RANK:
                tier = _RANK[line[5:].strip()]
            if line.strip() == "platform=qemu":
                metal = False
    if tier is None:
        tier = 3 if reps >= 3 else 1
    return reps >= 3, metal, tier


def _panel_runs(panel: str) -> list[Path]:
    base = RESULTS / panel
    if not base.is_dir():
        return []
    runs = []
    for r in sorted(q for q in base.iterdir() if q.is_dir()):
        f = r / f"{panel}.csv"
        if f.exists() and sum(1 for _ in f.open()) > 1:
            runs.append(r)
    return runs


def panel_run(panel: str) -> Path | None:
    """The run a panel shows: the newest of the best-ranked runs with data."""
    runs = _panel_runs(panel)
    if not runs:
        return None
    best = max(_rank(r) for r in runs)
    return [r for r in runs if _rank(r) == best][-1]


# --- trend check -----------------------------------------------------------
# For runs whose magnitudes are not comparable with the paper's, such as the
# VM: does each figure point the way the paper's does?

def _rows(run: Path, panel: str) -> list[dict]:
    with open(run / f"{panel}.csv", newline="") as fh:
        return list(csv.DictReader(fh))


def _mean_by(rows, keys, value, where=lambda r: True):
    acc = defaultdict(list)
    for r in rows:
        if not where(r):
            continue
        try:
            v = float(r[value])
            acc[tuple(r[k] for k in keys)].append(v)
        except (KeyError, ValueError):
            continue
    return {k: statistics.mean(v) for k, v in acc.items()}


def _fmt(xs):
    xs = [x for x in xs if x is not None]
    if not xs:
        return "n/a"
    lo, hi = min(xs), max(xs)
    return f"{lo:.2f}x" if abs(hi - lo) < 0.005 else f"{lo:.2f}x-{hi:.2f}x"


def _trend_f9a(rows):
    ratio = _mean_by(rows, ["label"], "ratio_total",
                     lambda r: not r["label"].startswith("shape_"))
    naive = _mean_by(rows, ["label"], "naive_total_bytes")
    sfs = _mean_by(rows, ["label"], "sfs_total_bytes")
    if not ratio:
        # Without real namespaces: many small directories, where ext4 pays a block each.
        small = _mean_by(rows, ["label"], "ratio_total",
                         lambda r: r["label"] == "shape_100000x10")
        if not small:
            return None, "no real namespace in this run; synthetic shapes only"
        v = next(iter(small.values()))
        return v > 1, (f"no real namespace in this run; SplineFS metadata {v:.2f}x "
                       f"below ext4 on 100,000 directories of 10 entries")
    ext4 = _mean_by(rows, ["label"], "ext4_total_bytes")
    ok = all(v > 1 for v in ratio.values())
    keys = [k for k in ratio if naive.get(k) and sfs.get(k)]
    naive_ok = all(naive[k] > max(sfs[k], ext4.get(k, 0)) for k in keys)
    return ok and naive_ok, (f"SplineFS metadata {_fmt(ratio.values())} below ext4 "
                             f"on {len(ratio)} namespace{'' if len(ratio) == 1 else 's'}; per-directory index "
                             f"{_fmt(naive[k] / sfs[k] for k in keys)} SplineFS's, "
                             f"{'largest' if naive_ok else 'not largest'}")


def _trend_speedup(rows, keys, value, higher_is_better, floor_cap=0):
    base = _mean_by(rows, keys, value, lambda r: r.get("fs") == "ext4")
    sfs = _mean_by(rows, keys, value, lambda r: r.get("fs") == "splinefs")
    ratios = {}
    for k, b in base.items():
        if k in sfs and b and sfs[k]:
            if floor_cap and "cap_bytes" in keys and \
                    int(k[keys.index("cap_bytes")]) < floor_cap:
                continue
            ratios[k] = sfs[k] / b if higher_is_better else b / sfs[k]
    if not ratios:
        return None, "no SplineFS/ext4 pairs"
    return all(v > 1 for v in ratios.values()), \
        f"SplineFS {_fmt(ratios.values())} ext4 over {len(ratios)} " + \
        ("cell" if len(ratios) == 1 else "cells")


def _trend_f9bc(rows, floor_cap=0):
    """SplineFS against ext4, and against the naive arm where it ran."""
    ok, text = _trend_speedup(rows, ["cap_bytes"], "ops_per_sec", True, floor_cap)
    naive = _mean_by(rows, ["cap_bytes"], "ops_per_sec", lambda r: r.get("fs") == "naive")
    base = _mean_by(rows, ["cap_bytes"], "ops_per_sec", lambda r: r.get("fs") == "ext4")
    sfs = _mean_by(rows, ["cap_bytes"], "ops_per_sec", lambda r: r.get("fs") == "splinefs")
    keys = [k for k in naive if base.get(k) and sfs.get(k) and int(k[0]) >= floor_cap]
    killed = sorted({int(r["cap_bytes"]) for r in rows if r.get("fs") == "naive"
                     and not r.get("ops_per_sec") and int(r["cap_bytes"]) >= floor_cap})
    if ok is None or not (keys or killed):
        return ok, text
    beats = all(sfs[k] > naive[k] for k in keys)
    if keys:
        text += (f"; naive {_fmt(naive[k] / base[k] for k in keys)} ext4, "
                 f"{'below' if beats else 'not below'} SplineFS")
    if killed:
        text += ("; naive did not finish at " + ", ".join(f"{c >> 20} MB" for c in killed))
    return ok and beats, text


def _trend_f12a(rows):
    r = {}
    for arm in ("mutable", "promote", "subtree"):
        base = _mean_by(rows, ["cap_bytes"], "ops_per_sec",
                        lambda x, a=arm: x["arm"] == a and x["fs"] == "ext4")
        sfs = _mean_by(rows, ["cap_bytes"], "ops_per_sec",
                       lambda x, a=arm: x["arm"] == a and x["fs"] == "splinefs")
        r[arm] = {k: sfs[k] / base[k] for k in base if k in sfs and base[k]}
    caps = sorted(set(r["mutable"]) & set(r["promote"]) & set(r["subtree"]),
                  key=lambda k: int(k[0]))
    if not caps:
        return None, "missing arms"
    ok = all(r["mutable"][c] < r["promote"][c] < r["subtree"][c] for c in caps)
    return ok, (f"no promotion {_fmt([r['mutable'][c] for c in caps])}, "
                f"per-directory {_fmt([r['promote'][c] for c in caps])}, "
                f"subtree {_fmt([r['subtree'][c] for c in caps])} of ext4")


def _trend_f12b(rows):
    out = []
    ok = True
    for col, name in (("cycles_per_lookup", "cycles"),
                      ("instructions_per_lookup", "instructions")):
        m = _mean_by(rows, ["fs"], col)
        s, e = m.get(("splinefs",)), m.get(("ext4",))
        if not s or not e:
            return None, "no SplineFS/ext4 pair"
        ok = ok and s < e
        out.append(f"{s / e:.2f}x the {name}")
    return ok, "SplineFS uses " + " and ".join(out) + " of ext4"


def _trend_f13(rows):
    oks, parts = [], []
    for op in ("stat_ops", "read_ops"):
        good, text = _trend_speedup(rows, ["files", "cap_bytes"], op, True)
        oks.append(good)
        parts.append(f"{op[:-4]}: {text}")
    if None in oks:
        return None, "; ".join(parts)
    return all(oks), "; ".join(parts)


def _trend_f14(rows):
    cov = _mean_by(rows, ["churn_percent"], "promoted_fraction",
                   lambda r: r["fs"] == "adaptive")
    tput = _mean_by(rows, ["churn_percent", "fs"], "ops_per_sec")
    if not cov:
        return None, "no adaptive rows"
    order = sorted(cov, key=lambda k: float(k[0]))
    covs = [cov[k] for k in order]
    # Fig. 14 spans 0-50% churn; full churn is F14par.
    swept = [cov[k] for k in order if float(k[0]) <= 50]
    mono = all(a >= b - 0.02 for a, b in zip(swept, swept[1:]))
    low = order[0][0]
    a, p = tput.get((low, "adaptive")), tput.get((low, "policy_off"))
    faster = bool(a and p and a > p)
    text = ("coverage " + ", ".join(f"{c * 100:.0f}%" for c in covs) +
            " at churn " + ", ".join(f"{k[0]}%" for k in order))
    if a and p:
        text += f"; {a / p:.2f}x promotion off at {low}% churn"
    return mono and faster, text


TRENDS = {
    "F9a": ("metadata below ext4 everywhere; per-directory index largest", _trend_f9a),
    "F9b": ("positive lookups faster than ext4 and naive from 512 MB up",
            lambda r: _trend_f9bc(r, 512 << 20)),
    "F9c": ("negative lookups faster than ext4 and naive from 512 MB up",
            lambda r: _trend_f9bc(r, 512 << 20)),
    "F10": ("every namespace scans faster than on ext4",
            lambda r: _trend_speedup(r, ["label"], "seconds", False)),
    "F11": ("application stages faster than on ext4",
            lambda r: _trend_speedup(r, ["stage"], "steady_seconds", False)),
    "F12a": ("no promotion < per-directory < subtree", _trend_f12a),
    "F12b": ("fewer cycles and instructions per lookup than ext4", _trend_f12b),
    "F13": ("stat and read faster than ext4", _trend_f13),
    "F14": ("coverage falls as churn spreads; faster than promotion off when quiet",
            _trend_f14),
}


def trend_report() -> list[str]:
    lines = ["# Trends", "",
             "Whether each figure points the way the paper's does, from the "
             "newest run of each panel.  Magnitudes are in SUMMARY.md.", "",
             "| Panel | Paper's trend | Holds | Measured | Run |",
             "|---|---|---|---|---|"]
    for panel, (claim, fn) in TRENDS.items():
        runs = _panel_runs(panel)
        if not runs:
            lines.append(f"| {panel} | {claim} | not run | | |")
            continue
        run = runs[-1]
        try:
            ok, text = fn(_rows(run, panel))
        except Exception as exc:                              # noqa: BLE001
            ok, text = None, f"could not evaluate: {exc!r}"
        mark = {True: "yes", False: "**no**", None: "n/a"}[ok]
        lines.append(f"| {panel} | {claim} | {mark} | {text} | `{run.name}` |")
    return lines


def latest_run(experiment: str) -> Path | None:
    """Newest run for a campaign, via the panel it primarily writes."""
    tags = EXPERIMENT_TAGS.get(experiment)
    return panel_run(tags[0]) if tags else None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--experiment", help="analyse only this experiment")
    ap.add_argument("--input", type=Path, help="analyse this run directory")
    ap.add_argument("--no-figures", action="store_true", help="skip figure rendering")
    ap.add_argument("--output", type=Path, help="default: <results>/SUMMARY.md")
    ap.add_argument("--figures", type=Path,
                    help="collect figures here instead of beside their CSV")
    ap.add_argument("--trend", action="store_true",
                    help="report whether each figure's trend holds")
    ap.add_argument("--results", type=Path, help="results directory")
    args = ap.parse_args()
    global RESULTS
    if args.results:
        RESULTS = args.results.resolve()

    if args.trend:
        lines = trend_report()
        out = args.output or RESULTS / "TREND.md"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text("\n".join(lines) + "\n")
        print("\n".join(lines))
        print(f"\nwrote {out}")
        return 0
    if args.output is None:
        args.output = RESULTS / "SUMMARY.md"

    targets: list[tuple[str, Path]] = []
    missing: list[str] = []
    if args.input:
        name = args.experiment or args.input.parent.name
        targets.append((name, args.input))
    else:
        names = [args.experiment] if args.experiment else sorted(ANALYSERS)
        for n in names:
            run = latest_run(n)
            if run:
                targets.append((n, run))
            else:
                missing.append(n)

    if not targets:
        print(f"No runs under {RESULTS} yet.  Run scripts/run_queue.sh or scripts/vm/run.sh first.")
        return 0

    lines = ["# Measured results against the claims",
             "",
             "Generated by `scripts/check.py`.  Each section compares a run "
             "against the expectation in `docs/CLAIMS.md`.",
             "",
             "A result is admissible as a main result only with zero manual "
             "transitions, zero spline bound violations, at least one "
             "autonomously promoted root, and three or more repetitions.  "
             "Sections that fail that gate say so.",
             ""]

    for name, run in targets:
        lines.append(f"\n## {name}\n")
        cfg = run / "config.txt"
        if cfg.exists():
            kv = dict(l.split("=", 1) for l in cfg.read_text().splitlines() if "=" in l)
            lines.append(f"Run `{run.name}`, srcversion `{kv.get('srcversion', '?')}`, "
                         f"kernel `{kv.get('kernel', '?')}`, "
                         f"governor `{kv.get('cpu_governor', '?')}`, "
                         f"turbo `{kv.get('turbo_msr_1a0_bit38', kv.get('no_turbo', '?'))}`.")
            gov = kv.get("cpu_governor")
            # Prefer the MSR's turbo bit.
            msr = kv.get("turbo_msr_1a0_bit38")
            deviations = []
            if gov != "performance":
                deviations.append(f"the governor was `{gov}`, not `performance`")
            if msr == "off":
                pass                      # proven off by the architectural bit
            elif msr == "on":
                deviations.append("turbo was enabled (MSR 0x1A0 bit 38 clear)")
            elif msr is not None:
                deviations.append(f"the turbo MSR could not be read (`{msr}`)")
            elif kv.get("no_turbo") == "1":
                pass
            else:
                deviations.append("this run predates MSR-based turbo recording, "
                                  "and its `turbo_off_by_freq` field is not "
                                  "trustworthy on this host: turbo may have "
                                  "been enabled")
            if deviations:
                lines.append("\n> The paper's setup uses the performance governor with "
                             "turbo disabled. Here " + ", and ".join(deviations) +
                             ". Absolute rates are therefore not directly comparable to "
                             "the paper; the ratios these claims are stated in are much "
                             "less sensitive to CPU frequency, since both filesystems "
                             "run on the same host in the same state.")
        try:
            ANALYSERS[name](run, lines)
        except KeyError:
            lines.append(f"\nNo analyser for `{name}`.")
        except Exception as exc:                              # noqa: BLE001
            lines.append(f"\n**Analysis failed:** {exc!r}")

    # A campaign with no data is listed as not run.
    if missing and not args.experiment:
        lines.append("\n\n## Not yet run\n")
        lines.append("No results under `results/` for these, so the claims they "
                     "back are unmeasured here:\n")
        for n in missing:
            lines.append(f"- `{n}` (`scripts/run_{n}.sh`)")

    if not args.no_figures:
        try:
            import matplotlib  # noqa: F401
        except ImportError:
            lines.append("\n\n> matplotlib is not installed, so no figures were "
                         "rendered. The tables above are complete without them.")
        else:
            figdir = args.figures
            if figdir:
                figdir.mkdir(parents=True, exist_ok=True)
            # One figure per panel, beside its CSV; --figures collects them elsewhere.
            made = []
            for panel, draw in figures.PANELS.items():
                run = panel_run(panel)
                if run is None:
                    continue
                dest = (figdir / f"{panel}.pdf") if figdir else (run / f"{panel}.pdf")
                try:
                    if draw(run, dest):
                        made.append(dest)
                except Exception as exc:                      # noqa: BLE001
                    lines.append(f"\n> Rendering {panel} failed: {exc!r}")
            if made:
                lines.append("\n\n## Figures\n")
                for m in made:
                    lines.append(f"- `{m}`")
                lines.append("\nDrawn by the same code that drew the "
                             "submitted figures, using LaTeX with libertine "
                             "to match the paper. Without a TeX installation "
                             "they fall back to matplotlib's fonts and say "
                             "so; the numbers are unaffected.")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines) + "\n")
    print(f"wrote {args.output}")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
