#!/usr/bin/env python3
"""Publish a finished run's CSVs as the figure panels they feed.

Drivers write the filenames that suit them; a panel directory holds exactly
one CSV named after the panel.  Where a campaign wrote several files they are
combined with a column naming the part, and where it feeds two panels the
second is written beside the first under the same stamp.

    publish.py <campaign> <run-dir>
"""
import csv, io, shutil, sys
from pathlib import Path

# campaign -> [(panel, spec)]; spec is a filename, or (column, [(label, file)])
PLAN = {
    "footprint":      [("F9a",  "footprint.csv")],
    "cache_pressure": [("F9b",  "positive.csv"), ("F9c", "negative.csv")],
    "realworld":      [("F10",  "realworld.csv")],
    "applookup":      [("F11",  ("stage", [("A1", "A1_container.csv"),
                                           ("A2", "A2_dataloader.csv"),
                                           ("A3", "A3_tar.csv"),
                                           ("A4", "A4_sqlite.csv"),
                                           ("A5", "A5_duckdb.csv"),
                                           ("A6", "A6_llamaindex.csv")]))],
    "ablation":       [("F12a", ("arm", [("mutable", "mutable.csv"),
                                         ("promote", "promote.csv"),
                                         ("subtree", "subtree.csv")]))],
    "lookup_cpu":     [("F12b", "index_cpu.csv")],
    "mdtest":         [("F13",  "mdtest.csv")],
    "locality_sweep": [("F14",  "locality_sweep.csv")],
}


def build(run: Path, spec):
    if isinstance(spec, str):
        f = run / spec
        return f.read_bytes() if f.exists() else None
    col, items = spec
    header, rows = None, []
    for label, name in items:
        f = run / name
        if not f.exists():
            continue
        r = list(csv.reader(io.StringIO(f.read_text())))
        if not r:
            continue
        if header is None:
            header = [col] + r[0]
        rows += [[label] + x for x in r[1:] if x]
    if header is None:
        return None
    buf = io.StringIO()
    w = csv.writer(buf, lineterminator="\n")
    w.writerow(header)
    w.writerows(rows)
    return buf.getvalue().encode()


def main(campaign: str, run: Path) -> int:
    plan = PLAN.get(campaign)
    if not plan:
        return 0
    for panel, spec in plan:
        data = build(run, spec)
        if data is None:
            continue
        dest = run if run.name and run.parent.name == panel else \
            run.parent.parent / panel / run.name
        dest.mkdir(parents=True, exist_ok=True)
        (dest / f"{panel}.csv").write_bytes(data)
        if dest != run:
            for extra in ("config.txt",):
                if (run / extra).exists():
                    shutil.copy2(run / extra, dest / extra)
        print(f"  {panel}: {dest / (panel + '.csv')}")
    # The driver's own files have been folded into the panel CSVs.
    for _, spec in plan:
        names = [spec] if isinstance(spec, str) else [n for _, n in spec[1]]
        for n in names:
            f = run / n
            if f.exists():
                f.unlink()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], Path(sys.argv[2])))
