Master Specification for Codex — IPBRS
You are implementing a final-year Parallel Computing project called:
IPBRS — Intelligent Parallel Backup and Selective Recovery System
The goal is to build a real, working prototype, not just a simulation.
The system is a single-machine, shared-memory parallel file backup and recovery system. It should work on a local filesystem and use multiple CPU worker threads/processes through a shared-memory parallel programming model such as OpenMP.
The central research contribution is an adaptive workload scheduler that attempts to distribute heterogeneous file-processing workloads efficiently.
Do not introduce machine learning, GPU/CUDA, distributed computing, cloud storage, networking, or unrelated features. The "intelligent" part refers to runtime workload-aware scheduling, not AI/ML.
1. What the system is supposed to do
At a high level:
SOURCE DIRECTORY
       |
       v
Discover files
       |
       v
Validate + estimate workload cost
       |
       v
Adaptive Scheduler
       |
       +----------------------+
       |                      |
       v                      v
Parallel Backup        Selective Recovery
       |                      |
       +----------+-----------+
                  |
                  v
          Parallel Verification
                  |
                  v
          Verified Repository

The system should be able to:
1. Take a directory containing many files.
2. Discover the files.
3. Estimate how expensive processing each file will be.
4. Divide the work among multiple workers.
5. Perform backup operations in parallel.
6. Maintain a persistent catalog describing backed-up files.
7. Detect files that have been modified, deleted, corrupted, or truncated.
8. Recover only the affected files instead of restoring everything.
9. Verify that backed-up/recovered files are correct.
10. Measure performance against several baseline scheduling approaches.
The project should demonstrate that intelligent workload scheduling can reduce:
- execution time,
- worker idle time,
- load imbalance,
while maintaining correct backup/recovery behavior.
2. Important: What is actually novel
Do NOT claim that parallelism, OpenMP, work stealing, checksums, or backup itself is novel.
Those are established techniques.
The project contribution is their system-level combination for heterogeneous file-level backup and selective recovery, specifically:
Cost-aware initial partitioning
              +
Runtime work stealing
              +
Adaptive task granularity
              =
IPBRS Adaptive Scheduler

The project should experimentally investigate whether this combination improves workload distribution and backup/recovery performance.
3. System scope
The system is:
- single machine
- multicore CPU
- shared-memory
- local filesystem
- file-level operations
- parallel backup
- parallel selective recovery
- checksum-based verification
Out of scope:
- distributed machines
- cloud storage
- network filesystem
- GPU/CUDA
- machine learning
- deep learning
- predictive ML models
- blockchain
- database-backed distributed storage
Do not add these unless explicitly requested.
4. Dataset
The project will use a controlled synthetic dataset.
The dataset should contain different types of files:
Documents
Images
Videos
Archives
Binary files

The purpose is to create a heterogeneous workload.
Files should have different sizes so that processing costs differ.
For example, conceptually:
small document
large document
small image
large image
small video
large video
archive
binary file
...

The exact file-size distribution should be configurable rather than hard-coded. The generator exposes named low, medium, and high heterogeneity presets with explicit weighted file-type and size-profile mixes; custom mixes remain configurable. Duplicate ratio remains an independent configuration value and is not implied by a heterogeneity preset.
5. Dataset sizes
The main experiments use exactly these dataset sizes:
10,000 files
50,000 files
100,000 files
250,000 files

The system should therefore have a configurable dataset generator.
For example:
generate_dataset(
    output_directory,
    number_of_files,
    heterogeneity_level,
    duplicate_ratio
)

Do not silently change these four main dataset sizes.
6. Dataset heterogeneity
The experiment should support different workload heterogeneity levels.
At minimum:
Low
Medium
High

The meaning is:
Low heterogeneity
Files have relatively similar:
- sizes
- types
- processing costs
Medium heterogeneity
There is moderate variation in:
- file sizes
- file types
- processing requirements
High heterogeneity
There is substantial variation in:
- file sizes
- file types
- processing costs
The purpose is to determine whether the scheduler behaves differently when workload costs become uneven.
7. Duplicate files
The dataset generator should support a controlled duplicate ratio.
Duplicates are useful because the dataset should resemble a realistic file repository rather than a collection of completely unrelated files.
The duplicate ratio should be configurable.
Do not invent a fixed experimental percentage yet unless the experiment configuration explicitly specifies one.
8. File damage simulation
The recovery system must be tested by intentionally damaging files after backup.
Possible damage types:
Missing
Modified
Corrupted
Truncated

For example:
Missing
Delete a backed-up source file.
Modified
Change the contents of the file.
Corrupted
Alter file contents in a way that makes its checksum differ.
Truncated
Reduce the file's contents/size.
The original backup remains available as the recovery source.
The original state should be retained as the reference/ground truth.
9. Persistent catalog
The system needs a persistent catalog.
For every backed-up file, store at least:
Path
File size
Timestamp
File type
Backup status
SHA-256 checksum

Conceptually:
FileRecord {
    path
    size
    timestamp
    type
    backup_status
    sha256
}

The catalog is extremely important.
It allows the recovery system to determine whether a file has changed since the backup.
The catalog must persist after the backup program exits.
Do not make it merely an in-memory data structure.
10. SHA-256
Every backed-up file should have a SHA-256 checksum.
Conceptually:
checksum = SHA256(file_contents)

The checksum is used to determine whether file contents have changed.
For example:
Catalog:
    file.txt
    size = 10000
    SHA256 = ABC123...

Current file:
    size = 10000
    SHA256 = ABC123...

=> unchanged

But:
Catalog SHA256 = ABC123
Current SHA256 = XYZ789

=> modified

Checksum verification is also used after recovery.
11. Backup process
The backup process should work approximately like this:
1. Receive source directory
2. Discover files
3. Validate discovered files
4. Collect metadata
5. Estimate processing cost
6. Create file-processing tasks
7. Give tasks to adaptive scheduler
8. Workers process files in parallel
9. Copy files into backup repository
10. Calculate SHA-256
11. Update catalog
12. Verify backup
13. Report results

Every file should be treated as an independent task wherever possible.
For example:
Task 1 -> file A
Task 2 -> file B
Task 3 -> file C
Task 4 -> file D
...

This allows the scheduler to distribute work dynamically.
12. Parallelism
The system should support different worker counts.
The primary experimental configurations are:
1 worker
2 workers
4 workers
8 workers

Subject to the actual hardware available.
The same dataset and processing requirements must be used when comparing worker counts.
A single worker acts as the sequential reference configuration where appropriate.
13. Parallel file discovery
File discovery should be implemented in parallel only if the actual implementation supports it.
The intended architecture is:
SOURCE DIRECTORY
       |
       v
Parallel File Discovery
       |
       v
File Metadata
       |
       v
Cost Estimation

Do not merely claim that discovery is parallel if the implementation is actually serial.
If discovery remains serial, document it honestly and exclude it from claims about parallel discovery.
14. Cost estimation
This is one of the most important parts.
Before scheduling the work, the system should estimate the processing cost of each file.
The primary measurable characteristic is:
file size

Other operation-related characteristics can be incorporated if they are actually implemented.
The scheduler should therefore have some concept like:
estimated_cost(file)

For example, conceptually:
cost(file) ≈ f(file_size, operation_characteristics)

Do not invent a complicated mathematical model unless the implementation actually uses one.
The goal is simply to distinguish:
cheap task
medium task
expensive task

before scheduling.
15. Cost-aware initial partitioning
The first stage of scheduling is cost-aware partitioning.
Instead of simply doing:
first N files -> worker 1
next N files  -> worker 2
...

the system should try to distribute the estimated total workload more evenly.
Conceptually:
Worker 1:
    small + medium + medium

Worker 2:
    large + small

Worker 3:
    medium + medium + small

Worker 4:
    large

The objective is approximately:
total estimated cost per worker

should be reasonably balanced.
This is the initial distribution.
It does not need to be perfect.
That is why runtime work stealing exists.
16. Runtime work stealing
Actual execution cost may differ from estimated cost.
For example:
Worker 1:
    estimated 10 seconds
    actual 15 seconds

Worker 2:
    estimated 10 seconds
    actual 6 seconds

Worker 2 may finish earlier.
Instead of letting Worker 2 sit idle, it should be able to obtain additional tasks from another worker's remaining workload.
Conceptually:
Worker A:
    [Task 1][Task 2][Task 3][Task 4]

Worker B:
    [Task 5]
          |
          v
       finishes

Worker B steals:
    Task 3

This is the runtime balancing mechanism.
The system should track worker progress and allow idle workers to obtain work when appropriate.
17. Adaptive task granularity
This is the third major component.
The system should not necessarily process the entire workload using one fixed task size.
Instead, task granularity should adapt based on workload conditions.
The design relationship is:
Task Granularity =
    f(Workload Variation, Scheduling Overhead)

This is a design relationship, not a fixed mathematical formula.
The implementation needs an explicit rule for deciding when/how granularity changes.
For example, conceptually:
High workload imbalance
        ↓
smaller tasks
        ↓
more opportunities for stealing
        ↓
better balancing

But:
Very small tasks
        ↓
too many scheduling operations
        ↓
higher overhead

Therefore the system needs to balance:
load balancing
        vs.
scheduling overhead

Do not invent the final thresholds now. They should be determined from the actual implementation/design.
18. The complete adaptive scheduler
The scheduler should therefore operate conceptually like:
                FILE TASKS
                    |
                    v
             Cost Estimation
                    |
                    v
          Cost-Aware Partitioning
                    |
                    v
              Initial Queues
             /      |      \
            /       |       \
       Worker 1 Worker 2 Worker 3 ...
            |       |       |
            v       v       v
          Execute Execute Execute
             \       |       /
              \      |      /
               v     v     v
             Monitor Progress
                    |
             Is worker idle?
                /       \
              YES        NO
               |          |
               v          v
          Work Stealing   Continue
               |
               v
       Adaptive Granularity
               |
               v
        Continue Processing

This scheduler is the core of IPBRS.
19. Parallel backup
Once tasks are scheduled:
Worker 1 -> files
Worker 2 -> files
Worker 3 -> files
Worker 4 -> files

Each worker performs the backup operation on its assigned files.
The workers should not corrupt each other's work.
The implementation must handle shared metadata/catalog updates safely.
Use appropriate synchronization for shared data structures.
Avoid unnecessary synchronization because excessive locking can destroy parallel performance.
20. Selective recovery
This is the second major functionality of the project.
Suppose the backup repository contains:
A
B
C
D
E
F

After some time:
A -> unchanged
B -> modified
C -> unchanged
D -> deleted
E -> corrupted
F -> unchanged

The recovery system should not restore all six files.
It should identify:
B
D
E

and recover only those.
The decision can be represented as:
R(f) = 0, if file f is unchanged

R(f) = 1, if file f is:
       missing
       modified
       corrupted
       truncated

21. How selective recovery detects damage
For every cataloged file:
1. Does the file exist?
2. Is its size correct?
3. Is its metadata consistent?
4. Is it accessible?
5. Does its SHA-256 match?

If everything is valid:
SKIP

If something is wrong:
RECOVER

Example:
                File exists?
                    |
              +-----+-----+
             NO           YES
             |             |
          RECOVER       Size okay?
                           |
                       +---+---+
                      NO       YES
                      |         |
                   RECOVER    SHA-256?
                                |
                            +---+---+
                           NO       YES
                           |         |
                        RECOVER     SKIP

22. Selective recovery uses the same scheduler
This is important.
Do not build a completely separate scheduling mechanism for recovery.
After determining which files need recovery:
Affected files
      |
      v
Cost estimation
      |
      v
Same adaptive scheduler
      |
      v
Parallel recovery workers

Therefore:
Backup
   ↓
Adaptive Scheduler
   ↓
Parallel Workers

Recovery
   ↓
Detect affected files
   ↓
Adaptive Scheduler
   ↓
Parallel Workers

This makes the scheduling framework reusable.
23. Full recovery baseline
Selective recovery must be compared against full recovery.
Full recovery
Process the complete repository:
A B C D E F

even if only B, D and E are damaged.
Selective recovery
Process only:
B D E

The experiment should measure how much work selective recovery avoids.
This can include:
number of files avoided
bytes avoided
execution time

24. Verification
Verification is a separate stage.
After backup:
Backup
  ↓
Verification

After recovery:
Recovery
  ↓
Verification

Verification should check:
1. File existence
2. File size
3. Metadata
4. Accessibility
5. SHA-256

A file that fails verification should be marked/report as unsuccessful.
Do not confuse:
performance

with:
correctness

A faster backup that produces incorrect files is not considered successful.
25. Experimental baselines
The project needs four main scheduling configurations:
Baseline 1 — Sequential
one worker

No parallel scheduling.
Used as the reference for speedup.
Baseline 2 — Static cost-based
Use estimated costs to create an initial partition.
Then workers process their assigned tasks without runtime stealing.
This isolates the benefit of runtime adaptation.
Baseline 3 — Conventional dynamic scheduling
Tasks are dynamically assigned as workers become available.
This provides a comparison with a simpler dynamic approach.
Proposed — IPBRS
Full:
Cost-aware partitioning
+
Runtime work stealing
+
Adaptive task granularity

26. Ablation study
The scheduler should also be tested component by component.
At minimum:
A. Cost-aware partitioning only

B. Cost-aware partitioning
   +
   Work stealing

C. Full IPBRS
   +
   Adaptive granularity

This answers:
Which components actually contribute to the observed performance?

This is important for the research quality of the project.
27. Main research questions
The experiments should answer these questions.
RQ1
Does parallel execution reduce backup/recovery time compared with sequential execution?
RQ2
Does cost-aware initial partitioning reduce load imbalance?
RQ3
Does runtime work stealing improve worker utilization when actual costs differ from estimated costs?
RQ4
Does adaptive task granularity improve load balance compared with fixed task sizes?
RQ5
Does selective recovery reduce recovery work while preserving correctness?
The implementation and experiment outputs should make these questions answerable.
28. Metrics
The program should collect enough data to calculate:
Execution time
T

Measure total backup/recovery execution time.
Dataset generation and damage-generation setup should not be included in the measured backup/recovery time.
Speedup
S = T_seq / T_parallel

Example:
Sequential = 100 s
Parallel   = 40 s

Speedup = 100 / 40 = 2.5×

Parallel efficiency
For P workers:
E = S / P × 100

Throughput
For example:
files processed / second

or another clearly defined workload unit.
CPU utilization
Measure how effectively the workers use the available CPU.
Worker idle time
Measure how long workers remain without useful work.
This is especially important for evaluating work stealing.
Load imbalance
Use:
LI = (Tmax - Tavg) / Tavg

where:
Tmax = maximum worker execution time
Tavg = average worker execution time

Lower imbalance indicates more even workload distribution.
Scheduling overhead
Measure the time/cost introduced by:
- task creation
- scheduling
- synchronization
- stealing
- queue management
This matters because overly fine-grained scheduling can become counterproductive.
Recovery work avoided
Measure:
full recovery workload
-
selective recovery workload

Preferably report both:
files avoided
bytes avoided

when possible.
Recovery success rate
Measure how many affected files are correctly recovered.
Checksum correctness
After recovery:
SHA256(recovered_file)
==
SHA256(reference_file)

This provides a strong correctness check.
29. Experimental matrix
The implementation should allow experiments across:
Dataset size
10K
50K
100K
250K

Workers
1
2
4
8

Heterogeneity
Low
Medium
High

Scheduling strategy
Sequential
Static cost-based
Conventional dynamic
IPBRS

Granularity
Fixed
Adaptive

Recovery damage
Different controlled levels of:
missing
modified
corrupted
truncated

The exact damage ratios should be configurable and finalized for the actual experiment.
30. Fair experimental comparison
When comparing methods:
- Use the same dataset.
- Use the same files.
- Use the same hardware.
- Use the same processing requirements.
- Use the same worker counts.
- Use the same verification requirements.
- Keep dataset generation outside the measured execution time.
- Keep damage-generation setup outside the measured recovery time.
- Run repeated trials once the final experiment protocol is decided.
- Report mean and variation when repeated measurements are available.
Do not compare methods using different workloads.
31. Suggested software architecture
Keep the implementation modular.
A sensible architecture would be:
IPBRS/
│
├── dataset/
│   ├── generator
│   ├── configuration
│   └── damage_generator
│
├── discovery/
│   └── file_discovery
│
├── metadata/
│   ├── file_metadata
│   └── catalog
│
├── checksum/
│   └── sha256
│
├── scheduler/
│   ├── cost_estimator
│   ├── cost_aware_partitioning
│   ├── work_stealing
│   └── adaptive_granularity
│
├── backup/
│   └── parallel_backup
│
├── recovery/
│   ├── change_detection
│   ├── selective_recovery
│   └── full_recovery
│
├── verification/
│   └── verifier
│
├── experiments/
│   ├── benchmark_runner
│   ├── metrics
│   └── result_export
│
└── main

The exact language/project structure can be chosen based on the existing codebase, but the conceptual separation should remain.
32. Important scheduler abstraction
The scheduler should ideally be reusable by both backup and recovery.
Conceptually:
Scheduler
    |
    +-- receives Tasks
    |
    +-- estimates cost
    |
    +-- partitions work
    |
    +-- manages worker queues
    |
    +-- performs stealing
    |
    +-- adapts granularity
    |
    +-- records scheduling metrics

Then:
BackupTask
RecoveryTask

can both use the scheduler.
This avoids duplicating scheduling logic.
33. Important correctness requirements
The implementation must guarantee:
No accidental data loss
Backup must not overwrite unrelated files incorrectly.
No task duplication
The same file should not accidentally be processed multiple times unless explicitly intended.
Thread safety
Shared structures such as the catalog must be synchronized appropriately.
Correct checksums
SHA-256 must be calculated consistently.
Correct recovery
A modified/missing/corrupted/truncated file must be restored from the backup.
Healthy files must remain untouched
Selective recovery should not unnecessarily restore healthy files.
34. Logging
The system should produce useful experimental logs.
For example:
Dataset size: 100000
Heterogeneity: High
Workers: 8
Scheduler: IPBRS

Files discovered: 100000
Files backed up: 100000

Total time: ...
Throughput: ...
Speedup: ...
Efficiency: ...

Worker 1:
    tasks: ...
    work time: ...
    idle time: ...

Worker 2:
    tasks: ...
    work time: ...
    idle time: ...

Work stealing operations: ...
Scheduling overhead: ...

Verification:
    passed: ...
    failed: ...

This will make later analysis much easier.
35. What Codex must NOT do
Do not:
- add ML
- train a neural network
- add CUDA
- add GPU acceleration
- turn this into a distributed system
- use cloud storage
- claim work stealing is novel
- claim OpenMP is novel
- invent experimental results
- invent scheduler thresholds
- invent performance numbers
- claim parallel discovery if it is not implemented
- hard-code fake benchmark results
- remove the baselines
- remove selective recovery
- replace the adaptive scheduler with ordinary OpenMP scheduling and call it IPBRS
- make the project merely copy files in parallel without measuring workload imbalance
The project needs to be an actual experimental parallel systems project.
36. The core conceptual distinction
The most important thing to understand is:
A normal parallel backup system might simply do:
files → OpenMP parallel loop → backup

That is not enough.
IPBRS should do:
Files
  ↓
Estimate cost
  ↓
Cost-aware initial distribution
  ↓
Parallel execution
  ↓
Monitor workload
  ↓
Steal work when workers become idle
  ↓
Adjust task granularity
  ↓
Complete backup/recovery
  ↓
Verify
  ↓
Measure performance

The research question is essentially:
Can adaptive workload scheduling make parallel backup and selective recovery more efficient when file-processing costs are heterogeneous?

37. The complete backup example
Imagine:
10,000 files
8 workers

The files have wildly different sizes.
A naïve static split gives:
Worker 1 → 1250 files
Worker 2 → 1250 files
...
Worker 8 → 1250 files

But this may be terrible because:
Worker 1 → mostly tiny files
Worker 2 → mostly huge files

So Worker 1 finishes early while Worker 2 is still processing.
IPBRS instead estimates:
file A → cost 2
file B → cost 100
file C → cost 5
file D → cost 80
...

Then creates approximately balanced initial workloads.
During execution, if Worker 3 finishes early:
Worker 3 → idle
Worker 6 → still has expensive tasks

Worker 3 can steal work.
If tasks become too large to balance efficiently:
large tasks
    ↓
split into smaller tasks

If tasks become excessively small:
too many tiny tasks
    ↓
increase granularity

That is the essence of the intelligent scheduler.
38. The complete recovery example
Suppose the original backup contains:
10,000 files

Later:
50 missing
100 modified
30 corrupted
20 truncated

Potentially only:
200 affected files

need recovery.
Instead of:
recover 10,000 files

IPBRS performs:
catalog comparison
        ↓
identify 200 affected files
        ↓
estimate recovery costs
        ↓
adaptive scheduling
        ↓
parallel recovery of 200 files
        ↓
SHA-256 verification

The other:
9,800 healthy files

are skipped.
This is the main reason selective recovery matters.
39. Final system
The final IPBRS system should therefore be understood as three major layers:
┌─────────────────────────────────────────────┐
│              DATA / FILE LAYER              │
│                                             │
│ Dataset → Discovery → Metadata → Catalog   │
└──────────────────────┬──────────────────────┘
                       ↓
┌─────────────────────────────────────────────┐
│          ADAPTIVE SCHEDULING LAYER          │
│                                             │
│ Cost Estimation                             │
│       ↓                                     │
│ Cost-Aware Partitioning                     │
│       ↓                                     │
│ Work Stealing                               │
│       ↓                                     │
│ Adaptive Task Granularity                   │
└──────────────────────┬──────────────────────┘
                       ↓
┌─────────────────────────────────────────────┐
│             EXECUTION LAYER                 │
│                                             │
│ Parallel Backup                             │
│ Parallel Selective Recovery                 │
│ Parallel Verification                       │
└─────────────────────────────────────────────┘

The system then produces measurable evidence about:
performance
scalability
load balancing
scheduling overhead
CPU utilization
recovery efficiency
correctness

40. What I expect from Codex
When implementing this project, work in this order:
1. Inspect the existing repository/code.

2. Do not rewrite everything blindly.

3. Identify what is already implemented.

4. Implement/fix:
      dataset generation
      file discovery
      metadata/catalog
      SHA-256
      cost estimation
      scheduler
      backup
      recovery
      verification
      metrics

5. Make the scheduler reusable by backup and recovery.

6. Make all experimental parameters configurable.

7. Implement the baseline schedulers.

8. Implement ablation configurations.

9. Implement benchmark execution and result logging.

10. Test correctness on small datasets first.

11. Test concurrency/thread safety.

12. Only then run large datasets.

13. Never fabricate benchmark results.

14. Keep the implementation consistent with the IPBRS methodology.

Most importantly, if an implementation detail is not yet defined, do not silently invent a research claim or experimental result. Flag it clearly and choose an implementation that can later be documented and measured.
The final code should allow us to answer the five research questions experimentally rather than merely demonstrating that files can be copied in parallel.

## Backup-copy integrity and fault scope

During backup, IPBRS associates each payload with a SHA-256 digest in its catalog and independently verifies stored backup files against the expected size and digest. During recovery, it checks the backup payload against that catalog before and after streaming, then verifies the restored output. A mismatch is reported as an integrity failure; SHA-256 detects a mismatch but cannot reconstruct the expected bytes.

This protects against accidental payload corruption when the catalog remains trustworthy. It does not protect against simultaneous loss or corruption of the payload and its catalog, malicious replacement of both, or a failure that affects the only backup copy. The current prototype does not maintain a redundant second copy or automatically repair a damaged backup payload. Describe those as limitations, not implemented protections.

An independently verified second copy on separate storage is a possible future fault-tolerance extension. It would need separate placement, protected integrity metadata, source selection and fallback, repair behavior, and experiments that corrupt or remove one copy. Do not claim that extension unless it is implemented and measured.
