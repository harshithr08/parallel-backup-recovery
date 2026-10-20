# IPBRS Coding-Level Implementation Specification

## 1. Purpose and status

This document is the implementation contract for coding IPBRS. The user-provided requirements in [MASTER_SPECIFICATION.md](MASTER_SPECIFICATION.md) are authoritative when older planning documents disagree. This spec tracks concrete module behavior, implementation status, scheduler rules, persistence semantics, and remaining acceptance criteria.

The project is a **single-machine, shared-memory multicore system** using OpenMP. The repository already contains a C++14/MSVC OpenMP engine, a Windows CryptoAPI SHA-256 implementation, a prototype C++ JSON catalog, a separate Python SQLite correctness reference, and Python workload tools. Preserve these actual choices unless an explicit implementation task changes them; do not describe the proposed profile below as the current build.

### Current implementation profile

- **Language:** C++14 for the Windows engine; Python standard library for the reference workflow and workload/benchmark tooling.
- **Parallel runtime:** MSVC OpenMP, built with the installed Visual Studio 2019 Build Tools.
- **File operations:** Windows APIs and streamed binary I/O.
- **Catalogs:** versioned C++ JSON catalog and separate Python SQLite catalog; the formats are not interchangeable.
- **C++ snapshot layout:** schema 3 stores the catalog at the snapshot root and source payload beneath `files/`; readers continue to support schema 1/2 snapshots with payload directly under the snapshot root.
- **Digest:** Windows CryptoAPI SHA-256 for C++; Python `hashlib.sha256` for the reference.
- **Configuration/results:** JSON benchmark matrix; CLI JSON results; benchmark JSONL raw records and derived CSV/JSON summaries.

These describe the repository today. The master specification defines the target behavior; do not present a planned feature or the proposed module layout as implemented until it exists and is verified.

## 2. Scope and non-goals

### In scope

- Generate reproducible synthetic local file workloads.
- Back up files to a separate destination on the same machine.
- Maintain a persistent reference catalog.
- Verify backup and restored file content with SHA-256.
- Identify and restore only missing or content-changed cataloged files.
- Compare sequential, static cost-based, fixed-chunk dynamic, and cost-aware scheduling with work stealing.
- Complete IPBRS with cost-aware partitioning, runtime work stealing, and adaptive task granularity; fixed granularity remains a baseline. The current initial adaptive rule uses estimated-cost coefficient of variation to choose its starting chunk, then shrinks it in proportion to unfinished work; configurable bounds and each worker's changes are recorded. Its performance benefit remains an empirical question.
- Measure workload sizes 10K/50K/100K/250K, low/medium/high heterogeneity, and worker counts 1/2/4/8 subject to hardware. Measure scheduling overhead and recovery work avoided.

### Out of scope for this prototype

- Distributed workers or network storage.
- Advanced backup features are excluded from this resume-focused project: deduplication, compression, encryption, retention policies, cloud integration, and network/distributed storage.
- Treating the catalog alone as protection against an attacker who can modify both catalog and backup.
- Silent deletion of extra files in the source tree.
- Claims of crash-safe production backup until commit/restart behavior has been implemented and validated.

## 3. Repository layout and compilation units

Adapt this layout to existing conventions; do not restructure an existing repository blindly.

```text
include/ipbrs/
  config.hpp
  types.hpp
  errors.hpp
  filesystem.hpp
  dataset.hpp
  damage.hpp
  cost.hpp
  scheduler.hpp
  catalog.hpp
  integrity.hpp
  backup.hpp
  recovery.hpp
  metrics.hpp
  benchmark.hpp
src/
  main.cpp
  config.cpp
  filesystem.cpp
  dataset.cpp
  damage.cpp
  cost.cpp
  scheduler_sequential.cpp
  scheduler_static.cpp
  scheduler_dynamic.cpp
  scheduler_ipbrs.cpp
  catalog_sqlite.cpp
  integrity_sha256.cpp
  backup.cpp
  recovery.cpp
  metrics.cpp
  benchmark.cpp
configs/
results/
```

Keep public types in `types.hpp`; module-specific headers should depend on those types and avoid circular dependencies. The orchestration layer coordinates modules. Workers should not print progress directly or write competing summary files.

## 4. Shared types and invariants

Use fixed-width integer types for byte counts and timestamps. Store paths relative to configured roots in persistent records; derive absolute paths only at the I/O boundary.

### 4.1 `FileRecord`

```text
FileRecord {
  task_id: stable string or 64-bit ID
  relative_path: normalized path relative to source root
  source_path: absolute or resolved input path for current run
  size_bytes: uint64
  modified_time_ns: signed integer or optional value
  file_type: normalized extension/category string
  estimated_cost: double, finite and >= 0
}
```

The stable ID should be derived from the normalized relative path plus operation/backup ID, or generated once and persisted. Do not use the absolute source path as the only identity because roots can move.

### 4.2 `CatalogEntry`

```text
CatalogEntry {
  backup_id: string
  relative_path: string
  backup_relative_path: string
  size_bytes: uint64
  source_last_write_time_100ns: uint64, Windows FILETIME ticks since 1601-01-01 UTC
  file_type: string
  sha256_hex: 64 lowercase hex characters
  status: COMPLETE | FAILED
}
```

A catalog entry is valid for recovery only when status is `COMPLETE` and backup verification passed. Store catalog schema version and creation time at catalog level. Catalog should also preserve the source-root identity as a display/reference value, but restore paths are computed from safe relative paths.

### 4.3 `TaskResult`

```text
TaskResult {
  task_id: string
  operation: BACKUP | VERIFY | RESTORE
  worker_id: integer
  status: SUCCESS | FAILED | SKIPPED
  bytes_read: uint64
  bytes_written: uint64
  started_monotonic_ns: uint64
  duration_ns: uint64
  sha256_hex: optional string
  error_code: optional stable code
  error_message: optional string
}
```

Error text is for humans; analysis should group by stable error code.

### 4.4 Global invariants

1. Never write backup output inside the source tree; reject overlapping source and backup roots after canonicalization.
2. Never allow a relative path to escape its configured root (`..`, absolute path, or platform-specific traversal).
3. Each scheduled task is claimed at most once per attempt. Retries, if later added, are new attempts with explicit IDs.
4. Catalog state is not published as a completed backup point until all required tasks finish and verification passes.
5. A restore is successful only after output verification passes.
6. Dataset generation and damage setup are outside backup/recovery timers.
7. Every run records the policy, worker count, workload ID, configuration, environment, and correctness outcome.
8. Extra source files absent from the catalog are reported/ignored by default; do not delete them.
9. Source files changing during a copy are detected when possible by comparing volume/file identity, size, and last-write time across hashing and copy; source path components are rechecked for reparse points. Unstable/unsafe tasks are not silently cataloged as a valid point.

## 5. Public module APIs

Equivalent signatures are acceptable in another language, but behavior must match.

```text
DatasetGenerator.generate(DatasetConfig) -> DatasetManifest
DamageInjector.apply(DatasetManifest, DamageConfig) -> DamageManifest
FileDiscovery.scan(root, FileSelectionPolicy) -> List<FileRecord>
MetadataCollector.enrich(List<FileRecord>) -> List<FileRecord + metadata>
CostEstimator.estimate(FileRecord, CostConfig) -> double
Partitioner.partition(tasks, worker_count) -> List<Deque<Task>>
Scheduler.run(tasks, worker_count, SchedulerConfig, worker_callback) -> SchedulerSummary
Catalog.create_backup_point(config) -> BackupTransaction
Catalog.commit_backup_point(transaction, entries) -> CommitResult
Catalog.load(backup_id) -> CatalogSnapshot
BackupWorker.process(task) -> TaskResult + CatalogEntry candidate
RecoveryClassifier.classify(snapshot, current_root) -> RecoveryPlan
RecoveryWorker.process(task) -> TaskResult
Verifier.verify(expected, actual_path) -> VerificationResult
Metrics.write_run(RunRecord) -> void
BenchmarkRunner.execute(ExperimentMatrix) -> List<RunRecord>
```

`worker_callback(task, worker_id)` executes the operation. Scheduler owns task assignment only; it must not contain copy/hash/catalog logic. This separation makes comparisons fair and lets the same scheduler run backup and restore jobs.

## 6. Module-by-module coding instructions

### 6.1 CLI and configuration

Implement commands with explicit operation boundaries:

```text
ipbrs generate --config configs/workload.json
ipbrs backup --source PATH --backup PATH --catalog PATH --policy ipbrs --threads 4
ipbrs recover --source PATH --backup PATH --catalog PATH --backup-id ID --policy ipbrs --threads 4
ipbrs verify --catalog PATH --backup PATH --backup-id ID
ipbrs damage --manifest PATH --ratio 0.05 --types missing,modified --seed 42
ipbrs benchmark --config configs/experiment.json
```

These command names are proposed; use existing command conventions if present. Validate all required options before touching files. Print a concise run ID and final summary; persist full details in results. Separate `generate` and `damage` commands from timed `backup` and `recover` commands.

Config objects should include schema version and all parameters that affect result reproduction. Reject unknown scheduler names, zero workers, nonpositive chunk bounds, malformed ratios, path overlap, and unsupported catalog versions.

### 6.2 Dataset generator

Input config fields:

```text
seed
file_count
file_types: [{name, fraction or count}]
size_profiles: [{min_bytes, max_bytes, distribution, fraction}]
duplicate_fraction
output_root
```

Algorithm:

1. Use a deterministic PRNG seeded from config.
2. Allocate exact file counts per type/profile using a documented rounding rule; ensure sum equals `file_count`.
3. Generate safe relative paths with deterministic naming, spread over a configurable number of subdirectories.
4. Generate file bytes deterministically. To create varied data without holding all content in memory, fill bounded buffers from the PRNG and stream them to disk.
5. For duplicates, choose earlier generated files and copy their bytes exactly; record the source duplicate relation in the manifest. Duplicates do not trigger deduplication.
6. Write a manifest after successful file creation, containing relative path, size, type, and optionally expected SHA-256.
7. Verify actual count and total bytes against manifest before returning success.

Do not pick final percentages or ranges in source code. Put them in versioned experiment config. Initial development may use small local values, clearly marked as smoke configuration rather than final experiment settings.

### 6.3 Damage injector

The injector must use a verified backup manifest and apply damage only to the source/test tree after backup. Use deterministic selection from seed and target ratio. The denominator must be stated: recommended default is the number of cataloged files. For mixed damage types, define whether ratio means total unique damaged files; avoid damaging the same path twice unless explicitly intended.

Operations:

- `missing`: remove or move the source file within the test dataset; never touch backup copy.
- `modified`: change bytes while preserving file existence.
- `corrupted`: alter a bounded byte range at a deterministic offset.
- `truncated`: shorten the file to a deterministic target length, including possible zero length.

Write `DamageManifest` before/after changes with path, damage type, original size/digest, seed, and result. If an operation fails, record failure and stop or mark that target unapplied; do not claim the requested ratio was achieved.

### 6.4 File discovery and path safety

**Current status:** C++ discovery uses a synchronized directory work queue and thread-safe task collection. Worker count follows the selected backup policy (one worker for sequential mode); final tasks are sorted by relative path. Reparse-point directories and files are skipped, and directory errors fail discovery rather than silently producing a partial catalog. Discovery elapsed time and actual workers used are returned and included in benchmark records and summaries.

Walk source root and produce one record per regular file according to documented symlink policy. Recommended initial policy: do not follow symlinks; record and skip them. Normalize separators to `/` for catalog identity. Sort final records by normalized relative path to make runs and logs stable.

Reject files that cannot be statted from the processing list but emit an error record. Do not silently skip inaccessible paths. Before opening any derived output path, join it to destination root, canonicalize/normalize, and verify it remains beneath that root. Create parent directories safely.

The implementation uses a thread-safe work queue of directories, a synchronized result collector, and deterministic sorting before scheduling. The source directory chain is rechecked for reparse points before each scan. Discovery remains outside the scheduler's file-processing interval and is reported as its own stage.

### 6.5 Metadata and validation

Collect size, last-write time, file type/extension, and open/read accessibility where required. Avoid treating extension as verified content type. Capture source metadata before copying. The C++ backup compares volume serial and file index from the SHA-256 and copy handles, along with size and last-write time, then rechecks the current path identity and rejects reparse points anywhere in the path chain. If identity or metadata changed, flag `SOURCE_CHANGED`; if the path becomes unsafe, flag `SOURCE_PATH_UNSAFE`. Do not commit a normal catalog entry for either case.

Validation errors belong to the file record/result and should not crash the whole run unless configuration says fail-fast. Summary must include discovered, eligible, skipped, and failed counts.

### 6.6 Cost estimator

Start with a transparent cost model:

```text
estimated_cost(file) = max(file.size_bytes, minimum_cost_unit)
```

The scale does not affect greedy partitioning; the minimum prevents zero-byte files having an undefined weight. Do not include extension multipliers without measured justification. Keep estimator ID/version in run metadata.

Actual cost can be approximated from elapsed time or bytes processed after runs for analysis, but do not train/tune the estimate on test runs without declaring that procedure. Cost estimation is a heuristic, not a prediction guarantee.

### 6.7 Partitioner

Proposed default: **Longest Processing Time (LPT) greedy assignment**.

1. Sort tasks by estimated cost descending; tie-break by stable task ID.
2. Maintain each worker’s estimated total and a min-heap keyed by total cost, tie-broken by worker ID.
3. Assign the next task to the worker with the smallest current sum.
4. Push that worker back with updated sum.
5. Store initial per-worker estimated totals for reporting.

Complexity is `O(n log n + n log P)` for `n` tasks and `P` workers. Validate that every task appears exactly once across partitions. This is the proposed concrete realization of cost-aware initial partitioning; if another partitioner is chosen, document it and keep the same invariant.

### 6.8 Scheduler interface and baselines

All policies call the same worker callback and produce the same `TaskResult` structure.

#### Sequential

Process tasks in stable path order on worker 0. This supplies the timing reference and correctness oracle.

#### Static cost-based

Run LPT partitioning once. Each worker processes only its own queue. No stealing, shared dynamic queue, or task-size change. Use OpenMP parallel region with one worker per queue, or equivalent runtime primitive.

#### Conventional dynamic

Use a stable task array and an atomic next-index or mutex-protected queue. Workers claim fixed `chunk_size` tasks. No cost-based local assignment; the configured chunk size stays fixed during the run. Record chunk size in run config.

#### IPBRS initial partition + stealing

Use the LPT partitions as worker-local queues. Each worker claims from its own queue; when empty, it visits victim queues in deterministic rotating order and steals work. The fixed policy uses the configured chunk size. The initial adaptive policy computes population coefficient of variation (CV) across estimated task costs, sets its base chunk to `ceil(configured_chunk / (1 + CV))`, clamps it to configured minimum/maximum bounds, then computes each requested chunk as `ceil(base_chunk * unfinished_tasks / total_tasks)` and clamps again. The API defaults bounds to 1 and 64; CLI and benchmark configs can override them. CV is zero when the mean estimated cost is zero. Decisions are logged when a worker's requested chunk changes, including unfinished work and whether the claim stole. This is an initial rule to evaluate, not a performance claim; retain the fixed-granularity policy for the ablation.

Maintain an atomic outstanding count incremented before publication and decremented exactly once after terminal completion. Queue locks protect queue contents; atomics do not replace ownership protection. Catch exceptions inside worker callbacks and convert them into failed `TaskResult`s so an exception cannot terminate the parallel region with tasks unaccounted for.

Record successful/failed steal count, tasks stolen, queue lock wait if instrumented, and per-worker idle time. Use a deterministic victim scan for reproducible policy behavior; runtime timings may still vary.

### 6.9 File copy / backup worker

For each task:

1. Resolve source and destination from validated roots and relative path.
2. Create destination parent directories.
3. Open source read-only and destination as a temporary sibling path.
4. Stream bytes in fixed-size buffers; count bytes and handle short reads/writes.
5. Flush and close; check all errors.
6. Compute destination SHA-256 (or hash bytes during copy and then verify destination independently; independent post-copy verification remains required).
7. Compare with expected source digest/reference state.
8. Rename temporary output to final backup path only after copy succeeds. Use platform-supported atomic rename semantics where possible.
9. Return candidate catalog entry and result. Do not directly publish a completed catalog point from individual worker threads.

Use unique temp names derived from run ID and task ID. Preserve or document metadata policy. If copy fails, remove only that task’s temporary destination; never delete existing source data.

### 6.10 Hash and integrity module

Expose streaming APIs so large files do not need to be loaded into memory:

```text
sha256_file(path) -> digest + bytes_read + status
sha256_stream(stream) -> digest + status
```

Digest is lowercase hexadecimal. Handle file-open, read, and close errors explicitly. Do not compare timestamp or size as a substitute for digest when the correctness claim requires content equality.

### 6.11 Catalog store and backup transaction

**Current status:** C++ JSON catalog schema version 2 records relative path, size, Windows source last-write FILETIME ticks, extension-derived file type, `COMPLETE` status, estimated cost, and SHA-256. Backup and recovery preserve the recorded timestamp, and timestamp-only mismatches trigger a restore. The recovery reader remains compatible with version 1 and validates required metadata in version 2. The Python SQLite reference remains a separate format and stores source mtime in its existing schema.

Recommended SQLite tables:

```sql
CREATE TABLE metadata(key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE backups(
  backup_id TEXT PRIMARY KEY,
  created_utc TEXT NOT NULL,
  source_root TEXT NOT NULL,
  status TEXT NOT NULL,
  schema_version INTEGER NOT NULL
);
CREATE TABLE files(
  backup_id TEXT NOT NULL,
  relative_path TEXT NOT NULL,
  backup_relative_path TEXT NOT NULL,
  size_bytes INTEGER NOT NULL,
  source_mtime_ns INTEGER,
  file_type TEXT,
  sha256 TEXT NOT NULL,
  status TEXT NOT NULL,
  PRIMARY KEY(backup_id, relative_path),
  FOREIGN KEY(backup_id) REFERENCES backups(backup_id)
);
```

Transaction flow:

1. Insert backup point as `IN_PROGRESS`.
2. Workers create backup files and return candidate entries; they do not mutate catalog rows.
3. Verifier checks every candidate destination against expected digest and metadata.
4. If all required files pass, write entries and set backup point `COMPLETE` in a single DB transaction.
5. If required work fails, set `FAILED` with summary; do not load it for recovery by default.

For JSON alternative, write full new catalog to a sibling temp file, flush, then atomic rename. Never append a half-written catalog in place. Catalog schema version must be validated at read time.

### 6.12 Verifier

The standalone C++ verifier schedules independent per-file catalog/hash checks with the selected policy, worker count, and chunk size. Benchmark verification runs after (and outside) the measured backup/recovery operation, using the run entry scheduler settings; its actual worker count is recorded in raw and summary results. Direct CLI invocation defaults to sequential and accepts the standard scheduler flags.

Implement one verifier used by backup and recovery. Given expected size/digest and actual path:

1. Check path exists and is a regular file under policy.
2. Check size.
3. Check accessibility/readability.
4. Compute SHA-256 and compare in constant-time or ordinary string comparison appropriate for a non-secret integrity check.
5. Return independent pass/fail details.

A source last-write timestamp mismatch is a restore trigger because the catalog treats that source metadata as part of the snapshot state. C++ and Python backups apply the captured source timestamp to the snapshot copy, and both recovery paths apply it to restored files, so a successful recovery does not repeatedly trigger on its own timestamp change. SHA-256 remains the content integrity gate; a timestamp mismatch alone is not evidence of content corruption. File-type category is inferred from the cataloged path extension rather than read from file contents. Renaming the original path makes the cataloged file missing and triggers restoration at its original path. Legacy C++ JSON schema 1 catalogs lack timestamps and retain their content-based classification behavior; the Python SQLite schema already stores source mtime.

### 6.13 Recovery classifier

Load only a `COMPLETE` backup point. For every catalog entry:

1. Resolve current path safely beneath source root.
2. If absent, classify `MISSING` and create restore task.
3. If present but cannot be read, classify `UNREADABLE`; report and follow explicit policy (recommended: attempt restore to a temp path and replace only if overwrite policy permits).
4. If present, compare size first as a cheap difference check. If size differs, classify `MODIFIED` without hashing unless metrics require digest evidence.
5. If size matches, compute SHA-256 and compare; equal is `UNCHANGED`, unequal is `MODIFIED`.
6. Record classification and why it was made.

For exact methodology consistency, one can hash all present files regardless of size; if using size short-circuit, document that size mismatch already proves content mismatch and still hash the restored result.

A timestamp mismatch triggers restoration to reproduce catalog metadata, but does not by itself classify file contents as modified; SHA-256 remains the content check. Files not in catalog are `EXTRA` and are not restored or deleted by default.

### 6.14 Recovery worker

For each selected task:

1. Verify backup source exists and matches catalog digest before using it.
2. Create safe destination parent directories.
3. Copy backup source to a temporary sibling file.
4. Verify temp file against expected size and digest.
   Recheck the backup payload identity after streaming it and reject recovery if volume, file index, size, modification time, or digest changed while copying.
5. Apply overwrite policy (recommended prototype default: replace only the damaged/missing target, after verified temp is ready; preserve an optional copy of changed target if configured).
   Capture the destination identity and digest during classification. Immediately before replacement, confirm the target is still the same file with the same content; if it changed or appeared since classification, stop with `DESTINATION_CHANGED` rather than overwriting newer data.
6. Rename temp to final path where atomic replacement is supported.
7. Recheck the committed destination path chain for reparse points, then verify the final file digest and catalog timestamp.
8. Return success only if final verification passes.

Immediately before the rename, re-resolve the destination and temporary paths under the source root and recheck each existing path component for reparse points. A path that changed or escaped during the restore fails with `UNSAFE_DESTINATION` and the temporary file is removed.

If restore fails, retain the failure record and avoid claiming success. Do not delete unrelated files or extra paths.

### 6.15 Metrics and logging

Use monotonic clocks for durations and UTC wall clock only for labels. Collect:

- discovery, metadata, scheduling, copying, hashing, catalog, verification, classification, and restore durations;
- end-to-end backup and recovery duration with declared inclusion boundaries;
- bytes and files processed;
- worker busy/idle time and per-worker completion time;
- task claim count and steal attempts/successes/tasks;
- failed/skipped/unchanged/missing/modified/extra counts;
- files and bytes restored, plus files and bytes avoided vs full recovery;
- scheduler overhead (claim, steal, synchronization time) with measurement method.

The C++ CLI reports both `scheduler_elapsed_ns` and `total_elapsed_ns`. The latter measures the operation from engine entry through return: backup includes discovery, scheduled copying and hashing, and catalog commit; recovery includes catalog loading, classification, restore work, and result aggregation; verification includes catalog loading and scheduled hashing. The benchmark runner uses total operation time for end-to-end speedup, efficiency, and throughput, while independent post-operation verification remains outside the timed interval. Per-worker idle time uses scheduler elapsed time as its capacity basis and is estimated as (`scheduler_elapsed_ns - worker_busy_ns`), so it includes waiting and scheduler/coordination gaps rather than isolating queue wait. Aggregate idle capacity is `scheduler_elapsed_ns * workers_used - sum(worker_busy_ns)`, clamped at zero. The runner records child-process user/kernel CPU time where supported and process utilization relative to one logical CPU. Summaries derive speedup and efficiency using actual `workers_used`, busy-time load imbalance, files/bytes avoided, and recovery success rate from the raw records.

Metrics definitions:

```text
speedup       = T_sequential / T_parallel
parallel_eff  = speedup / worker_count        # report ×100 if percentage
load_imbalance= (max_worker_time - mean_worker_time) / mean_worker_time
throughput    = bytes_processed / elapsed_seconds  # optionally also files/s
```

If the mean worker time is zero, mark load imbalance undefined rather than divide by zero. State whether discovery and final verification are included in the end-to-end number. Report stage times to expose where time is spent.

Write one JSON run record with schema version, run ID, workload ID, policy, workers, config, environment, timings, metrics, correctness counts, and result-file references. Write per-file rows separately when datasets are large.

### 6.16 Benchmark runner

Read a frozen matrix from config. Each combination specifies workload, heterogeneity, policy, worker count, damage ratio/type, fixed or adaptive granularity configuration, and repetition. Main workload sizes are 10K, 50K, 100K, and 250K; workers are 1, 2, 4, and 8 where hardware allows. For each run:

`src/experiment_matrix.py` prepares workload and benchmark configs from an explicit protocol. It requires unresolved repetition, duplicate/damage ratio, damage type, dataset/order/damage seed bases, directory-layout, and chunk choices rather than inheriting pilot values. The protocol's `approval_status` is `provisional`, `frozen`, or `unspecified`; final matrix materialization requires `frozen`. The audit reports structural completeness separately from approval status, and reserves `READY` for a complete, explicitly frozen protocol. Optional `environment_metadata` is copied into each generated benchmark config. Its audit reports backup and recovery run counts, estimates retained backup and isolated-fixture payload, and optionally compares that estimate to free space at an output path. `src/benchmark.py run --cleanup-backups` removes a successful backup only after independent verification, enabling a lower-storage backup-only matrix; use this only when those snapshots are not needed for recovery experiments. The planner writes the complete 12-workload backup matrix and a recovery-matrix plan. `src/recovery_fixtures.py` prepares copied workload fixtures from independently verified sequential catalogs, verifies source files against manifests, uses same-volume hard links for isolated snapshot views, injects damage, and emits fixture mappings. `src/recovery_matrix.py` validates full fixture coverage, damage logs, and C++ catalog root bindings, then writes recovery benchmark configs; neither tool starts measurements.

1. Validate source/backup/catalog roots and expected workload ID.
2. Confirm setup is ready; do not regenerate/damage inside measured execution.
3. Run backup or recovery operation.
4. Verify correctness.
5. Persist raw result before proceeding.
6. Stop or continue after a failure according to an explicit fail-fast option; never omit failure records.

Warm/cold cache policy must be consistent or explicitly labeled. Randomize or rotate policy execution order where feasible to reduce systematic thermal/cache bias. Summaries should include mean and standard deviation after repetition count is frozen.

## 7. End-to-end state machines

### 7.1 Backup

```text
CONFIG_VALIDATED
  → DISCOVERED
  → COSTED
  → SCHEDULED
  → COPYING
  → COPY_COMPLETE or COPY_FAILED
  → VERIFYING
  → BACKUP_COMMITTED or BACKUP_FAILED
```

Only `BACKUP_COMMITTED` creates a selectable completed backup ID. If any required task fails, record all task outcomes and mark backup failed unless partial-backup semantics are explicitly designed.

### 7.2 Recovery

```text
CONFIG_VALIDATED
  → CATALOG_LOADED (must be COMPLETE)
  → CLASSIFIED
  → RESTORE_TASKS_SCHEDULED
  → RESTORING
  → VERIFYING_RESTORES
  → RECOVERY_COMPLETE_WITH_COUNTS
```

A run may finish with individual failed restores; aggregate status should distinguish `COMPLETE`, `COMPLETE_WITH_FAILURES`, and `FAILED`.

## 8. Integration sequence and coding handoffs

Implement in this order; each handoff depends on stable contracts:

1. **Shared types/config/path safety.** Agree on `FileRecord`, `CatalogEntry`, `TaskResult`, error codes, root rules.
2. **Generator + manifest + damage log.** Produce reproducible inputs.
3. **Sequential backup + catalog + verifier.** Demonstrate verified reference points.
4. **Sequential recovery + classifier + verifier.** Demonstrate selective restore.
5. **Run records/metrics.** Instrument correctness path before adding concurrency.
6. **Scheduler abstraction + static + dynamic baselines.** Reuse same callback and result types.
7. **IPBRS cost partition + stealing.** Add safe queues and metrics.
8. **Adaptive granularity.** Implement and log the declared cost-variation/remaining-work rule, retaining fixed chunking as the baseline; run correctness and ablation comparisons.

8. **Parallel recovery.** Reuse scheduler, same worker and verifier contracts.
9. **Benchmark automation and analysis.** Freeze matrix and repetition plan.

At every integration, build the smallest runnable end-to-end path and retain old known-correct behavior. Avoid implementing each module against private incompatible data models.

## 9. Acceptance criteria by module

These are requirements for implementation review, not claims that tests have been run.

| Module | Acceptance condition |
|---|---|
| CLI/config | Invalid or unsafe configurations fail before modifying data; valid configuration is recorded. |
| Generator | Same seed/config recreates same manifest/content; counts and total bytes match. |
| Damage injector | Only requested source paths are changed; exact applied damage is logged; backup is untouched. |
| Discovery | Stable normalized paths; symlink/error policy is applied and reported. |
| Cost/partition | Finite costs; every task assigned once; estimated worker totals recorded. |
| Sequential backup | Output bytes and digest match reference; failures visible. |
| Catalog | Incomplete backup points cannot be selected for recovery; schema version checked. |
| Static/dynamic | Same verified output as sequential; scheduling policy parameters logged. |
| Work stealing | No lost/duplicate tasks; idle worker can obtain outstanding work; steals measured. |

| Classifier | Injected missing/changed files selected; unchanged files skipped; inaccessible paths are reported as failed and separately counted; extra regular files are counted and left untouched. |
| Recovery status | C++ recovery returns `COMPLETE`, `COMPLETE_WITH_FAILURES`, or `FAILED`, while retaining the boolean success field for compatibility. |
| Recovery worker | Restored temp and final file match catalog before success is returned. |
| Verifier | Emits separate per-file codes and aggregate counts for missing files, size mismatch, digest mismatch, access errors, and unsafe catalog paths. |
| Metrics | Units and timing boundaries stable; raw values can reproduce summary metrics. |
| Benchmark runner | Every configured run, including failure, has an immutable record. |

## 10. Configuration defaults and unresolved values

Keep these in configuration, not hard-coded logic:

- main dataset sizes: exactly 10K/50K/100K/250K (master requirement; do not substitute the earlier slide set);
- heterogeneity level: low, medium, or high, with profile definitions recorded in config;
- file type shares and file-size distributions;
- duplicate ratio;
- recovery damage percentages (slides suggest 0/1/5/10%, methodology does not finalize values);
- damage type allocation;
- worker count (planned 1/2/4/8 subject to hardware);
- repetition count;
- dynamic chunk size;
- adaptive granularity policy and any configured limits/parameters (to be frozen only after the rule is defined);

- timing inclusion rules and cache policy.

A small development config may use tiny values for iteration. Label it `development`; do not mix it with final experiment results.

## 11. Known design caveats

- OpenMP's built-in scheduler may perform runtime work distribution internally, but it does not automatically implement the specific cost-aware partition plus explicit stealing claimed here. Implement and instrument the intended behavior explicitly or narrow the claim.
- Backup throughput may become storage-bound; more threads can reduce efficiency or stop improving time.
- SHA-256 adds work. Include equivalent integrity work in scheduling comparisons or report it as a separate stage.
- Metadata may change during copy. Define source-stability detection and how such files are represented.
- SQLite is a recommendation, not a known project requirement. Confirm dependency availability before coding.
- This plan defines observable behavior, not production-grade durability, security, or fault tolerance beyond what is explicitly implemented.

## Optional Local Web Interface (Future Extension)

The web interface is not required for the core implementation or the experimental comparison. Schedule it after the command-line backup, catalog, verification, and selective recovery paths are working. Keep the CLI available so experiments can run without a browser or web service.

### Proposed components and behavior

- **UI:** overview and run history, backup setup/start, job progress/details, and recovery snapshot selection/preview/confirmation.
- **Local API:** validates operation requests and returns status, progress, and summaries. Candidate routes are `GET /api/status`, `GET /api/runs`, `POST /api/backup`, `POST /api/recovery/preview`, and `POST /api/recovery/confirm`.
- **Job manager:** tracks long-running work and maps engine output to stable job states and metrics.
- **Engine adapter:** initially starts the existing CLI as a child process and consumes machine-readable output; do not scrape human-formatted text if a structured output format can be added.
- **Path policy:** API requests refer to configured root IDs. Resolve and validate paths on the server, constrain them to allowed roots, and require an explicit confirmation before recovery can overwrite data.

For the prototype, listen only on `127.0.0.1`. A loopback binding is an initial exposure boundary, not a substitute for reviewing browser request protections. Do not enable access from other machines until authentication, authorization, request protections, and safe remote path rules are specified. The UI must show actual engine progress and results and must not report success before the engine exits successfully and required verification completes.

### Suggested integration order

1. Agree on structured engine progress and result fields.
2. Implement the local API and job lifecycle around the existing CLI.
3. Add a read-oriented dashboard and run history.
4. Add backup start and progress monitoring.
5. Add recovery preview and explicit confirmation.
6. Check that displayed summaries match CLI summaries and that requests cannot escape configured roots.

This section records a future design direction; it does not mean these components or routes already exist.








