"""Draws the report figures from the summary CSVs in results/.

Speedup uses the best repetition of each configuration (min time) over the
best sequential run, because write-heavy runs on the test machine show a
bimodal slow mode from the drive; read-only runs barely differ either way.

usage: python bench/plot.py [--results results] [--out results/figures]
"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

from common import REPO  # noqa: E402

# Validated two-series palette (light surface): CVD dE 24.7, normal dE 33.6.
SURFACE = "#fcfcfb"
TEXT = "#0b0b0b"
TEXT_2 = "#52514e"
GRID = "#e4e3df"
IDEAL = "#a8a79f"
SERIES = {
    "none": {"label": "File-level (steal)", "color": "#2a78d6", "marker": "o"},
    "adaptive": {"label": "IPBRS (steal + split)", "color": "#eb6834", "marker": "s"},
}
THREADS = [1, 2, 4, 8, 16]
WORKLOADS = [("small", "small: 20k x 8-16 KiB"), ("mixed", "mixed: 3k x 256 B-4 MiB"),
             ("huge", "huge: 4k small + 4 x 400 MiB")]


def load(path: Path) -> list[dict]:
    with path.open() as f:
        return list(csv.DictReader(f))


def pick(rows: list[dict], **match) -> dict | None:
    for row in rows:
        if all(row.get(k) == v for k, v in match.items()):
            return row
    return None


def style(ax, title: str) -> None:
    ax.set_facecolor(SURFACE)
    ax.set_title(title, color=TEXT, fontsize=10, loc="left")
    ax.grid(True, color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRID)
    ax.tick_params(colors=TEXT_2, labelsize=8)
    ax.set_xscale("log", base=2)
    ax.set_xticks(THREADS)
    ax.set_xticklabels([str(t) for t in THREADS])
    ax.set_xlim(0.85, 40)  # room for end labels right of 16
    ax.set_xlabel("threads", color=TEXT_2, fontsize=9)


def label_ends(ax, ends: dict) -> None:
    """Labels the final point of each series; the higher value is placed above.
    Values within 5% of each other share one label so neither is misattributed."""
    (xa, a), (xb, b) = ends["none"], ends["adaptive"]
    if abs(a - b) <= 0.05 * max(a, b):
        ax.annotate(f"both {max(a, b):.1f}x", (xa, max(a, b)), textcoords="offset points",
                    xytext=(6, 4), fontsize=8, color=TEXT)
        return
    for split, (x, y) in ends.items():
        other = b if split == "none" else a
        name = "IPBRS" if split == "adaptive" else "file-level"
        ax.annotate(f"{name} {y:.1f}x", (x, y), textcoords="offset points",
                    xytext=(6, 5 if y > other else -11), fontsize=8, color=TEXT)


def speedup_series(rows: list[dict], workload: str, base_match: dict, match: dict, split: str) -> list[float]:
    base = float(pick(rows, workload=workload, policy="seq", **base_match)["min_s"])
    values = [1.0]
    for threads in THREADS[1:]:
        row = pick(rows, workload=workload, policy="steal", threads=str(threads), split=split, **match)
        values.append(base / float(row["min_s"]) if row else float("nan"))
    return values


def speedup_figure(rows: list[dict], title: str, base_match: dict, match: dict, out: Path,
                   threads: list[int] | None = None) -> None:
    fig, axes = plt.subplots(1, 3, figsize=(11, 3.6), sharey=True, facecolor=SURFACE)
    for ax, (workload, label) in zip(axes, WORKLOADS):
        style(ax, label)
        ax.plot(THREADS, THREADS, linestyle="--", color=IDEAL, linewidth=1.2, label="ideal (linear)")
        ends = {}
        for split, spec in SERIES.items():
            values = speedup_series(rows, workload, base_match, match, split)
            xs = [t for t, v in zip(THREADS, values) if v == v]
            ys = [v for v in values if v == v]
            ax.plot(xs, ys, color=spec["color"], linewidth=1.6, marker=spec["marker"], markersize=6,
                    markeredgecolor=SURFACE, markeredgewidth=1.0, label=spec["label"])
            ends[split] = (xs[-1], ys[-1])
        label_ends(ax, ends)
        ax.set_ylim(0, 17)
    axes[0].set_ylabel("speedup over sequential", color=TEXT_2, fontsize=9)
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper right", ncol=3, frameon=False, fontsize=8, labelcolor=TEXT)
    fig.suptitle(title, x=0.01, ha="left", color=TEXT, fontsize=11)
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    fig.savefig(out, dpi=160, facecolor=SURFACE)
    plt.close(fig)
    print(f"wrote {out}")


def efficiency_figure(rows: list[dict], out: Path) -> None:
    fig, axes = plt.subplots(1, 3, figsize=(11, 3.4), sharey=True, facecolor=SURFACE)
    for ax, (workload, label) in zip(axes, WORKLOADS):
        style(ax, label)
        ax.axhline(1.0, linestyle="--", color=IDEAL, linewidth=1.2, label="ideal")
        for split, spec in SERIES.items():
            values = speedup_series(rows, workload, {"experiment": "verify", "trust_mtime": "False"},
                                    {"experiment": "verify", "trust_mtime": "False"}, split)
            eff = [v / t for v, t in zip(values, THREADS)]
            ax.plot(THREADS, eff, color=spec["color"], linewidth=1.6, marker=spec["marker"], markersize=6,
                    markeredgecolor=SURFACE, markeredgewidth=1.0, label=spec["label"])
        ax.set_ylim(0, 1.1)
    axes[0].set_ylabel("parallel efficiency (speedup / threads)", color=TEXT_2, fontsize=9)
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper right", ncol=3, frameon=False, fontsize=8, labelcolor=TEXT)
    fig.suptitle("Verify: parallel efficiency", x=0.01, ha="left", color=TEXT, fontsize=11)
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    fig.savefig(out, dpi=160, facecolor=SURFACE)
    plt.close(fig)
    print(f"wrote {out}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--results", type=Path, default=REPO / "results")
    parser.add_argument("--out", type=Path, default=REPO / "results" / "figures")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    scan = load(args.results / "scan_summary.csv")
    verify = {"experiment": "verify", "trust_mtime": "False"}
    detect = {"experiment": "detect", "trust_mtime": "False"}
    speedup_figure(scan, "Verify (read + hash every block): speedup, best of 3",
                   verify, verify, args.out / "verify_speedup.png")
    speedup_figure(scan, "Damage detection (dry-run scan of a 5%-damaged tree): speedup, best of 3",
                   detect, detect, args.out / "detect_speedup.png")
    efficiency_figure(scan, args.out / "verify_efficiency.png")
    backup = load(args.results / "backup_summary.csv")
    speedup_figure(backup, "Durable backup (flushed to disk): speedup, best of 3",
                   {}, {}, args.out / "backup_speedup.png")


if __name__ == "__main__":
    main()
