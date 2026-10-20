#!/usr/bin/env python3
"""Run reproducible IPBRS CLI experiments and summarize their raw JSONL records.

Workload generation and damage injection are intentionally separate commands;
this runner operates only on prepared datasets and records their manifests.
"""
from __future__ import annotations

import argparse
import csv
import ctypes
import hashlib
import json
import math
import os
import platform
import random
import shutil
import subprocess
import statistics
import sys
import time
import uuid
from pathlib import Path
from typing import Any

SCHEMA_VERSION = 1
POLICIES = {"sequential", "static", "dynamic", "stealing", "adaptive"}


def _load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot read JSON file {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return value


def _positive_int(value: Any, name: str, *, allow_zero: bool = False) -> int:
    minimum = 0 if allow_zero else 1
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise ValueError(f"{name} must be an integer >= {minimum}")
    return value


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def validate_config(path: Path) -> dict[str, Any]:
    config = _load_json(path)
    if config.get("schema_version") != SCHEMA_VERSION:
        raise ValueError(f"config schema_version must be {SCHEMA_VERSION}")
    environment_metadata = config.get("environment_metadata", {})
    if (not isinstance(environment_metadata, dict) or
            any(not isinstance(key, str) or not isinstance(value, str)
                for key, value in environment_metadata.items())):
        raise ValueError("environment_metadata must map string keys to string values")
    config["environment_metadata"] = environment_metadata
    timeout_seconds = config.get("timeout_seconds")
    if timeout_seconds is not None:
        if (isinstance(timeout_seconds, bool) or
                not isinstance(timeout_seconds, (int, float)) or
                not math.isfinite(timeout_seconds) or timeout_seconds <= 0):
            raise ValueError("timeout_seconds must be a positive number")
        config["timeout_seconds"] = float(timeout_seconds)
    if "execution_order_seed" in config:
        config["execution_order_seed"] = _positive_int(
            config["execution_order_seed"], "execution_order_seed", allow_zero=True)
    engine = Path(config.get("engine", "build/ipbrs-backup.exe")).resolve()
    runs = config.get("runs")
    if not isinstance(runs, list) or not runs:
        raise ValueError("runs must be a non-empty array")
    seen: set[str] = set()
    normalized = []
    for index, raw in enumerate(runs):
        if not isinstance(raw, dict):
            raise ValueError(f"runs[{index}] must be an object")
        item = dict(raw)
        item_timeout = item.get("timeout_seconds", config.get("timeout_seconds"))
        if item_timeout is not None:
            if (isinstance(item_timeout, bool) or
                    not isinstance(item_timeout, (int, float)) or
                    not math.isfinite(item_timeout) or item_timeout <= 0):
                raise ValueError(f"runs[{index}].timeout_seconds must be a positive number")
            item["timeout_seconds"] = float(item_timeout)
        item["engine_kind"] = item.get("engine_kind", "cpp")
        if item["engine_kind"] not in {"cpp", "python_reference"}:
            raise ValueError(f"runs[{index}].engine_kind must be cpp or python_reference")
        if item["engine_kind"] == "cpp" and not engine.is_file():
            raise ValueError(f"engine executable does not exist: {engine}")
        run_id = item.get("id")
        if not isinstance(run_id, str) or not run_id or run_id in seen:
            raise ValueError(f"runs[{index}].id must be unique and non-empty")
        seen.add(run_id)
        operation = item.get("operation", "backup")
        if operation not in {"backup", "recover"}:
            raise ValueError(f"runs[{index}].operation must be backup or recover")
        item["operation"] = operation
        if item["engine_kind"] == "python_reference" and operation != "recover":
            raise ValueError("engine_kind=python_reference currently supports recovery only")
        mode = item.get("recovery_mode", "selective")
        if mode not in {"selective", "full"}:
            raise ValueError(f"runs[{index}].recovery_mode must be selective or full")
        item["recovery_mode"] = mode
        item["repetition"] = _positive_int(item.get("repetition", 1), f"runs[{index}].repetition")
        policy = item.get("policy", "sequential")
        if policy not in POLICIES:
            raise ValueError(f"runs[{index}].policy must be one of {sorted(POLICIES)}")
        item["policy"] = policy
        item["workers"] = _positive_int(item.get("workers", 1), f"runs[{index}].workers")
        item["chunk"] = _positive_int(item.get("chunk", 1), f"runs[{index}].chunk")
        item["min_chunk"] = _positive_int(item.get("min_chunk", 1), f"runs[{index}].min_chunk")
        item["max_chunk"] = _positive_int(item.get("max_chunk", 64), f"runs[{index}].max_chunk")
        if item["max_chunk"] < item["min_chunk"]:
            raise ValueError(f"runs[{index}].max_chunk must be >= min_chunk")
        if policy == "sequential" and item["workers"] != 1:
            raise ValueError("sequential runs must use workers=1")
        if item["engine_kind"] == "python_reference" and (operation != "recover" or policy != "sequential" or item["workers"] != 1):
            raise ValueError("python_reference supports sequential recovery only")
        for field in ("source", "backup"):
            if not isinstance(item.get(field), str) or not item[field]:
                raise ValueError(f"runs[{index}].{field} must be a path string")
            configured_path = Path(item[field])
            if field == "backup":
                item["_backup_path_was_symlink"] = configured_path.is_symlink()
            item[field] = str(configured_path.resolve())
        source, backup = Path(item["source"]), Path(item["backup"])
        if operation == "backup" and not source.is_dir():
            raise ValueError(f"source directory does not exist: {source}")
        if source == backup or source in backup.parents or backup in source.parents:
            raise ValueError("source and backup directories must not overlap")
        manifest = item.get("manifest")
        if manifest:
            manifest_path = Path(manifest).resolve()
            metadata = _load_json(manifest_path)
            item["manifest"] = str(manifest_path)
            item["manifest_sha256"] = _sha256(manifest_path)
            item["workload_id"] = item.get("workload_id") or metadata.get("workload_id") or _sha256(manifest_path)
            dataset_config = metadata.get("config", {})
            item["dataset_metadata"] = {
                "seed": metadata.get("seed"),
                "file_count": metadata.get("file_count"),
                "total_bytes": metadata.get("total_bytes"),
                "heterogeneity": dataset_config.get("heterogeneity"),
                "duplicate_ratio": dataset_config.get("duplicate_ratio"),
                "size_profile_counts": metadata.get("size_profile_counts", {}),
                "type_counts": metadata.get("type_counts", {}),
            }
        else:
            item["manifest"] = None
            item["workload_id"] = item.get("workload_id", source.name)
            item["dataset_metadata"] = None
        if operation == "recover":
            if not source.is_dir():
                raise ValueError(f"recovery source must be an existing prepared damaged fixture: {source}")
            damage_ratio = item.get("damage_ratio")
            damage_types = item.get("damage_types")
            if damage_ratio is not None:
                if (isinstance(damage_ratio, bool) or not isinstance(damage_ratio, (int, float)) or
                        not math.isfinite(damage_ratio) or not 0 <= damage_ratio <= 1):
                    raise ValueError(f"runs[{index}].damage_ratio must be a finite number from 0 to 1")
                item["damage_ratio"] = float(damage_ratio)
            damage_seed = item.get("damage_seed")
            if damage_seed is not None:
                item["damage_seed"] = _positive_int(
                    damage_seed, f"runs[{index}].damage_seed", allow_zero=True)
            if damage_types is not None:
                allowed_damage_types = {"missing", "modified", "corrupted", "truncated"}
                if (not isinstance(damage_types, list) or not damage_types or
                        any(not isinstance(kind, str) or kind not in allowed_damage_types
                            for kind in damage_types) or len(set(damage_types)) != len(damage_types)):
                    raise ValueError(f"runs[{index}].damage_types must be a non-empty unique supported list")
            damage_log = item.get("damage_log")
            if damage_log is not None:
                if not isinstance(damage_log, str) or not damage_log:
                    raise ValueError(f"runs[{index}].damage_log must be a path string")
                damage_log_path = Path(damage_log).resolve()
                if not damage_log_path.is_file() or item.get("damage_ratio") is None or damage_types is None:
                    raise ValueError(f"runs[{index}].damage_log needs an existing file, ratio, and types")
                damage_data = _load_json(damage_log_path)
                logged_ratio = damage_data.get("requested_ratio")
                changes = damage_data.get("changes")
                if (damage_data.get("status") != "COMPLETE" or
                        (damage_seed is not None and damage_data.get("seed") != damage_seed) or
                        isinstance(logged_ratio, bool) or not isinstance(logged_ratio, (int, float)) or
                        not math.isfinite(logged_ratio) or
                        not math.isclose(logged_ratio, item["damage_ratio"], rel_tol=0, abs_tol=1e-12) or
                        not isinstance(changes, list) or
                        any(not isinstance(change, dict) or
                            change.get("damage_type") not in damage_types for change in changes)):
                    raise ValueError(f"runs[{index}].damage_log does not match its damage settings")
                item["damage_log"] = str(damage_log_path)
                item["damage_log_sha256"] = _sha256(damage_log_path)
            if item["engine_kind"] == "python_reference" and not item.get("verification_backup_id"):
                raise ValueError(f"runs[{index}].verification_backup_id is required for Python reference recovery")
            catalog = item.get("catalog")
            if not isinstance(catalog, str) or not Path(catalog).is_file():
                raise ValueError(f"runs[{index}].catalog must reference an existing catalog")
            item["catalog"] = str(Path(catalog).resolve())
            item["catalog_sha256"] = _sha256(Path(item["catalog"]))
            default_format = "python_sqlite" if item["engine_kind"] == "python_reference" else "cpp_json"
            catalog_format = item.get("catalog_format", default_format)
            if catalog_format not in {"cpp_json", "python_sqlite"}:
                raise ValueError(f"runs[{index}].catalog_format must be cpp_json or python_sqlite")
            item["catalog_format"] = catalog_format
            if mode == "full":
                item["full"] = True
            else:
                item["full"] = False
            if catalog_format == "python_sqlite":
                if item["engine_kind"] != "python_reference":
                    raise ValueError("python_sqlite catalogs require engine_kind=python_reference")
                reference = item.get("reference_script", "src/ipbrs_reference.py")
                reference_path = Path(reference).resolve()
                if not reference_path.is_file():
                    raise ValueError(f"reference_script does not exist: {reference_path}")
                item["reference_script"] = str(reference_path)
                item["verification_backup_id"] = item.get("verification_backup_id")
            elif item["engine_kind"] != "cpp":
                raise ValueError("cpp_json catalogs require engine_kind=cpp")
        normalized.append(item)
    config["engine"] = str(engine)
    config["runs"] = normalized
    return config


def _open_process_cpu_handle(pid: int) -> Any:
    if os.name != "nt":
        return None
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel32.OpenProcess.restype = wintypes.HANDLE
    return kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION


def _process_cpu_time_ns(handle: Any) -> int | None:
    if os.name == "nt":
        if not handle:
            return None
        from ctypes import wintypes

        class FILETIME(ctypes.Structure):
            _fields_ = [("dwLowDateTime", wintypes.DWORD), ("dwHighDateTime", wintypes.DWORD)]

        creation, exit_time, kernel, user = FILETIME(), FILETIME(), FILETIME(), FILETIME()
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.GetProcessTimes.argtypes = (
            wintypes.HANDLE, ctypes.POINTER(FILETIME), ctypes.POINTER(FILETIME),
            ctypes.POINTER(FILETIME), ctypes.POINTER(FILETIME))
        kernel32.GetProcessTimes.restype = wintypes.BOOL
        if not kernel32.GetProcessTimes(handle, ctypes.byref(creation), ctypes.byref(exit_time),
                                        ctypes.byref(kernel), ctypes.byref(user)):
            return None
        ticks = lambda value: (value.dwHighDateTime << 32) | value.dwLowDateTime
        return (ticks(kernel) + ticks(user)) * 100
    try:
        import resource
        if handle is None:
            return None
        return int((handle.ru_utime + handle.ru_stime) * 1e9)
    except (ImportError, AttributeError, OSError):
        return None


def _close_process_cpu_handle(handle: Any) -> None:
    if os.name == "nt" and handle:
        from ctypes import wintypes
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
        kernel32.CloseHandle.restype = wintypes.BOOL
        kernel32.CloseHandle(handle)


def _require_damaged_fixture(item: dict[str, Any]) -> None:
    """Reject recovery retries whose fixture has already been repaired or moved."""
    damage_log = item.get("damage_log")
    if not damage_log:
        return
    if item.get("damage_log_sha256") != _sha256(Path(damage_log)):
        raise ValueError(f"recovery damage log changed after config validation: {damage_log}")
    manifest = item.get("manifest")
    if manifest and item.get("manifest_sha256") != _sha256(Path(manifest)):
        raise ValueError(f"recovery manifest changed after config validation: {manifest}")
    catalog = item.get("catalog")
    if catalog and item.get("catalog_sha256") != _sha256(Path(catalog)):
        raise ValueError(f"recovery catalog changed after config validation: {catalog}")
    document = _load_json(Path(damage_log))
    source = Path(item["source"]).resolve()
    changes = document.get("changes")
    if document.get("status") != "COMPLETE" or not isinstance(changes, list):
        raise ValueError(f"recovery damage log is incomplete: {damage_log}")
    for change in changes:
        if not isinstance(change, dict) or not isinstance(change.get("relative_path"), str):
            raise ValueError(f"recovery damage log contains an invalid path: {damage_log}")
        target = (source / Path(change["relative_path"])).resolve()
        if target == source or source not in target.parents:
            raise ValueError(f"damage log path escapes the recovery source: {change['relative_path']}")
        if change.get("damage_type") == "missing":
            if target.exists():
                raise ValueError(f"missing-file fixture was already restored: {target}")
        else:
            if not target.is_file():
                raise ValueError(f"damaged fixture file is missing or inaccessible: {target}")
            digest = _sha256(target)
            if digest == change.get("original_sha256"):
                raise ValueError(f"damaged fixture was already restored: {target}")


def _invoke(engine: str, args: list[str], timeout_seconds: float | None = None
            ) -> tuple[dict[str, Any] | None, str, int, int, int | None]:
    try:
        import resource
        child_cpu_before = resource.getrusage(resource.RUSAGE_CHILDREN) if os.name != "nt" else None
    except (ImportError, AttributeError, OSError):
        child_cpu_before = None
    started = time.perf_counter_ns()
    with subprocess.Popen([engine, *args], text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE) as proc:
        cpu_handle = _open_process_cpu_handle(proc.pid)
        timed_out = False
        try:
            stdout, stderr = proc.communicate(timeout=timeout_seconds)
        except subprocess.TimeoutExpired:
            timed_out = True
            proc.kill()
            stdout, stderr = proc.communicate()
        exit_code = proc.returncode
        process_cpu_ns = _process_cpu_time_ns(cpu_handle)
        _close_process_cpu_handle(cpu_handle)
    elapsed = time.perf_counter_ns() - started
    if timed_out:
        if isinstance(stderr, bytes):
            stderr = stderr.decode("utf-8", errors="replace")
        if isinstance(stdout, bytes):
            stdout = stdout.decode("utf-8", errors="replace")
        stderr = (stderr + f"\nbenchmark timeout after {timeout_seconds:g} seconds")[-4000:]
        exit_code = 124
    if process_cpu_ns is None and child_cpu_before is not None:
        try:
            import resource
            child_cpu_after = resource.getrusage(resource.RUSAGE_CHILDREN)
            process_cpu_ns = int(((child_cpu_after.ru_utime - child_cpu_before.ru_utime) +
                                  (child_cpu_after.ru_stime - child_cpu_before.ru_stime)) * 1e9)
        except (ImportError, AttributeError, OSError):
            process_cpu_ns = None
    try:
        payload = json.loads(stdout) if stdout.strip() else None
    except json.JSONDecodeError:
        payload = None
    if payload is None:
        payload = {}
    return payload, stderr[-4000:], exit_code, elapsed, process_cpu_ns


def _config_fingerprint(config: dict[str, Any], item: dict[str, Any]) -> str:
    fingerprint_data = {"engine": config["engine"], "run": item}
    if config.get("environment_metadata"):
        fingerprint_data["environment_metadata"] = config["environment_metadata"]
    canonical = json.dumps(fingerprint_data, sort_keys=True,
                           separators=(",", ":"), ensure_ascii=False)
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def _record(config: dict[str, Any], item: dict[str, Any], payload: dict[str, Any] | None,
            stderr: str, exit_code: int, wall_ns: int, process_cpu_ns: int | None,
            run_id: str) -> dict[str, Any]:
    payload = payload or {}
    cpp_tasks = payload.get("tasks") if isinstance(payload.get("tasks"), list) else []
    py_results = payload.get("results") if isinstance(payload.get("results"), list) else []
    tasks = cpp_tasks or py_results
    python_reference = item["engine_kind"] == "python_reference"
    task_success = lambda row: row.get("success", False) if "success" in row else row.get("status") in {"SUCCESS", "SKIPPED"}
    failures = sum(1 for task in tasks if not task_success(task))
    reported_failures = payload.get("files_failed")
    if isinstance(reported_failures, int) and not isinstance(reported_failures, bool):
        failures = max(failures, reported_failures)
    workers = payload.get("workers_used", item["workers"] if python_reference else 0)
    if workers is None:
        workers = item["workers"] if python_reference else 0
    scheduler_elapsed = payload.get(
        "elapsed_ns", int(payload.get("elapsed_seconds", wall_ns / 1e9) * 1e9))
    elapsed = payload.get("total_elapsed_ns", scheduler_elapsed)
    bytes_done = payload.get("bytes_restored", payload.get("bytes_backed_up",
                          sum(int(t.get("bytes", t.get("bytes_processed", 0))) for t in tasks if task_success(t))))
    files_ok = payload.get("files_restored", payload.get("files_succeeded",
                       sum(1 for t in tasks if task_success(t))))
    files_avoided = payload.get("files_avoided", payload.get("files_skipped",
                            payload.get("unchanged_skipped", 0)))
    bytes_avoided = payload.get("bytes_avoided", 0)
    if item["operation"] == "recover":
        restore_tasks = payload.get("restore_tasks")
        if not isinstance(restore_tasks, int):
            restore_tasks = max(0, len(tasks) - int(files_avoided or 0))
        recovery_success_rate = (files_ok / restore_tasks) if restore_tasks else (1.0 if exit_code == 0 else 0.0)
    else:
        recovery_success_rate = None
    engine_success = payload.get("success", payload.get("status") == "COMPLETE")
    correctness = bool(engine_success) and exit_code == 0 and failures == 0
    if item["operation"] == "backup":
        correctness = correctness and bool(payload.get("catalog_path"))
    worker_metrics = payload.get("workers", []) if isinstance(payload.get("workers"), list) else []
    busy_ns = [int(worker.get("busy_ns", 0)) for worker in worker_metrics]
    workers_used = int(workers or 0)
    aggregate_worker_capacity_ns = int(scheduler_elapsed * workers_used)
    aggregate_busy_ns = sum(busy_ns)
    idle_ns = max(0, aggregate_worker_capacity_ns - aggregate_busy_ns)
    return {
        "schema_version": SCHEMA_VERSION,
        "run_id": run_id,
        "experiment_id": item["id"],
        "repetition": item["repetition"],
        "execution_order_seed": config.get("execution_order_seed"),
        "operation": item["operation"],
        "workload_id": item["workload_id"],
        "dataset": item.get("dataset_metadata"),
        "policy": item["policy"],
        "workers": item["workers"],
        "chunk": item["chunk"],
        "config": {"damage_ratio": item.get("damage_ratio"), "damage_types": item.get("damage_types"),
                   "recovery_mode": item.get("recovery_mode", "selective"),
                   "heterogeneity": (item.get("dataset_metadata") or {}).get("heterogeneity"),
                   "duplicate_ratio": (item.get("dataset_metadata") or {}).get("duplicate_ratio"),
                   "min_chunk": item["min_chunk"] if item["policy"] == "adaptive" else None,
                   "max_chunk": item["max_chunk"] if item["policy"] == "adaptive" else None,
                   "engine_kind": item["engine_kind"],
                   "environment_metadata": config.get("environment_metadata", {})},
        "input_artifacts": {
            "manifest": item.get("manifest"), "manifest_sha256": item.get("manifest_sha256"),
            "catalog": item.get("catalog"), "catalog_sha256": item.get("catalog_sha256"),
            "damage_log": item.get("damage_log"),
            "damage_log_sha256": item.get("damage_log_sha256"),
            "damage_seed": item.get("damage_seed"), "fixture_id": item.get("fixture_id"),
        },
        "config_fingerprint": _config_fingerprint(config, item),
        "environment": {"os": platform.platform(), "python": platform.python_version(),
                        "machine": platform.machine(), "processor": platform.processor(),
                        "engine_build": payload.get("build"),
                        "experiment_conditions": config.get("environment_metadata", {})},
        "measurement_notes": {
            "engine_elapsed_ns": "End-to-end operation time from engine entry to return; includes discovery/classification and catalog work, excludes the benchmark runner's independent verification.",
            "scheduler_elapsed_ns": "Scheduler execution interval reported by the engine; used as the worker-capacity basis.",
            "worker_idle_ns": "Estimated as scheduler elapsed time minus accumulated callback busy time; includes claim/coordination gaps.",
            "process_cpu_utilization": "Child process user plus kernel CPU time divided by subprocess wall time; percent of one logical CPU and may exceed 100%."
        },
        "timings": {"wall_ns": wall_ns, "engine_elapsed_ns": elapsed,
                    "scheduler_elapsed_ns": scheduler_elapsed,
                    "discovery_elapsed_ns": payload.get("discovery_elapsed_ns"),
                    "process_cpu_ns": process_cpu_ns,
                    "scheduling_overhead_ns": payload.get("scheduling_overhead_ns"),
                    "worker_capacity_ns": aggregate_worker_capacity_ns,
                    "worker_busy_ns": aggregate_busy_ns,
                    "worker_idle_ns": idle_ns,
                    "worker_idle_fraction": (idle_ns / aggregate_worker_capacity_ns)
                    if aggregate_worker_capacity_ns else None},
        "metrics": {"workers_used": workers, "files": len(tasks), "files_succeeded": files_ok,
                    "files_failed": failures, "bytes_processed": bytes_done,
                    "files_missing": payload.get("files_missing", 0),
                    "files_modified": payload.get("files_modified", 0),
                    "files_inaccessible": payload.get("files_inaccessible", 0),
                    "files_extra": payload.get("files_extra", 0),
                    "files_avoided": files_avoided, "bytes_avoided": bytes_avoided,
                    "recovery_success_rate": recovery_success_rate,
                    "steal_attempts": payload.get("steal_attempts", 0),
                    "steals_succeeded": payload.get("steals_succeeded", 0),
                    "estimated_cost_cv": payload.get("estimated_cost_cv"),
                    "initial_adaptive_chunk": payload.get("initial_adaptive_chunk"),
                    "granularity_decisions": payload.get("granularity_decisions", []),
                    "worker_busy_ns": busy_ns,
                    "worker_idle_ns": [max(0, scheduler_elapsed - busy) for busy in busy_ns],
                    "process_cpu_utilization_percent":
                    (process_cpu_ns * 100.0 / wall_ns) if process_cpu_ns is not None and wall_ns else None,
                    "discovery_workers_used": payload.get("discovery_workers_used"),
                    "independent_verification_workers_used": None,
                    "worker_metrics": worker_metrics},
        "correctness": {"passed": correctness, "engine_success": bool(engine_success),
                        "exit_code": exit_code, "error": payload.get("error") or stderr,
                        "failed_tasks": failures, "independent_verification": None},
        "results": {"catalog_path": payload.get("catalog_path"),
                    "backup_id": payload.get("backup_id")},
        "raw_engine_result": payload,
    }


def _summary(records: list[dict[str, Any]], output: Path) -> None:
    groups: dict[tuple[Any, ...], list[dict[str, Any]]] = {}
    for row in records:
        experiment_config = json.dumps(row.get("config", {}), sort_keys=True, separators=(",", ":"))
        key = (row["operation"], row["workload_id"], row["policy"], row["workers"], row["chunk"], experiment_config)
        groups.setdefault(key, []).append(row)
    summary_rows = []
    for key, items in sorted(groups.items()):
        valid = [x for x in items
                 if isinstance(x.get("timings", {}).get("engine_elapsed_ns"), (int, float))
                 and x.get("correctness", {}).get("passed") is True]
        if not valid:
            summary_rows.append({
                "operation": key[0], "workload_id": key[1], "policy": key[2],
                "workers": key[3], "chunk": key[4], "repetitions": 0,
                "attempted_runs": len(items), "failed_runs": len(items), "successes": 0,
                "experiment_config": json.loads(key[5]), "mean_seconds": None,
                "stdev_seconds": None, "throughput_bytes_per_second": None,
                "speedup_vs_sequential": None, "parallel_efficiency": None,
                "failure_messages": [
                    x.get("correctness", {}).get("error") or "run did not pass correctness"
                    for x in items],
            })
            continue
        measured = [x for x in items
                    if isinstance(x.get("timings", {}).get("engine_elapsed_ns"), (int, float))]
        times = [x["timings"]["engine_elapsed_ns"] / 1e9 for x in valid]
        mean = statistics.mean(times)
        imbalance_values = []
        for record in valid:
            busy = [value for value in record.get("metrics", {}).get("worker_busy_ns", [])
                    if isinstance(value, (int, float)) and value >= 0]
            if busy and statistics.mean(busy) > 0:
                imbalance_values.append(max(busy) / statistics.mean(busy) - 1.0)
        idle_fractions = [value for value in
                          (x.get("timings", {}).get("worker_idle_fraction") for x in valid)
                          if isinstance(value, (int, float))]
        process_cpu_values = [value for value in
                              (x.get("metrics", {}).get("process_cpu_utilization_percent") for x in valid)
                              if isinstance(value, (int, float))]
        verification_worker_values = [value for value in
                                      (x.get("metrics", {}).get("independent_verification_workers_used")
                                       for x in valid)
                                      if isinstance(value, (int, float))]
        discovery_worker_values = [value for value in
                                   (x.get("metrics", {}).get("discovery_workers_used") for x in valid)
                                   if isinstance(value, (int, float))]
        discovery_seconds = [value / 1e9 for value in
                             (x.get("timings", {}).get("discovery_elapsed_ns") for x in valid)
                             if isinstance(value, (int, float)) and value >= 0]
        missing_counts = [value for value in
                          (x.get("metrics", {}).get("files_missing") for x in measured)
                          if isinstance(value, (int, float)) and value >= 0]
        modified_counts = [value for value in
                           (x.get("metrics", {}).get("files_modified") for x in measured)
                           if isinstance(value, (int, float)) and value >= 0]
        inaccessible_counts = [value for value in
                               (x.get("metrics", {}).get("files_inaccessible") for x in measured)
                               if isinstance(value, (int, float)) and value >= 0]
        extra_counts = [value for value in
                        (x.get("metrics", {}).get("files_extra") for x in measured)
                        if isinstance(value, (int, float)) and value >= 0]
        def verifier_count(name: str) -> list[int | float]:
            return [value for value in
                    (x.get("correctness", {}).get("verification_counts", {}).get(name)
                     for x in measured)
                    if isinstance(value, (int, float)) and value >= 0]
        verifier_missing = verifier_count("missing")
        verifier_size_mismatch = verifier_count("size_mismatch")
        verifier_timestamp_mismatch = verifier_count("timestamp_mismatch")
        verifier_digest_mismatch = verifier_count("digest_mismatch")
        verifier_inaccessible = verifier_count("inaccessible")
        verifier_unsafe = verifier_count("unsafe")
        recovery_rates = [value for value in
                          (x.get("metrics", {}).get("recovery_success_rate") for x in valid)
                          if isinstance(value, (int, float))]
        workers_used_values = [value for value in
                               (x.get("metrics", {}).get("workers_used") for x in valid)
                               if isinstance(value, (int, float)) and value >= 0]
        def record_metric(record: dict[str, Any], name: str, fallback: Any = 0) -> Any:
            metrics = record.get("metrics", {})
            if name in metrics:
                return metrics[name]
            payload = record.get("raw_engine_result", {})
            aliases = {"files_avoided": ("files_avoided", "files_skipped", "unchanged_skipped"),
                       "bytes_avoided": ("bytes_avoided",),
                       "recovery_success_rate": ()}
            for key_name in aliases.get(name, (name,)):
                if key_name in payload:
                    if name == "recovery_success_rate":
                        continue
                    return payload[key_name]
            if name == "recovery_success_rate" and record.get("operation") == "recover":
                restore_tasks = payload.get("restore_tasks", 0)
                restored = payload.get("files_restored", 0)
                return restored / restore_tasks if restore_tasks else (
                    1.0 if record.get("correctness", {}).get("passed") else 0.0)
            return fallback
        summary_rows.append({"operation": key[0], "workload_id": key[1], "policy": key[2],
                             "workers": key[3], "chunk": key[4], "repetitions": len(valid),
                             "attempted_runs": len(items), "failed_runs": len(items) - len(valid),
                             "experiment_config": json.loads(key[5]),
                             "successes": sum(x["correctness"]["passed"] for x in valid),
                             "mean_seconds": mean,
                             "stdev_seconds": statistics.stdev(times) if len(times) > 1 else 0.0,
                             "throughput_bytes_per_second": statistics.mean(
                                 [x["metrics"]["bytes_processed"] / (x["timings"]["engine_elapsed_ns"] / 1e9)
                                  if x["timings"]["engine_elapsed_ns"] else 0 for x in valid]),
                             "mean_steal_attempts": statistics.mean(x["metrics"]["steal_attempts"] for x in valid),
                             "mean_steals_succeeded": statistics.mean(x["metrics"]["steals_succeeded"] for x in valid),
                             "mean_worker_idle_fraction": statistics.mean(idle_fractions) if idle_fractions else None,
                             "mean_process_cpu_utilization_percent": statistics.mean(process_cpu_values)
                             if process_cpu_values else None,
                             "mean_independent_verification_workers_used":
                             statistics.mean(verification_worker_values) if verification_worker_values else None,
                             "mean_discovery_workers_used": statistics.mean(discovery_worker_values)
                             if discovery_worker_values else None,
                             "mean_discovery_seconds": statistics.mean(discovery_seconds)
                             if discovery_seconds else None,
                             "mean_files_missing": statistics.mean(missing_counts)
                             if missing_counts else None,
                             "mean_files_modified": statistics.mean(modified_counts)
                             if modified_counts else None,
                             "mean_files_inaccessible": statistics.mean(inaccessible_counts)
                             if inaccessible_counts else None,
                             "mean_files_extra": statistics.mean(extra_counts)
                             if extra_counts else None,
                             "mean_verifier_missing": statistics.mean(verifier_missing)
                             if verifier_missing else None,
                             "mean_verifier_size_mismatch": statistics.mean(verifier_size_mismatch)
                             if verifier_size_mismatch else None,
                             "mean_verifier_timestamp_mismatch": statistics.mean(verifier_timestamp_mismatch)
                             if verifier_timestamp_mismatch else None,
                             "mean_verifier_digest_mismatch": statistics.mean(verifier_digest_mismatch)
                             if verifier_digest_mismatch else None,
                             "mean_verifier_inaccessible": statistics.mean(verifier_inaccessible)
                             if verifier_inaccessible else None,
                             "mean_verifier_unsafe": statistics.mean(verifier_unsafe)
                             if verifier_unsafe else None,
                             "mean_workers_used": statistics.mean(workers_used_values)
                             if workers_used_values else None,
                             "mean_files_avoided": statistics.mean(
                                 [record_metric(x, "files_avoided") or 0 for x in valid]),
                             "mean_bytes_avoided": statistics.mean(
                                 [record_metric(x, "bytes_avoided") or 0 for x in valid]),
                             "mean_recovery_success_rate": statistics.mean(recovery_rates)
                             if recovery_rates else (
                                 statistics.mean([record_metric(x, "recovery_success_rate") for x in valid])
                                 if key[0] == "recover" else None),
                             "mean_worker_busy_imbalance": statistics.mean(imbalance_values)
                             if imbalance_values else None,
                             "mean_scheduling_overhead_ns": statistics.mean(
                                 [x.get("timings", {}).get("scheduling_overhead_ns", 0) or 0 for x in valid]),
                             "speedup_vs_sequential": None,
                             "parallel_efficiency": None})

    def baseline_key(row: dict[str, Any]) -> tuple[Any, ...]:
        settings = dict(row.get("experiment_config", {}))
        settings.pop("min_chunk", None)
        settings.pop("max_chunk", None)
        return (row["operation"], row["workload_id"],
                json.dumps(settings, sort_keys=True, separators=(",", ":")))

    sequential_times: dict[tuple[Any, ...], list[float]] = {}
    for row in summary_rows:
        if (row["policy"] == "sequential" and row["successes"] > 0 and
                isinstance(row.get("mean_seconds"), (int, float))):
            sequential_times.setdefault(baseline_key(row), []).append(row["mean_seconds"])
    for row in summary_rows:
        baselines = sequential_times.get(baseline_key(row), [])
        if not baselines or not isinstance(row.get("mean_seconds"), (int, float)) or row["mean_seconds"] <= 0:
            continue
        baseline = statistics.mean(baselines)
        row["speedup_vs_sequential"] = baseline / row["mean_seconds"]
        effective_workers = row.get("mean_workers_used") or 0
        if effective_workers > 0:
            row["parallel_efficiency"] = row["speedup_vs_sequential"] / effective_workers
    summary_json = output / "summary.json"
    summary_json.write_text(json.dumps({"schema_version": SCHEMA_VERSION, "groups": summary_rows},
                                       indent=2) + "\n", encoding="utf-8")
    with (output / "summary.csv").open("w", newline="", encoding="utf-8") as stream:
        fieldnames = list(dict.fromkeys(key for row in summary_rows for key in row))
        writer = csv.DictWriter(stream, fieldnames=fieldnames or
                                ["operation", "workload_id", "policy", "workers", "chunk", "repetitions"])
        writer.writeheader()
        writer.writerows(summary_rows)


def run(config_path: Path, output: Path, *, fail_fast: bool = False,
        shuffle_seed: int | None = None, cleanup_backups: bool = False) -> int:
    config = validate_config(config_path)
    if shuffle_seed is None:
        shuffle_seed = config.get("execution_order_seed")
    if shuffle_seed is not None:
        config["execution_order_seed"] = _positive_int(
            shuffle_seed, "execution_order_seed", allow_zero=True)
    output.mkdir(parents=True, exist_ok=True)
    pending = list(config["runs"])
    if shuffle_seed is not None:
        random.Random(shuffle_seed).shuffle(pending)
    raw_path = output / "runs.jsonl"
    records: list[dict[str, Any]] = []
    if raw_path.exists():
        for line in raw_path.read_text(encoding="utf-8").splitlines():
            if line.strip():
                records.append(json.loads(line))
    by_id: dict[str, list[dict[str, Any]]] = {}
    for row in records:
        by_id.setdefault(row.get("experiment_id"), []).append(row)
    resumable: set[str] = set()
    for item in pending:
        prior = by_id.get(item["id"], [])
        fingerprint = _config_fingerprint(config, item)
        for row in prior:
            if row.get("config_fingerprint") != fingerprint:
                raise ValueError(f"experiment {item['id']} already has a result with a different configuration")
        successful_prior = [row for row in prior if row.get("correctness", {}).get("passed") is True]
        if cleanup_backups and item["operation"] == "backup" and successful_prior:
            if not all(row.get("backup_cleanup", {}).get("performed") is True for row in successful_prior):
                raise ValueError(
                    f"experiment {item['id']} already passed without backup cleanup; "
                    "use a fresh results directory to run with --cleanup-backups")
        if successful_prior:
            resumable.add(item["id"])
    pending = [item for item in pending if item["id"] not in resumable]
    failed = False
    with raw_path.open("a", encoding="utf-8") as raw:
        for item in pending:
            if item["operation"] == "recover":
                _require_damaged_fixture(item)
            if item["engine_kind"] == "python_reference":
                invocation = [sys.executable, item["reference_script"], "recover", "--source", item["source"],
                              "--backup", item["backup"], "--catalog", item["catalog"],
                              "--backup-id", str(item.get("verification_backup_id") or "")]
                if item.get("full"):
                    invocation.append("--full")
                engine_command = invocation[0]
                engine_args = invocation[1:]
            else:
                invocation = [item["operation"], "--source", item["source"], "--backup", item["backup"],
                              "--policy", item["policy"], "--threads", str(item["workers"]),
                              "--chunk", str(item["chunk"])]
                if item["policy"] == "adaptive":
                    invocation.extend(["--min-chunk", str(item["min_chunk"]),
                                       "--max-chunk", str(item["max_chunk"])])
                if item["operation"] == "recover":
                    invocation.extend(["--catalog", item["catalog"]])
                    if item.get("full"):
                        invocation.append("--full")
                engine_command = config["engine"]
                engine_args = invocation
            payload, stderr, code, wall_ns, process_cpu_ns = _invoke(
                engine_command, engine_args, item.get("timeout_seconds"))
            record = _record(config, item, payload, stderr, code, wall_ns, process_cpu_ns,
                             str(uuid.uuid4()))
            if record["correctness"]["passed"]:
                if item["engine_kind"] == "python_reference":
                    verify = [sys.executable, item["reference_script"], "verify", "--backup", item["backup"],
                              "--catalog", item["catalog"], "--backup-id", str(item["verification_backup_id"])]
                    checked = subprocess.run(verify, text=True, capture_output=True, check=False)
                    try:
                        verification = json.loads(checked.stdout)
                    except json.JSONDecodeError:
                        verification = {}
                    passed = checked.returncode == 0 and verification.get("status") == "COMPLETE"
                else:
                    catalog_path = payload.get("catalog_path") if item["operation"] == "backup" else item["catalog"]
                    verify_command = [config["engine"], "verify", "--backup", item["backup"],
                                      "--catalog", catalog_path, "--policy", item["policy"],
                                      "--threads", str(item["workers"]), "--chunk", str(item["chunk"])]
                    if item["policy"] == "adaptive":
                        verify_command.extend(["--min-chunk", str(item["min_chunk"]),
                                               "--max-chunk", str(item["max_chunk"])])
                    checked = subprocess.run(verify_command, text=True, capture_output=True, check=False)
                    try:
                        verification = json.loads(checked.stdout)
                    except json.JSONDecodeError:
                        verification = {}
                    passed = checked.returncode == 0 and verification.get("success") is True
                record["correctness"]["independent_verification"] = passed
                record["correctness"]["verification"] = verification
                record["correctness"]["verification_counts"] = {
                    "missing": verification.get("files_missing", 0),
                    "size_mismatch": verification.get("files_size_mismatch", 0),
                    "timestamp_mismatch": verification.get("files_timestamp_mismatch", 0),
                    "digest_mismatch": verification.get("files_digest_mismatch", 0),
                    "inaccessible": verification.get("files_inaccessible", 0),
                    "unsafe": verification.get("files_unsafe", 0),
                }
                record["metrics"]["independent_verification_workers_used"] = verification.get("workers_used")
                record["correctness"]["verification_error"] = checked.stderr[-4000:] if not passed else None
                record["correctness"]["passed"] = record["correctness"]["passed"] and passed
            if cleanup_backups and item["operation"] == "backup" and record["correctness"]["passed"]:
                backup_path = Path(item["backup"])
                cleanup = {"requested": True, "performed": False, "path": str(backup_path)}
                try:
                    if item.get("_backup_path_was_symlink") or backup_path.is_symlink():
                        raise ValueError("refusing to remove a backup path that is a symbolic link")
                    if not backup_path.exists():
                        raise FileNotFoundError("verified backup directory is missing")
                    if not backup_path.is_dir() or backup_path == Path(backup_path.anchor):
                        raise ValueError("refusing to remove a non-directory or filesystem root")
                    if backup_path in Path(item["source"]).parents or Path(item["source"]) in backup_path.parents:
                        raise ValueError("refusing to remove a backup path overlapping the source")
                    shutil.rmtree(backup_path)
                    cleanup["performed"] = True
                except (OSError, ValueError) as exc:
                    cleanup["error"] = str(exc)
                    failed = True
                    print(f"{item['id']}: backup cleanup failed: {exc}", file=sys.stderr)
                record["backup_cleanup"] = cleanup
            raw.write(json.dumps(record, sort_keys=True) + "\n")
            raw.flush()
            os.fsync(raw.fileno())
            records.append(record)
            print(f"{item['id']}: {'PASS' if record['correctness']['passed'] else 'FAIL'} "
                  f"{record['timings']['engine_elapsed_ns']} ns")
            if not record["correctness"]["passed"]:
                failed = True
                if fail_fast:
                    break
    _summary(records, output)
    return 1 if failed else 0


def summarize(raw_path: Path, output: Path) -> None:
    records = [json.loads(line) for line in raw_path.read_text(encoding="utf-8").splitlines() if line.strip()]
    output.mkdir(parents=True, exist_ok=True)
    _summary(records, output)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    run_parser = commands.add_parser("run", help="execute a prepared experiment matrix")
    run_parser.add_argument("--config", required=True, type=Path)
    run_parser.add_argument("--output", required=True, type=Path)
    run_parser.add_argument("--shuffle-seed", type=int)
    run_parser.add_argument("--fail-fast", action="store_true")
    run_parser.add_argument("--cleanup-backups", action="store_true",
                            help="remove each backup directory after its independent verification passes")
    summary_parser = commands.add_parser("summarize", help="regenerate summary files from JSONL")
    summary_parser.add_argument("--input", required=True, type=Path)
    summary_parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        if args.command == "run":
            return run(args.config, args.output, fail_fast=args.fail_fast,
                       shuffle_seed=args.shuffle_seed, cleanup_backups=args.cleanup_backups)
        summarize(args.input, args.output)
        return 0
    except (OSError, ValueError, json.JSONDecodeError, subprocess.SubprocessError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
