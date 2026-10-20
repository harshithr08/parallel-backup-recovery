#!/usr/bin/env python3
"""Expand isolated, already-prepared C++ recovery fixtures into benchmark configs.

This tool validates fixture/catalog bindings and writes configs only. It never
copies, damages, repairs, or deletes fixture data.
"""
from __future__ import annotations

import argparse
import json
import math
import re
import sys
from pathlib import Path
from typing import Any

try:
    from .experiment_matrix import _matrix_settings
except ImportError:  # direct `python src/recovery_matrix.py` execution
    from experiment_matrix import _matrix_settings

SCHEMA_VERSION = 1


def _read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot read JSON file {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return value


def _ratio_token(value: float) -> str:
    return format(value, ".12g").replace(".", "p")


def _key(row: dict[str, Any]) -> tuple[Any, ...]:
    dataset_id, policy = row.get("dataset_id"), row.get("policy")
    workers, chunk, ratio = row.get("workers"), row.get("chunk"), row.get("damage_ratio")
    damage_type, repetition, mode = (row.get("damage_type"), row.get("repetition"),
                                     row.get("mode"))
    if not isinstance(dataset_id, str) or not isinstance(policy, str):
        raise ValueError("fixture dataset_id and policy must be strings")
    if any(isinstance(value, bool) or not isinstance(value, int) or value < 1
           for value in (workers, chunk, repetition)):
        raise ValueError("fixture workers, chunk, and repetition must be positive integers")
    if (isinstance(ratio, bool) or not isinstance(ratio, (int, float)) or
            not math.isfinite(ratio)):
        raise ValueError("fixture damage_ratio must be a finite number")
    if not isinstance(damage_type, str) or not isinstance(mode, str):
        raise ValueError("fixture damage_type and mode must be strings")
    return (dataset_id, policy, workers, chunk, float(ratio), damage_type, repetition, mode)


def _expected(plan: dict[str, Any]) -> tuple[set[tuple[Any, ...]], dict[str, Any]]:
    if plan.get("schema_version") != SCHEMA_VERSION:
        raise ValueError(f"plan schema_version must be {SCHEMA_VERSION}")
    protocol = plan.get("protocol")
    datasets = plan.get("datasets")
    if not isinstance(protocol, dict) or not isinstance(datasets, list) or not datasets:
        raise ValueError("plan must contain protocol and non-empty datasets")
    repetitions = protocol.get("repetitions")
    if isinstance(repetitions, bool) or not isinstance(repetitions, int) or repetitions < 1:
        raise ValueError("plan protocol repetitions must be a positive integer")
    ratios = protocol.get("recovery_damage_ratios")
    damage_types = protocol.get("damage_types")
    if (not isinstance(ratios, list) or not ratios or
            any(isinstance(value, bool) or not isinstance(value, (int, float)) or
                not math.isfinite(value) or value <= 0 or value > 1 for value in ratios)):
        raise ValueError("plan protocol recovery_damage_ratios must be finite values in (0, 1]")
    if (not isinstance(damage_types, list) or not damage_types or
            any(not isinstance(value, str) or value not in
                {"missing", "modified", "corrupted", "truncated"} for value in damage_types) or
            len(set(damage_types)) != len(damage_types)):
        raise ValueError("plan protocol damage_types must be a non-empty string list")
    if len(set(float(value) for value in ratios)) != len(ratios):
        raise ValueError("plan protocol damage ratios must be unique")
    try:
        settings = _matrix_settings(protocol)
    except (KeyError, TypeError) as exc:
        raise ValueError("plan protocol is missing valid worker or chunk settings") from exc
    expected: set[tuple[Any, ...]] = set()
    dataset_ids: set[str] = set()
    for dataset in datasets:
        if (not isinstance(dataset, dict) or
                not isinstance(dataset.get("dataset_id"), str) or
                not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}", dataset["dataset_id"])):
            raise ValueError("every plan dataset must have a dataset_id")
        if dataset["dataset_id"] in dataset_ids:
            raise ValueError(f"duplicate dataset_id in plan: {dataset['dataset_id']}")
        dataset_ids.add(dataset["dataset_id"])
        for policy, workers, chunk, _, _ in settings:
            for ratio in ratios:
                for damage_type in damage_types:
                    for repetition in range(1, repetitions + 1):
                        for mode in ("selective", "full"):
                            expected.add((dataset["dataset_id"], policy, workers, chunk,
                                          float(ratio), damage_type, repetition, mode))
    return expected, protocol


def _validate_fixture(row: dict[str, Any], plan: dict[str, Any]) -> dict[str, Any]:
    protocol = plan["protocol"]
    required = ("source", "catalog", "backup", "snapshot_name", "manifest", "damage_log")
    for field in required:
        if not isinstance(row.get(field), str) or not row[field]:
            raise ValueError(f"fixture {_key(row)!r} must provide {field}")
    source_input = Path(row["source"]).expanduser()
    catalog_input = Path(row["catalog"]).expanduser()
    if source_input.is_symlink() or catalog_input.is_symlink():
        raise ValueError("fixture source root and catalog must not be symbolic links")
    source = source_input.resolve()
    catalog = catalog_input.resolve()
    manifest_input = Path(row["manifest"]).expanduser()
    damage_log_input = Path(row["damage_log"]).expanduser()
    if manifest_input.is_symlink() or damage_log_input.is_symlink():
        raise ValueError("fixture manifest and damage log must not be symbolic links")
    manifest = manifest_input.resolve()
    damage_log_path = damage_log_input.resolve()
    if not source.is_dir():
        raise ValueError(f"fixture source does not exist: {source}")
    if not catalog.is_file():
        raise ValueError(f"fixture catalog does not exist: {catalog}")
    if not manifest.is_file() or not damage_log_path.is_file():
        raise ValueError(f"fixture manifest or damage log does not exist: {manifest}, {damage_log_path}")
    manifest_document = _read_json(manifest)
    if (not isinstance(manifest_document.get("dataset_root_name"), str) or
            (manifest.parent / manifest_document["dataset_root_name"]).resolve() != source or
            not isinstance(manifest_document.get("files"), list) or
            not manifest_document["files"]):
        raise ValueError(f"fixture manifest does not describe its source tree: {manifest}")
    if manifest_document.get("file_count") != len(manifest_document["files"]):
        raise ValueError(f"fixture manifest file_count is inconsistent: {manifest}")
    manifest_paths = [entry.get("relative_path") for entry in manifest_document["files"]
                      if isinstance(entry, dict)]
    if (len(manifest_paths) != len(manifest_document["files"]) or
            any(not isinstance(path, str) or not path for path in manifest_paths) or
            len(set(manifest_paths)) != len(manifest_paths)):
        raise ValueError(f"fixture manifest has invalid or duplicate paths: {manifest}")
    manifest_path_set = set(manifest_paths)
    damage_log = _read_json(damage_log_path)
    expected_seed = damage_log.get("seed")
    expected_damage_count = int(len(manifest_document["files"]) * float(row["damage_ratio"]))
    logged_ratio = damage_log.get("requested_ratio")
    if (damage_log.get("status") != "COMPLETE" or
            isinstance(expected_seed, bool) or not isinstance(expected_seed, int) or expected_seed < 0 or
            damage_log_path != manifest.with_name(manifest.stem + f".damage-{expected_seed}.json") or
            isinstance(logged_ratio, bool) or not isinstance(logged_ratio, (int, float)) or
            not math.isfinite(logged_ratio) or
            not math.isclose(logged_ratio, float(row["damage_ratio"]), rel_tol=0, abs_tol=1e-12) or
            damage_log.get("eligible_files") != len(manifest_document["files"]) or
            damage_log.get("damaged_files") != expected_damage_count):
        raise ValueError(f"fixture damage log does not match its manifest or requested ratio: {damage_log_path}")
    if row.get("damage_seed") is not None and expected_seed != row.get("damage_seed"):
        raise ValueError(f"fixture damage log seed differs from the planned seed: {damage_log_path}")
    changes = damage_log.get("changes")
    if (not isinstance(changes, list) or len(changes) != expected_damage_count or
            any(not isinstance(change, dict) or change.get("damage_type") != row["damage_type"]
                or change.get("relative_path") not in manifest_path_set
                for change in changes) or
            len({change.get("relative_path") for change in changes if isinstance(change, dict)}) != len(changes)):
        raise ValueError(f"fixture damage log does not match requested damage type: {damage_log_path}")
    if damage_log.get("damage_type") not in (None, row["damage_type"]):
        raise ValueError(f"fixture damage log damage type mismatch: {damage_log_path}")
    document = _read_json(catalog)
    source_catalog_path = snapshot / "catalog.json"
    if not source_catalog_path.is_file():
        raise ValueError(f"immutable snapshot catalog is missing: {source_catalog_path}")
    source_catalog = _read_json(source_catalog_path)
    if (document.get("schema_version") not in {1, 2, 3} or
            (document.get("schema_version") == 3 and document.get("data_root") != "files") or
            not isinstance(document.get("backup_id"), str) or
            document.get("backup_id") != source_catalog.get("backup_id") or
            document.get("files") != source_catalog.get("files")):
        raise ValueError(f"unsupported or malformed C++ catalog: {catalog}")
    if source_catalog.get("schema_version") != document.get("schema_version"):
        raise ValueError(f"fixture catalog schema differs from immutable snapshot catalog: {catalog}")
    if not isinstance(document.get("files"), list):
        raise ValueError(f"catalog has no files array: {catalog}")
    recorded_source = document.get("source_root")
    recorded_snapshot = document.get("snapshot_root")
    if not isinstance(recorded_source, str) or Path(recorded_source).resolve() != source:
        raise ValueError(f"catalog source_root does not match fixture source: {catalog}")
    if not isinstance(recorded_snapshot, str):
        raise ValueError(f"catalog has no snapshot_root: {catalog}")
    if Path(recorded_snapshot).is_symlink():
        raise ValueError(f"catalog snapshot root cannot be a symbolic link: {recorded_snapshot}")
    snapshot = Path(recorded_snapshot).resolve()
    # Isolated fixtures may reuse the immutable backup payload while carrying a
    # small catalog copy rebound to their own source root.
    backup = Path(row.get("backup", "")).expanduser()
    if not str(backup):
        raise ValueError(f"fixture {_key(row)!r} must provide its verified backup root")
    backup = backup.resolve()
    snapshot_name = row.get("snapshot_name")
    if not isinstance(snapshot_name, str) or not snapshot_name or Path(snapshot_name).name != snapshot_name:
        raise ValueError(f"fixture {_key(row)!r} must provide a safe snapshot_name")
    snapshot = backup / snapshot_name
    if not snapshot.is_dir():
        raise ValueError(f"recorded snapshot_root does not exist: {snapshot}")
    if snapshot.is_symlink():
        raise ValueError(f"fixture snapshot root cannot be a symbolic link: {snapshot}")
    try:
        snapshot.resolve().relative_to(backup)
    except ValueError as exc:
        raise ValueError(f"fixture snapshot escapes its backup root: {snapshot}") from exc
    if source == backup or source in backup.parents or backup in source.parents:
        raise ValueError(f"fixture source and backup roots overlap: {source}, {backup}")
    if Path(recorded_snapshot).resolve() != snapshot:
        raise ValueError(f"catalog snapshot_root does not match fixture backup and snapshot name: {catalog}")
    if catalog != snapshot / "catalog.json":
        raise ValueError(f"C++ recovery requires a fixture-bound catalog at the snapshot's catalog.json path: {catalog}")
    if catalog.is_symlink():
        raise ValueError(f"fixture catalog cannot remain a hard or symbolic link: {catalog}")
    if not backup.is_dir():
        raise ValueError(f"fixture backup root is missing: {backup}")
    if source in catalog.parents:
        raise ValueError(f"fixture catalog must be outside the source tree: {catalog}")
    return {
        "source": str(source), "backup": str(backup), "catalog": str(catalog),
        "manifest": str(manifest), "damage_log": str(damage_log_path),
        "damage_seed": expected_seed,
        "snapshot_root": str(snapshot), "engine": protocol["engine"],
    }


def audit(plan_path: Path, fixtures_path: Path, *, allow_incomplete: bool = False) -> dict[str, Any]:
    plan = _read_json(plan_path)
    document = _read_json(fixtures_path)
    if document.get("schema_version") != SCHEMA_VERSION or not isinstance(document.get("fixtures"), list):
        raise ValueError("fixtures file must have schema_version 1 and a fixtures array")
    expected, _ = _expected(plan)
    seen: set[tuple[Any, ...]] = set()
    validated = 0
    fixture_sources: set[Path] = set()
    fixture_catalogs: set[Path] = set()
    errors: list[str] = []
    for index, row in enumerate(document["fixtures"]):
        if not isinstance(row, dict):
            errors.append(f"fixtures[{index}] must be an object")
            continue
        try:
            key = _key(row)
        except ValueError as exc:
            errors.append(f"fixtures[{index}]: {exc}")
            continue
        if key not in expected:
            errors.append(f"fixtures[{index}] is outside the planned matrix: {key!r}")
            continue
        if key in seen:
            errors.append(f"duplicate fixture key: {key!r}")
            continue
        seen.add(key)
        try:
            paths = _validate_fixture(row, plan)
            source = Path(paths["source"])
            catalog = Path(paths["catalog"])
            if any(source == previous or source in previous.parents or previous in source.parents
                   for previous in fixture_sources):
                raise ValueError(f"fixture source overlaps another run; each needs an isolated copy: {source}")
            if catalog in fixture_catalogs:
                raise ValueError(f"fixture catalog is reused; each run needs an isolated catalog: {catalog}")
            fixture_sources.add(source)
            fixture_catalogs.add(catalog)
            validated += 1
        except (OSError, ValueError) as exc:
            errors.append(f"fixtures[{index}]: {exc}")
    missing = len(expected - seen)
    if allow_incomplete and not seen:
        errors.append("at least one planned fixture is required")
    return {
        "status": "READY" if not errors and (allow_incomplete or missing == 0) else "INCOMPLETE",
        "planned_runs": len(expected), "fixtures_supplied": len(document["fixtures"]),
        "fixtures_validated": validated,
        "missing_fixtures": missing, "errors": errors,
    }


def prepare(plan_path: Path, fixtures_path: Path, output: Path, *,
            allow_incomplete: bool = False) -> dict[str, Any]:
    plan = _read_json(plan_path)
    fixture_document = _read_json(fixtures_path)
    result = audit(plan_path, fixtures_path, allow_incomplete=allow_incomplete)
    if result["status"] != "READY":
        raise ValueError(f"fixture set is incomplete or invalid ({len(result['errors'])} errors, "
                         f"{result['missing_fixtures']} missing)")
    expected, protocol = _expected(plan)
    fixture_by_key = {_key(row): row for row in fixture_document["fixtures"]}
    configurations: dict[str, dict[str, Any]] = {}
    dataset_plans = {dataset["dataset_id"]: dataset for dataset in plan["datasets"]}
    run_ids: set[str] = set()
    keys_to_emit = set(fixture_by_key) if allow_incomplete else expected
    for key in sorted(keys_to_emit, key=lambda value: tuple(str(part) for part in value)):
        dataset_id, policy, workers, chunk, ratio, damage_type, repetition, mode = key
        fixture = fixture_by_key[key]
        paths = _validate_fixture(fixture, plan)
        run_id = (f"recover-{dataset_id}-{policy}-w{workers}-c{chunk}-"
                  f"q{_ratio_token(ratio)}-{damage_type}-r{repetition}-{mode}")
        item = {
            "id": run_id, "operation": "recover", "repetition": repetition,
            "workload_id": dataset_id, "source": paths["source"],
            "manifest": paths["manifest"], "damage_log": paths["damage_log"],
            "damage_seed": paths["damage_seed"],
            "backup": paths["backup"], "catalog": paths["catalog"],
            "policy": policy, "workers": workers, "chunk": chunk,
            "min_chunk": protocol["adaptive_min_chunk"] if policy == "adaptive" else 1,
            "max_chunk": protocol["adaptive_max_chunk"] if policy == "adaptive" else 1,
            "recovery_mode": mode, "damage_ratio": ratio,
            "damage_types": [damage_type], "fixture_id": fixture.get("fixture_id", run_id),
        }
        if run_id in run_ids:
            raise ValueError(f"generated duplicate run id: {run_id}")
        run_ids.add(run_id)
        config = configurations.setdefault(dataset_id, {
            "schema_version": 1, "engine": paths["engine"],
            "execution_order_seed": dataset_plans[dataset_id].get("execution_order_seed"),
            "environment_metadata": protocol.get("environment_metadata", {}), "runs": [],
        })
        if config["engine"] != paths["engine"]:
            raise ValueError("fixture plan resolves to inconsistent engine paths")
        config["runs"].append(item)

    output = output.expanduser().resolve()
    if output.exists():
        raise FileExistsError(f"output directory already exists: {output}")
    output.mkdir(parents=True)
    for dataset_id, config in configurations.items():
        name = "".join(char if char.isalnum() or char in "._-" else "_" for char in dataset_id)
        (output / f"{name}.json").write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
    (output / "recovery-plan.json").write_text(json.dumps({
        "schema_version": 1, "planned_runs": len(keys_to_emit),
        "configs": len(configurations), "fixture_manifest": str(fixtures_path.resolve()),
        "generated_config_files": sorted(f"{dataset_id}.json" for dataset_id in configurations),
    }, indent=2) + "\n", encoding="utf-8")
    return {"output": str(output), "planned_runs": len(keys_to_emit), "config_files": len(configurations)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", required=True, type=Path, help="experiment_matrix.py plan.json")
    parser.add_argument("--fixtures", required=True, type=Path, help="isolated fixture mapping JSON")
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--audit", action="store_true", help="validate coverage without writing files")
    action.add_argument("--output", type=Path, help="directory for per-workload recovery configs")
    parser.add_argument("--allow-incomplete", action="store_true",
                        help="generate configs for supplied fixtures only; intended for staged runs")
    args = parser.parse_args()
    try:
        result = (audit(args.plan, args.fixtures, allow_incomplete=args.allow_incomplete)
                  if args.audit else
                  prepare(args.plan, args.fixtures, args.output,
                          allow_incomplete=args.allow_incomplete))
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2))
    return 0 if result.get("status", "READY") == "READY" else 1


if __name__ == "__main__":
    raise SystemExit(main())
