# 5. Demo and presenting

How to run the live demo, what to say at each step, how to present the slides, and ready-made pitches for interviews.

[Back to the guide](README.md) · Previous: [4. Experiments and results](4-experiments-and-results.md) · Next: [6. Interview questions](6-interview-qa.md)

---

## 5.1 Before the demo

1. **Build** (once, after any code change): open a terminal in the project folder and run `build.bat test`. It should end with `100% tests passed` and `Built build\ipbrs.exe`.
2. **Free space:** about 3 GB on the drive that holds your temp folder.
3. **For stable numbers:** plug in the laptop, close heavy programs, and run the demo once before presenting (the first run after a restart is slower).
4. *(Optional)* Exclude the project's `data` folder from Windows Defender (admin PowerShell): `Add-MpPreference -ExclusionPath "<project>\data"`.

## 5.2 Running it

```powershell
powershell -ExecutionPolicy Bypass -File demo.ps1 -Pause       # waits for Enter between steps
powershell -ExecutionPolicy Bypass -File demo.ps1              # runs straight through (~1 minute)
powershell -ExecutionPolicy Bypass -File demo.ps1 -BigMiB 256  # smaller and faster
```

The demo works in its own temporary folder and deletes it afterwards (`-Keep` keeps it).

```mermaid
flowchart LR
    S1["1 Generate<br/>2,000 small + 2 x 512 MiB"] --> S2["2 Backup<br/>durable, re-verified"]
    S2 --> S3["3 Damage<br/>4 types + silent bit flip"]
    S3 --> S4["4 Detect<br/>dry run: quick vs full hash"]
    S4 --> S5["5 Recover<br/>blocks only"]
    S5 --> S6["6 Novelty<br/>seq vs file-level vs IPBRS"]
    S6 --> S7["7 Proof<br/>Windows Get-FileHash"]
```

## 5.3 What you will see, and what to say

The numbers below are from a rehearsal on the development laptop; yours will differ slightly.

**Step 1: Generate.** `2,000 small (8-16 KiB) + 2 x 512 MiB`, 1,047 MiB in total, and "Get-FileHash SHA256 of all 2002 files recorded".
> "This is a test dataset with the shape that matters: lots of small files plus two big ones. Before anything else, I record Windows' own SHA-256 of every file, so that at the end we can prove recovery worked without trusting my code."

**Step 2: Backup.** 2,002 files in about 2.5 s; "large files split into 64 block tasks"; "[OK] backup committed and independently re-verified".
> "IPBRS backs up with 16 threads. The two 512 MiB files were split into 64 pieces so that all threads share them. Every file is flushed to disk, and the backup is only marked complete after an independent re-verification."

**Step 3: Damage.** 25 missing, 25 modified, 25 corrupted, 25 truncated, plus "1 byte inside b000.dat (512 MiB), timestamp unchanged".
> "Now I break things the way real failures do: deleted, overwritten, cut short, and silently corrupted, where a byte changes but the timestamp does not, like bit rot."

**Step 4: Detect.** Quick check finds 75; full hash check finds 101; "[OK] hashing caught 26 silently corrupted files that timestamps miss".
> "A quick check that only looks at size and timestamp misses all the silent corruption. Hashing every block catches all 101 damaged files, and this is a dry run, so nothing has been changed yet."

**Step 5: Recover.** 101 files restored; 2.2 MiB of 1,047 MiB (0.21%) written back; "[OK] every repaired file re-verified".
> "Because every block has its own hash, recovery knows exactly which blocks are broken. It rewrites 2 MiB instead of 1 GB, and verifies every block from the backup before writing it."

**Step 6: The novelty.**

```text
                         time   speedup   imbalance     idle
sequential (1 thread)  3.87 s      1.0x        0.00       0%
file-level (16 threads) 1.96 s     2.0x        5.52      85%
IPBRS (16 threads)     0.56 s      7.0x        0.04       4%
[OK] IPBRS is 3.5x faster than file-level scheduling on this dataset
```

> "This is the core result live. With normal file-level scheduling, two threads hash the two big files and the other fourteen wait: 85% idle. IPBRS splits the big files into blocks, so all sixteen threads work: 7 times faster than one thread, and 3.5 times faster than file-level."

(This dataset has 2 big files instead of 4, so file-level does even worse here than in the benchmark: with 2 big files, at most 2 threads are busy for most of the run.)

**Step 7: Proof.** "[OK] all 2002 files are byte-for-byte identical to the original".
> "Finally, Windows' own hashing confirms every file is exactly as it was."

### If something goes wrong

| Symptom | Fix |
|---|---|
| `build\ipbrs.exe not found` | Run `build.bat` first |
| "running scripts is disabled" | Use `powershell -ExecutionPolicy Bypass -File demo.ps1` |
| "need about X GB free" | Free space, or use `-BigMiB 256` |
| Numbers slower than expected | Plug in, close apps, run once more (first runs warm the caches) |
| Step 6 speedup smaller | Normal variation; the idle percentages still show the difference |

## 5.4 Presenting the slides (about 15 minutes)

The deck (13 slides) has speaker notes on every slide. A suggested timing:

| Slides | Time | Goal |
|---|---|---|
| 1–3: cover, problem, the wall | 3 min | Make them feel the problem: the busy-time chart with 12 idle workers |
| 4–6: idea, architecture, engineering | 4 min | The four parts; one scheduler for everything; measurements changed the design |
| 7: correctness | 1 min | Fast but wrong is worthless; 873 verified runs |
| 8–10: results | 3 min | 6.9× vs 3.6×; 0.15% rewritten; backup limited by the disk |
| 11: limits | 1 min | Say them before anyone asks |
| 12: live demo | 2 min | Run `demo.ps1` |
| 13: conclusion | 1 min | Three numbers, then questions |

## 5.5 Pitches

### 30 seconds

> "I built a parallel backup and recovery engine in C++ with OpenMP. Normal parallel backup gives each file to one thread, so a few big files leave most cores idle; I measured 72% idle at 16 threads. My engine hashes files as Merkle trees of 1 MB blocks, so big files can be split across threads with work stealing, and recovery can repair only the damaged blocks. On a 16-thread laptop that gave 6.9× speedup versus 3.6× for file-level scheduling, and recovery rewrote 0.15% of the data instead of all of it, with every run verified."

### 2 minutes

1. **Problem (20 s).** Backup tools parallelise by file, but SHA-256 must read a file from start to end, so one huge file is one thread's job. With 4 big files and 16 threads, 12 threads wait.
2. **Idea (30 s).** Hash each 1 MiB block separately and combine the hashes in a Merkle tree. Then a big file can be cut into pieces. A cost-aware work-stealing scheduler balances the pieces; a split rule based on Graham's bound only splits files bigger than total work ÷ (threads × 8), so small-file workloads are untouched.
3. **Engineering (30 s).** Measurements changed the design: NTFS serialises threads writing one file, so the backup stores big files as 16 MiB segments; whole-file restores stay unsplit; every write is flushed for durability.
4. **Results (30 s).** Verify 6.9× vs 3.6×; idle 72% → 2%; selective recovery 5.7× faster than full and rewrote 0.15% of the data; backup 2–4×, limited by the SSD.
5. **Rigour (10 s).** 38 tests, 873 shuffled and independently verified runs, honest limits.

### 5 minutes

The 2-minute version, plus: walk through the worker busy-time chart (chapter 4.4); explain why file-level stops at exactly 4 threads; explain best-of-3 and why identical backup runs varied up to 4×; mention the measurement mistake that was caught (old binary) as an example of checking your own results.

## 5.6 Resume bullets

Use numbers you can defend from [RESULTS.md](../../RESULTS.md):

- Built **IPBRS**, a C++17/OpenMP parallel backup and selective-recovery engine that hashes files as Merkle trees of 1 MiB blocks and schedules block tasks with cost-aware work stealing; **6.9× verification speedup at 16 threads versus 3.6× for file-level scheduling** (worker idle time 72% → 2%).
- Implemented **block-level selective recovery** that verifies every backup block before writing; rewrote **0.15% of a 1.6 GB dataset** and ran **5.7× faster than full recovery**.
- Built a reproducible benchmark suite (Python, 873 shuffled and independently verified runs) and 38 unit and end-to-end tests; diagnosed NTFS write serialisation and redesigned backup storage as segment objects.

## 5.7 Being honest about how it was built

If you used an AI coding assistant to write much of the code, say so if asked, and say what *you* did. Interviewers increasingly expect AI tools; what they test is whether you understand and can defend the work. A truthful answer:

> "I used an AI coding assistant heavily for the implementation. I directed the work: I audited the first version, chose the direction, reviewed each step, decided which results to trust, and ran and verified every experiment. I can explain every component and every number, and I am happy to go into any part in detail."

Then prove it by explaining the design from this guide. Never claim to have written alone code that you did not, and check your course's rules on AI assistance.
