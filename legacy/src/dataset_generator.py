#!/usr/bin/env python3
"""Create a deterministic synthetic IPBRS workload from a JSON config.

This setup tool uses only the Python standard library. It writes a manifest
beside the generated tree and refuses to overwrite an existing output tree.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import random
import shutil
import sys
import time
import uuid
from pathlib import Path, PurePosixPath
from typing import Any

SCHEMA_VERSION = 1
BUFFER_BYTES = 1024 * 1024
MAIN_DATASET_SIZES = (10_000, 50_000, 100_000, 250_000)
HETEROGENEITY_PROFILES = {
    "low": {
        "file_types": [
            {"name": "document", "extension": ".docdata", "weight": 5},
            {"name": "image", "extension": ".imgdata", "weight": 2},
            {"name": "binary", "extension": ".bin", "weight": 1},
        ],
        "size_profiles": [
            {"name": "small", "min_bytes": 4096, "max_bytes": 8192, "weight": 3},
            {"name": "medium", "min_bytes": 8193, "max_bytes": 16384, "weight": 1},
        ],
    },
    "medium": {
        "file_types": [
            {"name": "document", "extension": ".docdata", "weight": 4},
            {"name": "image", "extension": ".imgdata", "weight": 3},
            {"name": "video", "extension": ".viddata", "weight": 1},
            {"name": "archive", "extension": ".arcdata", "weight": 1},
            {"name": "binary", "extension": ".bin", "weight": 1},
        ],
        "size_profiles": [
            {"name": "small", "min_bytes": 1024, "max_bytes": 8192, "weight": 6},
            {"name": "medium", "min_bytes": 8193, "max_bytes": 65536, "weight": 3},
            {"name": "large", "min_bytes": 65537, "max_bytes": 262144, "weight": 1},
        ],
    },
    "high": {
        "file_types": [
            {"name": "document", "extension": ".docdata", "weight": 2},
            {"name": "image", "extension": ".imgdata", "weight": 2},
            {"name": "video", "extension": ".viddata", "weight": 2},
            {"name": "archive", "extension": ".arcdata", "weight": 2},
            {"name": "binary", "extension": ".bin", "weight": 2},
        ],
        "size_profiles": [
            {"name": "tiny", "min_bytes": 0, "max_bytes": 1024, "weight": 4},
            {"name": "small", "min_bytes": 1025, "max_bytes": 8192, "weight": 3},
            {"name": "medium", "min_bytes": 8193, "max_bytes": 131072, "weight": 2},
            {"name": "large", "min_bytes": 131073, "max_bytes": 1048576, "weight": 1},
        ],
    },
}


def _positive_int(value: Any, name: str, *, allow_zero: bool = False) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{name} must be an integer")
    lower = 0 if allow_zero else 1
    if value < lower:
        raise ValueError(f"{name} must be >= {lower}")
    return value


def _safe_component(value: str, name: str) -> str:
    if not isinstance(value, str) or not value or value in {".", ".."}:
        raise ValueError(f"{name} must be a non-empty safe name")
    if any(ch in value for ch in "/\\"):
        raise ValueError(f"{name} must not contain path separators")
    return value


def load_config(config_path: Path) -> dict[str, Any]:
    try:
        config = json.loads(config_path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot read config {config_path}: {exc}") from exc
    if not isinstance(config, dict):
        raise ValueError("config root must be a JSON object")

    _positive_int(config.get("seed"), "seed", allow_zero=True)
    _positive_int(config.get("file_count"), "file_count")
    _positive_int(config.get("files_per_directory", 1000), "files_per_directory")
    duplicate_ratio = config.get("duplicate_ratio", 0.0)
    if isinstance(duplicate_ratio, bool) or not isinstance(duplicate_ratio, (int, float)):
        raise ValueError("duplicate_ratio must be numeric")
    if not 0 <= duplicate_ratio <= 1:
        raise ValueError("duplicate_ratio must be between 0 and 1")
    if math.floor(config["file_count"] * duplicate_ratio) >= config["file_count"]:
        raise ValueError("duplicate_ratio must leave at least one original file")

    heterogeneity = config.get("heterogeneity", "custom")
    if heterogeneity not in {"custom", *HETEROGENEITY_PROFILES.keys()}:
        raise ValueError("heterogeneity must be custom, low, medium, or high")
    if heterogeneity != "custom":
        if "file_types" in config or "size_profiles" in config:
            raise ValueError("profiled configs must omit file_types and size_profiles; use custom for overrides")
        config.update(json.loads(json.dumps(HETEROGENEITY_PROFILES[heterogeneity])))
    file_types = config.get("file_types")
    profiles = config.get("size_profiles")
    if not isinstance(file_types, list) or not file_types:
        raise ValueError("file_types must be a non-empty list")
    if not isinstance(profiles, list) or not profiles:
        raise ValueError("size_profiles must be a non-empty list")

    for item in file_types:
        if not isinstance(item, dict):
            raise ValueError("each file_types item must be an object")
        item["name"] = _safe_component(item.get("name"), "file type name")
        ext = item.get("extension", ".dat")
        if not isinstance(ext, str) or not ext.startswith(".") or any(c in ext for c in "/\\"):
            raise ValueError(f"invalid extension for {item['name']}")
        item["extension"] = ext
        weight = item.get("weight", 1)
        if isinstance(weight, bool) or not isinstance(weight, (int, float)) or weight <= 0:
            raise ValueError(f"weight for {item['name']} must be positive")
        item["weight"] = float(weight)

    for item in profiles:
        if not isinstance(item, dict):
            raise ValueError("each size_profiles item must be an object")
        item["name"] = _safe_component(item.get("name"), "size profile name")
        item["min_bytes"] = _positive_int(item.get("min_bytes"), f"{item['name']}.min_bytes", allow_zero=True)
        item["max_bytes"] = _positive_int(item.get("max_bytes"), f"{item['name']}.max_bytes", allow_zero=True)
        if item["max_bytes"] < item["min_bytes"]:
            raise ValueError(f"max_bytes must be >= min_bytes for {item['name']}")
        weight = item.get("weight", 1)
        if isinstance(weight, bool) or not isinstance(weight, (int, float)) or weight <= 0:
            raise ValueError(f"weight for {item['name']} must be positive")
        item["weight"] = float(weight)

    if sum(x["weight"] for x in file_types) <= 0 or sum(x["weight"] for x in profiles) <= 0:
        raise ValueError("weights must sum to a positive value")
    config["files_per_directory"] = config.get("files_per_directory", 1000)
    config["duplicate_ratio"] = float(duplicate_ratio)
    config["heterogeneity"] = heterogeneity
    return config


def _weighted_choice(rng: random.Random, entries: list[dict[str, Any]]) -> dict[str, Any]:
    return rng.choices(entries, weights=[entry["weight"] for entry in entries], k=1)[0]


def _random_bytes(rng: random.Random, count: int) -> bytes:
    # Using getrandbits makes bytes deterministic across a single Python runtime.
    return rng.getrandbits(count * 8).to_bytes(count, byteorder="little")


def _write_content(path: Path, size: int, rng: random.Random) -> str:
    digest = hashlib.sha256()
    remaining = size
    with path.open("xb") as stream:
        while remaining:
            chunk_size = min(BUFFER_BYTES, remaining)
            chunk = _random_bytes(rng, chunk_size)
            stream.write(chunk)
            digest.update(chunk)
            remaining -= chunk_size
    return digest.hexdigest()


def _copy_and_hash(source: Path, destination: Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    size = 0
    with source.open("rb") as src, destination.open("xb") as dst:
        while True:
            block = src.read(BUFFER_BYTES)
            if not block:
                break
            dst.write(block)
            digest.update(block)
            size += len(block)
    return digest.hexdigest(), size


def generate(config: dict[str, Any], output_root: Path) -> Path:
    output_root = output_root.expanduser().resolve()
    manifest_path = output_root.parent / f"{output_root.name}.manifest.json"
    if output_root.exists():
        raise FileExistsError(f"output directory already exists: {output_root}")
    if manifest_path.exists():
        raise FileExistsError(f"manifest already exists: {manifest_path}")
    output_root.parent.mkdir(parents=True, exist_ok=True)

    # tempfile.mkdtemp applies restrictive temporary-directory permissions on
    # some Windows environments. Use a unique name with normal inherited ACLs
    # so the generator can create its nested workload directories there.
    for attempt in range(100):
        temp_root = output_root.parent / f".{output_root.name}.building-{uuid.uuid4().hex}"
        try:
            temp_root.mkdir()
            break
        except FileExistsError:
            continue
    else:
        raise FileExistsError("could not allocate a unique dataset staging directory")
    rng = random.Random(config["seed"])
    count = config["file_count"]
    duplicate_count = math.floor(count * config["duplicate_ratio"])
    original_count = count - duplicate_count
    originals: list[dict[str, Any]] = []
    entries: list[dict[str, Any]] = []
    type_counts: dict[str, int] = {}
    profile_counts: dict[str, int] = {}
    total_bytes = 0
    started = time.perf_counter()

    try:
        for index in range(count):
            type_info = _weighted_choice(rng, config["file_types"])
            profile = _weighted_choice(rng, config["size_profiles"])
            relative = PurePosixPath(f"dir_{index // config['files_per_directory']:05d}") / (
                f"file_{index:08d}{type_info['extension']}"
            )
            destination = temp_root.joinpath(*relative.parts)
            destination.parent.mkdir(parents=True, exist_ok=True)

            duplicate_of = None
            if index >= original_count and originals:
                source_record = rng.choice(originals)
                source_file = temp_root.joinpath(*PurePosixPath(source_record["relative_path"]).parts)
                digest, size = _copy_and_hash(source_file, destination)
                duplicate_of = source_record["relative_path"]
                actual_profile_name = source_record["size_profile"]
            else:
                size = rng.randint(profile["min_bytes"], profile["max_bytes"])
                digest = _write_content(destination, size, rng)
                actual_profile_name = profile["name"]
                originals.append({
                    "relative_path": relative.as_posix(),
                    "size_bytes": size,
                    "sha256": digest,
                    "size_profile": actual_profile_name,
                })

            total_bytes += size
            type_counts[type_info["name"]] = type_counts.get(type_info["name"], 0) + 1
            profile_counts[actual_profile_name] = profile_counts.get(actual_profile_name, 0) + 1
            entries.append({
                "relative_path": relative.as_posix(),
                "size_bytes": size,
                "file_type": type_info["name"],
                "size_profile": actual_profile_name,
                "sha256": digest,
                "duplicate_of": duplicate_of,
            })

        elapsed = time.perf_counter() - started
        manifest = {
            "schema_version": SCHEMA_VERSION,
            "generator": "ipbrs_dataset_generator",
            "seed": config["seed"],
            "dataset_root_name": output_root.name,
            "config": config,
            "file_count": len(entries),
            "total_bytes": total_bytes,
            "type_counts": type_counts,
            "size_profile_counts": profile_counts,
            "duplicate_count": sum(1 for item in entries if item["duplicate_of"] is not None),
            "generation_seconds": elapsed,
            "files": entries,
        }
        staged_manifest = temp_root / "manifest.json"
        staged_manifest.write_text(json.dumps(manifest, indent=2, sort_keys=True), encoding="utf-8")

        # The manifest is outside the dataset tree in its final location.
        os.replace(temp_root, output_root)
        os.replace(output_root / "manifest.json", manifest_path)
        return manifest_path
    except Exception:
        if temp_root.exists():
            shutil.rmtree(temp_root, ignore_errors=True)
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, help="dataset configuration JSON")
    parser.add_argument("--output", type=Path, help="new dataset directory (must not exist)")
    parser.add_argument("--list-profiles", action="store_true", help="list named heterogeneity profiles and main sizes")
    args = parser.parse_args()
    if args.list_profiles:
        print(json.dumps({"heterogeneity_profiles": sorted(HETEROGENEITY_PROFILES),
                          "main_dataset_sizes": list(MAIN_DATASET_SIZES)}, indent=2))
        return 0
    if args.config is None or args.output is None:
        parser.error("--config and --output are required unless --list-profiles is used")
    try:
        config = load_config(args.config)
        manifest = generate(config, args.output)
    except (ValueError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print(f"Dataset: {args.output.expanduser().resolve()}")
    print(f"Manifest: {manifest}")
    print(f"Files: {config['file_count']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

