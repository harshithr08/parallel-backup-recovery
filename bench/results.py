"""Result files: append-only JSON lines plus summary tables."""

from __future__ import annotations

import csv
import json
import statistics
from collections import defaultdict
from pathlib import Path


def load_runs(path: Path) -> list[dict]:
    if not path.is_file():
        return []
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def append_run(path: Path, record: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a") as out:
        out.write(json.dumps(record) + "\n")


def summarize(runs: list[dict], keys: tuple[str, ...], metric, baseline: dict | None = None) -> list[dict]:
    """Groups runs by config keys and reports mean/stdev/min of metric(run) in seconds.

    baseline maps a grouping (all keys except policy/threads/split/mode) to the
    sequential mean, giving speedup and parallel efficiency.
    """
    groups: dict[tuple, list[dict]] = defaultdict(list)
    for run in runs:
        groups[tuple(run["config"][k] for k in keys)].append(run)
    rows = []
    for group, members in sorted(groups.items(), key=lambda item: tuple(str(v) for v in item[0])):
        values = [metric(run) for run in members]
        row = dict(zip(keys, group))
        row.update({
            "reps": len(values),
            "mean_s": statistics.fmean(values),
            "stdev_s": statistics.stdev(values) if len(values) > 1 else 0.0,
            "min_s": min(values),
        })
        sched = [run["result"].get("scheduler") or run["result"].get("scan", {}).get("scheduler") for run in members]
        sched = [s for s in sched if s]
        if sched:
            row["load_imbalance"] = statistics.fmean(s["load_imbalance"] for s in sched)
            row["idle_fraction"] = statistics.fmean(s["idle_fraction"] for s in sched)
            row["finish_spread"] = statistics.fmean(s["finish_spread"] for s in sched)
            row["overhead_fraction"] = statistics.fmean(s["overhead_fraction"] for s in sched)
            row["steals"] = statistics.fmean(s["steals"] for s in sched)
        if baseline is not None:
            base = baseline.get(row.get("workload"))
            if base:
                row["speedup"] = base / row["mean_s"]
                row["efficiency"] = row["speedup"] / row["threads"]
        row["all_verified"] = all(run.get("verified", True) for run in members)
        rows.append(row)
    return rows


def write_csv(path: Path, rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    columns: list[str] = []
    for row in rows:
        columns.extend(k for k in row if k not in columns)
    with path.open("w", newline="") as out:
        writer = csv.DictWriter(out, fieldnames=columns)
        writer.writeheader()
        for row in rows:
            writer.writerow({k: (f"{v:.4f}" if isinstance(v, float) else v) for k, v in row.items()})
