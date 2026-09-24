"""The paper's figures, one function per panel, in the paper's style.

A function takes the panel's run directory and an output path, and returns
the path it wrote, or None if that panel was not measured.  Figures plot
measured data only; the paper's values live in docs/expected.csv and are
compared in the text summary.
"""
from __future__ import annotations

import statistics
from collections import defaultdict
from pathlib import Path

import paneldata as D
import paperstyle as S

BASE4 = ("ext4", "xfs", "btrfs", "f2fs")


def f9a(run: Path, out: Path):
    """Directory metadata per namespace, three bars each, log y."""
    import numpy as np
    rows = D.panel(run, "F9a")
    if not rows:
        return None
    plt = S.setup()

    acc = defaultdict(lambda: defaultdict(list))
    for r in rows:
        for col, key in (("ext4_total_bytes", "ext4"),
                         ("naive_total_bytes", "naive"),
                         ("sfs_total_bytes", "splinefs")):
            v = D.num(r, col)
            if v is not None:
                acc[r.get("label", "")][key].append(v)

    # Real namespaces only, in the paper's order.
    PREF = ["apks", "corpus", "kernel", "repos", "yellusers"]
    have = sorted((k for k, v in acc.items()
                   if v.get("ext4") and not k.startswith("shape_")),
                  key=lambda k: (PREF.index(k) if k in PREF else len(PREF), k))
    if not have:
        return None
    labels = [{"yellusers": "GUFI"}.get(k, k) for k in have]

    MB = 1 / 1048576
    xs = np.arange(len(have))
    series = {k: [statistics.mean(acc[n][k]) * MB if acc[n].get(k) else np.nan
                  for n in have] for k in ("ext4", "naive", "splinefs")}

    fig, ax = plt.subplots(figsize=(4.4, 3.7))
    w = 0.27
    for off, key in ((-w, "ext4"), (0.0, "naive"), (w, "splinefs")):
        ax.bar(xs + off, series[key], w, color=S.COL[key], hatch=S.HATCH[key],
               edgecolor="#222", linewidth=0.5, label=S.LABEL[key], zorder=3)
    for x, a, b in zip(xs, series["ext4"], series["splinefs"]):
        if b == b and b:
            ax.annotate(f"{a / b:.1f}$\\times$", xy=(x + w, b), ha="center",
                        va="bottom", rotation=90, fontsize=14.5, color=S.DARK,
                        xytext=(4, 3), textcoords="offset points")
    ax.set_yscale("log")
    ax.set_ylim(1, 1e4)
    ax.set_yticks([1, 1e1, 1e2, 1e3])
    ax.set_yticklabels(["1", "10", "100", "1000"])
    ax.set_xticks(xs)
    ax.set_xticklabels(labels, rotation=35, ha="right")
    ax.set_ylabel(S.tx("dir.\\ metadata (MB)"))
    S.style_axis(ax)
    ax.legend(frameon=False, loc="upper left", handlelength=1.1,
              borderpad=0.2, labelspacing=0.2, handletextpad=0.4,
              fontsize=14.5)
    return _save(fig, plt, out, tight=True)


def f9b(run: Path, out: Path):
    """Cold positive lookups against the memcg cap, categorical x, log y."""
    import numpy as np
    rows = D.panel(run, "F9b")
    if not rows:
        return None
    plt = S.setup()
    caps = D.caps(rows)
    if not caps:
        return None

    per = D.by(rows, "cap_bytes", "ops_per_sec")
    fig, ax = plt.subplots(figsize=(5.0, 3.7))
    for fs in ("ext4", "naive", "splinefs"):
        ys = [per.get(str(c), {}).get(fs, np.nan) for c in caps]
        if all(y != y for y in ys):
            continue
        ax.plot(range(len(caps)), ys, color=S.COL[fs], label=S.LABEL[fs],
                mec="white" if fs != "ext4" else S.BLUE, **S.LINE[fs])
    ax.set_xticks(range(len(caps)))
    ax.set_xticklabels([S.caplab(c) for c in caps])
    ax.set_xlabel("memcg cap")
    ax.set_ylabel("cold lookups/s")
    ax.set_yscale("log")
    ax.set_ylim(top=ax.get_ylim()[1] * 6)
    S.style_axis(ax)
    ax.legend(frameon=False, ncol=3, fontsize=15.5, handlelength=1.0,
              columnspacing=0.7, labelspacing=0.2, handletextpad=0.3,
              loc="upper center", bbox_to_anchor=(0.5, 1.0), borderpad=0.1)
    S.panel_label(ax, "(b)")
    return _save(fig, plt, out)


def f9c(run: Path, out: Path):
    """Negative lookups at the widest cap, bars, annotated against ext4."""
    from matplotlib.ticker import NullLocator, NullFormatter
    rows = D.panel(run, "F9c")
    if not rows:
        return None
    caps = D.caps(rows)
    if not caps:
        return None
    plt = S.setup()

    at = [r for r in rows if r.get("cap_bytes") == str(caps[-1])]
    per = defaultdict(list)
    for r in at:
        v = D.num(r, "ops_per_sec")
        if v is not None:
            per[r.get("fs", "")].append(v)
    shown = [fs for fs in ("ext4", "naive", "splinefs") if per.get(fs)]
    if not shown:
        return None

    vals = [statistics.mean(per[f]) for f in shown]
    base = statistics.mean(per["ext4"]) if per.get("ext4") else vals[0]
    fig, ax = plt.subplots(figsize=(3.0, 3.7))
    bars = ax.bar(range(len(shown)), vals, 0.66,
                  color=[S.COL[f] for f in shown], edgecolor="#222",
                  linewidth=0.5)
    for bar, f in zip(bars, shown):
        bar.set_hatch(S.HATCH[f])
    for xi, v in enumerate(vals):
        ax.text(xi, v, f"{v / base:.1f}$\\times$", ha="center", va="bottom",
                fontsize=14.5)
    ax.set_xticks(range(len(shown)))
    ax.set_xticklabels([S.LABEL[f] for f in shown], rotation=35, ha="right")
    ax.set_ylabel("negative lookups/s")
    ax.set_yscale("log")
    ax.set_ylim(bottom=min(vals) / 2.2, top=max(vals) * 1.35)
    ax.yaxis.set_minor_locator(NullLocator())
    ax.yaxis.set_minor_formatter(NullFormatter())
    S.style_axis(ax)
    S.panel_label(ax, "(c)")
    return _save(fig, plt, out)


def f10(run: Path, out: Path):
    """One small panel per namespace: speedup over each baseline, clipped at 5x."""
    import numpy as np
    rows = D.panel(run, "F10")
    if not rows:
        return None
    plt = S.setup(fonts=dict(S.FONTS, **{
        "font.size": 13, "axes.labelsize": 13, "xtick.labelsize": 11.5,
        "ytick.labelsize": 11.5, "legend.fontsize": 9}))
    COLOR = {"ext4": S.BLUE, "xfs": S.TEAL, "btrfs": S.AMBER, "f2fs": S.ORANGE}
    HATCH = {"ext4": "", "xfs": "xx", "btrfs": "++", "f2fs": "--"}
    CLIP = 5.0

    secs = D.by(rows, "label", "seconds")
    labels = sorted(k for k, v in secs.items() if v.get("splinefs"))
    if not labels:
        return None

    ncol = min(4, len(labels))
    nrow = -(-len(labels) // ncol)
    fig, axes = plt.subplots(nrow, ncol, figsize=(1.75 * ncol, 1.8 * nrow),
                             sharey=True, squeeze=False,
                             gridspec_kw={"wspace": 0.18, "hspace": 0.55})
    xs = np.arange(len(BASE4))
    for ax, lab in zip(axes.flat, labels):
        sfs = secs[lab]["splinefs"]
        vals = [secs[lab][b] / sfs if secs[lab].get(b) and sfs else None
                for b in BASE4]
        bars = ax.bar(xs, [min(v, CLIP) if v else 0 for v in vals], width=0.7,
                      color=[COLOR[b] if v else "#dddddd"
                             for v, b in zip(vals, BASE4)],
                      edgecolor="#333", linewidth=0.4)
        for bar, b in zip(bars, BASE4):
            bar.set_hatch(HATCH[b])
        for x, v in zip(xs, vals):
            if v is None:
                ax.text(x, 0.08, "---", ha="center", va="bottom", fontsize=9,
                        color="#888", style="italic")
            elif v > CLIP:
                ax.text(x, CLIP * 0.93, f"{v:.1f}", ha="center", va="top",
                        fontsize=9, color="#fff", fontweight="bold")
            else:
                ax.text(x, v + 0.10, f"{v:.2f}", ha="center", va="bottom",
                        fontsize=9, color="#333")
        ax.axhline(1.0, color="red", ls=":", lw=1.1, zorder=1)
        ax.set_xticks(xs)
        ax.set_xticklabels(BASE4, rotation=30, ha="right")
        ax.set_title(lab.replace("_", " "), fontsize=11.5, pad=2)
        ax.set_ylim(0, CLIP + 0.3)
        S.style_axis(ax)
    for ax in axes.flat[len(labels):]:
        ax.set_visible(False)
    for i in range(nrow):
        axes[i, 0].set_ylabel("speedup ($\\times$)")
    fig.tight_layout(pad=0.3)
    return _save(fig, plt, out, tight=True)


def f11(run: Path, out: Path):
    """Grouped bars per application stage, speedup over each baseline."""
    import numpy as np
    rows = D.panel(run, "F11")
    if not rows:
        return None
    plt = S.setup(fonts=dict(S.FONTS, **{
        "font.size": 15, "axes.labelsize": 15, "xtick.labelsize": 13,
        "ytick.labelsize": 13, "legend.fontsize": 12.5}))
    COLOR = {"ext4": S.BLUE, "xfs": S.TEAL, "btrfs": S.AMBER, "f2fs": S.ORANGE}
    HATCH = {"ext4": "", "xfs": "xx", "btrfs": "++", "f2fs": "--"}
    NICE = {"dataloader": "DataLoader", "sqlite": "SQLite",
            "duckdb": "DuckDB", "llamaindex": "LlamaIndex"}

    present = []
    for stage, srows in sorted(D.groups(rows, "stage").items()):
        per = defaultdict(list)
        for r in srows:
            v = D.num(r, "steady_seconds")
            if v is not None:
                per[r.get("fs", "")].append(v)
        if not per.get("splinefs"):
            continue
        sfs = statistics.mean(per["splinefs"])
        name = next((r.get("workload") for r in srows if r.get("workload")),
                    stage)
        present.append((NICE.get(name, name),
                        {b: statistics.mean(per[b]) / sfs
                         for b in BASE4 if per.get(b) and sfs}))
    if not present:
        return None

    fig, ax = plt.subplots(figsize=(max(4.0, 1.2 * len(present) + 2.2), 2.15))
    x = np.arange(len(present))
    w = 0.80 / len(BASE4)
    for i, b in enumerate(BASE4):
        ys = [sp.get(b, np.nan) for _, sp in present]
        off = (i - (len(BASE4) - 1) / 2) * w
        ax.bar(x + off, ys, w, color=COLOR[b], hatch=HATCH[b],
               edgecolor="#222", linewidth=0.4, label=b)
        if b == "ext4":
            for xi, y in zip(x + off, ys):
                if y == y:
                    ax.annotate(f"{y:.2f}$\\times$", xy=(xi, y), xytext=(0, 2),
                                textcoords="offset points", ha="center",
                                va="bottom", rotation=90, fontsize=11)
    ax.axhline(1.0, ls=":", color="red", lw=1.2, zorder=1)
    ax.set_xticks(x)
    ax.set_xticklabels([l for l, _ in present])
    ax.set_ylabel("speedup ($\\times$)")
    vals = [v for _, sp in present for v in sp.values()]
    ax.set_ylim(0, max(vals) * 1.18)
    S.style_axis(ax)
    ax.legend(ncol=4, frameon=False, loc="lower center",
              bbox_to_anchor=(0.5, 1.0), handlelength=1.1, columnspacing=1.0,
              handletextpad=0.4, borderpad=0.1)
    fig.tight_layout(pad=0.3)
    return _save(fig, plt, out, tight=True)


def f12a(run: Path, out: Path):
    """The three policy arms against ext4, from 512 MB up."""
    import numpy as np
    rows = D.panel(run, "F12a")
    if not rows:
        return None
    plt = S.setup(fonts=_F12_FONTS())
    ARMS = [("mutable", "mutable", S.GREY, ""),
            ("promote", "+ promote", S.TEAL, "//"),
            ("subtree", "+ subtree model", S.ORANGE, "xx")]
    by_arm = D.groups(rows, "arm")
    caps = D.caps(rows, floor=536870912)
    if not caps or not by_arm:
        return None

    fig, ax = plt.subplots(figsize=(4.4, 2.22))
    x = np.arange(len(caps))
    w = 0.26
    tops = []
    for i, (arm, lab, col, hatch) in enumerate(ARMS):
        rat = D.ratio(by_arm.get(arm, []), "cap_bytes", "ops_per_sec")
        ys = [rat.get(str(c), np.nan) for c in caps]
        if arm == "subtree":
            tops = [y for y in ys if y == y]
        off = (i - 1) * w
        ax.bar(x + off, ys, w, color=col, edgecolor="#222", linewidth=0.5,
               hatch=hatch, label=lab)
        for xi, y in zip(x + off, ys):
            if y == y:
                ax.text(xi - 0.045, y + 0.06, f"{y:.1f}", ha="center",
                        va="bottom", fontsize=10.5)
    ax.axhline(1.0, ls=":", color="red", lw=1.2, zorder=1)
    ax.set_xticks(x)
    ax.set_xticklabels([S.caplab(c) for c in caps], fontsize=12.5)
    ax.set_xlabel("memcg cap", fontsize=15)
    ax.set_ylabel("speedup ($\\times$)", fontsize=15)
    ax.set_ylim(top=(max(tops) if tops else 1.0) * 1.12)
    S.style_axis(ax)
    ax.legend(frameon=False, loc="lower center", bbox_to_anchor=(0.5, 1.0),
              ncol=3, fontsize=12, handlelength=1.0, handletextpad=0.4,
              columnspacing=0.9, borderpad=0.2)
    S.panel_label(ax, "(a)", y=-0.40)
    return _save(fig, plt, out)


def f12b(run: Path, out: Path):
    """Cycles per lookup, y clamped at 20k with taller bars annotated inside."""
    rows = D.panel(run, "F12b")
    if not rows:
        return None
    plt = S.setup(fonts=_F12_FONTS())
    ORDER = [("ext4", S.BLUE, ""), ("xfs", S.TEAL, "xx"),
             ("btrfs", S.AMBER, "++"), ("f2fs", "#CC79A7", "--"),
             ("splinefs", S.ORANGE, "||")]
    TOP = 20000

    cyc = defaultdict(list)
    for r in rows:
        v = D.num(r, "cycles_per_lookup")
        if v is not None:
            cyc[r.get("fs", "")].append(v)
    shown = [t for t in ORDER if cyc.get(t[0])]
    if not shown:
        return None

    means = [statistics.mean(cyc[fs]) for fs, _, _ in shown]
    errs = [statistics.stdev(cyc[fs]) if len(cyc[fs]) > 1 else 0.0
            for fs, _, _ in shown]
    fig, ax = plt.subplots(figsize=(3.1, 2.22))
    bars = ax.bar(range(len(shown)), means, 0.62,
                  yerr=[e if m <= TOP else 0.0 for m, e in zip(means, errs)],
                  color=[c for _, c, _ in shown], edgecolor="#222",
                  linewidth=0.5, capsize=2.5, error_kw={"lw": 0.8})
    for bar, (_, _, hatch) in zip(bars, shown):
        bar.set_hatch(hatch)
    for xi, (m, e) in enumerate(zip(means, errs)):
        if m > TOP:
            ax.text(xi, TOP * 0.97, f"{m / 1000:.1f}$\\pm${e / 1000:.1f}k",
                    ha="center", va="top", fontsize=10.5, color="white",
                    fontweight="bold", rotation=90)
    ax.set_xticks(range(len(shown)))
    ax.set_xticklabels([S.LABEL[fs] for fs, _, _ in shown], rotation=32,
                       ha="right", rotation_mode="anchor")
    ax.tick_params(axis="x", labelsize=12)
    ax.set_ylabel("cycles / lookup", fontsize=15)
    ax.set_ylim(top=TOP)
    S.style_axis(ax)
    S.panel_label(ax, "(b)", y=-0.40)
    return _save(fig, plt, out)


def f13(run: Path, out: Path):
    """Four heatmaps of the ratio to ext4, file count by memcg cap."""
    import numpy as np
    import matplotlib.colors as mcolors
    rows = D.panel(run, "F13")
    if not rows:
        return None
    plt = S.setup(fonts=dict(S.FONTS, **{
        "font.size": 13, "axes.labelsize": 13, "xtick.labelsize": 11.5,
        "ytick.labelsize": 11.5, "legend.fontsize": 9}))
    PHASES = [("create_ops", "create"), ("stat_ops", "stat"),
              ("read_ops", "read"), ("remove_ops", "remove")]

    acc = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
    for r in rows:
        f, c = r.get("files", ""), r.get("cap_bytes", "")
        if not (f.isdigit() and c.isdigit()):
            continue
        for phase, _ in PHASES:
            v = D.num(r, phase)
            if v is not None:
                acc[(int(f), int(c))][r.get("fs", "")][phase].append(v)
    counts = sorted({k[0] for k in acc})
    caps = sorted({k[1] for k in acc})
    if not counts or not caps:
        return None

    def cell(key, fs, phase):
        v = acc.get(key, {}).get(fs, {}).get(phase, [])
        return statistics.mean(v) if v else None

    # The paper's colour scale.
    cmap = mcolors.LinearSegmentedColormap.from_list(
        "div_around_one",
        ["#0072B2", "#7FC4E0", "#FFFFFF", "#F5B061", "#D55E00"])
    norm = mcolors.TwoSlopeNorm(vmin=0.5, vcenter=1.0, vmax=1.6)

    fig, axes = plt.subplots(1, 4, figsize=(7.0, 2.45), sharey=True,
                             gridspec_kw={"wspace": 0.18})
    for ax, (phase, title) in zip(axes, PHASES):
        M = np.full((len(counts), len(caps)), np.nan)
        for i, fc in enumerate(counts):
            for j, c in enumerate(caps):
                s, e = cell((fc, c), "splinefs", phase), cell((fc, c), "ext4", phase)
                if s is not None and e:
                    M[i, j] = s / e
        im = ax.imshow(M, cmap=cmap, norm=norm, aspect="auto", origin="lower")
        ax.set_xticks(range(len(caps)))
        ax.set_xticklabels([S.caplab(c) for c in caps], rotation=30, ha="right")
        ax.set_yticks(range(len(counts)))
        ax.set_yticklabels([f"{n // 1000}K" if n < 10**6 else f"{n / 1e6:g}M"
                            for n in counts])
        ax.set_title(title, fontsize=13, pad=2)
        for i in range(M.shape[0]):
            for j in range(M.shape[1]):
                if M[i, j] == M[i, j]:
                    bg = norm(M[i, j])
                    ax.text(j, i, f"{M[i, j]:.2f}", ha="center", va="center",
                            fontsize=9,
                            color="#fff" if bg < 0.18 or bg > 0.82 else "#222")
        ax.tick_params(direction="out", length=3, color="#555555")
        for sp in ("top", "right"):
            ax.spines[sp].set_visible(False)
    axes[0].set_ylabel("file count")
    fig.text(0.5, -0.04, "memcg cap", ha="center", va="top", fontsize=13)
    cbar = fig.colorbar(im, ax=axes, shrink=0.7, pad=0.015, aspect=22,
                        fraction=0.025, ticks=[0.5, 0.75, 1.0, 1.25, 1.5])
    cbar.ax.set_yticklabels(["0.50", "0.75", "1.00", "1.25", "1.50"],
                            fontsize=10.5)
    cbar.ax.tick_params(length=2.5)
    cbar.outline.set_linewidth(0.4)
    cbar.set_label(S.tx("ratio vs.\\ ext4"), fontsize=10.5)
    return _save(fig, plt, out, tight=True)


def f14(run: Path, out: Path):
    """Throughput lines and promoted coverage bars against churn, dual axis."""
    rows = D.panel(run, "F14")
    if not rows:
        return None
    plt = S.setup(fonts=dict(S.FONTS, **{
        "font.size": 18, "axes.labelsize": 15, "xtick.labelsize": 14.5,
        "ytick.labelsize": 14.5, "legend.fontsize": 12}))
    BAR, BARDK = "#56B4E9", "#2C7FB8"

    kops = defaultdict(list)
    cover = defaultdict(list)
    for r in rows:
        cp = r.get("churn_percent", "")
        if not cp.lstrip("-").isdigit():
            continue
        p = int(cp)
        v = D.num(r, "ops_per_sec")
        if v is not None:
            kops[(r.get("fs", ""), p)].append(v / 1000)
        if r.get("fs") == "adaptive":
            f = D.num(r, "promoted_fraction")
            if f is not None:
                cover[p].append(100 * f)
    pts = sorted({p for _, p in kops})
    if not pts:
        return None
    x = range(len(pts))

    fig, left = plt.subplots(figsize=(7.0, 2.34))
    right = left.twinx()
    right.bar(x, [statistics.mean(cover[p]) if cover.get(p) else float("nan")
                  for p in pts], width=0.56, color=BAR, alpha=0.45,
              edgecolor=BARDK, linewidth=0.55, zorder=1)
    right.set_ylabel(S.tx("promoted dirs.\\ (\\%)"), color=BARDK, fontsize=15)
    right.set_ylim(0, 108)
    right.set_yticks([0, 25, 50, 75, 100])
    right.tick_params(axis="y", colors=BARDK, direction="out", length=3)
    right.spines["top"].set_visible(False)
    right.spines["right"].set_color(BARDK)

    handles = []
    for fs in ("ext4", "policy_off", "adaptive"):
        if not any((fs, p) in kops for p in pts):
            continue
        ys = [statistics.mean(kops[(fs, p)]) if kops.get((fs, p))
              else float("nan") for p in pts]
        h, = left.plot(x, ys, color=S.COL[fs], label=S.LABEL[fs],
                       mec="white" if fs != "ext4" else S.BLUE, **S.LINE[fs])
        handles.append(h)
    left.set_xticks(list(x))
    left.set_xticklabels([str(p) for p in pts])
    left.set_xlabel("churned subtree regions (\\%)", fontsize=15)
    left.set_ylabel("throughput (Kops/s)", fontsize=15)
    left.grid(axis="y", linestyle=":", linewidth=0.5, color="#999999",
              alpha=0.7)
    left.set_axisbelow(True)
    left.spines["top"].set_visible(False)
    left.tick_params(direction="out", length=3, color="#555555")
    left.set_zorder(2)
    left.patch.set_visible(False)
    leg = left.legend(handles, [h.get_label() for h in handles],
                      loc="lower left", bbox_to_anchor=(0.0, 1.0), ncol=3,
                      frameon=False, handlelength=1.3, columnspacing=1.0,
                      handletextpad=0.4, borderpad=0.2)
    left.add_artist(leg)
    if right.patches:
        left.legend([right.patches[0]], ["promoted fraction"],
                    loc="lower right", bbox_to_anchor=(1.0, 1.0), ncol=1,
                    frameon=False, handlelength=1.3, handletextpad=0.4,
                    borderpad=0.2)
    fig.subplots_adjust(left=0.085, right=0.905, top=0.86, bottom=0.20)
    return _save(fig, plt, out)


PANELS = {"F9a": f9a, "F9b": f9b, "F9c": f9c, "F10": f10, "F11": f11,
          "F12a": f12a, "F12b": f12b, "F13": f13, "F14": f14}


def _F12_FONTS():
    return dict(S.FONTS, **{"font.size": 18, "axes.labelsize": 18,
                            "xtick.labelsize": 14.5, "ytick.labelsize": 14.5,
                            "legend.fontsize": 11.5})


def _save(fig, plt, out: Path, tight: bool = False):
    fig.savefig(out, bbox_inches="tight", pad_inches=0.04 if tight else 0.02)
    plt.close(fig)
    return out
