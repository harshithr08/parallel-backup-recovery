# IPBRS completion plan (harshith-dev)

Contribution: hybrid file/block work-stealing scheduler with a Merkle (per-block SHA-256) catalog.
File-level scheduling cannot finish faster than its largest file; splitting large files into
stealable block tasks removes that bound. The same tree enables parallel damage detection and
block-level selective recovery.

## Audit findings that drive the plan
- `src/scheduler.cpp` adaptive policy computes cost CV but never uses it (documented rule missing).
- No task splitting: "granularity" is files-per-claim only.
- `src/recovery.cpp` classification hashes every file serially; only restore is parallel.
- `src/backup.cpp` reads each file 4 times (hash, copy, hash temp, hash final).
- Cost model is size only; per-file overhead ignored.
- Teammate's large-scale results live in gitignored `data/` on another machine; treat as unavailable.

## Milestones (one commit or more each; measure before/after)
1. Build: CMake + VS 2022 Build Tools; C++ tests must fail, not skip, when exe missing. Record env.
2. Baseline: current engine on 3 small workloads, 1/2/4/8/16 threads, 5 reps, local non-synced disk.
3. Scheduler stress test (exactly-once, all policies/thread counts); fix steal edge cases.
4. Quick wins: single-pass read-hash-write; parallel classification via scheduler; size check
   before hashing; implement or drop CV rule; calibrated cost a + b*size.
5. Catalog schema 4: per-block hashes + Merkle root (C++ writer/verifier, Python oracle reader).
6. Hybrid scheduler: split files above threshold into block tasks; same scheduler for
   backup/verify/recover.
7. Block-level selective recovery: parallel tree compare, restore damaged blocks only.
8. Evaluation: workloads small-only / mixed / few-huge-files; policies sequential, static,
   dynamic, stealing, file-IPBRS, hybrid-IPBRS; threads 1-16; warm and cold cache; 5 reps.
9. Analysis + charts from raw records (speedup, efficiency, imbalance, idle, overhead, bytes avoided).
10. Docs/paper/slides updated so every claim matches code and measurements.

## Status
- [ ] 1 Build
- [ ] 2 Baseline
- [ ] 3 Stress tests
- [ ] 4 Quick wins
- [ ] 5 Merkle catalog
- [ ] 6 Hybrid scheduler
- [ ] 7 Block recovery
- [ ] 8 Evaluation
- [ ] 9 Analysis
- [ ] 10 Docs
