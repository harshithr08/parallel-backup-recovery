"""Selective-recovery experiment.

Each workload gets one reference backup, reused by every run. Before each run
the same damage (fixed seed per workload and ratio, all four damage types) is
applied to the source outside the timing; the timed recover then repairs it.
Modes:
  block  selective, restores only damaged blocks of same-size files
  file   selective, restores whole damaged files
  full   baseline, restores every cataloged file without inspecting the source
Every run must leave the source fully repaired (post-verify clean); otherwise
the experiment stops, because later runs would start from a broken tree.

usage: python bench/run_recovery.py [--workloads ...] [--modes block file full]
           [--ratios 0.05 0.2] [--threads 4 16] [--splits none adaptive] [--reps 3]
"""

from __future__ import annotations

import argparse
import json
import random
import time
from datetime import datetime, timezone
from pathlib import Path

from common import DEFAULT_DATA, REPO, EngineError, environment, remove_tree, run_engine
from results import append_run, load_runs, summarize, write_csv
from workloads import WORKLOADS, is_current, manifest_path, source_dir

KEYS = ("workload", "mode", "ratio", "policy", "threads", "split")
BASE_KEYS = ("workload", "mode", "ratio")
DAMAGE_SEED = 7


def reference_backup(root: Path, workload: str) -> Path:
    """Creates (once) the backup every recovery run restores from."""
    dest = root / workload / "ref-backup"
    stamp = dest.with_suffix(".json")
    identity = manifest_path(root, workload).read_text()
    if (dest / "catalog.ipbrs").is_file() and stamp.is_file() and json.loads(stamp.read_text()) == identity:
        return dest
    remove_tree(dest)
    print(f"{workload}: creating reference backup ...", flush=True)
    run_engine("backup", "--source", str(source_dir(root, workload)), "--backup", str(dest))
    stamp.write_text(json.dumps(identity))
    return dest


def repair(src: Path, backup: Path) -> dict:
    result = run_engine("recover", "--source", str(src), "--backup", str(backup), allow_status=(0, 2))
    if result["failed_files"] or result["verify_bad"]:
        raise SystemExit(f"could not repair {src}: {result['failed_files']} failed, {result['verify_bad']} unverified;"
                         " regenerate it with bench/workloads.py --force")
    return result


def configurations(args) -> list[dict]:
    configs = []
    for workload in args.workloads:
        for mode in args.modes:
            for ratio in args.ratios:
                base = {"workload": workload, "mode": mode, "ratio": ratio}
                configs.append({**base, "policy": "seq", "threads": 1, "split": "none"})
                for threads in args.threads:
                    for split in args.splits:
                        configs.append({**base, "policy": "steal", "threads": threads, "split": split})
    return configs


def run_key(config: dict, rep: int) -> tuple:
    return tuple(config[k] for k in KEYS) + (rep,)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=DEFAULT_DATA)
    parser.add_argument("--workloads", nargs="+", default=list(WORKLOADS), choices=sorted(WORKLOADS))
    parser.add_argument("--modes", nargs="+", default=["block", "file", "full"], choices=["block", "file", "full"])
    parser.add_argument("--ratios", nargs="+", type=float, default=[0.05, 0.2])
    parser.add_argument("--threads", nargs="+", type=int, default=[4, 16])
    parser.add_argument("--splits", nargs="+", default=["none", "adaptive"], choices=["none", "adaptive"])
    parser.add_argument("--reps", type=int, default=3)
    parser.add_argument("--settle", type=float, default=2.0)
    parser.add_argument("--seed", type=int, default=1, help="shuffle seed for run order")
    parser.add_argument("--out", type=Path, default=REPO / "results" / "recovery.jsonl")
    args = parser.parse_args()

    for workload in args.workloads:
        if not is_current(args.root, workload):
            raise SystemExit(f"workload {workload} missing; run: python bench/workloads.py --only {workload}")

    done = {run_key(r["config"], r["rep"]) for r in load_runs(args.out)}
    plan = [(c, rep) for c in configurations(args) for rep in range(1, args.reps + 1)]
    random.Random(args.seed).shuffle(plan)
    todo = [(c, rep) for c, rep in plan if run_key(c, rep) not in done]
    print(f"{len(plan)} runs planned, {len(plan) - len(todo)} already recorded, {len(todo)} to run")

    env = environment()
    backups: dict[str, Path] = {}
    for index, (config, rep) in enumerate(todo, 1):
        workload = config["workload"]
        src = source_dir(args.root, workload)
        if workload not in backups:
            backups[workload] = reference_backup(args.root, workload)
            repair(src, backups[workload])  # untimed: start clean and warm the cache
        damage = run_engine("damage", "--target", str(src), "--ratio", str(config["ratio"]),
                            "--seed", str(DAMAGE_SEED))
        time.sleep(args.settle)
        cmd = ["recover", "--source", str(src), "--backup", str(backups[workload]), "--mode", config["mode"],
               "--sched", config["policy"], "--threads", str(config["threads"]), "--split", config["split"]]
        record = {"config": config, "rep": rep, "damage": damage, "environment": env,
                  "timestamp": datetime.now(timezone.utc).isoformat(timespec="seconds")}
        try:
            result = run_engine(*cmd, allow_status=(0, 2))
        except EngineError as error:
            record["error"] = str(error)
            append_run(args.out, record)
            raise SystemExit(f"{config} rep {rep}: {error}")
        record["result"] = result
        record["verified"] = result["failed_files"] == 0 and result["verify_bad"] == 0
        append_run(args.out, record)
        print(f"[{index}/{len(todo)}] {workload:5} {config['mode']:5} q={config['ratio']:<4} {config['policy']:5} "
              f"t={config['threads']:<2} split={config['split']:8} rep {rep}: {result['total_ns'] / 1e9:6.2f} s, "
              f"restored {result['restored_bytes'] / 2**20:7.1f} MiB", flush=True)
        if not record["verified"]:
            repair(src, backups[workload])
            raise SystemExit("recovery left damage behind; stopped (record kept for diagnosis)")

    runs = [r for r in load_runs(args.out) if "result" in r]
    metric = lambda r: r["result"]["total_ns"] / 1e9  # noqa: E731
    seq = {tuple(row[k] for k in BASE_KEYS): row["mean_s"]
           for row in summarize([r for r in runs if r["config"]["policy"] == "seq"], KEYS, metric)}
    extras = {
        "restored_mib": lambda r: r["result"]["restored_bytes"] / 2**20,
        "avoided_fraction": lambda r: r["result"]["bytes_avoided"] / max(1, r["result"]["catalog_bytes"]),
        "scan_s": lambda r: (r["result"].get("scan", {}).get("hash_ns", 0) +
                             r["result"].get("scan", {}).get("stat_ns", 0)) / 1e9,
        "restore_s": lambda r: r["result"]["restore_ns"] / 1e9,
    }
    rows = summarize(runs, KEYS, metric, baseline=seq, baseline_keys=BASE_KEYS, extras=extras)
    summary = args.out.with_name(args.out.stem + "_summary.csv")
    write_csv(summary, rows)
    print(f"summary: {summary}")


if __name__ == "__main__":
    main()
