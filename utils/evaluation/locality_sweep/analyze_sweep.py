#!/usr/bin/env python3
"""Summarize the churned-region locality sweep."""

import csv
import math
import statistics
import sys
from collections import defaultdict


def mean_ci(values):
    mean = statistics.mean(values)
    if len(values) < 2:
        return mean, 0.0
    return mean, 1.96 * statistics.stdev(values) / math.sqrt(len(values))


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: analyze_sweep.py RESULTS.csv SUMMARY.md")
    source, output = sys.argv[1:]
    with open(source, newline="") as handle:
        rows = list(csv.DictReader(handle))

    by_fraction = defaultdict(list)
    for row in rows:
        by_fraction[int(row["churn_percent"])].append(row)

    warnings = []
    lines = [
        "# Churned-region locality sweep",
        "",
        "| Churned regions | Promoted fraction | Adaptive kops/s | "
        "Adaptive / policy-off | Adaptive / ext4 |",
        "|---:|---:|---:|---:|---:|",
    ]
    for percent in sorted(by_fraction):
        selected = by_fraction[percent]
        adaptive = [r for r in selected if r["fs"] == "adaptive"]
        promoted = [float(r["promoted_fraction"]) for r in adaptive]
        rates = defaultdict(dict)
        for row in selected:
            rates[int(row["rep"])][row["fs"]] = float(row["ops_per_sec"])
            if int(row["errors"]) != 0:
                warnings.append(
                    f"{percent}% {row['fs']} rep {row['rep']}: errors"
                )
        adaptive_rates = [v["adaptive"] for v in rates.values()]
        off_ratios = [v["adaptive"] / v["policy_off"] for v in rates.values()]
        ext4_ratios = [v["adaptive"] / v["ext4"] for v in rates.values()]
        pmean, _ = mean_ci(promoted)
        amean, _ = mean_ci(adaptive_rates)
        omean, _ = mean_ci(off_ratios)
        emean, _ = mean_ci(ext4_ratios)
        lines.append(
            f"| {percent}% | {100 * pmean:.1f}% | {amean / 1000:.1f} | "
            f"{omean:.3f}x | {emean:.3f}x |"
        )
        if percent == 0 and pmean < 0.95:
            warnings.append("0%: less than 95% of directories promoted")
        if percent == 100 and pmean != 0:
            warnings.append("100%: some directories promoted")
        if percent > 0 and pmean > 1.0 - percent / 100.0 + 0.03:
            warnings.append(
                f"{percent}%: promoted fraction exceeds quiet fraction"
            )

    lines += ["", "## Warnings", ""]
    lines.extend(
        ["- None."] if not warnings else [f"- {item}" for item in warnings]
    )
    with open(output, "w") as handle:
        handle.write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
