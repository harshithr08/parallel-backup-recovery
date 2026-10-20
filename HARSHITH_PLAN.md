# IPBRS completion plan (harshith-dev)

Contribution: hybrid file/block work-stealing scheduler with a Merkle (per-block SHA-256) catalog.
File-level scheduling cannot finish faster than its largest file; splitting large files into
stealable block tasks removes that bound. The same tree enables parallel damage detection and
block-level selective recovery.

## Findings so far (Ryzen 7 7435HS 8C/16T, 16 GB, WD SN740 NVMe, C: 94% full)
Workload: 4,000 x 8-16 KiB + 4 x 400 MiB, warm cache.
- Read+hash (verify/scan): file-level stalls at ~1.7 s from 4 threads (72% idle at 16);
  block split 0.78-0.86 s at 16 threads = 2.2x over file-level, 7.8x over 1 thread.
- Backup is SSD-write-bound (~270 MB/s): ~2x speedup max. Concurrent writes into one
  file were slow on NTFS -> backup stores files > 16 blocks as segment objects; with
  segments, split and file-level backup are about equal in time, and imbalance drops 0.42 -> 0.
- Backup/recovery flush (fsync) every stored object; --no-sync for comparison.
- Always build from PowerShell: `cmd /c "<repo>\build.bat"`; check for "Built".

## Commit plan (one focused commit per step; report after each)
Done:
- [x] Rewrite engine (198fb6f)
- [x] Segment layout + durable writes (6140f80)

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
- [ ] C4 run_scan.py (serial vs parallel detection, trust-mtime vs full hash)

Phase D - experiments (raw results committed)
- [ ] D1 backup scaling
- [ ] D2 recovery comparison
- [ ] D3 detection/verify scaling

Phase E - analysis
- [ ] E1 plot.py speedup/efficiency
- [ ] E2 imbalance/idle/bytes-avoided charts
- [ ] E3 RESULTS.md

Phase F - docs and cleanup
- [ ] F1 README.md
- [ ] F2 DESIGN.md
- [ ] F3 remove legacy/ (with approval)
- [ ] F4 final plan update
