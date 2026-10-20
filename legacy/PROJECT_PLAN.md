
# 14. Implementation roadmap and software modules

The target requirements are defined by [MASTER_SPECIFICATION.md](MASTER_SPECIFICATION.md). This execution plan is aligned to it and marks unimplemented target features as future work.

This section turns the research methodology into an implementation sequence. Build a small, correct end-to-end system first, then add scheduling mechanisms one at a time. This makes failures easier to locate and ensures every performance result can be compared with a known-correct reference.

## 14.1 Proposed module map

The names below are conceptual. They can map to source files, classes, or packages according to the language and repository structure selected by the team.

| Module | Responsibility | Inputs | Outputs / contract |
|---|---|---|---|
| **CLI / Configuration** | Parse commands and options for dataset generation, backup, recovery, verification, and benchmarking. Validate paths and incompatible options. | User arguments/config file | A validated operation configuration; clear error on invalid input. |
| **Dataset Generator** | Generate deterministic synthetic files, duplicate content where configured, and a manifest. Keep setup separate from timed runs. | Seed, file count, size/type distributions, duplicate ratio | Dataset tree, manifest, and generation summary. |
| **Damage Injector** | Apply controlled missing, modified, corrupted, or truncated states after a verified backup. Record every changed path. | Manifest, selected damage ratio/type, seed | Damaged source tree and damage manifest. |
| **File Discovery** | Enumerate eligible files and normalize their relative paths. Handle directory traversal and exclusion rules. | Source root, include/exclude policy | File records or manifest; traversal errors surfaced. |
| **Metadata / Validation** | Collect size, modification time, type, accessibility, and any required pre-backup validation. | Discovered paths | Validated file records and per-file status. |
| **Cost Estimator** | Assign an estimated work value from documented available features, initially file size unless code supports more. | File records | Records with estimated cost; estimator version/config recorded. |
| **Partitioner** | Create fixed initial worker assignments with approximately balanced estimated total cost. | Costed task list, worker count | Worker-local task queues; every input task assigned once. |
| **Scheduler Interface** | Abstract the policy so sequential, static, conventional dynamic, and IPBRS runs share the same processing pipeline. | Tasks, worker count, policy options | A task stream/claim interface with no duplicate or lost tasks. |
| **Static Scheduler** | Execute preassigned work without stealing, using fixed assignments. | Static partitions | Per-worker completion and timing data. |
| **Dynamic Scheduler** | Supply fixed-size chunks to workers as they become available. | Task list, fixed chunk size | Tasks claimed exactly once; fixed chunk policy recorded. |
| **IPBRS Scheduler** | Use cost-aware starting partitions, runtime work stealing, and an adaptive task-granularity rule. Keep fixed granularity as a baseline. | Costed partitions, worker progress, configured granularity policy | Completed tasks, steal events, granularity decisions, scheduling metrics. |
| **File Copy / Backup Worker** | Copy one file safely to its corresponding backup path; report bytes, duration, and errors. | Task, source root, destination root | Per-file result; no silent failures. |
| **Hash / Integrity** | Compute SHA-256 consistently for source, backup, and restored data. | File path or stream | Digest and status/error. |
| **Catalog Store** | Persist and load backup reference records. Commit only records belonging to a verified backup point. | Catalog entries and backup identifier | Durable catalog; schema/version and operation status. |
| **Recovery Classifier** | Compare current tree with catalog and classify files as unchanged, missing, changed, or inaccessible. | Catalog, current file metadata/digests | Recovery task list and classification report. |
| **Recovery Worker** | Restore one selected file from the backup copy to its original relative path, following the documented overwrite policy. | Recovery task, backup root, source root | Per-file restore result. |
| **Verifier** | Independently check expected existence, size/metadata, accessibility, and SHA-256 after backup or restore. | Expected catalog entries and output paths | Pass/fail result per file and aggregate correctness summary. |
| **Metrics / Logger** | Record stage and end-to-end timings, per-worker activity, bytes/files, idle time, steals, and errors. | Events/results from modules | Machine-readable run record and human-readable summary. |
| **Benchmark Runner** | Execute a fixed workload/policy/thread matrix repeatedly and label every result with its configuration. | Experiment matrix and run count | Raw measurements plus summary statistics. |
| **Report Generator** | Produce tables/plots from raw benchmark records without altering measurements. | Benchmark result files | Reproducible figures and summary tables. |

### Shared task record

Use one stable record format across schedulers and workers. At minimum, a task should identify the normalized relative path, source path, destination or backup path, file size, estimated cost, and a stable task identifier. Runtime fields can include assigned worker, actual bytes processed, start/end timestamps, status, checksum, and error text. Paths should be stored relative to configured roots in the catalog so backups can be moved as a unit where supported.

### Shared per-file result

Every worker should return a structured result rather than only printing a message. Include task ID, operation, success/failure, bytes read/written, elapsed time, digest where applicable, and an error category. This supports correctness checks and useful metrics without parsing console output.

## 14.2 Dependency order

```text
Configuration + shared records
              ↓
Dataset generator ──→ damage injector
              ↓
Discovery → metadata/validation → cost estimator
              ↓
Sequential processing → catalog + hashing + verifier
              ↓
Static scheduler → dynamic scheduler → IPBRS scheduler
              → adaptive-granularity policy and ablation
              ↓
Recovery classifier → recovery worker → recovery verification
              ↓
Metrics/logging → benchmark runner → analysis/reporting
```

The dataset generator and damage injector can be developed alongside configuration and shared records. The critical path is the sequential correctness path: do not optimize a parallel path before the catalog and verifier provide a trustworthy reference.

## 14.3 Execution phases and milestones

### Phase 0 — Freeze the implementable scope

**Tasks**

- Inspect the existing repository and identify language, build system, runtime, and current code.
- Choose the catalog format and schema version.
- Define path behavior, exclusions, symbolic-link handling, overwrite behavior, and treatment of files changing during a run.
- [x] Implemented parallel directory discovery and scheduler-backed independent verification; both expose actual worker counts and discovery has a separate stage timer.
- Adopt the master specification's exact main dataset sizes: 10K, 50K, 100K, and 250K files. The earlier slide size set is superseded.
- Exercise the implemented low, medium, and high heterogeneity profiles. Keep duplicate ratio and damage levels configurable and do not assign final values until the experiment protocol is frozen.
- Choose exact file-size distributions, file-type proportions, duplicate ratio, damage percentages, and repeat count.
- Evaluate the implemented adaptive task-granularity rule against fixed chunks, including overhead and load-balance measurements.


**Exit criteria**

A short design specification records these decisions, command/interface examples, and the exact feature claims the implementation will support.

### Phase 1 — Shared foundations and reproducible workloads

**Tasks**

- Define common file-task and operation-result records.
- Implement CLI/config validation and a run identifier/configuration manifest.
- Implement deterministic synthetic dataset generation with a fixed seed.
- Generate final experiment datasets using the supported 10K/50K/100K/250K counts and low/medium/high heterogeneity profiles; retain small development profiles for functional checks.
- Produce a manifest containing expected paths, sizes, types, and checksums or enough information to reproduce them.
- Implement damage injection that consumes the manifest and writes a separate damage log.

**Exit criteria**

The same seed/configuration recreates the same file set and damage selection. Dataset and damage setup are not included in benchmark timing.

### Phase 2 — Sequential reference backup and recovery

**Tasks**

- Implement file discovery and explicit exclusions.
- Implement sequential copy, digest generation, and metadata capture.
- Implement catalog write/read and backup-point status.
- Implement independent backup verification.
- Implement recovery classification for unchanged, missing, changed, and unreadable files.
- Implement sequential restore and post-restore verification.
- Report failures without marking them successful.

**Exit criteria**

Clean backup passes verification; unchanged files are skipped; each injected damage type is detected and restored; restored files match the reference digest; injected errors are visible in the report. This path becomes the oracle for parallel implementations.

### Phase 3 — Measurement and baseline schedulers

**Tasks**

- Define timing boundaries and add stage-level plus end-to-end timers.
- Record bytes, file counts, per-worker work/time, CPU/idle data where available, and failures.
- Implement static cost-based assignment using the selected estimator.
- Implement conventional dynamic scheduling with a documented fixed chunk size.
- Run both with the same file operations, catalog semantics, and verification rules as sequential mode.

**Exit criteria**

All baseline modes produce identical verified output for a given workload. Every run emits a structured result record with policy, thread count, workload ID, timing, and correctness status.

### Phase 4 — Work-stealing scheduler

**Tasks**

- Add worker-local queues or the chosen ownership structure and define synchronization/claim rules.
- Let a worker claim additional work only when its local work is exhausted and outstanding tasks remain.
- Instrument steal attempts, successful steals, task/chunk counts, lock/wait time, and per-worker idle time.
- Adaptive mode uses cost variation to set an initial chunk and remaining work to reduce it within configured limits; retain fixed granularity as a baseline and log each worker's adjustment.
- Stress-test for duplicate execution, lost work, deadlocks, and catalog update races.

**Exit criteria**

Each task completes at most once under the selected retry policy; all tasks reach a terminal status; output is equivalent to sequential output; work-stealing metrics are available. Small same-fixture selective and full-recovery pilots pass for all five C++ scheduler policies; final-scale and repeated comparisons remain part of evaluation.

### Phase 5 — Parallel selective recovery

Reuse the catalog and scheduler for recovery. Classify files as unchanged, missing, changed, or inaccessible; schedule only files that need restore; verify restored SHA-256; report skipped, recovered, and failed work. Never delete uncataloged files as a side effect.
### Phase 6 — Benchmark and ablation

**Tasks**

- Pilot a small matrix to validate runtime, instrumentation, repeatability, and storage capacity.
- Freeze final workload parameters and repetition count before full measurement.
- Run sequential, static cost-based, fixed-chunk dynamic, and cost-aware-plus-stealing IPBRS configurations.
- Compare fixed and adaptive granularity and include ablations for partitioning, stealing, and granularity.
- Run worker counts 1/2/4/8 when supported; report omitted counts and why.
- Measure backup and recovery separately; include full-recovery comparison.
- Record hardware/software environment and note cache/storage conditions.
- Preserve raw data and configuration alongside summaries.

**Exit criteria**

Every planned configuration has the required repetitions or a documented reason for omission; all benchmark runs have correctness status; raw results can regenerate reported means, standard deviations, plots, and tables.

### Phase 7 — Analysis and final presentation

**Tasks**

- Analyze speedup, efficiency, throughput, load imbalance, idle time, scheduling overhead, and recovery work avoided.
- Plot results against worker count, file count, heterogeneity, and damage ratio.
- Compare ablations to explain which mechanisms contributed and when.
- Discuss negative or neutral results, storage bottlenecks, estimator limits, and correctness failures.
- Update methodology and presentation so every claim matches implemented and measured behavior.

**Exit criteria**

Each research question is answered with a metric/result, limitations are stated, and no proposed capability is presented as completed without evidence.

## 14.4 First vertical slice

The first runnable end-to-end milestone should be deliberately small:

1. Generate a small dataset with a recorded seed.
2. Run sequential backup and write a catalog.
3. Verify the backup against the source reference.
4. Delete or modify one known file after backup.
5. Run sequential selective recovery.
6. Verify restored content and emit a machine-readable summary.

Once this works reliably, replace the sequential task source with static, dynamic, and IPBRS policies while preserving the worker and verification contracts. This vertical slice validates the project’s core value before investing in tuning.

## 14.5 Suggested repository layout

Adapt this to the existing language and build conventions rather than creating folders mechanically:

```text
src/
  app/              CLI, configuration, command dispatch
  dataset/          generator, manifest, damage injection
  filesystem/       discovery, path rules, metadata
  integrity/        SHA-256 and verification
  catalog/          catalog schema and persistence
  scheduler/        interface, sequential, static, dynamic, IPBRS
  backup/           copy workers and backup orchestration
  recovery/         classification, restore workers, orchestration
  metrics/           instrumentation and run records
  benchmark/         matrix runner and summary generation
results/             raw run records and generated summaries
configs/             frozen experiment configurations
```

Do not store generated large datasets or benchmark outputs in source control unless that is an explicit team decision. Store compact manifests, seeds, configuration files, and scripts needed to reproduce them.

## 14.6 Team work split

If the team divides implementation, agree on shared records and interfaces first. A workable split is:

- **Workload and reproducibility:** dataset generation, manifest, damage injection, benchmark configs.
- **Correctness and persistence:** catalog, hashing, verifier, sequential recovery path.
- **Scheduling and parallel execution:** common scheduler interface, static/dynamic baselines, work stealing
- **Integration and evaluation:** CLI/orchestration, metrics, end-to-end integration, benchmark runner, plots and report.

These are workstreams, not isolated components: all work should merge against shared file-task/result definitions, and the sequential reference path is the integration contract.

## 14.7 Decisions required before full implementation

1. Implementation language and OpenMP/runtime version.
2. Catalog format, atomic update/commit behavior, and backup version model.
3. Whether discovery and verification are parallel in the code.
4. Cost-estimation formula and partitioning algorithm.
5. Work-stealing queue ownership and synchronization design.

7. Exact dataset scale, file distributions, duplicate ratio, and damage levels.
8. Repetition count and timing boundaries.
9. Treatment of symlinks, permissions, extra files, path collisions, and source changes during backup.

Resolve these decisions in the design specification and configuration before collecting final benchmark results.

## Proposed Extension: Local Web Interface

A browser-based interface can make IPBRS easier to operate and demonstrate. Treat it as an optional layer over the backup and recovery engine, not as a replacement for the core C++ engine or its command-line interface. The first project milestone remains a reliable, measurable local backup and recovery implementation; web work should begin only after those operations and their results are stable.

The initial interface idea is a dashboard for a single machine. It could show configured source and backup locations, the most recent run, file counts and bytes processed, job state and progress, worker utilization, verification results, and recovery summaries. Additional screens could let an operator start a backup, inspect run history, and select a catalog snapshot and destination for recovery. These screens would expose the same operations and measurements as the engine rather than implement separate backup logic.

### Suggested interaction flow

1. The operator selects a previously configured source and backup destination.
2. The interface displays the selected settings and starts a backup job.
3. A job view reports discovery, processing, verification, completion or failure, and summary metrics.
4. For recovery, the operator selects a catalog snapshot and destination, reviews the affected files, and explicitly confirms the operation.

For the first version, keep the service on the same machine and bind it to `127.0.0.1`. Restrict operations to configured roots; do not accept arbitrary filesystem paths from browser requests. Require a clear confirmation before recovery can overwrite or replace data. Do not expose the service to a network until authentication, authorization, request protections, and safe remote path handling have been designed.

The proposed flow is: browser UI → local HTTP API → job manager → adapter to the existing C++ engine → filesystem and backup catalog. The job manager should report progress and final results in a form the UI can display. Initially, the adapter may launch the CLI executable and collect structured output; a library or persistent engine service can be considered later if process launching becomes limiting. This is a design proposal; the web application and API are not part of the current implementation unless separately scheduled.



## Resume-Focused Scope Decision

The master specification is authoritative for scope. The project targets local, single-machine shared-memory backup and selective recovery, SHA-256 integrity, and measured sequential, static, conventional dynamic, and IPBRS scheduling. IPBRS combines cost-aware partitioning, runtime work stealing, and an implemented adaptive task-granularity heuristic; fixed chunking remains a required baseline. The fixed-versus-adaptive performance ablation remains outstanding. Main workload sizes are exactly 10K, 50K, 100K, and 250K files, with low/medium/high heterogeneity and worker counts 1/2/4/8 subject to available hardware. Duplicate ratio and damage ratios remain configurable and unfrozen. Distributed/cloud/network storage, GPU/CUDA, machine learning, and unrelated features remain out of scope. The optional web interface remains a later presentation layer and is not required for the core deliverable.

Pilot provenance is retained with workload manifests and seeds, execution-order seeds, config fingerprints, raw JSONL/CSV summaries, and recovery damage manifests. Cost estimation, LPT partitioning, baseline policies, and pilot chunk settings are documented in the scheduler API and benchmark configs. Pilot environment details are recorded; the exact MSVC compiler version and processor model were unavailable.

Final experiment work remains: freeze workload/damage ratios, repetition count and timing boundaries; run repeated 10K/50K/100K/250K cells at the required heterogeneity and worker counts; and collect process CPU utilization across the final matrix. Per-run child-process CPU instrumentation is implemented. Independent verification now supports the scheduler policies and has passed a 12-file, four-worker check. Parallel discovery is implemented and passed a 512-file, 32-directory pilot across all five policies; each parallel policy used four discovery workers. Timestamp-only recovery classification and timestamp preservation are implemented in both C++ and Python reference paths and covered by focused pilots. The 10K/50K and small recovery/granularity results are preflights, not final experiments.

`src/experiment_matrix.py` and `configs/final_experiment_protocol.template.json` prepare the complete required backup-matrix configs and a separate recovery-fixture plan after protocol choices are frozen. Its audit reports run counts, estimates retained payload storage, and can compare that estimate with free space on the output drive. For backup-only measurement, `src/benchmark.py run --cleanup-backups` can remove each verified snapshot after its run, reducing peak payload storage; those snapshots are then unavailable for recovery experiments. The planner does not generate or benchmark the large workloads itself.





