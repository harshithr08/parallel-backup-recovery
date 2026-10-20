# IPBRS Implementation Roadmap

## Purpose

This roadmap sequences implementation of the Intelligent Parallel Backup and Selective Recovery System (IPBRS) so each stage builds on a verified foundation. The goal is to first deliver a correct small end-to-end system, then add parallel baselines and the proposed hybrid scheduler, and finally conduct reproducible experiments.

The authoritative requirements are in [MASTER_SPECIFICATION.md](MASTER_SPECIFICATION.md). This roadmap distinguishes those target requirements from current implementation; a checked item means implemented and reviewed, not merely planned.

For a concise current snapshot, see [data/project_progress.md](data/project_progress.md). It separates functional implementation from backup-matrix execution, recovery-matrix execution, and final analysis; these do not combine into a meaningful single percentage.

## Guiding order

1. Freeze design choices that affect correctness and measurements.
2. Build a sequential reference implementation.
3. Add static and conventional dynamic baselines.
4. Add explicit work stealing and adaptive granularity; retain fixed chunks as a baseline and measure both policies.
5. Reuse the scheduler for selective recovery.
6. Freeze the experiment matrix, run it, and analyze both performance and correctness.

## Phase 0 — Scope and design freeze

### Work

- Inspect the repository, language, build system, and available OpenMP/runtime support.
- Decide catalog format, fields, versioning, and how a completed backup point is committed.
- Specify path rules, exclusions, symbolic links, permissions, overwrite behavior, and how source files changing during backup are handled.
- Confirm which stages are genuinely parallel in the implementation: discovery, copying, hashing, verification, and recovery.
- Use the master specification's fixed main dataset sizes: 10,000, 50,000, 100,000, and 250,000 files. Do not substitute the earlier slide set.
- Configurable `low`, `medium`, and `high` heterogeneity profiles are implemented; profile size/type weights are explicit and duplicate ratio remains a separate user-selected config value.
- Define file-size distributions, file-type mix, duplicate ratio, damage percentages, and repetition count.
- Define the cost estimate, partition method, and work-stealing ownership/synchronization.
- The initial adaptive rule is implemented: cost coefficient of variation sets the starting chunk and remaining-work fraction scales it down, within configured bounds. Measure it against fixed chunks before making performance claims.
- Define timing boundaries and which stages are included in end-to-end metrics.

### Deliverable and exit criteria

A short design specification lists decisions, command examples, catalog schema, scheduler policies, experiment variables, and feature claims. No planned feature is described as implemented until it exists and is verified.

## Phase 1 — Reproducible workload foundation

### Work

- Define shared file-task, catalog-entry, and operation-result records.
- Add configuration parsing and validation.
- Build deterministic dataset generation with a recorded seed and manifest; the generator supports the exact main counts of 10,000, 50,000, 100,000, and 250,000 files.
- Build damage injection for missing, modified, corrupted, and truncated cases after a verified backup.
- Log exact damaged paths and exclude dataset/damage setup from timing.

### Exit criteria

A workload can be recreated from its configuration and seed. Manifest counts, sizes, and damage set can be validated independently.

## Phase 2 — Sequential correctness path

### Work

- Discover eligible files and normalize their paths.
- Copy files sequentially to the backup location.
- Compute SHA-256 and write catalog records.
- Verify the backup independently against the source reference.
- Compare current files to the catalog and classify unchanged, missing, modified, or inaccessible files.
- Restore selected files sequentially and verify each restored result.
- Report per-file failures and aggregate correctness results.

### Exit criteria

A clean backup verifies. Healthy files are skipped. Every injected damage type is detected and restored to the expected digest. Failure cases remain failures in the output summary. This implementation is the correctness oracle for parallel modes.

## Phase 3 — Measurement and baseline schedulers

### Work

- Add run IDs and machine-readable records for configuration, stage times, total time, bytes/files, errors, and correctness.
- Record worker busy and estimated idle time, task counts, and child-process CPU time/utilization where supported; keep per-worker idle time labeled as an estimate.
- Implement static cost-based assignment with no stealing/adaptation.
- Implement conventional dynamic scheduling with a fixed, documented chunk size.
- Preserve the same file operations, catalog semantics, and verification for all policies.

### Exit criteria

Sequential, static, and dynamic modes produce equivalent verified output. Each run can be traced to its input workload and configuration.

## Phase 4 — Runtime work stealing

### Work

- Implement the selected worker-queue ownership model.
- Allow an idle worker to acquire unclaimed outstanding work when its local work is exhausted.
- Prevent duplicate claims, lost tasks, deadlocks, and unsafe catalog updates.
- Record steal attempts/successes, wait time, and per-worker idle/busy time.
- Measure worker idleness and queue/lock wait where practical; current metrics do not yet cover all of these values.

### Exit criteria

Every task reaches one terminal result under the chosen retry policy. Output matches the sequential reference. Scheduler metrics are available.

## Phase 5 — Parallel selective recovery

### Work

- Reuse the verified catalog and the same sequential/static/dynamic/work-stealing scheduler policies.
- Compare current files with catalog entries and schedule only missing or changed files for restore.
- Restore through temporary files and verify SHA-256 before replacing targets.
- Report recovered, skipped, and failed files and bytes; never delete extra files implicitly.
- Run isolated selective-recovery correctness checks for every C++ scheduler policy against the same missing/modified/truncated fixture.

### Exit criteria

Selective recovery restores damaged test files to their cataloged digests, skips unchanged files, reports failures accurately, and matches the sequential reference behavior. A small isolated five-policy pilot passes for missing, modified, and truncated files; full-scale and full-recovery comparisons remain pending.
## Phase 6 — Pilot and focused experiments

### Work

- Run a small pilot to check scale, runtime, storage limits, instrumentation, and repeatability.
- Subsequent protocol-scale backup runs are retained for 10K low/medium/high and 50K low in `data/final-n*-results/`: 424 of 1,272 planned backup attempts (33.3%). Each workload has 53 scheduler/chunk groups, two repetitions per group (106 attempts), all five policies and workers 1/2/4/8; summaries report zero failed runs and zero independent-verifier missing/size/digest/inaccessible/unsafe findings. These runs use the still-provisional protocol, so they are not frozen final results.
- 50K medium/high and all 100K/250K backup matrices remain outstanding. Recovery matrices remain outstanding at all scales.
- Small pilots compare fixed/adaptive granularity (27 runs) and full recovery across all five policies; final-scale repetitions remain required.
- Freeze the final matrix and repetition count before final runs. The current protocol file is marked provisional and must not be treated as approved.
- Compare sequential, static cost-based, fixed-chunk dynamic, and cost-aware-plus-stealing IPBRS.
- Run fixed-versus-adaptive granularity configurations and ablations for cost partitioning, stealing, and granularity.
- Recovery run counts and isolated-fixture payload are estimated by policy. `src/recovery_fixtures.py` audits capacity and prepares isolated damaged fixtures from independently verified sequential backups; `src/recovery_matrix.py` audits complete mappings and creates configs. The current protocol audit estimates about 83 TB retained for backup and recovery payload and 20,352 recovery measurements; capacity and protocol decisions are unresolved, so these runs have not been launched. Of 21,624 planned backup plus recovery measurements, 424 backup measurements are complete and 21,200 remain.
- Use worker counts 1, 2, 4, and 8 when supported by the machine; record any hardware-based omission.
- Exercise low, medium, and high workload heterogeneity at each selected main dataset size.
- Evaluate backup and recovery separately; include full-recovery comparison and damage ratios.
- Record hardware, operating system, compiler, OpenMP/runtime, storage, and cache conditions.
- Preserve raw run records and frozen configuration files.

### Exit criteria

Each planned configuration has all repetitions or a documented omission reason. Every performance result is paired with correctness status, and summary statistics can be regenerated from raw data.

## Phase 7 — Analysis and reporting

### Work

- Calculate speedup, efficiency, throughput, load imbalance, idle time, scheduling overhead, and recovery files/bytes avoided.
- Plot results against worker count, workload size, heterogeneity, fixed chunk size, and damage ratio.
- Use ablation to attribute effects to cost-aware partitioning and stealing.
- [x] Benchmark summaries retain all-failure configuration groups, and the report generator produces matched fixed-versus-adaptive granularity comparisons from recorded results.
- [x] `src/matrix_status.py` audits backup and recovery coverage against the configured settings, distinguishes not-started/partial/failed/correctness-issue/complete states, reports independent-verifier findings, and labels the protocol provisional; the current audit is retained at `data/matrix_coverage.md`.
- Discuss cases where IPBRS is neutral or slower, including storage bottlenecks and small-task overhead.
- Update paper and slides so every claim matches implementation and measured evidence.

### Exit criteria

Each research question has a result and interpretation. Correctness, limitations, and unresolved production-grade concerns are stated clearly.

## First end-to-end milestone

Before implementing parallel scheduling, complete this small vertical slice:

1. Generate a small deterministic dataset.
2. Back it up sequentially and write a catalog.
3. Verify the backup.
4. Delete or modify one known file after backup.
5. Run sequential selective recovery.
6. Verify restored content and emit a machine-readable summary.

This validates the core project value before performance tuning begins.

## Benchmark readiness checklist

- [ ] Final experimental workload distributions, duplicate ratio, and repetitions frozen; current protocol values are provisional.
- [x] Generator exposes low, medium, and high heterogeneity and the four fixed main dataset sizes.
- [ ] Damage types, ratios, and seeds recorded.
- [x] C++ catalog schema 3 records source timestamp, file type, completion status, size, cost estimate, digest, and a separate payload root; schema 1/2 direct-payload snapshots remain readable.
- [x] Cost model and LPT partitioner documented in `IMPLEMENTATION_SPEC.md`.
- [ ] Baseline policies and final chunk-size values frozen.
- [x] Work stealing implemented and logged.
- [ ] Timing boundaries and repetition count frozen.
- [x] Adaptive granularity rule and logging implemented.
- [ ] Fixed/adaptive granularity ablation matrix run and frozen; only the 27-run small pilot exists.
- [x] Worker busy and estimated idle time are recorded; child-process CPU time/utilization is captured where supported and idle time is labeled as an estimate.
- [x] All five C++ policies passed independent backup verification and a same-fixture selective-recovery pilot for missing, modified, and truncated files.
- [x] Full-recovery pilot and 10K-per-profile backup preflight completed; pilot analysis retained, with final protocol still unfrozen.
- [x] Provisional protocol-scale backup matrix completed for 10K low/medium/high and 50K low (4 workloads × 106 attempts; zero failed groups); artifacts and summaries are retained under `data/final-n*-results/`.
- [x] Recovery-matrix config generator audits coverage, fixture isolation, damage logs, and catalog root bindings from prepared fixtures.
- [x] Recovery-fixture preparer verifies manifest/catalog consistency, creates isolated source copies and hard-linked snapshot views, then records deterministic damage manifests.
- [ ] Full-recovery and final-scale recovery policy comparisons completed.
- [ ] Hardware/software environment recorded.
- [x] Report generator creates Markdown tables, configuration-separated SVG metric charts, fixed-versus-adaptive comparisons, and per-policy run coverage from benchmark summaries; final-matrix analysis is still pending.
- [x] Pilot raw data, frozen matrix, and regenerable summary retained; final-matrix retention remains required.

## Optional Extension: Local Web Interface

Add this only after the CLI backup, verification, catalog, and selective recovery workflows produce reliable results. It is a usability and demonstration layer; the command-line engine and benchmark experiments remain independently usable. Do not let this extension delay the core evaluation.

### Suggested staged work

1. **Define the boundary:** specify configured source/destination IDs, job states, progress fields, error reporting, and recovery confirmation. Keep the API local-only (`127.0.0.1`) for the prototype.
2. **Build a status view:** display recent jobs, state, elapsed time, files/bytes processed, verification outcome, and worker statistics from engine output or a job-status store.
3. **Add backup controls:** choose from configured roots, review settings, start a job, and follow its progress. Prevent concurrent operations if the engine or catalog cannot safely support them.
4. **Add recovery controls:** choose a catalog snapshot and destination, preview the selected scope, require explicit confirmation, and report recovered/skipped/failed files.
5. **Review safeguards and usability:** validate path boundaries, error handling, restart behavior, and browser-to-local-service protections before considering any network-accessible deployment.

**Completion criteria:** the interface can start and monitor a backup and a confirmed selective recovery using the existing engine; displays truthful status and results; and cannot direct the engine outside configured roots. Its results must agree with CLI operation summaries. This phase is optional and does not replace core correctness or performance measurements.






