# IPBRS completion checklist

Work through these items in order. Keep this file and `data/project_progress.md` aligned with the evidence in the raw results. Do not begin the next item until the current item is complete or an external blocker is recorded.

## 1. Freeze a feasible experiment protocol — COMPLETE

- [x] Record workload sizes, heterogeneity profiles, duplicate ratio, damage ratios/types, seeds, repetitions, policies, worker counts, and matched fixed/adaptive chunk sizes in `configs/final_experiment_protocol.json`.
- [x] Record the timing boundary: end-to-end engine operation time; independent verification outside the timed interval. Record worker idle time as an estimate and process CPU utilization as subprocess CPU time divided by wall time.
- [x] Record a one-workload-at-a-time storage plan and measured C: capacity.
- [x] Document fixture-at-a-time recovery preparation, execution, and cleanup; retain raw results and damage manifests before fixture cleanup. Tooling support is a prerequisite tracked under item 3.
- [x] Verify the staged peak-space estimate, including 20% headroom, against the available volume; the audit estimates 99,991,015,500 bytes against 545,221,451,776 bytes available.
- [x] Record unavailable environment fields as unknown rather than inferring exact hardware or cache conditions.

## 2. Complete backup measurements — IN PROGRESS

- Live continuation observed 2026-10-09: `n100000-high-s20261012` has completed with 106/106 passing records. The serial benchmark process for `n100000-low-s20261010` remains active; 33/106 attempts are logged. `dynamic-w8-c1-r1` passed in about 101.3 s. `dynamic-w1-c16-r1` passed in about 562.0 s and `dynamic-w1-c64-r2` in 537.2 s, similar slow one-worker timings; still compare CPU time and matched repetitions before interpreting them. `stealing-w2-c64-r2` passed in 287.5 s, and `stealing-w8-c64-r2` passed in 92.3 s. `stealing-w1-c64-r1` and `r2` both passed independent verification at about 1,696.8 s and 953.5 s respectively; the spread is suspicious, but neither is invalidated without wall/process-CPU audit. `stealing-w4-c16-r2` passed in 157.7 s, `dynamic-w8-c16-r2` in 102.6 s, and `adaptive-w4-c1-r1` in 155.1 s. The next shuffled cell is `dynamic-w1-c1-r1`. Last refreshed coverage is 5/12 complete workloads and 530/1,272 successful planned backup attempts. The older 50K-medium result set shows all 50,000 tasks failed with `COPY: could not create temporary backup file`; its backup path was under the synced workspace. The staged local-temp 50K-medium result set has at least one exact config-fingerprint match (`adaptive-w4-c4-r1`) that independently passed and was cleaned up. Resume logic skips successful records and retries failed records; audit the full result after this workload runs. The 100K-high `stealing-w1-c1-r1` timing (~22,403 seconds) also needs wall/process-CPU audit; preserve records and rerun any attempt proven invalid after the current batch finishes.
- [ ] Let the existing continuation batch finish without interruption, then audit its results against the frozen protocol. Its settings match the frozen matrix; retain its recorded environment metadata as captured.
- [ ] Complete every missing workload/policy/worker/chunk/repetition cell, with independent verification.
- [ ] Refresh `data/matrix_coverage.md` and retain raw logs, manifests, and configs.

## 3. Run the recovery matrix — NOT STARTED

- [ ] Prepare an independently verified sequential repetition-1 reference for each workload.
- [ ] Run selective and full recovery for every protocol damage ratio/type and scheduler cell using isolated, staged fixtures.
- [ ] Retain raw records, damage manifests, and correctness/verifier results; audit all expected cells.

## 4. Run fixed-versus-adaptive granularity ablation — NOT STARTED

- [ ] Compare matched fixed stealing and adaptive runs at the same workloads, workers, starting chunks, and repetitions.
- [ ] Preserve failed cells and summarize correctness and performance for the completed matrix.

## 5. Record final environment details — NOT STARTED

- [ ] Record processor/model and logical core count, exact compiler/build/runtime versions, OS build, storage device/filesystem, cache policy, and available-space date.
- [ ] Record hardware-based omissions and their reasons.

## 6. Regenerate analysis and reports — NOT STARTED

- [ ] Regenerate summaries, coverage, Markdown report, and charts from final raw records.
- [ ] Analyze speedup, efficiency, throughput, load imbalance, estimated idle time, scheduling overhead, and recovery work avoided.
- [ ] Explain where IPBRS helps, is neutral, or is slower; include correctness and limitations.

## 7. Finish paper and slides — NOT STARTED

- [ ] Update every result and claim from the final evidence only.
- [ ] State limitations, unresolved production concerns, and any omitted configurations.
- [ ] Cross-check paper, slides, and generated reports for consistent values and conclusions.

## 8. Close the backup-copy integrity gap — DOCUMENTATION / DESIGN OPEN

- [x] Confirm the implemented integrity path: backup records a source SHA-256 in the catalog, independently verifies the stored backup after the operation, and recovery checks the backup payload before and after streaming and verifies the restored output.
- [x] State the scope precisely in `MASTER_SPECIFICATION.md`: checks detect a bad backup copy relative to a trustworthy catalog; they do not repair a corrupted backup or protect against loss/corruption of both payload and catalog.
- [x] Keep an independently verified second copy on separate storage out of the current prototype and experiment claims; document it as a future extension only.
- [ ] If redundant copies become in scope, design independent placement, catalog/checksum protection, copy selection/fallback, repair behavior, and a corruption/loss experiment before implementation.
