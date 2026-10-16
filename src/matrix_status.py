#!/usr/bin/env python3
"""Compare saved benchmark summaries with the configured experiment matrix."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

try:
    from .experiment_matrix import _matrix_settings, audit_protocol, load_protocol
except ImportError:  # direct `python src/matrix_status.py` execution
    from experiment_matrix import _matrix_settings, audit_protocol, load_protocol

VERIFIER_ERROR_FIELDS = (
    "mean_verifier_missing", "mean_verifier_size_mismatch",
    "mean_verifier_digest_mismatch", "mean_verifier_inaccessible",
    "mean_verifier_unsafe",
)


def _expected_workloads(protocol: dict[str, Any]) -> list[dict[str, Any]]:
    result = []
    index = 0
    for count in protocol["dataset_sizes"]:
        for profile in protocol["heterogeneity_profiles"]:
            seed = protocol["dataset_seed_base"] + index
            result.append({
                "workload_id": f"n{count}-{profile}-s{seed}",
                "file_count": count,
                "heterogeneity": profile,
            })
            index += 1
    return result


def _summaries(results_root: Path) -> list[Path]:
    # Keep older pilot runs separate from final backup and recovery result trees.
    paths = list(results_root.glob("final-*-results/summary.json"))
    # The explicitly provisional continuation matrix is nested so its configs,
    # result logs, and execution plan stay together for resumable runs.
    paths.extend((results_root / "final-experiment-provisional-remaining" /
                  "results").glob("*/summary.json"))
    return sorted(set(paths))


def _verification_errors(group: dict[str, Any]) -> list[str]:
    errors = []
    for field in VERIFIER_ERROR_FIELDS:
        if field not in group:
            errors.append(f"missing_metric:{field}")
            continue
        value = group[field]
        if isinstance(value, (int, float)) and value > 0:
            errors.append(field.removeprefix("mean_verifier_"))
    return errors


def _group_result(expected_keys: set[tuple[Any, ...]],
                  pairs: list[tuple[str, dict[str, Any]]],
                  repetitions: int, kind: str) -> dict[str, Any]:
    by_key: dict[tuple[Any, ...], list[tuple[str, dict[str, Any]]]] = {}
    malformed = []
    for path, group in pairs:
        try:
            key = (group["policy"], int(group["workers"]), int(group["chunk"]))
            if kind == "recovery":
                config = group["experiment_config"]
                damage_types = config.get("damage_types") or []
                if isinstance(damage_types, str):
                    damage_types = [damage_types]
                if len(damage_types) != 1:
                    raise ValueError("expected exactly one damage type per recovery group")
                key += (float(config["damage_ratio"]), damage_types[0], config["recovery_mode"])
        except (KeyError, TypeError, ValueError) as exc:
            malformed.append({"summary": path, "error": str(exc)})
            continue
        by_key.setdefault(key, []).append((path, group))

    missing = sorted(expected_keys - set(by_key), key=str)
    duplicate = sorted((key for key, rows in by_key.items() if len(rows) > 1), key=str)
    incomplete = []
    failed_runs = 0
    successes = 0
    verifier_issues = []
    for key, rows in by_key.items():
        for path, group in rows:
            attempted = int(group.get("attempted_runs", 0))
            success = int(group.get("successes", max(0, attempted - int(group.get("failed_runs", 0)))))
            failed = int(group.get("failed_runs", max(0, attempted - success)))
            failed_runs += failed
            successes += success
            if attempted < repetitions or success < repetitions:
                incomplete.append({
                    "key": list(key), "attempted": attempted, "successful": success,
                    "expected_repetitions": repetitions, "summary": path,
                })
            issues = _verification_errors(group) if kind == "backup" else []
            if issues:
                verifier_issues.append({"key": list(key), "findings": issues, "summary": path})

    if not pairs:
        status = "NOT_STARTED"
    elif verifier_issues:
        status = "CORRECTNESS_ISSUES"
    elif missing or duplicate or incomplete or malformed:
        status = "FAILED" if failed_runs else "PARTIAL"
    elif failed_runs:
        status = "COMPLETE_WITH_FAILED_ATTEMPTS"
    else:
        status = "COMPLETE"
    return {
        "status": status,
        "expected_groups": len(expected_keys),
        "observed_groups": len(by_key),
        "expected_attempts": len(expected_keys) * repetitions,
        "successful_runs": successes,
        "failed_runs": failed_runs,
        "missing_groups": [list(key) for key in missing],
        "duplicate_groups": [list(key) for key in duplicate],
        "incomplete_groups": incomplete,
        "malformed_groups": malformed,
        "verifier_findings": verifier_issues,
        "summary_files": sorted({path for path, _group in pairs}),
    }


def audit_coverage(protocol_path: Path, results_root: Path) -> dict[str, Any]:
    protocol = load_protocol(protocol_path)
    protocol_audit = audit_protocol(protocol_path)
    expected_workloads = _expected_workloads(protocol)
    expected_settings = {
        (policy, workers, chunk)
        for policy, workers, chunk, _min_chunk, _max_chunk in _matrix_settings(protocol)
    }
    expected_repetitions = protocol["repetitions"]
    recovery_settings = {
        setting + (float(ratio), damage_type, mode)
        for setting in expected_settings
        for ratio in protocol["recovery_damage_ratios"]
        for damage_type in protocol["damage_types"]
        for mode in ("selective", "full")
    }

    summaries: dict[str, list[tuple[str, dict[str, Any]]]] = {}
    unreadable = []
    loaded = []
    supplemental_root = (results_root / "final-experiment-provisional-remaining" /
                         "results").resolve()
    for path in _summaries(results_root):
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            unreadable.append({"path": str(path), "error": str(exc)})
            continue
        loaded.append((path, data))

    # When a failed provisional result was rerun into the continuation matrix,
    # prefer that explicit replacement instead of counting duplicate settings.
    replacement_workloads = {
        group.get("workload_id")
        for path, data in loaded
        if path.resolve().is_relative_to(supplemental_root)
        for group in data.get("groups", [])
        if isinstance(group.get("workload_id"), str)
    }
    for path, data in loaded:
        if (not path.resolve().is_relative_to(supplemental_root) and
                any(group.get("workload_id") in replacement_workloads
                    for group in data.get("groups", []))):
            continue
        for group in data.get("groups", []):
            workload_id = group.get("workload_id")
            if isinstance(workload_id, str):
                summaries.setdefault(workload_id, []).append((str(path), group))

    workloads = []
    expected_attempts_total = 0
    observed_successes_total = 0
    expected_recovery_attempts_total = 0
    observed_recovery_successes_total = 0
    complete_workloads = 0
    started_recovery_workloads = 0
    complete_recovery_workloads = 0
    for workload in expected_workloads:
        workload_id = workload["workload_id"]
        workload_pairs = summaries.get(workload_id, [])
        backup_pairs = [(path, group) for path, group in workload_pairs
                        if group.get("operation") == "backup"]
        recovery_pairs = [(path, group) for path, group in workload_pairs
                          if group.get("operation") == "recover"]
        backup = _group_result(expected_settings, backup_pairs, expected_repetitions, "backup")
        recovery = _group_result(recovery_settings, recovery_pairs, expected_repetitions, "recovery")
        expected_attempts_total += backup["expected_attempts"]
        observed_successes_total += backup["successful_runs"]
        expected_recovery_attempts_total += recovery["expected_attempts"]
        observed_recovery_successes_total += recovery["successful_runs"]
        if backup["status"] == "COMPLETE":
            complete_workloads += 1
        if recovery["status"] != "NOT_STARTED":
            started_recovery_workloads += 1
        if recovery["status"] == "COMPLETE":
            complete_recovery_workloads += 1
        workloads.append({
            **workload,
            "backup": backup,
            "recovery": recovery,
        })

    expected_backup_runs = protocol_audit.get("matrix_scale", {}).get("total_backup_runs")
    expected_recovery_runs = protocol_audit.get("matrix_scale", {}).get("total_recovery_runs")
    return {
        "schema_version": 1,
        "protocol_id": protocol.get("protocol_id"),
        "protocol_status": protocol.get("approval_status", "unspecified").upper(),
        "results_root": str(results_root.resolve()),
        "expected_workload_count": len(expected_workloads),
        "complete_workload_count": complete_workloads,
        "started_recovery_workload_count": started_recovery_workloads,
        "complete_recovery_workload_count": complete_recovery_workloads,
        "expected_backup_runs": expected_backup_runs,
        "expected_recovery_runs": expected_recovery_runs,
        "expected_total_measurements": ((expected_backup_runs or 0) + (expected_recovery_runs or 0)),
        "expected_backup_attempts_in_checked_summaries": expected_attempts_total,
        "successful_backup_runs_in_checked_summaries": observed_successes_total,
        "expected_recovery_attempts_in_checked_summaries": expected_recovery_attempts_total,
        "successful_recovery_runs_in_checked_summaries": observed_recovery_successes_total,
        "unreadable_summaries": unreadable,
        "workloads": workloads,
    }


def markdown_report(report: dict[str, Any]) -> str:
    lines = [
        "# Experiment Matrix Coverage",
        "",
        f"- Protocol: `{report['protocol_id']}` ({report['protocol_status']})",
        f"- Complete backup workloads: {report['complete_workload_count']}/{report['expected_workload_count']}",
        f"- Recovery workloads started: {report['started_recovery_workload_count']}/{report['expected_workload_count']}; "
        f"complete: {report['complete_recovery_workload_count']}/{report['expected_workload_count']}",
        f"- Successful backup runs found: {report['successful_backup_runs_in_checked_summaries']}/"
        f"{report['expected_backup_attempts_in_checked_summaries']} planned",
        f"- Successful recovery runs found: {report['successful_recovery_runs_in_checked_summaries']}/"
        f"{report['expected_recovery_attempts_in_checked_summaries']} planned",
        f"- Planned measurements: {report['expected_backup_runs']} backup + "
        f"{report['expected_recovery_runs']} recovery",
        "",
        "| Workload | Files | Profile | Backup status | Backup groups | Backup pass | Recovery status | Recovery groups | Recovery pass |",
        "|---|---:|---|---|---:|---:|---|---:|---:|",
    ]
    for row in report["workloads"]:
        lines.append(
            f"| {row['workload_id']} | {row['file_count']:,} | {row['heterogeneity']} | "
            f"{row['backup']['status']} | {row['backup']['observed_groups']}/{row['backup']['expected_groups']} | "
            f"{row['backup']['successful_runs']}/{row['backup']['expected_attempts']} | "
            f"{row['recovery']['status']} | {row['recovery']['observed_groups']}/{row['recovery']['expected_groups']} | "
            f"{row['recovery']['successful_runs']}/{row['recovery']['expected_attempts']} |"
        )
    if report["unreadable_summaries"]:
        lines.extend(["", "## Unreadable summaries", ""])
        for item in report["unreadable_summaries"]:
            lines.append(f"- `{item['path']}`: {item['error']}")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--protocol", required=True, type=Path)
    parser.add_argument("--results-root", required=True, type=Path,
                        help="directory containing final backup/recovery result folders named final-*-results")
    parser.add_argument("--output", type=Path,
                        help="optional .json or .md report path; otherwise print Markdown")
    args = parser.parse_args()
    try:
        report = audit_coverage(args.protocol, args.results_root)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
        return 2
    rendered = (json.dumps(report, indent=2) + "\n" if args.output and args.output.suffix.lower() == ".json"
                else markdown_report(report))
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
    else:
        print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
