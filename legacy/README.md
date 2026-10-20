# IPBRS initial implementation

> **Project status (2026-10-07):** The functional prototype is implemented and the
> Python regression suite passes. Experimental completion is separate: the saved
> saved baseline matrix covers 424/1,272 backup runs (4/12 workload conditions)
> and 0/20,352 recovery runs. A continuation batch for the eight remaining backup
> workloads is currently running under the same provisional settings. Do not
> interpret these timings as final findings.
> The protocol and required storage plan are not frozen; the current retained-data
> estimate is about 83 TB. See [data/project_progress.md](data/project_progress.md).

The authoritative target requirements are in [MASTER_SPECIFICATION.md](MASTER_SPECIFICATION.md). The roadmap and module documents track implementation against that target; planned requirements are not represented as completed features.

The initial implementation has two parts:

- `src/dataset_generator.py`: deterministic synthetic workload setup.
- `src/ipbrs_reference.py`: sequential reference backup, SQLite catalog, SHA-256 verification, controlled damage, and selective/full recovery.

## Development workflow

## Automated regression tests

The Python regression suite uses only the standard library and covers deterministic dataset generation, backup verification (including timestamp-only mismatches), selective recovery, damage injection, unsafe catalog paths, benchmark configuration validation, and scheduler-matrix coverage. Run it from the repository root:

```powershell
python -m unittest discover -s tests -v
```

Generate a small dataset using a named low, medium, or high heterogeneity profile, and list the supported profiles/main experiment sizes:

```powershell
python src/dataset_generator.py --config configs/development_dataset.json --output data/dev-100
python src/dataset_generator.py --list-profiles
```

Set `heterogeneity` to `low`, `medium`, or `high` in a dataset config and omit `file_types` and `size_profiles` to use the named size/type mix. Set it to `custom` (or omit it) to provide custom weighted `file_types` and `size_profiles`. `file_count` remains configurable and supports the exact main sizes 10,000, 50,000, 100,000, and 250,000; duplicate ratio stays an explicit config value and is not chosen by the profile.

Create a backup (the catalog and backup tree must be separate from the source):

```powershell
python src/ipbrs_reference.py backup --source data/dev-100 --backup data/dev-100-backup --catalog data/dev-catalog.sqlite
```

Each run writes into its own `data/dev-100-backup/<backup_id>/` snapshot directory, so a later backup does not overwrite an earlier snapshot. The command prints JSON containing the `backup_id`. Use that ID for verification and recovery:

```powershell
python src/ipbrs_reference.py verify --backup data/dev-100-backup --catalog data/dev-catalog.sqlite --backup-id BACKUP_ID
python src/ipbrs_reference.py damage --manifest data/dev-100.manifest.json --ratio 0.05 --types missing,modified,corrupted,truncated --seed 42
python src/ipbrs_reference.py recover --source data/dev-100 --backup data/dev-100-backup --catalog data/dev-catalog.sqlite --backup-id BACKUP_ID
```

Use `--full` on `recover` to restore every cataloged file for the full-recovery comparison. The development configuration is intentionally small and is not a final benchmark configuration.

## Current implementation status

The ordered completion checklist is in [TODO.md](TODO.md). Work through its items in order and update the checklist only from retained evidence.

For completion status by workstream and the exact experiment coverage, see [data/project_progress.md](data/project_progress.md) and the generated [data/matrix_coverage.md](data/matrix_coverage.md). There is no single reliable project percentage because implementation, workload, and recovery measurements are different units.

- [x] Deterministic local synthetic dataset generator with manifest and per-file SHA-256.
- [x] Sequential backup, SQLite reference catalog, backup verification, damage injection, selective/full restore, and result JSON.
- [x] Run the sequential workflow on a disposable development dataset and review its behavior (20 files; independent verification passed before and after four injected damage cases).
- [x] C++ scheduler API plus sequential, LPT static cost-based, fixed-chunk dynamic, and LPT-seeded work-stealing policies.
- [x] C++ Windows backup path using the scheduler, CryptoAPI SHA-256, post-copy verification, and JSON catalog.
- [x] C++ selective/full recovery from its own JSON catalog, with pre-restore and post-restore SHA-256 checks.
- [x] Isolated C++ selective-recovery correctness pilot across sequential, static, dynamic, stealing, and adaptive policies; all restored the same three damaged files and passed independent catalog verification.
- [x] Standalone C++ JSON-catalog verification operation, used by benchmark records after C++ operations.
- [x] Parallel C++ directory discovery with reparse-point exclusion, synchronized collection, deterministic task ordering, and discovery timing/worker metrics.
- [x] IPBRS cost-aware partitioning and fixed-chunk work stealing.
- [x] Adaptive work-stealing granularity with a configurable cost-variation and remaining-work rule; decisions are logged.
- [ ] Fixed-versus-adaptive performance ablation on the required workload matrix.
- [x] Configurable low/medium/high heterogeneity profiles and support for the exact final-scale workload sizes (10K, 50K, 100K, 250K).
- [ ] Generate and validate final-scale workloads and complete their benchmark matrix.
- [x] Benchmark runner and analysis (prepared C++ CLI matrix; five-policy backup and selective-recovery pilots passed with regenerable JSON/CSV summaries).
- [x] Full-recovery pilot across all five C++ policies and exploratory 27-run fixed/adaptive granularity pilot.
- [x] 10K-file low/medium/high scale preflight with all five backup policies (15/15 correctness and independent verification passes; one repetition per cell).
- [x] 50K-file low-heterogeneity scale preflight with all five backup policies (5/5 independent verification passes; one repetition).

The workspace contains a Python sequential correctness reference and a C++/OpenMP backup and recovery prototype. The C++ implementation currently targets the installed Visual Studio 2019 OpenMP runtime. The Python reference remains separate and can serve as an oracle; the two implementations use different catalog formats.

## Master-spec implementation gaps

These target requirements remain outstanding:

- Fixed-versus-adaptive granularity measurements and component ablations; only a small exploratory 27-run pilot is complete.
- Complete backup runs for every combination of 10K/50K/100K/250K and low/medium/high. Provisional protocol-scale backup matrices are complete for 10K low/medium/high and 50K low (424 of 1,272 planned attempts; 106 per workload, two repetitions per group, zero failed runs); 50K medium/high and all 100K/250K combinations remain outstanding.
- Main worker-count experiments at 1, 2, 4, and 8, subject to available hardware.
- Collect process CPU utilization across the final experiment matrix using the implemented child-process user/kernel CPU instrumentation. Per-worker idle time remains an estimate from engine elapsed time minus callback busy time and includes scheduler/coordination gaps.
- C++ recovery experiments across prepared damage fixtures and full-versus-selective comparisons at the final matrix scale; only isolated pilots are complete.

The current protocol remains provisional. Its audit estimates roughly 83 TB of retained backup and recovery payload and 20,352 recovery measurements; across 21,624 planned backup and recovery measurements, 424 backup measurements are complete and 21,200 remain. Recovery execution needs a feasible protocol and storage plan. The four existing protocol-scale backup result directories are `data/final-n10000-low-results`, `data/final-n10000-medium-results`, `data/final-n10000-high-results`, and `data/final-n50000-low-results`. Their run summaries carry provisional environment metadata and should not be presented as frozen final research results. Earlier pilots and preliminary analysis are in [data/pilot_analysis.md](data/pilot_analysis.md).


## Scheduler API

The C++ scheduler API is in `include/ipbrs/scheduler.hpp`, with policies in `src/scheduler.cpp`. A caller supplies tasks, scheduler configuration, and a callback; backup and recovery use that callback for their file operations. Static assignment uses longest-processing-time greedy partitions; dynamic mode claims fixed-size task chunks from an atomic index; stealing mode seeds per-worker queues with LPT cost partitions and lets idle workers claim fixed-size batches. Adaptive mode uses the same partitioning and stealing queues. It reduces the configured starting chunk by the coefficient of variation of task estimated costs, then scales that chunk down with the unfinished-task fraction, bounded by `--min-chunk` and `--max-chunk`. Results include per-task status/timing, per-worker task/byte/busy-time totals, steal attempts/successes, the cost coefficient of variation, initial adaptive chunk, and each worker's chunk-size changes. This is an initial deterministic heuristic; performance comparison remains pending.

## Benchmark runner

The final matrix config planner is `src/experiment_matrix.py`. Copy `configs/final_experiment_protocol.template.json`, fill every `null` protocol choice from the approved experiment design, set `approval_status` to `frozen` only after those choices are approved, then prepare the 12 workload configs and their benchmark matrices. The template defaults to `provisional`; the planner refuses to materialize a final matrix while the protocol is provisional or unspecified:

```powershell
Copy-Item configs\final_experiment_protocol.template.json configs\final_experiment_protocol.json
# Edit the unresolved protocol values before planning.
python src\experiment_matrix.py --protocol configs\final_experiment_protocol.json --output data\final-experiment
```

Before generating configs, audit the template or completed protocol to list unresolved choices. For a completed protocol, the audit also reports the exact run counts and estimated payload storage without creating files:

```powershell
python src\experiment_matrix.py --protocol configs\final_experiment_protocol.json --audit
```

Include `--check-space` with an output path to compare estimated source-plus-backup payload against free space on its nearest existing drive. This is advisory and excludes filesystem overhead and a safety margin; ensure additional headroom before running the matrix.

```powershell
python src\experiment_matrix.py --protocol configs\final_experiment_protocol.json --audit --check-space data\final-experiment
```

For staged recovery, use `--staged-recovery`. It estimates all generated workload sources plus one verified sequential reference snapshot and one copied recovery fixture for the current workload, then applies the protocol's safety margin. `recovery_fixtures.py --fixture-id` prepares exactly one fixture, and `recovery_matrix.py --allow-incomplete` can create its one-run config. Run, archive the damage manifest and raw record, then remove that fixture before preparing the next one.

```powershell
python src\experiment_matrix.py --protocol configs\final_experiment_protocol.json --audit --check-space data\final-experiment --staged-recovery
```

If only the backup performance matrix is being run and snapshots are not needed afterward for recovery experiments, benchmark runs can opt into deleting each successfully verified backup. The runner keeps failures and verification failures for diagnosis. The corresponding audit estimates a one-backup-at-a-time peak; this does not preserve catalog snapshots for the later recovery matrix.

```powershell
python src\experiment_matrix.py --protocol configs\final_experiment_protocol.json --audit --check-space data\final-experiment --cleanup-backups
python src\benchmark.py run --config data\final-experiment\benchmark_configs\n10000-low-s1.json --output results\n10000-low --cleanup-backups
```

The planner enforces the master specification's exact sizes, profiles, worker counts, and policies. Its audit distinguishes `structural_status` (whether required fields are filled) from `approval_status`; `status` is `READY` only when the protocol is explicitly marked `frozen`. It writes configs and a recovery-matrix plan only; it does not generate large datasets, inject damage, or launch measurements. It requires repetitions, duplicate and recovery damage ratios, damage types, dataset/order/damage seed bases, directory layout, and chunk settings instead of treating pilot values as final.

Compare saved matrix summaries with the protocol to report missing workloads/settings and failed repetitions:

```powershell
python src/matrix_status.py --protocol configs/final_experiment_protocol.json --results-root data --output data/matrix_coverage.md
```

The coverage report checks backup and recovery policy/worker/chunk groups, recovery damage ratio/type/mode, repetition counts, and independent-verifier findings. It distinguishes `NOT_STARTED`, `PARTIAL`, `FAILED`, correctness issues, and complete results. It reads `final-*-results/summary.json` and excludes older pilot directories.

The fixture preparer emits `fixtures.json` with `schema_version: 1`; each row identifies the dataset, scheduler settings, damage case, isolated source, catalog, backup, manifest, and completed damage log. The copied C++ catalog records that fixture's source root and private snapshot view. The manifest and damage log agree on the fixture tree, ratio, selected files, and damage type. Use `--fixture-id` to limit preparation and its disk-space check to one planned measurement. IDs have the form `<dataset-id>-<policy>-w<workers>-c<chunk>-q<ratio-token>-<damage-type>-r<repetition>-<mode>`; decimal points in ratio tokens become `p`. Then validate and create a one-run config:

```powershell
python src/recovery_fixtures.py --plan data/final-experiment/plan.json --runs results/backups/runs.jsonl --fixture-id n10000-low-s20261004-sequential-w1-c1-q0p05-missing-r1-selective --audit data/recovery-stage/fixture-001
python src/recovery_fixtures.py --plan data/final-experiment/plan.json --runs results/backups/runs.jsonl --fixture-id n10000-low-s20261004-sequential-w1-c1-q0p05-missing-r1-selective --output data/recovery-stage/fixture-001
python src/recovery_matrix.py --plan data/final-experiment/plan.json --fixtures data/recovery-stage/fixture-001/fixtures.json --allow-incomplete --output data/recovery-stage/config-001
python src/benchmark.py run --config data/recovery-stage/config-001/n10000-low-s20261004.json --output data/recovery-results/n10000-low-s20261004
```

The selector must match exactly one planned fixture and requires a verified sequential repetition-1 reference for its workload. Config generation validates each supplied fixture; `--allow-incomplete` permits omitted matrix entries for staged runs. Omit it for the final full-coverage audit. The benchmark checks that damage is still present and refuses retries against a repaired fixture. Before cleanup, retain the fixture's manifest and damage log alongside the raw results; each JSONL record stores those artifact paths and SHA-256 hashes.

Benchmark configs may include an `environment_metadata` object with string fields such as `storage_description`, `cache_policy`, `cache_state`, and `notes`. The final protocol planner carries this object into each generated benchmark config. The engine reports its compiler and OpenMP version; the benchmark record retains these build details alongside the operator-supplied storage and cache conditions. These values are included in run fingerprints when provided.

Once a workload has a successful, independently verified sequential repetition 1, `src/recovery_fixtures.py` prepares its recovery fixtures. It copies and verifies workload files; creates isolated snapshot views with hard links to verified backup files; rebinds each catalog to its fixture source root; applies damage outside timed runs; and writes `fixtures.json`. Its output directory must be new and on the same volume as the backups. Use `--fixture-id` and remove each fixture after archiving its logs and raw result:

```powershell
python src/recovery_fixtures.py --plan data/final-experiment/plan.json --runs results/backups/runs.jsonl --fixture-id YOUR_FIXTURE_ID --audit data/recovery-stage/YOUR_FIXTURE_ID
python src/recovery_fixtures.py --plan data/final-experiment/plan.json --runs results/backups/runs.jsonl --fixture-id YOUR_FIXTURE_ID --output data/recovery-stage/YOUR_FIXTURE_ID
python src/recovery_matrix.py --plan data/final-experiment/plan.json --fixtures data/recovery-stage/YOUR_FIXTURE_ID/fixtures.json --allow-incomplete --output data/recovery-stage/configs-YOUR_FIXTURE_ID
```

Set `timeout_seconds` at the config or individual run level to cap a long-running engine invocation. Timed-out processes are terminated and recorded as failed attempts, so they remain visible in regenerated summaries and reports.

Prepare a dataset and any recovery damage before starting measurements. Copy `configs/benchmark.example.json`, update its paths and run matrix, and use a distinct run entry for every policy, worker count, chunk size, operation, and repetition. The runner never generates or damages a workload during a measured run:

```powershell
python src/benchmark.py run --config configs/benchmark.example.json --output results/pilot --shuffle-seed 42
python src/benchmark.py summarize --input results/pilot/runs.jsonl --output results/pilot
```

Each invocation is appended and flushed to `runs.jsonl` immediately. The runner writes `summary.json` and `summary.csv` with mean and standard deviation, correctness counts, byte throughput, speedup versus sequential, parallel efficiency using workers actually used, worker busy-time imbalance, estimated idle fraction, scheduling overhead, steal counters, recovery files/bytes avoided, recovery success rate, recovery missing/modified/inaccessible file counts, and independent-verifier worker count. Mean runtime, speedup, efficiency, and throughput use end-to-end engine time; C++ records also retain scheduler-only elapsed time, which is used for worker capacity and idle-time estimates. End-to-end time includes discovery/classification and catalog work, while the runner's independent post-operation verification is outside the timed interval. Raw records include worker capacity/busy/estimated-idle time, child-process CPU time, and process CPU utilization as a percentage of one logical CPU (values may exceed 100% for multithreaded runs). CPU utilization divides user plus kernel CPU time by subprocess wall time, which includes process startup and shutdown; worker idle time remains an estimate. Each record also stores the execution-order seed, manifest-derived dataset metadata, and a fingerprint of its normalized configuration; recovery pilots retain a damage manifest with affected paths and operations. After each successful C++ backup or recovery it runs the standalone C++ catalog verifier outside the measured operation using the same selected policy, chunk, and worker count, then records the independent result. The verifier defaults to sequential when invoked directly and accepts the normal scheduler options. Python reference recovery uses its SQLite catalog and sequential verifier. It preserves source modification times in snapshots and restores and triggers recovery for timestamp-only differences. Prepare a separate damaged source/catalog fixture for each recovery matrix entry so one policy does not change the next policy's input. Resume skips only successful entries with an identical configuration fingerprint, retries prior failures, and rejects reuse of an ID after its configuration changes. Keep raw records and prepared workload manifests with experiment results.
Each invocation is appended and flushed to `runs.jsonl` immediately. The runner writes `summary.json` and `summary.csv` with mean and standard deviation, correctness counts, byte throughput, speedup versus sequential, parallel efficiency using workers actually used, worker busy-time imbalance, estimated idle fraction, scheduling overhead, steal counters, recovery files/bytes avoided, recovery success rate, recovery missing/modified/inaccessible file counts, and independent-verifier worker count. Mean runtime, speedup, efficiency, and throughput use end-to-end engine time; C++ records also retain scheduler-only elapsed time, which is used for worker capacity and idle-time estimates. End-to-end time includes discovery/classification and catalog work, while the runner's independent post-operation verification is outside the timed interval. Raw records include worker capacity/busy/estimated-idle time, child-process CPU time, and process CPU utilization as a percentage of one logical CPU (values may exceed 100% for multithreaded runs). CPU utilization divides user plus kernel CPU time by subprocess wall time, which includes process startup and shutdown; worker idle time remains an estimate. Each record also stores the execution-order seed, manifest-derived dataset metadata, and a fingerprint of its normalized configuration; recovery pilots retain a damage manifest with affected paths and operations. After each successful C++ backup or recovery it runs the standalone C++ catalog verifier outside the measured operation using the same selected policy, chunk, and worker count, then records the independent result. The verifier defaults to sequential when invoked directly and accepts the normal scheduler options. Python reference recovery uses its SQLite catalog and sequential verifier. It preserves source modification times in snapshots and restores and triggers recovery for timestamp-only differences. Prepare a separate damaged source/catalog fixture for each recovery matrix entry so one policy does not change the next policy's input. Resume skips only successful entries with an identical configuration fingerprint, retries prior failures, and rejects reuse of an ID after its configuration changes. Keep raw records and prepared workload manifests with experiment results.

Create a reproducible Markdown table and standalone SVG charts from a benchmark summary:

```powershell
python src/report.py --summary results/pilot/summary.json --output results/pilot/report
```

The report separates operation, workload, and experiment configuration in its charts and lists speedup, efficiency, throughput, worker busy-time imbalance, estimated idle fraction, scheduling overhead, steal counts, recovery work avoided, recovery success, and classifier/verifier counts. It also compares matched fixed-stealing and adaptive groups and retains configurations with no successful repetitions so failures stay visible. It does not make cross-workload performance claims automatically.

The report includes pass rates by operation and policy, based on attempts present in the summary. It does not infer which configurations were never scheduled.

Recovery classifies uncataloged regular files under the source root as `files_extra`. It reports their count in the C++ JSON result and benchmark summaries; these files are left untouched. Reparse points are skipped during this scan, matching discovery policy.


## Review notes

Recent code review tightened catalog schema validation and read-only opening for Python verification/recovery, bound operations to roots recorded in catalogs, persisted damage-injection progress/failure logs, and rejected impossible zero-byte truncation/corruption plans. The C++ scheduler rejects invalid policies/costs and refuses to claim parallelism when built without OpenMP. The sequential Python workflow and C++ five-policy backup pilot have been run on isolated generated data; all policy runs passed independent verification. Isolated 12-file selective and full-recovery pilots passed for all five policies, including independent catalog verification and per-file SHA-256 comparison. The 10K low/medium/high preflight passed for all policies. These are functional pilots only, not performance conclusions; final-scale recovery comparisons remain outstanding. Python SQLite and C++ JSON catalogs remain intentionally non-interoperable.

## C++ OpenMP backup prototype

Build on this Windows environment with Visual Studio 2019 Build Tools:

```powershell
cmd /c build_ipbrs.bat
```

The executable provides `backup`, `recover`, and `verify` operations. Each backup creates a unique snapshot folder and writes `catalog.json` only after all copied files pass SHA-256 verification. During backup, the engine compares the source file's volume/file identity across hashing and copying, validates size and last-write time against discovery metadata, and rechecks the path after copying; source replacement or observable mutation fails that task. The copy handle denies concurrent write/delete sharing while it streams. New C++ catalog schema 3 snapshots store source payload under `files/`, separate from snapshot metadata, and use unique exclusive temporary files during parallel copy. This prevents source names such as `catalog.json` or `catalog.json.tmp` from colliding with metadata. Schema 3 records `data_root: "files"`, source last-write time as Windows FILETIME ticks (100 ns units since 1601-01-01 UTC), file type, per-file `COMPLETE` status, size, estimated cost, and SHA-256. Readers remain compatible with schema 1 and 2 snapshots whose payload is stored directly under the snapshot directory. Backup snapshots and restored files retain the cataloged last-write time. Recovery restores a file when it is missing, its timestamp differs, or its size/SHA-256 differs. A file-type category is derived from its catalog path, so renaming the original path is detected as a missing catalog entry. The C++ catalog is a prototype JSON format and is not interoperable with the Python SQLite catalog. Directory reparse points are skipped during discovery. Recovery accepts only the C++ JSON catalog whose recorded source and snapshot match the supplied roots. Use `--full` to restore every cataloged file. Backup and recovery accept `--policy sequential|static|dynamic|stealing|adaptive`, `--threads`, and `--chunk`; adaptive also accepts `--min-chunk` and `--max-chunk`. `verify` independently checks every cataloged snapshot file by SHA-256:

```powershell
build\ipbrs-backup.exe verify --backup data\cpp-backups --catalog data\cpp-backups\BACKUP_ID\catalog.json
```

Verification results separate missing files, size mismatches, timestamp mismatches, digest mismatches, inaccessible files, and unsafe catalog paths in aggregate counts and per-file error codes. Legacy schema 1 catalogs without source timestamps continue to use content checks only. The benchmark record retains the same counts from independent verification, and generated reports include timestamp mismatch totals.

The MinGW GCC 6.3 toolchain compiles the scheduler sources but cannot link its OpenMP runtime here because its pthread import library is missing. The integrated executable is therefore built with the installed MSVC OpenMP runtime.

Example C++ backup invocation after building:

```powershell
build\ipbrs-backup.exe backup --source data\dev-100 --backup data\cpp-backups --policy static --threads 4
```

Use the `catalog_path` reported by the backup command to recover damaged or missing files:

```powershell
build\ipbrs-backup.exe recover --source data\dev-100 --backup data\cpp-backups --catalog data\cpp-backups\BACKUP_ID\catalog.json --policy dynamic --threads 4 --chunk 8
```

Add `--full` to overwrite every cataloged file from that snapshot. Recovery first verifies each backup file, writes to a temporary file, verifies it, then replaces the target and verifies the final file. Its JSON result reports restored, skipped, failed, and avoided work.

## Proposed Web Interface

A local browser dashboard is a planned optional extension. It could show run history, backup progress, worker and verification results, and provide controls for starting backups and confirmed selective recovery. The proposed architecture is a browser UI over a loopback-only API and job manager that invokes the existing C++ engine. The core CLI remains the primary interface for implementation and experiments. The web interface is not yet implemented; see the project idea, roadmap, module design, and coding specification for the proposed scope and safeguards.



## Resume-focused project scope

The master specification targets single-machine shared-memory local backup and selective recovery with SHA-256 integrity, sequential/static/dynamic baselines, and an IPBRS scheduler combining cost-aware partitioning, runtime work stealing, and adaptive task granularity. The adaptive heuristic is implemented and logged, while its fixed-versus-adaptive performance ablation remains outstanding. The required main workload sizes are exactly 10K, 50K, 100K, and 250K files; heterogeneity levels are low/medium/high and worker counts are 1/2/4/8 subject to hardware. Duplicate and damage ratios remain configurable and must be frozen in the experiment protocol. Distributed/cloud/network storage, GPU/CUDA, machine learning, and unrelated features are excluded. The web interface remains an optional future presentation layer.
# Parallel-Backup-and-Recovery-System
