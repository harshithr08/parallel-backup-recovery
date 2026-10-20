# IPBRS design summary

The decisions that shape the engine, each with its reason. The full walkthrough, with diagrams, is chapter 3 of the [study guide](docs/study-guide/3-how-it-works.md); measurements are in [RESULTS.md](RESULTS.md).

## Core model

| Decision | Reason |
|---|---|
| Files are sequences of 1 MiB blocks; each block has its own SHA-256 leaf, combined into a Merkle root per file | SHA-256 is sequential within its input, so whole-file hashing ties a file to one thread. Per-block leaves make blocks independent, and localise damage to a block |
| Leaf = SHA-256(0x00 ‖ block), node = SHA-256(0x01 ‖ left ‖ right), odd node promoted, empty file = one empty block | Domain separation keeps leaves and inner nodes distinct |
| One task type for every operation: a run of blocks of one file, with cost = 64 KiB + bytes | The scheduler stays independent of the operation, so backup, verification, detection and recovery share the same balancing |
| The fixed 64 KiB term in the cost | Opening and creating a file costs more than reading a small file; without it, thousands of small files would look free |

## Scheduling

| Decision | Reason |
|---|---|
| Four policies: `seq`, `static` (LPT), `dynamic` (shared atomic counter), `steal` | Baselines that isolate what cost awareness, runtime balancing and splitting each contribute |
| `steal`: LPT seeds per-worker queues; owners take the largest task first; an idle worker picks the queue with the most remaining cost and takes tasks from its back until it holds half of that cost | Corrects wrong cost estimates at runtime; taking half keeps steals rare (12–34 per 16-thread run) |
| A mutex and an atomic remaining-cost counter per queue | Simple and correct; measured scheduling overhead is at most 0.45% of thread time |
| Split any splittable range larger than max(1 block, total cost ÷ (threads × 8)) into pieces cut at absolute multiples of the piece size | Graham's bound: finish time ≤ total ÷ threads + largest task, so capping the task size caps the tail. The rule switches itself off for small-file workloads and at one thread |
| Every task runs inside `try`/`catch` | An exception must not escape an OpenMP region; a failure is recorded per task and per file |

## Storage and durability

| Decision | Reason |
|---|---|
| Files of up to 16 blocks are stored whole under `files/`; larger files as 16-block segment objects under `segments/<path>/` | Measured: threads writing pieces into one NTFS file are serialised, making split backup slower. Separate segment objects remove the conflict; split pieces align to segments |
| Whole-file restores are single tasks; scans and in-place block repairs split | Same effect in recovery: full recovery took 14.9 s split against 9.8 s unsplit, and 6.0 s both ways after the change |
| Every written object, and the catalog, is flushed (`FlushFileBuffers`) | A backup must survive power loss; flushing also removed write-cache effects from timings |
| Single-pass backup: each block is read once, hashed and written | The first prototype read every file four times |
| The catalog is written last, atomically (temporary file, flush, rename), and only if every file succeeded | A partial backup must never look complete |

## Recovery and safety

| Decision | Reason |
|---|---|
| Scan in two parallel phases: one metadata call per file, then block hashing through the scheduler | Missing and resized files are found cheaply; content is checked block by block |
| Hash by default; `--trust-mtime` is opt-in | The shortcut found 75% of damage and missed every silent corruption |
| Block mode restores only runs of damaged blocks in same-size files; missing, resized and unreadable files are restored whole | Minimises data written back (0.15% of the data at 5% damage) |
| Every block read from the backup is checked against the catalog before it is written | A damaged backup copy is never spread into the source |
| Catalog validation on load: geometry, counts, root vs leaves, duplicates, file-as-folder conflicts, unsafe paths, trailing data | Corrupt or tampered indexes are rejected instead of misused |
| Source and backup must not overlap; the backup folder must be empty; uncataloged source files are never deleted | Avoids self-inclusion and accidental data loss |
| `--dry-run` classifies and plans without writing | Inspect damage first; lets the detection benchmark rescan one damaged tree |

## Known limitations

- Disk-bound operations (durable backup, full recovery) are limited by storage bandwidth; scheduling cannot remove that.
- The split factor (8), block size (1 MiB) and per-task cost (64 KiB) are reasoned defaults, not tuned by experiment.
- The text catalog is loaded fully into memory; very large trees would need a binary or streaming format.
- Windows is the tested platform; paths beyond 260 characters are not handled.
- No incremental backups, compression or deduplication.
