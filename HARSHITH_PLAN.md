# IPBRS completion plan (harshith-dev) - COMPLETE

Contribution: hybrid file/block work-stealing scheduler with a Merkle (per-block SHA-256) catalog.
File-level scheduling cannot finish faster than its largest file; splitting large files into
stealable block tasks removes that bound. The same tree enables parallel damage detection and
block-level selective recovery.

## Final state
- Repositories: team repo `sadm888/Parallel-Backup-and-Recovery-System` (branch `harshith-dev`)
  and own repo `harshithr08/parallel-backup-recovery` (branch `main`); push to both.
- Results: 873 runs, all verified; see RESULTS.md. Headline (huge workload, 16 threads, best of 3):
  verify 6.9x vs 3.6x file-level, idle 72% -> 2%, block recovery 1.01 s vs 1.89 s and 0.15% of
  data rewritten, durable backup 2-4x (SSD-bound).
- Docs: README.md, DESIGN.md, RESULTS.md, docs/study-guide/ (6 chapters, 52 interview questions).
- Demo: `powershell -ExecutionPolicy Bypass -File demo.ps1 -Pause`.
- Slides: claude.ai artifact deck (13 slides, speaker notes), downloadable as PPTX/PDF.
- Build from PowerShell: `cmd /c "<repo>\build.bat" test`; check for "Built" and "100% tests passed".

## Commit plan (one focused commit per step)
Engine
- [x] Rewrite engine (79128f5)
- [x] Segment layout + durable writes (1ffff96)

Phase A - tests
- [x] A1 test harness + ctest via `build.bat test`
- [x] A2 SHA-256 NIST vectors
- [x] A3 Merkle tree + catalog root checks
- [x] A4 scheduler exactly-once, all policies x 1/2/3/8/16 threads, failure accounting
- [x] A5 end-to-end backup -> 4 damage types -> block/file/full recovery -> byte compare

Phase B - fixes from testing/review
- [x] B1 planner edge cases (zero-byte, single block, segment alignment) + tests
- [x] B2 catalog robustness (corrupt/tampered catalogs) + tests
- [x] B3 steal statistics and scheduling-overhead reporting
- [x] B4 keep whole-file restores unsplit (found by C3 trial)

Phase C - benchmark tooling (bench/)
- [x] C1 workloads.py (small-only, mixed, huge-files)
- [x] C2 run_backup.py (scaling matrix, repeats, settle, CSV)
- [x] C3 run_recovery.py (block/file/full at 5% and 20% damage)
- [x] C4 recover --dry-run + run_scan.py (parallel detection, trust-mtime vs full hash)

Phase D - experiments (raw results committed)
- [x] D1 backup scaling (279 runs, all verified; drive's intermittent slow mode makes identical runs up to 4x slower)
- [x] D2 recovery comparison (270 runs, all repaired+verified)
- [x] D3 detection/verify scaling (324 runs, all clean)

Phase E - analysis
- [x] E1 plot.py speedup/efficiency
- [x] E2 imbalance/idle/bytes-avoided charts
- [x] E3 RESULTS.md

Phase G - presentation
- [x] G1 demo.ps1 live demo
- [x] G2 slide deck
- [x] Study guide (replaces the separate Q&A sheet)

Phase F - docs and cleanup
- [x] F1 README.md (with credits)
- [x] F2 DESIGN.md
- [x] F3 legacy/ kept: it is the team's first prototype, referenced in the credits
- [x] F4 final plan update

## Possible future work
- Tune split factor, block size and per-task cost experimentally.
- Hardware SHA-256 (SHA-NI); other storage (HDD, network); incremental backups; binary catalog.
- Rerun write-heavy benchmarks on an emptier disk with Defender exclusions.
