"""Reading a panel's CSV.

Runs are filed by figure panel: results/<panel>/<stamp>/<panel>.csv, plus
config.txt and raw/.  Where a campaign writes several parts into one CSV, a
column names the part and `groups()` splits it back out.
"""
from __future__ import annotations

import csv
import statistics
from collections import defaultdict
from pathlib import Path


def read(path: Path) -> list[dict]:
    with path.open(newline="") as fh:
        return list(csv.DictReader(fh))


def panel(run: Path, tag: str) -> list[dict]:
    """Rows of <tag>.csv for this run, or [] if it was not measured."""
    f = run / f"{tag}.csv"
    return read(f) if f.exists() else []


def sibling(run: Path, tag: str) -> Path:
    """The same run under another panel: results/<tag>/<same stamp>."""
    return run.parent.parent / tag / run.name


def num(row: dict, *names: str):
    """First parseable float among the named columns."""
    for n in names:
        v = row.get(n)
        if v not in (None, "", "NA", "-"):
            try:
                return float(v)
            except ValueError:
                pass
    return None


def groups(rows: list[dict], col: str) -> dict[str, list[dict]]:
    """Split rows on a discriminator column, keeping first-seen order."""
    out: dict[str, list[dict]] = {}
    for r in rows:
        out.setdefault(r.get(col, ""), []).append(r)
    return out


def _order(k):
    """Sort numerically where the key looks like a number, else by text."""
    parts = k if isinstance(k, tuple) else (k,)
    out = []
    for p in parts:
        t = str(p)
        try:
            out.append((0, float(t), ""))
        except ValueError:
            out.append((1, 0.0, t))
    return tuple(out)


def by(rows, key, value) -> dict:
    """{key: {fs: mean}}, ordered by key.  `key` is a column or a tuple."""
    cols = (key,) if isinstance(key, str) else tuple(key)
    acc = defaultdict(lambda: defaultdict(list))
    for r in rows:
        v = num(r, value)
        if v is None:
            continue
        k = tuple(r.get(c, "") for c in cols)
        acc[k[0] if len(cols) == 1 else k][r.get("fs", "")].append(v)
    return {k: {fs: statistics.mean(vs) for fs, vs in per.items()}
            for k, per in sorted(acc.items(), key=lambda kv: _order(kv[0]))}


def ratio(rows, key, value, base="ext4", target="splinefs") -> dict:
    """{key: target/base}, for the keys where both were measured."""
    return {k: per[target] / per[base]
            for k, per in by(rows, key, value).items()
            if per.get(base) and per.get(target)}


def caps(rows, floor: int = 0) -> list[int]:
    """Sorted numeric cap_bytes present, at or above `floor`."""
    return sorted({int(r["cap_bytes"]) for r in rows
                   if r.get("cap_bytes", "").isdigit()
                   and int(r["cap_bytes"]) >= floor})
