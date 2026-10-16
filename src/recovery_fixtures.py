#!/usr/bin/env python3
"""Prepare isolated C++ recovery fixtures from verified backup runs.

The audit mode is read-only. Preparation copies workload files, creates a
source-root-bound catalog copy, and injects damage only under a new output
directory. It never edits original workloads, catalogs, or backup snapshots.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import sys
from pathlib import Path, PurePosixPath
from typing import Any

try:
    from .experiment_matrix import _matrix_settings
    from .ipbrs_reference import inject_damage
    from .recovery_matrix import _expected, _read_json, _ratio_token
except ImportError:  # direct `python src/recovery_fixtures.py` execution
    from experiment_matrix import _matrix_settings
    from ipbrs_reference import inject_damage
    from recovery_matrix import _expected, _read_json, _ratio_token


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _manifest_files(manifest: dict[str, Any], source: Path) -> list[dict[str, Any]]:
    rows = manifest.get("files")
    if not isinstance(rows, list) or not rows:
        raise ValueError("workload manifest must have a non-empty files array")
    normalized: list[dict[str, Any]] = []
    seen: set[str] = set()
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            raise ValueError(f"manifest files[{index}] must be an object")
        relative = row.get("relative_path")
        if not isinstance(relative, str) or not relative:
            raise ValueError(f"manifest files[{index}].relative_path must be a string")
        rel = PurePosixPath(relative)
        if rel.is_absolute() or any(part in {"", ".", ".."} for part in rel.parts):
            raise ValueError(f"unsafe manifest path: {relative}")
        key = rel.as_posix()
        if key in seen:
            raise ValueError(f"duplicate manifest path: {relative}")
        seen.add(key)
        size, digest = row.get("size_bytes"), row.get("sha256")
        if isinstance(size, bool) or not isinstance(size, int) or size < 0:
            raise ValueError(f"manifest size is invalid for {relative}")
        if not isinstance(digest, str) or len(digest) != 64:
            raise ValueError(f"manifest SHA-256 is invalid for {relative}")
        source_file = source.joinpath(*rel.parts)
        if source_file.is_symlink() or not source_file.is_file():
            raise ValueError(f"workload file is missing or is a symbolic link: {source_file}")
        resolved = source_file.resolve()
        if resolved == source or source not in resolved.parents:
            raise ValueError(f"workload path escapes source root: {relative}")
        normalized.append({**row, "relative_path": key, "size_bytes": size, "sha256": digest})
    return normalized


def _verified_catalogs(plan: dict[str, Any], runs_path: Path,
                       selected_datasets: set[str] | None = None) -> dict[str, tuple[Path, Path, dict[str, Any]]]:
    datasets = plan["datasets"]
    dataset_by_id = {item["dataset_id"]: item for item in datasets
                     if selected_datasets is None or item["dataset_id"] in selected_datasets}
    records: dict[str, tuple[Path, Path, dict[str, Any]]] = {}
    log_seen_ids: set[str] = set()
    try:
        lines = runs_path.read_text(encoding="utf-8-sig").splitlines()
    except OSError as exc:
        raise ValueError(f"cannot read backup run log {runs_path}: {exc}") from exc
    for line_number, line in enumerate(lines, 1):
        if not line.strip():
            continue
        try:
            row = json.loads(line)
        except json.JSONDecodeError as exc:
            raise ValueError(f"invalid JSON in {runs_path} line {line_number}: {exc}") from exc
        if (not isinstance(row, dict) or row.get("operation") != "backup" or
                row.get("policy") != "sequential" or row.get("repetition") != 1 or
                row.get("correctness", {}).get("passed") is not True or
                row.get("correctness", {}).get("independent_verification") is not True):
            continue
        dataset_id = row.get("workload_id")
        if not isinstance(dataset_id, str):
            continue
        experiment_id = row.get("experiment_id")
        if (dataset_id not in dataset_by_id or dataset_id in records or
                experiment_id != f"{dataset_id}-sequential-w1-c1-r1" or
                not isinstance(row.get("config_fingerprint"), str) or
                not row["config_fingerprint"] or experiment_id in log_seen_ids):
            continue
        log_seen_ids.add(experiment_id)
        catalog_text = row.get("results", {}).get("catalog_path")
        if not isinstance(catalog_text, str) or not catalog_text:
            continue
        catalog = Path(catalog_text).expanduser().resolve()
        if not catalog.is_file():
            continue
        document = _read_json(catalog)
        source = Path(dataset_by_id[dataset_id]["source"]).expanduser().resolve()
        if Path(dataset_by_id[dataset_id]["source"]).expanduser().is_symlink():
            continue
        snapshot_text = document.get("snapshot_root")
        if (document.get("schema_version") not in {1, 2, 3} or
                (document.get("schema_version") == 3 and document.get("data_root") != "files") or
                not isinstance(document.get("source_root"), str) or
                Path(document["source_root"]).resolve() != source or
                not isinstance(snapshot_text, str)):
            continue
        recorded_catalog = row.get("results", {}).get("catalog_path")
        if (not isinstance(recorded_catalog, str) or
                Path(recorded_catalog).expanduser().resolve() != catalog or
                row.get("results", {}).get("backup_id") != document.get("backup_id")):
            continue
        snapshot = Path(snapshot_text).resolve()
        if (not snapshot.is_dir() or catalog.parent != snapshot or
                not (snapshot / "catalog.json").is_file()):
            continue
        records[dataset_id] = (catalog, snapshot.parent, document)
    missing = sorted(set(dataset_by_id) - set(records))
    if missing:
        raise ValueError("no independently verified sequential repetition 1 catalog found for: " +
                         ", ".join(missing))
    return records


def _inventory(plan: dict[str, Any], runs_path: Path,
               fixture_id: str | None = None) -> tuple[list[dict[str, Any]], int, dict[str, Any]]:
    expected, protocol = _expected(plan)
    if fixture_id is not None and (not isinstance(fixture_id, str) or not fixture_id):
        raise ValueError("fixture_id must be a non-empty string")
    selected_dataset = next((item["dataset_id"] for item in plan["datasets"]
                             if fixture_id is not None and fixture_id.startswith(item["dataset_id"] + "-")), None)
    if fixture_id is not None and selected_dataset is None:
        raise ValueError(f"fixture_id does not begin with a planned dataset ID: {fixture_id}")
    backups = _verified_catalogs(plan, runs_path,
                                 {selected_dataset} if selected_dataset is not None else None)
    datasets = {item["dataset_id"]: item for item in plan["datasets"]}
    inventory: dict[str, tuple[dict[str, Any], list[dict[str, Any]], int]] = {}
    for dataset_id, dataset in datasets.items():
        if selected_dataset is not None and dataset_id != selected_dataset:
            continue
        source_input = Path(dataset["source"]).expanduser()
        manifest_input = Path(dataset["manifest"]).expanduser()
        if source_input.is_symlink() or manifest_input.is_symlink():
            raise ValueError(f"workload source or manifest cannot be a symbolic link for {dataset_id}")
        source = source_input.resolve()
        manifest_path = manifest_input.resolve()
        if not source.is_dir() or not manifest_path.is_file():
            raise ValueError(f"workload source or manifest is missing for {dataset_id}")
        manifest = _read_json(manifest_path)
        if (manifest.get("dataset_root_name") != source.name or
                manifest.get("file_count") != len(manifest.get("files", [])) or
                not isinstance(manifest.get("files"), list) or
                manifest.get("seed") != dataset.get("dataset_seed")):
            raise ValueError(f"manifest does not describe planned workload {dataset_id}")
        if manifest.get("file_count") != dataset.get("file_count"):
            raise ValueError(f"manifest count does not match planned workload {dataset_id}")
        files = _manifest_files(manifest, source)
        for record in files:
            source_file = source.joinpath(*PurePosixPath(record["relative_path"]).parts)
            if (source_file.stat().st_size != record["size_bytes"] or
                    _sha256(source_file) != record["sha256"]):
                raise ValueError(f"workload no longer matches manifest for {dataset_id}: {record['relative_path']}")
        total_bytes = sum(item["size_bytes"] for item in files)
        if (manifest.get("total_bytes") != total_bytes or
                sum(item["size_bytes"] for item in files) != manifest.get("total_bytes")):
            raise ValueError(f"manifest total_bytes disagrees with its files for {dataset_id}")
        catalog_document = backups[dataset_id][2]
        catalog_entries = catalog_document.get("files")
        if not isinstance(catalog_entries, list):
            raise ValueError(f"verified backup catalog has no files array for {dataset_id}")
        manifest_by_path = {item["relative_path"]: item for item in files}
        catalog_by_path: dict[str, dict[str, Any]] = {}
        for index, entry in enumerate(catalog_entries):
            if not isinstance(entry, dict) or not isinstance(entry.get("relative_path"), str):
                raise ValueError(f"backup catalog files[{index}] is invalid for {dataset_id}")
            relative = entry["relative_path"].replace("\\", "/")
            if relative in catalog_by_path:
                raise ValueError(f"backup catalog has a duplicate path for {dataset_id}: {relative}")
            catalog_by_path[relative] = entry
        if set(catalog_by_path) != set(manifest_by_path):
            raise ValueError(f"verified backup catalog does not cover exactly the manifest files for {dataset_id}")
        for relative, manifest_entry in manifest_by_path.items():
            catalog_entry = catalog_by_path[relative]
            if (catalog_entry.get("sha256") != manifest_entry["sha256"] or
                    catalog_entry.get("size_bytes") != manifest_entry["size_bytes"] or
                    catalog_entry.get("status", "COMPLETE") != "COMPLETE"):
                raise ValueError(f"backup catalog disagrees with manifest for {dataset_id}: {relative}")
        inventory[dataset_id] = (manifest, files, total_bytes)

    ratios = protocol["recovery_damage_ratios"]
    damage_types = protocol["damage_types"]
    repetitions = protocol["repetitions"]
    case_order: dict[tuple[str, float, str, int], int] = {}
    case_index = 0
    for dataset in plan["datasets"]:
        dataset_id = dataset["dataset_id"]
        for ratio in ratios:
            for damage_type in damage_types:
                for repetition in range(1, repetitions + 1):
                    case_order[(dataset_id, float(ratio), damage_type, repetition)] = case_index
                    case_index += 1

    entries = []
    fixture_bytes = 0
    for key in sorted(expected, key=lambda value: tuple(str(part) for part in value)):
        dataset_id, policy, workers, chunk, ratio, damage_type, repetition, mode = key
        if selected_dataset is not None and dataset_id != selected_dataset:
            continue
        manifest, files, byte_count = inventory[dataset_id]
        catalog, backup_root, catalog_document = backups[dataset_id]
        seed = protocol["recovery_damage_seed_base"] + case_order[
            (dataset_id, ratio, damage_type, repetition)]
        fixture_id = (f"{dataset_id}-{policy}-w{workers}-c{chunk}-q{_ratio_token(ratio)}-"
                      f"{damage_type}-r{repetition}-{mode}")
        entries.append({
            "key": key, "fixture_id": fixture_id, "seed": seed,
            "dataset": datasets[dataset_id], "manifest": manifest, "files": files,
            "source": Path(datasets[dataset_id]["source"]).resolve(),
            "catalog": catalog, "backup_root": backup_root,
            "catalog_document": catalog_document, "bytes": byte_count,
        })
        fixture_bytes += byte_count
    if fixture_id is not None:
        entries = [entry for entry in entries if entry["fixture_id"] == fixture_id]
        if len(entries) != 1:
            raise ValueError(f"fixture_id is not an exact planned fixture: {fixture_id}")
        fixture_bytes = entries[0]["bytes"]
    return entries, fixture_bytes, protocol


def audit(plan_path: Path, runs_path: Path, output: Path,
          fixture_id: str | None = None) -> dict[str, Any]:
    plan = _read_json(plan_path)
    entries, fixture_bytes, _ = _inventory(plan, runs_path, fixture_id)
    output = output.expanduser().resolve()
    if output == Path(output.anchor):
        raise ValueError("fixture output cannot be a filesystem root")
    if output.exists():
        raise FileExistsError(f"output directory already exists: {output}")
    for entry in entries:
        source, backup_root = entry["source"], entry["backup_root"]
        if (output == source or output in source.parents or source in output.parents or
                output == backup_root or output in backup_root.parents or backup_root in output.parents):
            raise ValueError(f"fixture output overlaps a workload or backup root: {output}")
    probe = output
    while not probe.exists() and probe != probe.parent:
        probe = probe.parent
    if not probe.is_dir():
        raise ValueError(f"nearest existing output ancestor is not a directory: {probe}")
    available = shutil.disk_usage(probe).free
    drives = {Path(entry["catalog_document"]["snapshot_root"]).resolve().drive.casefold()
              for entry in entries}
    if len(drives) != 1 or probe.drive.casefold() not in drives:
        raise ValueError("fixture output and all verified snapshots must share one volume for hard links")
    if not hasattr(Path, "hardlink_to"):
        raise ValueError("this Python runtime does not support the required hard-link fixture isolation")
    if os.name == "nt" and platform.release() in {"7", "8", "8.1"}:
        raise ValueError("hard-link creation may require elevated privileges on this Windows version")
    try:
        probed_volumes: set[str] = set()
        for entry in entries:
            source_snapshot = Path(entry["catalog_document"]["snapshot_root"])
            if source_snapshot.drive.casefold() in probed_volumes:
                continue
            for file_path in source_snapshot.rglob("*"):
                if file_path.is_file():
                    probe_file = probe / ".ipbrs-hardlink-probe"
                    try:
                        probe_file.hardlink_to(file_path)
                    except OSError as exc:
                        raise ValueError(f"hard links are unavailable between output and backup volume: {exc}") from exc
                    else:
                        probe_file.unlink()
                        probed_volumes.add(source_snapshot.drive.casefold())
                    break
            else:
                raise ValueError(f"verified snapshot contains no files: {source_snapshot}")
    except OSError as exc:
        raise ValueError(f"cannot inspect output volume capacity: {exc}") from exc
    return {
        "status": "READY" if available >= fixture_bytes else "INSUFFICIENT_SPACE",
        "output": str(output), "fixture_count": len(entries),
        "fixture_id": fixture_id,
        "planned_fixture_payload_bytes": fixture_bytes,
        "available_bytes": available, "payload_estimate_fits": available >= fixture_bytes,
        "snapshot_view": "hard links to immutable backup files on same volume; source fixture payload is copied",
        "note": "Conservative source fixture payload estimate; hard-linked snapshot views add metadata but not file payload. Excludes filesystem allocation, catalogs, manifests, logs, and safety margin.",
    }


def _copy_workload(source: Path, destination: Path, files: list[dict[str, Any]]) -> None:
    destination.mkdir(parents=True)
    for record in files:
        rel = PurePosixPath(record["relative_path"])
        source_path = source.joinpath(*rel.parts)
        target = destination.joinpath(*rel.parts)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source_path, target)
        if target.stat().st_size != record["size_bytes"] or _sha256(target) != record["sha256"]:
            raise ValueError(f"copied fixture file failed manifest verification: {target}")


def _hardlink_snapshot(source: Path, destination: Path) -> None:
    if destination.exists():
        raise FileExistsError(f"snapshot fixture view already exists: {destination}")
    destination.mkdir()
    try:
        for current, directories, files in os.walk(source, followlinks=False):
            current_path = Path(current)
            directories[:] = [name for name in directories
                              if not (current_path / name).is_symlink()]
            relative_dir = current_path.relative_to(source)
            (destination / relative_dir).mkdir(parents=True, exist_ok=True)
            for filename in files:
                original = current_path / filename
                if original.is_symlink() or not original.is_file():
                    raise ValueError(f"unsupported object in verified snapshot: {original}")
                linked = destination / relative_dir / filename
                linked.hardlink_to(original)
    except Exception:
        shutil.rmtree(destination, ignore_errors=True)
        raise


def prepare(plan_path: Path, runs_path: Path, output: Path,
            fixture_id: str | None = None) -> dict[str, Any]:
    plan = _read_json(plan_path)
    preflight = audit(plan_path, runs_path, output, fixture_id)
    if preflight["status"] != "READY":
        raise ValueError(f"recovery fixture preflight failed: {preflight['status']} "
                         f"(need {preflight['planned_fixture_payload_bytes']} bytes; "
                         f"have {preflight['available_bytes']})")
    entries, _, _ = _inventory(plan, runs_path, fixture_id)
    output = output.expanduser().resolve()
    output.mkdir(parents=True)
    fixtures = []
    current_fixture_root: Path | None = None
    try:
        for index, entry in enumerate(entries, 1):
            dataset_id, policy, workers, chunk, ratio, damage_type, repetition, mode = entry["key"]
            fixture_root = output / entry["fixture_id"]
            current_fixture_root = fixture_root
            fixture_root.mkdir(parents=True)
            source_copy = fixture_root / "source"
            _copy_workload(entry["source"], source_copy, entry["files"])
            snapshot_name = Path(entry["catalog_document"]["snapshot_root"]).name
            backup_root_copy = fixture_root / "backup-root"
            backup_root_copy.mkdir()
            snapshot_link = backup_root_copy / snapshot_name
            source_snapshot = Path(entry["catalog_document"]["snapshot_root"])
            _hardlink_snapshot(source_snapshot, snapshot_link)

            manifest_copy = fixture_root / "source.manifest.json"
            manifest_document = dict(entry["manifest"])
            manifest_document["dataset_root_name"] = source_copy.name
            manifest_copy.write_text(json.dumps(manifest_document, indent=2) + "\n", encoding="utf-8")

            catalog_copy = snapshot_link / "catalog.json"
            catalog_document = dict(entry["catalog_document"])
            catalog_document["source_root"] = str(source_copy)
            catalog_document["snapshot_root"] = str(snapshot_link)
            catalog_temp = snapshot_link / "catalog.json.fixture-tmp"
            with catalog_temp.open("x", encoding="utf-8") as stream:
                json.dump(catalog_document, stream, indent=2)
                stream.write("\n")
            # Replace the catalog hard link in this isolated view without
            # truncating or modifying the immutable source snapshot catalog.
            os.replace(catalog_temp, catalog_copy)

            damage = inject_damage(manifest_copy, ratio, [damage_type], entry["seed"])
            fixtures.append({
                "dataset_id": dataset_id, "policy": policy, "workers": workers,
                "chunk": chunk, "damage_ratio": ratio, "damage_type": damage_type,
                "repetition": repetition, "mode": mode,
                "fixture_id": entry["fixture_id"], "source": str(source_copy),
                "catalog": str(catalog_copy), "backup": str(backup_root_copy),
                "snapshot_name": snapshot_name,
                "manifest": str(manifest_copy),
                "damage_log": damage["damage_log"],
            })
            current_fixture_root = None
            if index % 25 == 0:
                print(f"prepared {index}/{len(entries)} recovery fixtures", file=sys.stderr)
        fixture_map = {"schema_version": 1, "fixtures": fixtures}
        fixture_path = output / "fixtures.json"
        fixture_path.write_text(json.dumps(fixture_map, indent=2) + "\n", encoding="utf-8")
        metadata = {
            "schema_version": 1, "fixture_count": len(fixtures),
            "fixture_id": fixture_id,
            "fixture_payload_bytes": preflight["planned_fixture_payload_bytes"],
            "recovery_damage_seed_base": plan["protocol"]["recovery_damage_seed_base"],
            "protocol_id": plan.get("protocol_id"),
            "verified_backup_run_log": str(runs_path.expanduser().resolve()),
            "fixtures_manifest": str(fixture_path),
        }
        (output / "preparation.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
        return {"output": str(output), **metadata}
    except Exception as exc:
        try:
            (output / "PREPARATION_FAILED.json").write_text(
                json.dumps({"error": str(exc), "fixtures_completed": len(fixtures),
                            "partial_fixture_directory": (str(current_fixture_root)
                                                           if current_fixture_root else None)}, indent=2) + "\n",
                encoding="utf-8")
        except OSError:
            pass
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", required=True, type=Path)
    parser.add_argument("--runs", required=True, type=Path,
                        help="runs.jsonl containing independently verified sequential backup repetitions")
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--audit", type=Path, metavar="OUTPUT_PATH",
                        help="check fixture count, disk space, and path overlap without writing")
    action.add_argument("--output", type=Path,
                        help="new directory where fixture copies and fixtures.json will be written")
    parser.add_argument("--fixture-id",
                        help="prepare or audit only this exact planned fixture ID; use one fixture at a time")
    args = parser.parse_args()
    try:
        if args.audit is not None:
            result = audit(args.plan, args.runs, args.audit, args.fixture_id)
        else:
            result = prepare(args.plan, args.runs, args.output, args.fixture_id)
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2))
    return 0 if result.get("status", "READY") == "READY" else 1


if __name__ == "__main__":
    raise SystemExit(main())
