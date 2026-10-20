"""Read-and-hash scaling: backup verification and damage detection.

verify  re-hashes every block of a workload's reference backup. Read-only,
        so it isolates the CPU-bound part that block splitting targets.
detect  applies seeded damage once (all four types), then times recover
        --dry-run scans of the same damaged tree. Full hashing is compared
        with --trust-mtime, which skips files whose size and mtime match and
        so cannot see silent corruption (the 'corrupted' damage type keeps
        the old mtime). The tree is repaired afterwards.

usage: python bench/run_scan.py [--workloads ...] [--experiments verify detect]
           [--threads 1 2 4 8 16] [--policies static dynamic steal] [--reps 3]
"""

from __future__ import annotations

import argparse
import random
from datetime import datetime, timezone
from pathlib import Path

from common import DEFAULT_DATA, REPO, environment, run_engine
from results import append_run, load_runs, summarize, write_csv
from run_recovery import DAMAGE_SEED, reference_backup, repair
from workloads import WORKLOADS, is_current, source_dir

KEYS = ("experiment", "workload", "policy", "threads", "split", "trust_mtime")
DETECT_RATIO = 0.05


def configurations(args, experiment: str, workload: str) -> list[dict]:
    base = {"experiment": experiment, "workload": workload, "trust_mtime": False}
    configs = [{**base, "policy": "seq", "threads": 1, "split": "none"}]
    policies = args.policies if experiment == "verify" else ["steal"]
    for policy in policies:
        for threads in args.threads:
            if threads == 1:
                continue
            for split in args.splits:
                configs.append({**base, "policy": policy, "threads": threads, "split": split})
    if experiment == "detect":
        top = max(args.threads)
        configs.append({**base, "policy": "seq", "threads": 1, "split": "none", "trust_mtime": True})
        configs.append({**base, "policy": "steal", "threads": top, "split": "adaptive", "trust_mtime": True})
    return configs


def command(experiment: str, config: dict, src: Path, backup: Path) -> list[str]:
    sched = ["--sched", config["policy"], "--threads", str(config["threads"]), "--split", config["split"]]
    if experiment == "verify":
        return ["verify", "--backup", str(backup), *sched]
    cmd = ["recover", "--source", str(src), "--backup", str(backup), "--dry-run", *sched]
    return cmd + (["--trust-mtime"] if config["trust_mtime"] else [])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=DEFAULT_DATA)
    parser.add_argument("--workloads", nargs="+", default=list(WORKLOADS), choices=sorted(WORKLOADS))
    parser.add_argument("--experiments", nargs="+", default=["verify", "detect"], choices=["verify", "detect"])
    parser.add_argument("--threads", nargs="+", type=int, default=[1, 2, 4, 8, 16])
    parser.add_argument("--policies", nargs="+", default=["static", "dynamic", "steal"],
                        choices=["static", "dynamic", "steal"])
    parser.add_argument("--splits", nargs="+", default=["none", "adaptive"], choices=["none", "adaptive"])
    parser.add_argument("--reps", type=int, default=3)
    parser.add_argument("--seed", type=int, default=1, help="shuffle seed for run order")
    parser.add_argument("--out", type=Path, default=REPO / "results" / "scan.jsonl")
    args = parser.parse_args()

    for workload in args.workloads:
        if not is_current(args.root, workload):
            raise SystemExit(f"workload {workload} missing; run: python bench/workloads.py --only {workload}")

    done = {tuple(r["config"][k] for k in KEYS) + (r["rep"],) for r in load_runs(args.out)}
    env = environment()
    rng = random.Random(args.seed)
    for workload in args.workloads:
        src = source_dir(args.root, workload)
        backup = reference_backup(args.root, workload)
        for experiment in args.experiments:
            plan = [(c, rep) for c in configurations(args, experiment, workload) for rep in range(1, args.reps + 1)]
            rng.shuffle(plan)
            todo = [(c, rep) for c, rep in plan if tuple(c[k] for k in KEYS) + (rep,) not in done]
            print(f"{workload} {experiment}: {len(todo)} of {len(plan)} runs to do", flush=True)
            if not todo:
                continue
            damage = None
            repair(src, backup)  # start clean; also warms the cache
            if experiment == "detect":
                damage = run_engine("damage", "--target", str(src), "--ratio", str(DETECT_RATIO),
                                    "--seed", str(DAMAGE_SEED))
            for index, (config, rep) in enumerate(todo, 1):
                result = run_engine(*command(experiment, config, src, backup), allow_status=(0, 2))
                record = {"config": config, "rep": rep, "result": result, "environment": env,
                          "timestamp": datetime.now(timezone.utc).isoformat(timespec="seconds")}
                if experiment == "verify":
                    record["verified"] = result["bad"] == 0
                else:
                    record["damage"] = damage
                    record["detected_files"] = result["planned_files"]
                    record["damaged_files"] = damage["damaged"]
                append_run(args.out, record)
                extra = (f"bad={result['bad']}" if experiment == "verify"
                         else f"detected {result['planned_files']}/{damage['damaged']}")
                print(f"  [{index}/{len(todo)}] {config['policy']:7} t={config['threads']:<2} "
                      f"split={config['split']:8} trust_mtime={config['trust_mtime']!s:5} rep {rep}: "
                      f"{result['total_ns'] / 1e9:6.2f} s  {extra}", flush=True)
            if experiment == "detect":
                repair(src, backup)

    runs = [r for r in load_runs(args.out) if "result" in r]
    metric = lambda r: r["result"]["total_ns"] / 1e9  # noqa: E731
    base_keys = ("experiment", "workload")
    seq = {tuple(row[k] for k in base_keys): row["mean_s"]
           for row in summarize([r for r in runs if r["config"]["policy"] == "seq" and not r["config"]["trust_mtime"]],
                                KEYS, metric)}
    extras = {"detected_fraction": lambda r: r.get("detected_files", 0) / max(1, r.get("damaged_files", 1))
              if r["config"]["experiment"] == "detect" else 1.0}
    rows = summarize(runs, KEYS, metric, baseline=seq, baseline_keys=base_keys, extras=extras)
    summary = args.out.with_name(args.out.stem + "_summary.csv")
    write_csv(summary, rows)
    print(f"summary: {summary}")


if __name__ == "__main__":
    main()
