# IPBRS Software Module Design

## Purpose and design principles

This document divides IPBRS into modules with clear responsibilities and contracts. Module names are conceptual and should be adapted to the current repository language and build conventions.

The authoritative target is [MASTER_SPECIFICATION.md](MASTER_SPECIFICATION.md). This design describes current modules plus planned modules; adaptive granularity is implemented, while its final-scale ablation and process CPU utilization remain outstanding.

Design principles:

- Keep correctness logic shared across sequential and parallel scheduling policies.
- Separate workload generation/setup from timed operations.
- Make each task and result explicit data, not console text.
- Persist enough reference metadata to make recovery decisions reproducible.
- Record scheduler behavior so claimed improvements can be explained.
- Never report a backup or restore as successful before verification.

## Architecture at a glance

```text
CLI / Configuration
        │
        ├── Dataset Generator ── Damage Injector
        │
        └── Backup / Recovery Orchestrator
                   │
          Discovery + Metadata
                   │
             Cost Estimator
                   │
        Scheduler Interface
        ├── Sequential
        ├── Static Cost-Based
        ├── Fixed-Chunk Dynamic
        └── IPBRS (cost-aware partition + work stealing + adaptive granularity)
             │                         │
        Backup Workers            Recovery Classifier
             │                         │
        Hash + Catalog             Recovery Workers
             └────────── Verifier ─────┘
                         │
                Metrics / Run Records
                         │
                Benchmark + Reports
```

## Core data contracts

Workload configuration exposes exactly 10K, 50K, 100K, and 250K main dataset sizes and named low, medium, and high heterogeneity profiles. The development workload may remain small and is not a substitute for these experiment profiles. Duplicate ratio is independently configurable. Worker-count targets are 1, 2, 4, and 8 when supported by available hardware.

### File task

A task should carry at least:

- stable task ID;
- normalized path relative to the configured source root;
- source path;
- destination or backup path;
- file size and any metadata used by the estimator;
- estimated processing cost;
- operation type (backup, verify, or restore).

Runtime fields may include worker ID, claim time, start/end time, bytes processed, result status, digest, and error details. Store relative paths in the catalog where possible so a backup can be relocated as a unit.

### Catalog entry

A catalog entry should bind a backup version to:

- original relative path;
- backup object/path;
- size and relevant timestamps/type;
- SHA-256 digest;
- backup status;
- schema/version and backup-point identifier.

Choose the physical storage format and atomic commit behavior during design. Only publish a completed backup point after its data and required verification succeed.

### Operation result

Each worker returns a structured result containing task ID, operation, success/failure, bytes read/written, elapsed time, digest if applicable, and categorized error information. This result is the source for summaries and metrics.

## Modules

### 1. CLI and configuration

**Responsibilities:** Parse commands and options for generation, backup, recovery, verification, and benchmarking; validate paths and incompatible settings; create a run ID.

**Inputs:** Command-line arguments/config file.

**Outputs:** Validated operation configuration and actionable errors.

**Must enforce:** Distinct source, backup, and output roots where required; valid worker count; explicit overwrite and exclusion behavior.

### 2. Dataset generator

**Responsibilities:** Create synthetic files with configured counts, size distributions, file types, duplicate content, and deterministic seed. Write a manifest.

**Outputs:** Dataset tree, manifest, generation summary.

**Must enforce:** Reproducibility and no inclusion of generation time in backup/recovery timing.

### 3. Damage injector

**Responsibilities:** After a verified reference backup, apply controlled missing, modified, corrupted, or truncated changes and log affected paths.

**Inputs:** Dataset manifest, damage types/ratio, random seed.

**Outputs:** Damaged tree and damage manifest.

**Must enforce:** Never damage the backup reference; setup remains outside timed intervals.

### 4. File discovery

**Responsibilities:** Enumerate eligible files, normalize relative paths, apply include/exclude rules, and surface traversal errors.

**Inputs:** Root path and traversal policy.

**Outputs:** File records or manifest.

**Must enforce:** Implementation and report agree on whether traversal is parallel. The C++ backup uses a directory work queue with synchronized result collection, skips reparse points, and sorts discovered tasks before scheduling.

### 5. Metadata and validation

**Responsibilities:** Collect size, modification time, type, accessibility, and any validation status needed before processing.

**Outputs:** Validated file records and per-file warnings/errors.

### 6. Cost estimator

**Responsibilities:** Compute estimated work using available pre-execution information. Start with file size unless other predictors are implemented and justified.

**Outputs:** Costed task records and estimator configuration/version.

**Must enforce:** Estimated cost is a scheduling heuristic, not a promise of actual duration.

### 7. Partitioner

**Responsibilities:** Distribute tasks across workers to approximately balance aggregate estimated cost.

**Inputs:** Costed task list and worker count.

**Outputs:** Worker-local initial queues.

**Must enforce:** Every task is assigned exactly once. The actual algorithm (for example, sorted greedy assignment) is documented.

### 8. Scheduler interface and policies

The orchestration layer should depend on one scheduler interface so the worker logic remains common across experiments.

**Sequential policy:** One worker processes tasks in a fixed order; serves as reference.

**Static cost-based policy:** Preassign tasks using the partitioner; no stealing, with a fixed assignment.

**Conventional dynamic policy:** Workers claim fixed-size chunks from a dynamic source; chunk size remains fixed and recorded.

**IPBRS policy:** Begin with cost-aware partitions, let idle workers steal outstanding work, and adapt task granularity using an explicit rule. Fixed chunking is the baseline. Adaptive mode uses the coefficient of variation of estimated task costs to set its initial chunk, then reduces the requested chunk with the unfinished-work fraction, within configured minimum and maximum limits. It logs each worker's chunk-size changes. Its performance remains subject to fixed-granularity ablation.

**Adaptive granularity responsibilities:** Select a task/chunk size from estimated workload-cost variation and observed remaining work, record each selected granularity and decision, and compare the policy with fixed granularity.

**Interface responsibilities:** Provide task claims, completion reporting, outstanding-work status, and scheduler metrics. Guarantee no task is silently lost or claimed twice.

### 9. Backup worker

**Responsibilities:** Copy one file to the mapped backup path and return a structured per-file result.

**Must handle:** Read/write errors, path creation, file metadata capture, cancellation/failure behavior, and source mutation policy.

**Current status:** The C++ worker compares volume serial and file index across its SHA-256 and copy handles, checks size/last-write metadata against discovery and both handles, and rechecks the source path chain for reparse points after copying. The copy handle denies concurrent write/delete sharing while it streams the file.

### 10. Hash and integrity service

**Responsibilities:** Compute SHA-256 consistently for source, backed-up, and restored files.

**Outputs:** Digest plus operation status.

**Must enforce:** Hash the bytes actually associated with the catalog/reference state; handle unreadable files as explicit failures.

### 11. Catalog store

**Responsibilities:** Persist and load entries, associate them with a backup point, track status, and provide a stable reference for recovery.

**Must enforce:** Do not expose partially completed catalog state as a valid backup. Define update atomicity and schema migration/versioning.

### 12. Recovery classifier

**Responsibilities:** Compare catalog expectations with current paths and content, classifying unchanged, missing, changed/corrupt, or inaccessible files.

**Outputs:** Recovery task list and classification counts.

**Must enforce:** Extra files absent from the catalog are handled according to a documented policy; classification itself must not silently delete data.

### 13. Recovery worker

**Responsibilities:** Restore one selected file from the backup to its original relative path according to overwrite rules.

**Outputs:** Per-file restore result.

**Must enforce:** No success status until verification; path collisions and partial writes handled safely. The C++ worker verifies the backup payload's volume, file index, size, modification time, and digest again after streaming, rejecting a payload changed during the copy. It records destination identity and digest during classification and checks them again immediately before replacement; a changed or newly appeared destination fails as `DESTINATION_CHANGED`. Immediately before commit, it also re-resolves the destination and temporary paths and rechecks existing path chains for reparse points; reject a changed or escaped path as `UNSAFE_DESTINATION`. After commit, it rechecks the destination path chain and verifies both final digest and timestamp before reporting success.

### 14. Verifier

**Responsibilities:** Independently verify backup and restored outputs using expected existence, size/metadata, accessibility, and SHA-256.

**Outputs:** Per-file pass/fail and aggregate correctness summary.

**Must enforce:** Verification is independent of scheduler success signals. Failed files remain separately counted.

**Current status:** The C++ CLI has an independent `verify` operation that schedules per-file hashes through the selected policy and worker count. Benchmark runs execute verification outside the timed operation and record verification worker counts. Recovery also verifies backup, temporary restore, and final target bytes in its worker path. Recovery classification uses existence/accessibility, cataloged last-write timestamp, size, and SHA-256. C++ schema 2 and 3 backup/restore preserve the recorded timestamp, and a timestamp-only mismatch triggers restore; schema 1 remains content-based. Schema 3 separates payload under the snapshot's `files/` directory to avoid collisions with catalog metadata, and temporary copy files use exclusive unique names. Readers continue to support schema 1/2 direct-payload snapshots. File type is inferred from the catalog path extension, so a renamed original is detected as missing.

### 15. Metrics and run logger

**Responsibilities:** Record configuration, stage/end-to-end time, throughput, worker activity, idle time, scheduling overhead, task counts, steals, bytes/files, and failures.

**Outputs:** Machine-readable run records plus a concise console summary.

**Must enforce:** Stable units and timing boundaries; preserve raw measurements.

**Current status:** Worker task/byte/busy-time and steal counters are available. The benchmark runner records estimated idle time and documents the estimate; process CPU utilization remains unavailable without an external profiler.

### 16. Benchmark runner

**Responsibilities:** Execute a frozen workload × scheduler × worker-count × damage matrix for a fixed repetition count.

**Outputs:** Raw results linked to workload and configuration IDs.

**Must enforce:** Identical workload and processing logic across scheduling comparisons; setup excluded from measured operation time.

### 17. Analysis/report generator

**Responsibilities:** Compute speedup, efficiency, load imbalance, throughput, recovery work avoided, and summary statistics; generate plots/tables from raw results.

**Must enforce:** Do not discard failed or slow runs without a documented rule; connect each result to its experiment configuration.

## Dependency and integration order

1. Shared records, configuration, and path rules.
2. Dataset generator, manifest, and damage injector.
3. Discovery, metadata, cost estimator.
4. Sequential backup, catalog, hashing, and verifier.
5. Sequential recovery classification, restore, and verification.
6. Metrics and run records.
7. Static and dynamic schedulers.
8. Work stealing with fixed task chunking as the baseline policy.
9. Adaptive task granularity with logged decisions and fixed/adaptive ablation.
10. Parallel recovery and full-recovery comparison.
11. Benchmark automation and analysis across the master dataset/worker matrix.

## Integration checkpoints

- **Checkpoint A:** Generator reproduces a dataset from seed/configuration.
- **Checkpoint B:** Sequential backup creates a verified catalog.
- **Checkpoint C:** Sequential selective restore repairs injected damage and passes digest checks.
- **Checkpoint D:** Static and dynamic modes produce equivalent outputs.
- **Checkpoint E:** Work-stealing mode completes every task exactly once and exposes steal metrics.
- **Checkpoint F:** Adaptive granularity decisions are recorded and fixed/adaptive comparisons pass the same correctness checks.

- **Checkpoint G:** Full experiment matrix is reproducible from raw records.

## Suggested source layout

Adapt to the existing repository rather than imposing this layout blindly:

```text
src/
  app/          CLI and configuration
  dataset/      generator, manifest, damage injection
  filesystem/   discovery, paths, metadata
  integrity/    hashing and verification
  catalog/      schema and persistence
  scheduler/    interface and scheduling policies
  backup/       copy workers and orchestration
  recovery/     classification and restore
  metrics/      instrumentation and run records
  benchmark/    matrix runner and analysis inputs
configs/        frozen experiment configurations
results/        raw records and generated summaries
```

Keep large generated datasets and raw bulky artifacts out of source control unless the team explicitly decides otherwise. Track compact manifests, seeds, configs, and scripts needed to reproduce results.

## Suggested parallel workstreams

- **Workload/reproducibility:** generator, manifests, damage injection, experiment config.
- **Correctness/persistence:** catalog, hashing, verifier, sequential backup/recovery.
- **Scheduling/parallel execution:** common scheduler interface, static/dynamic baselines, stealing.
- **Integration/evaluation:** orchestration, metrics, benchmark runner, plots, report.

Agree on shared records and interfaces before splitting work. The sequential reference path is the integration contract for all parallel workstreams.

## Optional Web Interface Modules

These modules are a later extension, not prerequisites for the core engine or its experiments.

| Module | Responsibility | Boundary |
|---|---|---|
| Browser dashboard | Show configured locations, job status/progress, history, verification results, and run summaries; provide backup and recovery forms. | Presents engine capabilities; contains no backup or recovery implementation. |
| Local HTTP API | Validate requests and expose status, run history, backup start, and recovery preview/confirm operations. | Bind to loopback for the prototype and accept configured root identifiers rather than arbitrary paths. |
| Job manager/status store | Start and track long-running engine operations and retain progress, exit status, and summaries. | Prevent unsupported overlapping jobs; distinguish queued, running, completed, and failed states. |
| Engine adapter | Invoke the C++ CLI and translate its structured progress/results into API job state. | Preserve CLI behavior and exit/error semantics; a library integration can be evaluated later. |
| Path and recovery policy | Resolve configured roots and enforce destination and overwrite rules. | Reject paths outside allowed roots; require explicit confirmation for destructive recovery behavior. |

**Initial interface contract idea:** `GET /api/status`, `GET /api/runs`, `POST /api/backup`, `POST /api/recovery/preview`, and `POST /api/recovery/confirm`. These are examples for design discussion, not implemented endpoints. Before any non-local deployment, define authentication, authorization, request-origin protections, and remote path policy.




