#!/usr/bin/env python3
"""Create reproducible Markdown and SVG analysis artifacts from benchmark summaries."""
from __future__ import annotations

import argparse
import hashlib
import html
import json
import re
import sys
from pathlib import Path
from typing import Any

COLORS = ("#2563eb", "#dc2626", "#059669", "#7c3aed", "#d97706",
          "#0891b2", "#be185d", "#4d7c0f", "#475569", "#9333ea")
METRICS = (
    ("speedup_vs_sequential", "Speedup vs sequential", "×"),
    ("throughput_bytes_per_second", "Throughput (bytes/s)", "bytes/s"),
    ("parallel_efficiency", "Parallel efficiency", ""),
    ("mean_worker_busy_imbalance", "Worker busy-time imbalance", "ratio"),
    ("mean_worker_idle_fraction", "Estimated worker idle fraction", "fraction"),
    ("mean_scheduling_overhead_ns", "Scheduling overhead", "ns"),
    ("mean_steal_attempts", "Mean steal attempts", "attempts"),
    ("mean_steals_succeeded", "Mean successful steals", "steals"),
    ("mean_bytes_avoided", "Recovery bytes avoided", "bytes"),
    ("mean_recovery_success_rate", "Recovery success rate", "fraction"),
)


def _read_summary(path: Path) -> list[dict[str, Any]]:
    try:
        document = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot read summary {path}: {exc}") from exc
    if not isinstance(document, dict) or not isinstance(document.get("groups"), list):
        raise ValueError("summary must be a JSON object with a groups array")
    rows = document["groups"]
    if any(not isinstance(row, dict) for row in rows):
        raise ValueError("every summary group must be an object")
    return rows


def _slug(value: str) -> str:
    readable = re.sub(r"[^A-Za-z0-9._-]+", "-", value).strip("-_") or "workload"
    return f"{readable[:64]}-{hashlib.sha256(value.encode('utf-8')).hexdigest()[:8]}"


def _markdown_text(value: Any) -> str:
    return str(value).replace("|", "\\|").replace("`", "\\`").replace("\r", " ").replace("\n", " ")


def _chart(rows: list[dict[str, Any]], title: str, metric: str, label: str,
           destination: Path) -> bool:
    points_by_series: dict[tuple[str, int], list[tuple[int, float]]] = {}
    for row in rows:
        x, y = row.get("workers"), row.get(metric)
        if (isinstance(x, int) and not isinstance(x, bool) and
                isinstance(y, (int, float)) and not isinstance(y, bool)):
            points_by_series.setdefault((str(row.get("policy", "unknown")),
                                         int(row.get("chunk", 1))), []).append((x, float(y)))
    if not points_by_series:
        return False

    legend_rows = (len(points_by_series) + 2) // 3
    width, height = 920, 560 + max(0, legend_rows - 3) * 23
    left, right, top, bottom = 88, 28, 66, 136
    plot_width, plot_height = width - left - right, height - top - bottom
    all_points = [point for series in points_by_series.values() for point in series]
    x_values = [point[0] for point in all_points]
    y_values = [point[1] for point in all_points]
    x_min, x_max = min(x_values), max(x_values)
    if x_min == x_max:
        x_min, x_max = max(0, x_min - 1), x_max + 1
    y_min = min(0.0, min(y_values))
    y_max = max(y_values) * 1.08 if max(y_values) > 0 else 1.0
    if y_max <= y_min:
        y_max = y_min + 1.0

    def px(x: int) -> float:
        return left + (x - x_min) * plot_width / (x_max - x_min)

    def py(y: float) -> float:
        return top + (y_max - y) * plot_height / (y_max - y_min)

    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
           '<rect width="100%" height="100%" fill="white"/>',
           f'<text x="{width / 2}" y="32" text-anchor="middle" font-family="sans-serif" font-size="19" font-weight="bold">{html.escape(title)}</text>',
           f'<text x="{left + plot_width / 2}" y="{height - 18}" text-anchor="middle" font-family="sans-serif" font-size="14">Workers</text>',
           f'<text transform="translate(22 {top + plot_height / 2}) rotate(-90)" text-anchor="middle" font-family="sans-serif" font-size="14">{html.escape(label)}</text>']
    for tick in range(5):
        value = y_min + (y_max - y_min) * tick / 4
        y = py(value)
        svg.append(f'<line x1="{left}" y1="{y:.2f}" x2="{left + plot_width}" y2="{y:.2f}" stroke="#e2e8f0"/>')
        svg.append(f'<text x="{left - 10}" y="{y + 5:.2f}" text-anchor="end" font-family="sans-serif" font-size="12">{value:.3g}</text>')
    for worker in sorted(set(x_values)):
        x = px(worker)
        svg.append(f'<line x1="{x:.2f}" y1="{top}" x2="{x:.2f}" y2="{top + plot_height}" stroke="#f1f5f9"/>')
        svg.append(f'<text x="{x:.2f}" y="{top + plot_height + 22}" text-anchor="middle" font-family="sans-serif" font-size="12">{worker}</text>')
    svg.append(f'<line x1="{left}" y1="{top + plot_height}" x2="{left + plot_width}" y2="{top + plot_height}" stroke="#334155"/>')
    svg.append(f'<line x1="{left}" y1="{top}" x2="{left}" y2="{top + plot_height}" stroke="#334155"/>')

    legend_y = top + plot_height + 50
    for index, (series_name, points) in enumerate(sorted(points_by_series.items())):
        color = COLORS[index % len(COLORS)]
        ordered = sorted(points)
        coords = " ".join(f"{px(x):.2f},{py(y):.2f}" for x, y in ordered)
        if len(ordered) > 1:
            svg.append(f'<polyline points="{coords}" fill="none" stroke="{color}" stroke-width="2.5"/>')
        for x, y in ordered:
            svg.append(f'<circle cx="{px(x):.2f}" cy="{py(y):.2f}" r="4" fill="{color}"/>')
        legend_x = left + (index % 3) * 270
        row_y = legend_y + (index // 3) * 23
        name = f"{series_name[0]} (chunk {series_name[1]})"
        svg.append(f'<line x1="{legend_x}" y1="{row_y - 4}" x2="{legend_x + 22}" y2="{row_y - 4}" stroke="{color}" stroke-width="3"/>')
        svg.append(f'<text x="{legend_x + 28}" y="{row_y}" font-family="sans-serif" font-size="12">{html.escape(name)}</text>')
    svg.append("</svg>")
    destination.write_text("\n".join(svg) + "\n", encoding="utf-8")
    return True


def _granularity_comparisons(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    pairs: dict[tuple[Any, ...], dict[str, dict[str, Any]]] = {}
    for row in rows:
        policy = row.get("policy")
        if policy not in {"stealing", "adaptive"}:
            continue
        config = dict(row.get("experiment_config", {}))
        config.pop("min_chunk", None)
        config.pop("max_chunk", None)
        key = (row.get("operation"), row.get("workload_id"), row.get("workers"),
               row.get("chunk"), json.dumps(config, sort_keys=True, separators=(",", ":")))
        pairs.setdefault(key, {})[policy] = row

    comparisons = []
    for key, pair in sorted(pairs.items(), key=lambda item: tuple(str(part) for part in item[0])):
        fixed, adaptive = pair.get("stealing"), pair.get("adaptive")
        if not fixed or not adaptive:
            continue
        fixed_time, adaptive_time = fixed.get("mean_seconds"), adaptive.get("mean_seconds")
        if not (isinstance(fixed_time, (int, float)) and isinstance(adaptive_time, (int, float))):
            continue
        time_change = ((adaptive_time / fixed_time) - 1.0) * 100 if fixed_time > 0 else None
        fixed_imbalance = fixed.get("mean_worker_busy_imbalance")
        adaptive_imbalance = adaptive.get("mean_worker_busy_imbalance")
        fixed_idle = fixed.get("mean_worker_idle_fraction")
        adaptive_idle = adaptive.get("mean_worker_idle_fraction")
        comparisons.append({
            "operation": key[0], "workload_id": key[1], "workers": key[2], "chunk": key[3],
            "fixed_seconds": fixed_time, "adaptive_seconds": adaptive_time,
            "adaptive_time_change_percent": time_change,
            "imbalance_change": (adaptive_imbalance - fixed_imbalance)
            if isinstance(adaptive_imbalance, (int, float)) and isinstance(fixed_imbalance, (int, float))
            else None,
            "idle_fraction_change": (adaptive_idle - fixed_idle)
            if isinstance(adaptive_idle, (int, float)) and isinstance(fixed_idle, (int, float))
            else None,
            "fixed_repetitions": fixed.get("repetitions"),
            "adaptive_repetitions": adaptive.get("repetitions"),
        })
    return comparisons


def generate(summary_path: Path, output: Path) -> dict[str, Any]:
    rows = _read_summary(summary_path)
    output.mkdir(parents=True, exist_ok=True)
    workloads: dict[tuple[str, str, str], list[dict[str, Any]]] = {}
    for row in rows:
        operation = str(row.get("operation", "unknown"))
        workload = str(row.get("workload_id", "unknown"))
        config_key = json.dumps(row.get("experiment_config", {}), sort_keys=True,
                                separators=(",", ":"))
        workloads.setdefault((operation, workload, config_key), []).append(row)

    artifacts: list[dict[str, str]] = []
    for (operation, workload, config_key), workload_rows in sorted(workloads.items()):
        config = json.loads(config_key)
        config_label = ", ".join(
            f"{key}={config[key]}" for key in
            ("heterogeneity", "duplicate_ratio", "recovery_mode", "damage_ratio", "damage_types")
            if config.get(key) is not None)
        prefix = _slug(f"{operation}-{workload}-{config_key}")
        for metric, title, unit in METRICS:
            filename = f"{prefix}-{metric}.svg"
            suffix = f" ({config_label})" if config_label else ""
            chart_title = f"{title}: {operation} / {workload}{suffix}"
            if _chart(workload_rows, chart_title, metric, unit or title, output / filename):
                artifacts.append({"file": filename, "operation": operation,
                                  "workload_id": workload, "metric": metric,
                                  "configuration": config_label})

    report = ["# Benchmark analysis", "",
              f"Source summary: `{summary_path.name}`.", "",
              "Values below reproduce the benchmark summary. They describe measured runs and do not by themselves establish that one scheduling policy is generally faster.", "",
              "## Run coverage", "",
              "This section counts only attempts present in the summary. It cannot identify configurations that were never scheduled.", "",
              "| Operation | Policy | Attempts | Successful repetitions | Failed attempts | Pass rate |",
              "|---|---|---:|---:|---:|---:|"]
    coverage: dict[tuple[str, str], list[int]] = {}
    for row in rows:
        operation = str(row.get("operation", "unknown"))
        policy = str(row.get("policy", "unknown"))
        counts = coverage.setdefault((operation, policy), [0, 0, 0])
        attempts = row.get("attempted_runs", row.get("repetitions", 0))
        successes = row.get("successes", row.get("repetitions", 0))
        attempts = attempts if isinstance(attempts, int) and not isinstance(attempts, bool) else 0
        successes = successes if isinstance(successes, int) and not isinstance(successes, bool) else 0
        failed = row.get("failed_runs", max(0, attempts - successes))
        for index, value in enumerate((attempts, successes, failed)):
            if isinstance(value, int) and not isinstance(value, bool) and value >= 0:
                counts[index] += value
    for (operation, policy), (attempts, successes, failed) in sorted(coverage.items()):
        pass_rate = successes / attempts if attempts else None
        rate_text = f"{pass_rate:.1%}" if pass_rate is not None else "n/a"
        report.append(f"| {_markdown_text(operation)} | {_markdown_text(policy)} | {attempts} | "
                      f"{successes} | {failed} | {rate_text} |")
    report.extend(["", "## Charts", ""])
    if artifacts:
        for item in artifacts:
            suffix = f" ({item['configuration']})" if item["configuration"] else ""
            report.append(f"- [{item['operation']} / {item['workload_id']} — {item['metric']}{suffix}]({item['file']})")
    else:
        report.append("No chartable metric rows were present.")
    report.extend(["", "## Results", "",
                   "| Operation | Workload | Policy | Workers | Chunk | Successful reps | Attempts | Failed runs | Mean seconds | SD seconds | Speedup | Efficiency | Throughput bytes/s | Busy imbalance | Idle fraction | Scheduling overhead ns | Steal attempts | Successful steals | Files avoided | Bytes avoided | Recovery success | Missing | Modified | Inaccessible | Extra | Verifier missing | Size mismatch | Timestamp mismatch | Digest mismatch | Verifier inaccessible | Verifier unsafe |", "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"])
    for row in sorted(rows, key=lambda item: (str(item.get("operation")), str(item.get("workload_id")),
                                               str(item.get("policy")), int(item.get("workers", 0)),
                                               int(item.get("chunk", 0)))):
        values = [row.get("operation"), row.get("workload_id"), row.get("policy"), row.get("workers"),
                  row.get("chunk"), row.get("repetitions"), row.get("attempted_runs"),
                  row.get("failed_runs"), row.get("mean_seconds"), row.get("stdev_seconds"),
                  row.get("speedup_vs_sequential"), row.get("parallel_efficiency"),
                  row.get("throughput_bytes_per_second"), row.get("mean_worker_busy_imbalance"),
                  row.get("mean_worker_idle_fraction"), row.get("mean_scheduling_overhead_ns"),
                  row.get("mean_steal_attempts"), row.get("mean_steals_succeeded"),
                  row.get("mean_files_avoided"), row.get("mean_bytes_avoided"),
                  row.get("mean_recovery_success_rate"), row.get("mean_files_missing"),
                  row.get("mean_files_modified"), row.get("mean_files_inaccessible"),
                  row.get("mean_files_extra"), row.get("mean_verifier_missing"),
                  row.get("mean_verifier_size_mismatch"),
                  row.get("mean_verifier_timestamp_mismatch"),
                  row.get("mean_verifier_digest_mismatch"),
                  row.get("mean_verifier_inaccessible"), row.get("mean_verifier_unsafe")]
        cells = []
        for value in values:
            if value is None:
                cells.append("—")
            elif isinstance(value, float):
                cells.append(f"{value:.4g}")
            else:
                cells.append(str(value).replace("|", "\\|"))
        report.append("| " + " | ".join(cells) + " |")
    comparisons = _granularity_comparisons(rows)
    report.extend(["", "## Fixed versus adaptive granularity", "",
                   "Matched `stealing` and `adaptive` groups use the same operation, workload, worker count, chunk, and workload/damage configuration. A negative time change means adaptive mode had a lower mean runtime in these recorded groups; it is descriptive and not a general performance claim.", "",
                   "| Operation | Workload | Workers | Chunk | Fixed seconds | Adaptive seconds | Adaptive time change % | Imbalance change | Idle-fraction change | Fixed reps | Adaptive reps |",
                   "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|"])
    for row in comparisons:
        values = [row["operation"], row["workload_id"], row["workers"], row["chunk"],
                  row["fixed_seconds"], row["adaptive_seconds"], row["adaptive_time_change_percent"],
                  row["imbalance_change"], row["idle_fraction_change"],
                  row["fixed_repetitions"], row["adaptive_repetitions"]]
        report.append("| " + " | ".join(
            "—" if value is None else (f"{value:.4g}" if isinstance(value, float)
                                       else str(value).replace("|", "\\|"))
            for value in values) + " |")
    if not comparisons:
        report.append("| No matched successful fixed/adaptive groups | | | | | | | | | | |")
    failed_groups = [row for row in rows if row.get("successes") == 0 and row.get("attempted_runs", 0) > 0]
    report.extend(["", "## Configurations with no successful repetitions", ""])
    if failed_groups:
        for row in failed_groups:
            failures = "; ".join(dict.fromkeys(row.get("failure_messages", [])))
            report.append(
                f"- `{_markdown_text(row.get('operation'))}` / `{_markdown_text(row.get('workload_id'))}` / "
                f"`{_markdown_text(row.get('policy'))}` "
                f"with {row.get('workers')} workers, chunk {row.get('chunk')}: "
                f"{row.get('failed_runs')} failed run(s). {_markdown_text(failures)}")
    else:
        report.append("No all-failure configuration groups were present in the summary.")
    report.append("")
    report_path = output / "report.md"
    report_path.write_text("\n".join(report), encoding="utf-8")
    return {"groups": len(rows), "charts": len(artifacts), "report": str(report_path)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--summary", required=True, type=Path,
                        help="summary.json written by src/benchmark.py summarize or run")
    parser.add_argument("--output", required=True, type=Path,
                        help="directory for report.md and SVG charts")
    args = parser.parse_args()
    try:
        result = generate(args.summary, args.output)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
