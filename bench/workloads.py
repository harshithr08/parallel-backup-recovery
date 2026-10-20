"""Generates the standard benchmark workloads.

  small  20,000 files of 8-16 KiB         per-file overhead dominates
  mixed  3,000 files, log-uniform 256 B-4 MiB   heterogeneous sizes
  huge   4,000 small files + 4 x 400 MiB  a few files dominate the work

Each workload is generated once with a fixed seed into <root>/<name>/src and
described in <root>/<name>/workload.json. Re-running skips workloads whose
recorded parameters still match.

usage: python bench/workloads.py [--root DIR] [--only NAME ...] [--list] [--force]
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from common import DEFAULT_DATA, free_bytes, remove_tree, run_engine

WORKLOADS = {
    "small": {"files": 20000, "profile": "low", "big_files": 0, "big_mib": 0,
              "estimate_bytes": 20000 * 12 * 1024},
    "mixed": {"files": 3000, "profile": "high", "big_files": 0, "big_mib": 0,
              "estimate_bytes": 3000 * 440 * 1024},
    "huge": {"files": 4000, "profile": "low", "big_files": 4, "big_mib": 400,
             "estimate_bytes": 4000 * 12 * 1024 + 4 * 400 * 2**20},
}
SEED = 20261020
DUPLICATE_RATIO = 0.05
SAFETY_FACTOR = 2.5  # source + one backup at a time + headroom


def source_dir(root: Path, name: str) -> Path:
    return root / name / "src"


def manifest_path(root: Path, name: str) -> Path:
    return root / name / "workload.json"


def parameters(name: str) -> dict:
    spec = WORKLOADS[name]
    return {"name": name, "files": spec["files"], "profile": spec["profile"], "seed": SEED,
            "duplicate_ratio": DUPLICATE_RATIO, "big_files": spec["big_files"], "big_mib": spec["big_mib"]}


def load_manifest(root: Path, name: str) -> dict | None:
    path = manifest_path(root, name)
    if not path.is_file():
        return None
    return json.loads(path.read_text())


def is_current(root: Path, name: str) -> bool:
    manifest = load_manifest(root, name)
    return bool(manifest) and manifest["parameters"] == parameters(name) and source_dir(root, name).is_dir()


def generate(root: Path, name: str, force: bool = False) -> dict:
    if is_current(root, name) and not force:
        print(f"{name}: up to date ({load_manifest(root, name)['result']['bytes'] / 2**20:.0f} MiB)")
        return load_manifest(root, name)
    need = int(WORKLOADS[name]["estimate_bytes"] * SAFETY_FACTOR)
    if free_bytes(root) < need:
        sys.exit(f"{name}: needs about {need / 2**30:.1f} GiB free, only {free_bytes(root) / 2**30:.1f} GiB available")
    remove_tree(root / name)
    p = parameters(name)
    print(f"{name}: generating {p['files'] + p['big_files']} files ...", flush=True)
    result = run_engine("gen", "--out", str(source_dir(root, name)), "--files", str(p["files"]),
                        "--profile", p["profile"], "--seed", str(p["seed"]), "--dup", str(p["duplicate_ratio"]),
                        "--big-files", str(p["big_files"]), "--big-mib", str(p["big_mib"]))
    manifest = {"parameters": p, "result": result}
    manifest_path(root, name).write_text(json.dumps(manifest, indent=2))
    print(f"{name}: {result['files']} files, {result['bytes'] / 2**20:.0f} MiB in {result['total_ns'] / 1e9:.1f} s")
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=DEFAULT_DATA)
    parser.add_argument("--only", nargs="+", choices=sorted(WORKLOADS))
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--force", action="store_true", help="regenerate even if up to date")
    args = parser.parse_args()

    names = args.only or list(WORKLOADS)
    if args.list:
        for name in names:
            state = "ready" if is_current(args.root, name) else "missing"
            print(f"{name:6} {state:8} {parameters(name)}")
        return
    args.root.mkdir(parents=True, exist_ok=True)
    for name in names:
        generate(args.root, name, args.force)


if __name__ == "__main__":
    main()
