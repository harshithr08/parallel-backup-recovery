#!/usr/bin/env python3
"""Prepare reproducible final workload and benchmark matrix configurations.

This planner does not generate datasets or start timed runs. It requires every
experiment choice left open by the master specification to be supplied in an
explicit protocol file before it writes a plan.
"""
from __future__ import annotations

import argparse
import json
import math
import shutil
import re
import sys
from pathlib import Path
from typing import Any

try:
    from .dataset_generator import HETEROGENEITY_PROFILES, MAIN_DATASET_SIZES
except ImportError:  # direct `python src/experiment_matrix.py` execution
    from dataset_generator import HETEROGENEITY_PROFILES, MAIN_DATASET_SIZES

SCHEMA_VERSION = 1
POLICIES = ("sequential", "static", "dynamic", "stealing", "adaptive")
DAMAGE_TYPES = ("missing", "modified", "corrupted", "truncated")
REQUIRED_PROTOCOL_FIELDS = (
    "repetitions", "duplicate_ratio", "recovery_damage_ratios", "damage_types",
    "dataset_seed_base", "execution_order_seed_base", "recovery_damage_seed_base",
    "files_per_directory",
    "fixed_chunk_sizes", "adaptive_start_chunks", "adaptive_min_chunk",
    "adaptive_max_chunk",
)


def _integer(value: Any, field: str, minimum: int = 1) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise ValueError(f"{field} must be an integer >= {minimum}")
    return value


def _ratio(value: Any, field: str, *, positive: bool = False) -> float:
    if (isinstance(value, bool) or not isinstance(value, (int, float)) or
            not math.isfinite(value)):
        raise ValueError(f"{field} must be a number between 0 and 1")
    if value < 0.0 or value > 1.0 or (positive and value == 0.0):
        minimum = "greater than 0" if positive else "at least 0"
        raise ValueError(f"{field} must be {minimum} and at most 1")
    return float(value)


def load_protocol(path: Path) -> dict[str, Any]:
    try:
        protocol = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot read protocol {path}: {exc}") from exc
    if not isinstance(protocol, dict) or protocol.get("schema_version") != SCHEMA_VERSION:
        raise ValueError(f"protocol schema_version must be {SCHEMA_VERSION}")
    environment_metadata = protocol.get("environment_metadata", {})
    if (not isinstance(environment_metadata, dict) or
            any(not isinstance(key, str) or not isinstance(value, str)
                for key, value in environment_metadata.items())):
        raise ValueError("environment_metadata must map string keys to string values")
    protocol["environment_metadata"] = environment_metadata
    approval_status = protocol.get("approval_status", "unspecified")
    if not isinstance(approval_status, str) or approval_status not in {"unspecified", "provisional", "frozen"}:
        raise ValueError("approval_status must be 'unspecified', 'provisional', or 'frozen'")
    protocol["approval_status"] = approval_status
    missing = [field for field in REQUIRED_PROTOCOL_FIELDS if protocol.get(field) is None]
    if missing:
        raise ValueError("freeze these protocol fields before planning final runs: " + ", ".join(missing))
    protocol_id = protocol.get("protocol_id", "")
    if not isinstance(protocol_id, str) or not re.fullmatch(
            r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", protocol_id):
        raise ValueError("protocol_id must be 1-64 safe filename characters")

    if protocol.get("dataset_sizes") != list(MAIN_DATASET_SIZES):
        raise ValueError(f"dataset_sizes must be exactly {list(MAIN_DATASET_SIZES)} in that order")
    expected_profiles = list(HETEROGENEITY_PROFILES)
    if protocol.get("heterogeneity_profiles") != expected_profiles:
        raise ValueError(f"heterogeneity_profiles must be exactly {expected_profiles} in that order")
    if protocol.get("workers") != [1, 2, 4, 8]:
        raise ValueError("workers must be exactly [1, 2, 4, 8]")
    if protocol.get("policies") != list(POLICIES):
        raise ValueError(f"policies must be exactly {list(POLICIES)} in that order")

    protocol["repetitions"] = _integer(protocol["repetitions"], "repetitions", 2)
    protocol["duplicate_ratio"] = _ratio(protocol["duplicate_ratio"], "duplicate_ratio")
    if protocol["duplicate_ratio"] >= 1.0:
        raise ValueError("duplicate_ratio must be less than 1 so every workload has original content")
    ratios = protocol["recovery_damage_ratios"]
    if not isinstance(ratios, list) or not ratios:
        raise ValueError("recovery_damage_ratios must be a non-empty list")
    protocol["recovery_damage_ratios"] = [
        _ratio(value, "recovery_damage_ratios[]", positive=True) for value in ratios]
    if len(set(protocol["recovery_damage_ratios"])) != len(protocol["recovery_damage_ratios"]):
        raise ValueError("recovery_damage_ratios must not contain duplicates")
    kinds = protocol["damage_types"]
    if (not isinstance(kinds, list) or not kinds or
            any(not isinstance(kind, str) or kind not in DAMAGE_TYPES for kind in kinds) or
            len(set(kinds)) != len(kinds)):
        raise ValueError(f"damage_types must be a non-empty, unique list from {list(DAMAGE_TYPES)}")
    protocol["dataset_seed_base"] = _integer(
        protocol["dataset_seed_base"], "dataset_seed_base", 0)
    protocol["execution_order_seed_base"] = _integer(
        protocol["execution_order_seed_base"], "execution_order_seed_base", 0)
    protocol["recovery_damage_seed_base"] = _integer(
        protocol["recovery_damage_seed_base"], "recovery_damage_seed_base", 0)
    protocol["files_per_directory"] = _integer(
        protocol["files_per_directory"], "files_per_directory")
    for field in ("fixed_chunk_sizes", "adaptive_start_chunks"):
        values = protocol[field]
        if not isinstance(values, list) or not values:
            raise ValueError(f"{field} must be a non-empty list of positive integers")
        normalized = [_integer(value, f"{field}[]") for value in values]
        if len(set(normalized)) != len(normalized):
            raise ValueError(f"{field} must not contain duplicates")
        protocol[field] = sorted(normalized)
    if protocol["fixed_chunk_sizes"] != protocol["adaptive_start_chunks"]:
        raise ValueError("fixed_chunk_sizes and adaptive_start_chunks must match for a fair granularity ablation")
    protocol["adaptive_min_chunk"] = _integer(protocol["adaptive_min_chunk"], "adaptive_min_chunk")
    protocol["adaptive_max_chunk"] = _integer(protocol["adaptive_max_chunk"], "adaptive_max_chunk")
    if protocol["adaptive_max_chunk"] < protocol["adaptive_min_chunk"]:
        raise ValueError("adaptive_max_chunk must be >= adaptive_min_chunk")
    if any(not protocol["adaptive_min_chunk"] <= value <= protocol["adaptive_max_chunk"]
           for value in protocol["adaptive_start_chunks"]):
        raise ValueError("adaptive_start_chunks must fall within the adaptive chunk bounds")
    if not isinstance(protocol.get("engine", "build/ipbrs-backup.exe"), str):
        raise ValueError("engine must be a path string")
    engine = Path(protocol.get("engine", "build/ipbrs-backup.exe")).expanduser().resolve()
    if not engine.is_file():
        raise ValueError(f"engine executable does not exist: {engine}")
    protocol["engine"] = str(engine)
    return protocol


def _matrix_runs(protocol: dict[str, Any], dataset_id: str,
                 source: Path, manifest: Path, output_root: Path) -> list[dict[str, Any]]:
    settings = _matrix_settings(protocol)
    runs = []
    for policy, workers, chunk, min_chunk, max_chunk in settings:
        for repetition in range(1, protocol["repetitions"] + 1):
            experiment_id = (f"{dataset_id}-{policy}-w{workers}-c{chunk}-r{repetition}")
            runs.append({
                "id": experiment_id,
                "operation": "backup",
                "repetition": repetition,
                "workload_id": dataset_id,
                "source": str(source),
                "manifest": str(manifest),
                "backup": str(output_root / "backups" / dataset_id / experiment_id),
                "policy": policy,
                "workers": workers,
                "chunk": chunk,
                "min_chunk": min_chunk,
                "max_chunk": max_chunk,
            })
    return runs


def _matrix_settings(protocol: dict[str, Any]) -> list[tuple[str, int, int, int, int]]:
    settings: list[tuple[str, int, int, int, int]] = []
    for policy in POLICIES:
        for workers in protocol["workers"]:
            if policy == "sequential":
                if workers == 1:
                    settings.append((policy, workers, 1, 1, 1))
                continue
            if policy in {"dynamic", "stealing"}:
                for chunk in protocol["fixed_chunk_sizes"]:
                    settings.append((policy, workers, chunk, 1, 1))
            elif policy == "adaptive":
                for chunk in protocol["adaptive_start_chunks"]:
                    settings.append((policy, workers, chunk,
                                     protocol["adaptive_min_chunk"],
                                     protocol["adaptive_max_chunk"]))
            else:
                settings.append((policy, workers, 1, 1, 1))

    return settings


def audit_protocol(path: Path, output_path: Path | None = None,
                   cleanup_backups: bool = False,
                   staged_recovery: bool = False) -> dict[str, Any]:
    """Audit frozen choices and estimate matrix scale without writing files."""
    try:
        raw = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot read protocol {path}: {exc}") from exc
    if not isinstance(raw, dict) or raw.get("schema_version") != SCHEMA_VERSION:
        raise ValueError(f"protocol schema_version must be {SCHEMA_VERSION}")

    missing = [field for field in REQUIRED_PROTOCOL_FIELDS if raw.get(field) is None]
    approval_status = raw.get("approval_status")
    if approval_status is None:
        protocol_id = raw.get("protocol_id", "")
        approval_status = "provisional" if isinstance(protocol_id, str) and "provisional" in protocol_id.lower() else "unspecified"
    if not isinstance(approval_status, str) or approval_status not in {"unspecified", "provisional", "frozen"}:
        raise ValueError("approval_status must be 'unspecified', 'provisional', or 'frozen'")
    structurally_ready = not missing
    report: dict[str, Any] = {
        "protocol_id": raw.get("protocol_id"),
        "status": ("NEEDS_DECISIONS" if missing else
                   "READY" if approval_status == "frozen" else
                   "PROVISIONAL" if approval_status == "provisional" else "NOT_FROZEN"),
        "structural_status": "COMPLETE" if structurally_ready else "INCOMPLETE",
        "approval_status": approval_status.upper(),
        "unresolved_fields": missing,
        "fixed_dimensions": {
            "dataset_sizes": list(MAIN_DATASET_SIZES),
            "heterogeneity_profiles": list(HETEROGENEITY_PROFILES),
            "workers": [1, 2, 4, 8],
            "policies": list(POLICIES),
        },
        "dataset_count": len(MAIN_DATASET_SIZES) * len(HETEROGENEITY_PROFILES),
        "generated_file_count": sum(MAIN_DATASET_SIZES) * len(HETEROGENEITY_PROFILES),
    }
    if missing:
        report["matrix_scale"] = "run counts and storage estimates require frozen protocol choices"
        return report

    protocol = load_protocol(path)
    settings = _matrix_settings(protocol)
    dataset_count = len(MAIN_DATASET_SIZES) * len(HETEROGENEITY_PROFILES)
    runs_per_dataset = len(settings) * protocol["repetitions"]
    total_backup_runs = dataset_count * runs_per_dataset

    # Profiles use weighted, inclusive integer size ranges. This is the
    # expected payload size, not an exact pre-generation byte count.
    expected_source_bytes = 0.0
    largest_workload_bytes = 0.0
    profile_bytes: dict[str, float] = {}
    for profile_name, profile in HETEROGENEITY_PROFILES.items():
        size_profiles = profile["size_profiles"]
        weight_total = sum(item["weight"] for item in size_profiles)
        mean_file_bytes = sum(
            item["weight"] * (item["min_bytes"] + item["max_bytes"]) / 2
            for item in size_profiles
        ) / weight_total
        profile_bytes[profile_name] = mean_file_bytes
        for file_count in MAIN_DATASET_SIZES:
            # The generator samples sizes for original files, then selects an
            # existing original for each duplicate, preserving that file size.
            workload_bytes = file_count * mean_file_bytes
            expected_source_bytes += workload_bytes
            largest_workload_bytes = max(largest_workload_bytes, workload_bytes)

    expected_backup_bytes = expected_source_bytes * runs_per_dataset
    streamed_peak_bytes = expected_source_bytes + largest_workload_bytes
    recovery_fixture_count = (dataset_count * len(settings) * protocol["repetitions"] *
                              len(protocol["recovery_damage_ratios"]) *
                              len(protocol["damage_types"]))
    recovery_measurement_count = recovery_fixture_count * 2  # selective and full modes
    settings_by_policy = {
        policy: sum(1 for setting in settings if setting[0] == policy)
        for policy in POLICIES
    }
    recovery_runs_by_policy = {
        policy: (dataset_count * count * protocol["repetitions"] *
                 len(protocol["recovery_damage_ratios"]) *
                 len(protocol["damage_types"]) * 2)
        for policy, count in settings_by_policy.items()
    }
    # Every recovery measurement needs its own damaged source fixture so an
    # earlier restore cannot change the input seen by a later policy or mode.
    # This is a payload estimate; filesystem overhead and duplicate storage
    # optimizations are intentionally excluded.
    estimated_recovery_fixture_bytes = expected_source_bytes * (
        len(settings) * protocol["repetitions"] *
        len(protocol["recovery_damage_ratios"]) * len(protocol["damage_types"]) * 2)
    retained_matrix_payload_bytes = (expected_source_bytes + expected_backup_bytes +
                                     estimated_recovery_fixture_bytes)
    safety_margin_percent = protocol.get("storage_plan", {}).get("safety_margin_percent", 20)
    if (isinstance(safety_margin_percent, bool) or
            not isinstance(safety_margin_percent, int) or
            not 0 <= safety_margin_percent <= 1000):
        raise ValueError("storage_plan.safety_margin_percent must be an integer from 0 to 1000")
    # Staged recovery retains generated source workloads, one current-workload
    # reference snapshot, and one copied fixture at a time.
    staged_peak_bytes = expected_source_bytes + largest_workload_bytes * 2
    staged_peak_with_margin_bytes = math.ceil(
        staged_peak_bytes * (1 + safety_margin_percent / 100))
    report.update({
        "matrix_scale": {
            "settings_per_dataset_per_repetition": len(settings),
            "settings_per_policy": settings_by_policy,
            "repetitions": protocol["repetitions"],
            "runs_per_dataset": runs_per_dataset,
            "total_backup_runs": total_backup_runs,
            "total_recovery_runs_by_policy": recovery_runs_by_policy,
            "total_recovery_runs": sum(recovery_runs_by_policy.values()),
            "recovery_fixture_count": recovery_fixture_count,
            "recovery_measurement_count_if_each_fixture_runs_both_modes": recovery_measurement_count,
            "recovery_config_status": "counts and payload estimated; fixture and config materialization remains a separate step",
            "expected_source_payload_bytes": round(expected_source_bytes),
            "expected_retained_backup_payload_bytes": round(expected_backup_bytes),
            "expected_source_plus_backup_payload_bytes": round(expected_source_bytes + expected_backup_bytes),
            "estimated_recovery_fixture_payload_bytes": round(estimated_recovery_fixture_bytes),
            "estimated_retained_matrix_payload_bytes": round(retained_matrix_payload_bytes),
            "estimated_peak_payload_bytes_with_verified_backup_cleanup": round(streamed_peak_bytes),
            "estimated_staged_recovery_peak_payload_bytes": round(staged_peak_bytes),
            "estimated_staged_recovery_peak_with_safety_margin_bytes": staged_peak_with_margin_bytes,
            "staged_recovery_safety_margin_percent": safety_margin_percent,
            "staged_recovery_storage_note": (
                "Expected peak: all generated source workloads, one current-workload sequential reference "
                "snapshot, and one copied recovery fixture, plus the protocol safety margin. This requires "
                "fixture-at-a-time execution and must be rechecked against actual manifest bytes."
            ),
            "expected_mean_file_bytes_by_profile": {
                name: round(value, 2) for name, value in profile_bytes.items()
            },
            "storage_estimate_note": (
                "Expected retained payload if each backup run stores a full snapshot. This estimate uses "
                "weighted inclusive size ranges; duplicate files preserve source size. It excludes filesystem "
                "allocation, catalogs, logs, and temporary files. Actual source bytes are available from manifests."
            ),
        },
    })
    if output_path is not None:
        output_path = output_path.expanduser().resolve()
        probe = output_path
        while not probe.exists() and probe != probe.parent:
            probe = probe.parent
        if not probe.is_dir():
            raise ValueError(f"nearest existing output ancestor is not a directory: {probe}")
        free_bytes = shutil.disk_usage(probe).free
        if cleanup_backups and staged_recovery:
            raise ValueError("--cleanup-backups and --staged-recovery are mutually exclusive capacity plans")
        required_bytes = (staged_peak_with_margin_bytes if staged_recovery else
                          round(streamed_peak_bytes) if cleanup_backups else
                          retained_matrix_payload_bytes)
        report["capacity_check"] = {
            "output_path": str(output_path),
            "checked_existing_path": str(probe),
            "available_bytes": free_bytes,
            "estimated_required_payload_bytes": required_bytes,
            "payload_estimate_fits": free_bytes >= required_bytes,
            "backup_cleanup_mode": cleanup_backups,
            "staged_recovery_mode": staged_recovery,
            "note": ("Cleanup mode estimates a streamed workload plus one verification copy; it does not retain "
                     "backup snapshots or recovery fixtures for later experiments. "
                     "Excludes metadata, filesystem allocation, failed-run artifacts, other users, and safety margin."
                     if cleanup_backups else
                     "Staged mode assumes all workload sources, one verified sequential reference snapshot, and one "
                     "recovery fixture coexist, with the configured safety margin. It requires fixture-at-a-time "
                     "execution and excludes metadata, filesystem allocation, failed-run artifacts, and other users."
                     if staged_recovery else
                     "Includes all retained backup payload and one isolated source fixture per recovery measurement. "
                     "Advisory only; excludes metadata, filesystem allocation, other users, and safety margin."),
        }
    return report


def prepare(protocol: dict[str, Any], output_root: Path,
            selected_datasets: set[str] | None = None) -> dict[str, Any]:
    output_root = output_root.expanduser().resolve()
    if output_root.exists():
        raise FileExistsError(f"output directory already exists: {output_root}")
    workload_configs = output_root / "workload_configs"
    benchmark_configs = output_root / "benchmark_configs"
    workload_configs.mkdir(parents=True)
    benchmark_configs.mkdir()

    workload_index = 0
    total_runs = 0
    dataset_plans = []
    known_datasets: set[str] = set()
    for file_count in protocol["dataset_sizes"]:
        for heterogeneity in protocol["heterogeneity_profiles"]:
            dataset_seed = protocol["dataset_seed_base"] + workload_index
            order_seed = protocol["execution_order_seed_base"] + workload_index
            dataset_id = f"n{file_count}-{heterogeneity}-s{dataset_seed}"
            known_datasets.add(dataset_id)
            if selected_datasets is not None and dataset_id not in selected_datasets:
                workload_index += 1
                continue
            source = output_root / "workloads" / dataset_id
            manifest = output_root / "workloads" / f"{dataset_id}.manifest.json"
            workload_config = {
                "seed": dataset_seed,
                "file_count": file_count,
                "files_per_directory": protocol["files_per_directory"],
                "duplicate_ratio": protocol["duplicate_ratio"],
                "heterogeneity": heterogeneity,
            }
            workload_path = workload_configs / f"{dataset_id}.json"
            workload_path.write_text(json.dumps(workload_config, indent=2) + "\n", encoding="utf-8")
            runs = _matrix_runs(protocol, dataset_id, source, manifest, output_root)
            benchmark_config = {
                "schema_version": 1,
                "engine": protocol["engine"],
                "execution_order_seed": order_seed,
                "environment_metadata": protocol.get("environment_metadata", {}),
                "runs": runs,
            }
            benchmark_path = benchmark_configs / f"{dataset_id}.json"
            benchmark_path.write_text(json.dumps(benchmark_config, indent=2) + "\n", encoding="utf-8")
            total_runs += len(runs)
            dataset_plans.append({
                "dataset_id": dataset_id,
                "file_count": file_count,
                "heterogeneity": heterogeneity,
                "dataset_seed": dataset_seed,
                "execution_order_seed": order_seed,
                "workload_config": str(workload_path),
                "source": str(source),
                "manifest": str(manifest),
                "benchmark_config": str(benchmark_path),
                "runs": len(runs),
            })
            workload_index += 1

    if selected_datasets is not None:
        unknown_datasets = selected_datasets - known_datasets
        if unknown_datasets:
            raise ValueError("unknown dataset IDs: " + ", ".join(sorted(unknown_datasets)))
        if not dataset_plans:
            raise ValueError("select at least one dataset ID")

    plan = {
        "schema_version": SCHEMA_VERSION,
        "protocol_id": protocol["protocol_id"],
        "protocol": protocol,
        "dataset_count": len(dataset_plans),
        "runs_per_dataset": dataset_plans[0]["runs"] if dataset_plans else 0,
        "total_backup_runs": total_runs,
        "datasets": dataset_plans,
        "execution": [
            "Generate each workload with its listed workload_config and source path.",
            "Validate each benchmark config with src/benchmark.py after its dataset manifest exists.",
            "Run each benchmark config to a separate results directory; configs do not generate or damage data.",
            "Prepare isolated recovery fixtures only after the backup matrix completes.",
        ],
        "recovery_matrix": {
            "damage_ratios": protocol["recovery_damage_ratios"],
            "damage_types": protocol["damage_types"],
            "modes": ["selective", "full"],
            "policies": list(POLICIES),
            "worker_counts": protocol["workers"],
            "fixed_chunk_sizes": protocol["fixed_chunk_sizes"],
            "adaptive_start_chunks": protocol["adaptive_start_chunks"],
            "adaptive_chunk_bounds": [protocol["adaptive_min_chunk"], protocol["adaptive_max_chunk"]],
            "repetitions": protocol["repetitions"],
            "requires_isolated_fixture_per_policy_worker_ratio_type_repetition": True,
            "catalogs_and_fixtures": "materialize after successful backup runs; config generation is a separate step",
        },
    }
    (output_root / "plan.json").write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")
    return plan


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--protocol", required=True, type=Path)
    parser.add_argument("--check-space", type=Path, metavar="OUTPUT_PATH",
                        help="include free space on the nearest existing ancestor of OUTPUT_PATH")
    parser.add_argument("--cleanup-backups", action="store_true",
                        help="estimate storage assuming verified backup outputs are deleted after each run")
    parser.add_argument("--staged-recovery", action="store_true",
                        help="estimate one workload/reference/fixture at a time with the protocol safety margin")
    parser.add_argument("--allow-provisional", action="store_true",
                        help="materialize a clearly labeled provisional plan without marking it frozen")
    parser.add_argument("--dataset", action="append", dest="datasets",
                        help="include only this dataset ID (repeat for multiple IDs)")
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--audit", action="store_true",
                         help="validate choices and report matrix/run/storage scale without writing files")
    action.add_argument("--output", type=Path,
                         help="directory for generated workload and benchmark configurations")
    args = parser.parse_args()
    try:
        if args.audit:
            print(json.dumps(audit_protocol(args.protocol, args.check_space,
                                            args.cleanup_backups,
                                            args.staged_recovery), indent=2))
            return 0
        if args.output is None:
            parser.error("provide either --audit or --output")
        protocol = load_protocol(args.protocol)
        if protocol["approval_status"] != "frozen" and not args.allow_provisional:
            raise ValueError(
                "final matrix configuration requires approval_status='frozen'; "
                "review and freeze protocol choices before materializing the matrix; "
                "--allow-provisional can be used to prepare a plan that remains labeled provisional")
        if args.check_space is not None or args.cleanup_backups or args.staged_recovery:
            parser.error("--check-space, --cleanup-backups, and --staged-recovery are available with --audit only")
        plan = prepare(protocol, args.output,
                       set(args.datasets) if args.datasets else None)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print(json.dumps({"output": str(args.output.resolve()),
                      "dataset_count": plan["dataset_count"],
                      "runs_per_dataset": plan["runs_per_dataset"],
                      "total_backup_runs": plan["total_backup_runs"]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
