# 6. Interview and viva questions

About 50 questions you are likely to be asked, each with an answer you can say out loud. Answers use only measured numbers from [RESULTS.md](../../RESULTS.md). When an answer says "we did not test this", say exactly that: it is far better than guessing.

[Back to the guide](README.md) · Previous: [5. Demo and presenting](5-demo-and-presenting.md)

---

## A. The project and its novelty

**1. Explain your project in one minute.**
Use the 30-second pitch in [chapter 5.5](5-demo-and-presenting.md#55-pitches), then offer the busy-time chart.

**2. What problem does it solve?**
File-level parallel backup is limited by its biggest files. SHA-256 must read a file from start to end, so one huge file is one thread's job. With 4 big files and 16 threads, we measured 72% of worker time idle. IPBRS removes that limit, and also makes recovery rewrite only damaged blocks.

**3. Isn't this just `robocopy /MT` or rsync with threads?**
Those parallelise across *files*. IPBRS parallelises *inside* files: a 400 MiB file is cut into block-range tasks that different threads hash and copy, which is possible because each block has its own hash. They also do not do block-level selective recovery with verified blocks. On our benchmark, file-level scheduling stopped at 3.6× while IPBRS reached 6.9×.

**4. Merkle trees and work stealing already exist. What exactly is new?**
Neither technique is new, and I don't claim they are. The contribution is combining them for backup and recovery: per-block hashes so the scheduler can split big files, a cost-aware work-stealing scheduler over mixed file and block tasks, a split rule that bounds the largest task and switches itself off when no file dominates, and block-level recovery that verifies every block before writing. Plus measurements showing where it helps and where it doesn't.

**5. Did you solve an unsolved problem?**
No, and I would not claim that. The course allowed "applying parallelism where it is not normally used". Backup tools normally don't parallelise inside a file; this project does, and measures the gain.

**6. What does "intelligent" mean in the name?**
Runtime decisions based on estimated work: cost-aware partitioning, work stealing when estimates are wrong, and the adaptive split rule that decides granularity from the total work and thread count. No machine learning.

**7. Who is it useful for?**
Anyone backing up data where a few large files dominate: virtual-machine disk images, videos, databases, game assets. For many small files it is no faster than file-level tools (but also not slower).

**8. What would you do next?**
Hardware-accelerated SHA-256 (SHA-NI), tests on other storage (hard disks, network shares), incremental backups (per-block hashes make that natural), a binary catalog for very large trees, and tuning the split factor and block size experimentally.

## B. Parallel computing concepts

**9. What is speedup? Efficiency?**
Speedup = 1-thread time ÷ P-thread time. Efficiency = speedup ÷ P. IPBRS verify on `huge`: 5.98 s → 0.87 s at 16 threads, speedup 6.9, efficiency 0.43 against 16 threads, or 0.86 against the 8 physical cores.

**10. Why not 16× with 16 threads?**
The laptop has 8 physical cores with 2 hardware threads each, and two threads on one core share its execution units. For CPU-bound hashing about 8× is the realistic ceiling; `mixed` reached 7.7×. Other limits: per-file overhead for small files, and the disk for writes.

**11. Explain Amdahl's law and how it applies here.**
If a fraction *s* of the work is serial, speedup is at most 1/*s*. A file that only one thread can hash is serial work. In the old version recovery's whole scan was serial too. IPBRS turns big files into parallel work by splitting them.

**12. What is load imbalance? How did you measure it?**
(Busiest worker's busy time − average) ÷ average. Each worker records the time spent inside tasks. File-level verify on `huge` at 16 threads: 2.54; IPBRS: 0.02.

**13. Explain work stealing.**
Each worker has its own queue. When it runs out, it takes work from another worker's queue. In IPBRS, an idle worker picks the worker with the most remaining estimated cost and takes tasks from the back of its queue until it holds half of that cost. It balances automatically when cost estimates are wrong. A test shows this: when one worker is artificially slow, static scheduling waits for it, while stealing finishes more than 4× sooner.

**14. Why steal half, and why from the back?**
Half moves a lot of work per steal, so steals are rare: we measured only about 12–34 steals per 16-thread run. The owner works from the front (largest tasks first, from LPT), so taking from the back leaves the owner's next tasks alone.

**15. What is LPT and why use it?**
Longest Processing Time first: sort tasks by cost and give each to the least-loaded worker. It is a classic approximation, never worse than 4/3 of the optimal finish time. IPBRS uses it for the initial plan; work stealing corrects it at runtime.

**16. What is Graham's bound and how did you use it?**
For greedy list scheduling with P workers, finish time ≤ total work ÷ P + the largest task. The largest task is the penalty. IPBRS caps it by splitting any file larger than total ÷ (threads × 8), so the penalty is at most about 1/8 of the ideal finish time.

**17. What is granularity, and what is the trade-off?**
Task size. Coarse tasks leave workers idle at the end (a running task cannot be split); very fine tasks add overhead (locks, opening files). IPBRS adapts: it splits only what is needed, and nothing at all when no file is big relative to the total.

**18. Static vs dynamic vs work stealing?**
Static: fixed plan, no runtime balancing. Dynamic: workers take the next task from a shared counter; adapts but ignores costs. Stealing: cost-aware plan plus runtime balancing. On `huge` all three stall at about 1.65 s without splitting, which shows the limit was granularity, not the policy.

**19. Why OpenMP and not `std::thread`?**
OpenMP is the course's shared-memory model and makes parallel regions simple. The scheduling logic is our own code on top, so `std::thread` would work equally well. MSVC supports OpenMP 2.0, which has no task constructs; we didn't need them.

**20. What are race conditions, and how did you avoid them?**
Two threads touching the same data with a timing-dependent result. Each queue has a mutex; the dynamic counter is an atomic `fetch_add`; each worker has its own statistics slot and buffer; different tasks always write different blocks; per-file failure flags are atomic. The exactly-once test runs every policy at 1–16 threads, and it passed 30 repeated runs.

**21. Doesn't locking slow it down? Why not a lock-free deque?**
Steals are rare, so the locks are almost never contended. The measured scheduling overhead (planning plus time spent claiming work) was at most 0.45% of thread time in every configuration. A lock-free Chase–Lev deque would not change the results.

## C. Design and engineering

**22. Why 1 MiB blocks?**
A trade-off: smaller blocks give finer recovery and finer splitting but a bigger catalog (32 bytes per block) and more per-block work; bigger blocks do the opposite. 1 MiB keeps the catalog small (1.6 GB of large files is about 1,600 blocks, so about 50 KB of hashes; every small file still needs one block of its own) and is the recovery unit. It was chosen, not tuned experimentally.

**23. Why is the split factor 8 and the per-task cost 64 KiB?**
8 limits the largest-task penalty to about 1/8 of the ideal time while keeping the number of extra tasks small. 64 KiB approximates the fixed cost of opening and creating a file. Both are reasonable defaults, adjustable by flag, and **not tuned experimentally**: that is future work.

**24. Why store big files as segments in the backup?**
Measured: when several threads wrote pieces into one backup file, splitting made backup slower than not splitting, because NTFS serialises writers that extend a file. With one 16 MiB segment file per piece, no two threads write the same file. Pieces align to segment boundaries so that each task owns whole segments.

**25. Why are whole-file restores not split?**
Same NTFS effect, measured in recovery: full recovery took 14.9 s with splitting against 9.8 s without. Restoring a whole file writes one target file, so it stays one task. Scans and in-place block repairs still split. After the fix both versions took 6.0 s.

**26. Why flush every file? Isn't that slow?**
A backup that is lost in a power cut is not a backup. Without flushing, the OS keeps data in memory and reports success early, and timings also swung by 2–3× between identical runs. Flushing makes backups durable and timings honest. `--no-sync` exists only for comparison.

**27. How does the engine read a file in parallel?**
With positioned I/O: `ReadFile`/`WriteFile` with an explicit offset (an `OVERLAPPED` structure), so several handles can read or write different parts of one file at the same time.

**28. What happens if a file changes during backup?**
Each task checks that it read the expected number of bytes, a whole-file task checks that the file did not grow, and after copying the engine re-reads the size and modification time. Any change marks the file failed, and the backup is not committed.

**29. How do you make sure a corrupted backup never damages the source?**
Every block read from the backup is hashed and compared with the catalog *before* it is written. A mismatch fails that task and leaves the file as it was. An end-to-end test corrupts a backup block and checks this.

**30. How is the catalog protected?**
It is written last and atomically (temporary file, flush, rename). On load it is validated: header, block geometry, counts, every file's root recomputed from its leaves, no duplicate paths, no file that is also a folder, no unsafe paths such as `..`, no trailing data. It does **not** protect against an attacker who rewrites both the data and the catalog; that would need a signed catalog.

**31. What happens to files added after the backup?**
They are ignored, never deleted. Recovery only touches files listed in the catalog.

**32. How does discovery work, and is it parallel?**
Level by level: all folders at one depth are listed in parallel, their sub-folders form the next level, and the file list is sorted at the end. Symbolic links are skipped.

**33. How much memory does it use?**
The catalog (32 bytes per block plus a path per file) and one 1 MiB buffer per thread. For 1.6 GB, a few MB. A tree with hundreds of millions of blocks would need a streaming or binary catalog.

**34. What are the limitations of the implementation?**
Windows-first (POSIX paths exist but are not the tested configuration); paths longer than Windows' 260-character limit are not handled; the catalog is loaded fully into memory; no incremental backups, compression or deduplication.

## D. Results and methodology

**35. How do you know the comparison is fair?**
Same data, same seed, same machine, shuffled run order, same scheduler code. File-level and IPBRS differ by exactly one flag, `--split none` vs `--split adaptive`.

**36. Why report the best of 3 and not the average?**
Write-heavy runs suffered outside interference: identical single-thread backups of `small` took 36–43 s in 11 of 21 runs and 76–172 s in the others, from a nearly full SSD and Defender scanning. The minimum shows the system's capability without interference. The headline results are read-only runs, where mean and best agree within about 0.1 s, and every raw run is in the repository.

**37. Why only 3 repetitions?**
Time: the full matrix was 873 runs, about 3.5 hours. For read-only runs the spread was tiny, so 3 was enough; for the noisy backup runs more repetitions would help, and I say so as a limitation.

**38. Why does file-level stop improving at exactly 4 threads on `huge`?**
There are 4 big files. Up to 4 threads, every thread has a big file. Beyond that, the extra threads finish the small files quickly and have nothing to steal, so the run lasts as long as one big file (about 1.6 s).

**39. Why is backup only 2–4× faster?**
Backup must write and flush every byte, so the SSD is the bottleneck. Threads help by overlapping flush delays and keeping the drive busy, but cannot make the disk faster. On `huge`, IPBRS was best (3.3× vs 2.8×) and removed the imbalance.

**40. Why is IPBRS no faster on `small` and `mixed`?**
No file is above the split threshold, so nothing is split and IPBRS does the same as file-level. That is by design, and it shows the split rule costs nothing when it is not needed.

**41. Block and file recovery take the same time. So what is the point of block mode?**
Both spend most of their time scanning, so their times are similar. Block mode's advantage is data written: at 20% damage on `huge`, 25% of the data against 49%. That matters for SSD wear and slow or remote storage, and it would show in time on a slower disk.

**42. Would the results hold on another machine?**
The shape should: file-level is always capped by the biggest files, and splitting removes the cap. The exact numbers depend on cores and storage. More cores would make the gap larger; a faster SSD would raise backup speedup. We measured only one laptop.

**43. Did you find any bugs in your own measurements?**
Yes, two. A silently failing rebuild meant three experiments measured the old binary; the catalog was missing a field the new code always writes, so I discarded those numbers and reran. And dry-run summaries showed no imbalance because they read the empty restore phase; they now use the dominant phase. (A good answer to "tell me about a mistake you made".)

## E. Correctness and testing

**44. How did you test it?**
38 automated tests: SHA-256 against NIST vectors, the Merkle tree, catalog tampering and corruption, every scheduler policy running every task exactly once at 1–16 threads, the planner's split rule, and end-to-end backup → damage → recovery compared byte for byte. Every benchmark run was also verified independently, and the demo ends with Windows' `Get-FileHash`.

**45. How do you test a concurrent program for races?**
By checking invariants under many interleavings: each of 3,000 tasks must run exactly once, for every policy and thread count, repeated 30 times. MSVC has no thread sanitizer, so we did not run one: a limitation.

**46. What is silent corruption and how do you catch it?**
A change that keeps the file's size and timestamp, like a flipped bit. Only hashing catches it. The size-and-timestamp shortcut found 75% of our damage and missed every silently corrupted file; full hashing found 100%.

**47. How likely is a SHA-256 collision giving a false "OK"?**
Negligible: no SHA-256 collision has ever been found, and the chance of an accidental one is astronomically small.

## F. Harder and behavioural questions

**48. What would break at 100× the data?**
The text catalog is loaded fully into memory and grows with the number of blocks, so it would need a binary or streaming format. Discovery and scheduling would scale; disk bandwidth would dominate more.

**49. How would this work over a network or to the cloud?**
Block-level recovery would matter more: only damaged blocks travel. The scheduler would need to account for network latency (more tasks in flight). Not implemented.

**50. What was the hardest part?**
Explaining the backup results. Splitting first made backup slower, and timings swung between runs. Finding the causes (NTFS write serialisation and write caching) and redesigning the storage as segments taught me to measure before assuming.

**51. Did you write this yourself?**
Answer truthfully. If an AI coding assistant wrote much of the code, say so, and say what you did: directed the design, reviewed every step, decided which results to trust, and ran and verified the experiments. Then show you understand it by explaining any part in detail. See [chapter 5.7](5-demo-and-presenting.md#57-being-honest-about-how-it-was-built).

**52. If you started again, what would you change?**
Run the experiments on an emptier disk with Defender exclusions from the start; sweep the split factor and block size; use a binary catalog; and add a thread sanitizer run on Linux.

---

## Last-minute checklist

- [ ] I can draw the busy-time chart from memory and explain both halves.
- [ ] I can explain why file-level stops at 4 threads on `huge`.
- [ ] I can state the split rule and why Graham's bound justifies it.
- [ ] I know the headline numbers: 6.9× vs 3.6×, 72% → 2% idle, 0.15% rewritten, 5.7× vs full, backup 2–4×.
- [ ] I can say what is *not* new, and what is.
- [ ] I can explain best of 3, and the two design changes that came from measurements.
- [ ] I have run `demo.ps1` once today.
