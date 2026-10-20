"""Backup scaling experiment.

For each workload, runs every (policy, threads, split) configuration for a
number of repetitions in a shuffled order, one backup at a time, deleting each
backup afterwards. Repetition 1 of every configuration is verified
independently (outside the timed operation). Results are appended to a JSON
lines file so an interrupted run resumes where it stopped; a summary CSV with
speedup over the sequential baseline is rewritten at the end.

usage: python bench/run_backup.py [--workloads small mixed huge] [--threads 1 2 4 8 16]
           [--policies static dynamic steal] [--splits none adaptive] [--reps 3]
           [--settle 2] [--out results/backup.jsonl] [--no-sync]
"""

from __future__ import annotations

import argparse
import random
import time
from datetime import datetime, timezone
from pathlib import Path

from common import DEFAULT_DATA, REPO, EngineError, environment, remove_tree, run_engine
from results import append_run, load_runs, summarize, write_csv
from workloads import WORKLOADS, is_current, source_dir

KEYS = ("workload", "policy", "threads", "split")


def configurations(args) -> list[dict]:
    configs = []
    for workload in args.workloads:
        configs.append({"workload": workload, "policy": "seq", "threads": 1, "split": "none"})
        for policy in args.policies:
            for threads in args.threads:
                for split in args.splits:
                    configs.append({"workload": workload, "policy": policy, "threads": threads, "split": split})
    return configs


def run_key(config: dict, rep: int) -> tuple:
    return tuple(config[k] for k in KEYS) + (rep,)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=DEFAULT_DATA)
    parser.add_argument("--workloads", nargs="+", default=list(WORKLOADS), choices=sorted(WORKLOADS))
    parser.add_argument("--threads", nargs="+", type=int, default=[1, 2, 4, 8, 16])
    parser.add_argument("--policies", nargs="+", default=["static", "dynamic", "steal"],
                        choices=["static", "dynamic", "steal"])
    parser.add_argument("--splits", nargs="+", default=["none", "adaptive"], choices=["none", "adaptive"])
    parser.add_argument("--reps", type=int, default=3)
    parser.add_argument("--settle", type=float, default=2.0, help="seconds to pause between runs")
    parser.add_argument("--seed", type=int, default=1, help="shuffle seed for run order")
    parser.add_argument("--out", type=Path, default=REPO / "results" / "backup.jsonl")
    parser.add_argument("--no-sync", action="store_true", help="do not flush backups to disk")
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
    warmed: set[str] = set()
    for index, (config, rep) in enumerate(todo, 1):
        workload = config["workload"]
        dest = args.root / workload / "backup"
        if workload not in warmed:  # one untimed pass to bring the source into a steady cache state
            remove_tree(dest)
            run_engine("backup", "--source", str(source_dir(args.root, workload)), "--backup", str(dest))
            warmed.add(workload)
        remove_tree(dest)
        time.sleep(args.settle)
        cmd = ["backup", "--source", str(source_dir(args.root, workload)), "--backup", str(dest),
               "--sched", config["policy"], "--threads", str(config["threads"]), "--split", config["split"]]
        if args.no_sync:
            cmd.append("--no-sync")
        record = {"config": config, "rep": rep, "sync": not args.no_sync, "environment": env,
                  "timestamp": datetime.now(timezone.utc).isoformat(timespec="seconds")}
        try:
            record["result"] = run_engine(*cmd)
        except EngineError as error:
            record["error"] = str(error)
            append_run(args.out, record)
            print(f"[{index}/{len(todo)}] {config} rep {rep}: FAILED {error}")
            continue
        if rep == 1:
            check = run_engine("verify", "--backup", str(dest), allow_status=(0, 2))
            record["verified"] = check["bad"] == 0
        append_run(args.out, record)
        seconds = record["result"]["total_ns"] / 1e9
        flag = "" if record.get("verified", True) else "  VERIFY FAILED"
        print(f"[{index}/{len(todo)}] {workload:5} {config['policy']:7} t={config['threads']:<2} "
              f"split={config['split']:8} rep {rep}: {seconds:6.2f} s{flag}", flush=True)
        remove_tree(dest)

    runs = [r for r in load_runs(args.out) if "result" in r]
    seq = {}
    for row in summarize([r for r in runs if r["config"]["policy"] == "seq"], KEYS, lambda r: r["result"]["total_ns"] / 1e9):
        seq[(row["workload"],)] = row["mean_s"]
    rows = summarize(runs, KEYS, lambda r: r["result"]["total_ns"] / 1e9, baseline=seq)
    summary = args.out.with_name(args.out.stem + "_summary.csv")
    write_csv(summary, rows)
    print(f"summary: {summary}")


if __name__ == "__main__":
    main()
