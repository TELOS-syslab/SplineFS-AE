"""The paper's figure style.

`usetex` needs a TeX installation with libertine and newtxmath; without one,
`setup()` falls back to matplotlib's fonts and says so.
"""
from __future__ import annotations

import glob
import os
import shutil

# Wong's colourblind-safe palette, as in the paper.
ORANGE = "#D55E00"
TEAL   = "#56B4E9"
BLUE   = "#0072B2"
AMBER  = "#E69F00"
PINK   = "#CC79A7"
GREY   = "#999999"
DARK   = "#444444"
GREEN  = "#009E73"

# The paper's colours: SplineFS orange, ext4 blue.
COL = {"ext4": BLUE, "naive": GREY, "splinefs": ORANGE, "adaptive": ORANGE,
       "xfs": TEAL, "btrfs": AMBER, "f2fs": ORANGE,
       "mutable": GREY, "promote": TEAL, "subtree": ORANGE,
       "policy_off": "#888888"}
# Hatching as in the paper's figures.
HATCH = {"ext4": "", "naive": "..", "splinefs": "||", "adaptive": "||",
         "xfs": "xx", "btrfs": "++", "f2fs": "--",
         "mutable": "", "promote": "", "subtree": ""}
LABEL = {"ext4": "ext4", "naive": "naive", "splinefs": "SplineFS",
         "adaptive": "SplineFS", "xfs": "xfs", "btrfs": "btrfs",
         "f2fs": "f2fs", "policy_off": "promotion off"}

# Markers for the line figures, 9b and 14.
LINE = {"ext4":       dict(marker="x", linestyle=(0, (5, 2)), mew=1.4, ms=6.0, lw=1.9),
        "naive":      dict(marker="s", linestyle=(0, (1, 1)), mew=0.6, ms=5.5, lw=1.9),
        "policy_off": dict(marker="s", linestyle=(0, (1, 1)), mew=0.6, ms=5.5, lw=1.9),
        "splinefs":   dict(marker="o", linestyle="-", mew=0.8, ms=6.5, lw=2.2),
        "adaptive":   dict(marker="o", linestyle="-", mew=0.8, ms=6.5, lw=2.2)}

CAPLAB = {268435456: "256M", 536870912: "512M",
          1073741824: "1G", 2147483648: "2G",
          4294967296: "4G", 8589934592: "8G", 67108864: "64M",
          134217728: "128M"}

_PREAMBLE = (r"\usepackage[utf8]{inputenc}\usepackage[T1]{fontenc}"
             r"\usepackage{textcomp}\usepackage{libertine}"
             r"\usepackage[libertine]{newtxmath}")

# Font sizes of the paper's Fig. 9.
FONTS = {"font.size": 24.5, "axes.labelsize": 20, "xtick.labelsize": 19.5,
         "ytick.labelsize": 19.5, "legend.fontsize": 13.5,
         "axes.linewidth": 0.7}


_NOTED = False


def tx(s: str) -> str:
    """A label written for TeX, made plain when TeX is not in use."""
    import matplotlib.pyplot as plt
    if plt.rcParams.get("text.usetex"):
        return s
    return s.replace("\\ ", " ").replace("\\%", "%")


def setup(usetex: bool = True, fonts: dict | None = None):
    """Apply the paper's rcParams and return the pyplot module."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    if usetex and not shutil.which("latex"):
        # TeX Live's default location, when it is not on PATH.
        tex = sorted(glob.glob("/usr/local/texlive/*/bin/x86_64-linux"))
        if tex:
            os.environ["PATH"] = tex[-1] + os.pathsep + os.environ.get("PATH", "")
    if usetex:
        try:
            plt.rcParams.update({
                "text.usetex": True, "font.family": "serif",
                "font.serif": ["Linux Libertine O", "Times New Roman",
                               "DejaVu Serif"],
                "text.latex.preamble": _PREAMBLE,
            })
            # Render once so a missing TeX fails here, where we can fall back.
            fig = plt.figure(); fig.text(0.5, 0.5, r"$x$"); fig.canvas.draw()
            plt.close(fig)
        except Exception:                                     # noqa: BLE001
            plt.rcParams.update({"text.usetex": False,
                                 "font.family": "serif"})
            global _NOTED
            if not _NOTED:
                print("  note: no usable TeX with libertine; using matplotlib's "
                      "own fonts. Numbers are unaffected, typography differs "
                      "from the paper.")
                _NOTED = True
    plt.rcParams["hatch.linewidth"] = 0.4
    plt.rcParams.update(fonts or FONTS)
    return plt


def style_axis(ax, *, ygrid: bool = True):
    ax.grid(True, axis=("y" if ygrid else "both"), which="major", ls=":",
            lw=0.5, color="#999999", alpha=0.7)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    ax.tick_params(direction="out", length=3, color="#555555")
    ax.set_axisbelow(True)


def panel_label(ax, label: str, *, y: float = -0.34):
    ax.text(0.5, y, label, transform=ax.transAxes, ha="center", va="top",
            fontsize=21, fontweight="normal", fontfamily="serif", color=DARK,
            clip_on=False)


def caplab(v) -> str:
    try:
        return CAPLAB.get(int(v), f"{int(v) // (1 << 20)}M")
    except (TypeError, ValueError):
        return str(v)
