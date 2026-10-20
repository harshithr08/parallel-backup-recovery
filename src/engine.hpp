#pragma once

#include "catalog.hpp"
#include "scheduler.hpp"
#include "workload.hpp"

#include <cstdint>
#include <string>

namespace ipbrs {

// Scheduling settings shared by every operation. IPBRS = Steal + Adaptive.
struct RunConfig {
    SchedulerConfig sched;
    Split split = Split::Adaptive;
    std::uint64_t split_factor = 8;
    std::uint64_t per_task_cost = 64u * 1024;
    int threads() const { return sched.policy == Policy::Sequential ? 1 : sched.threads; }
    void write_json(Json& json) const;
};

struct BackupOptions {
    fs::path source;
    fs::path backup; // must be new or empty; receives files/ and catalog.ipbrs
    std::uint32_t block_size = 1u << 20;
    bool sync = true; // flush every stored object before committing the catalog
    RunConfig run;
};

struct BackupResult {
    std::uint64_t files = 0;
    std::uint64_t bytes = 0;
    std::uint64_t failed_files = 0;
    bool committed = false; // catalog written only when every file succeeded
    std::string json;
};

BackupResult run_backup(const BackupOptions& options);

struct VerifyOptions {
    fs::path backup;
    RunConfig run;
};

struct VerifyResult {
    std::uint64_t files = 0;
    std::uint64_t bad = 0;
    std::string json;
};

// Re-hashes every stored block of a backup against its catalog.
VerifyResult run_verify(const VerifyOptions& options);

enum class RecoveryMode {
    Block, // selective: restore only damaged blocks of same-size files
    File,  // selective: restore whole damaged files
    Full   // baseline: restore every cataloged file without inspecting the target
};

RecoveryMode parse_recovery_mode(const std::string& name);
const char* recovery_mode_name(RecoveryMode mode);

struct RecoverOptions {
    fs::path source; // tree to repair
    fs::path backup;
    RecoveryMode mode = RecoveryMode::Block;
    bool trust_mtime = false; // skip hashing files whose size and mtime match
    bool post_verify = true;  // re-hash repaired files afterwards (timed separately)
    bool sync = true;         // flush repaired files to stable storage
    bool dry_run = false;     // classify and plan only; the source is not modified
    RunConfig run;
};

struct RecoverResult {
    std::uint64_t catalog_files = 0;
    std::uint64_t missing = 0;
    std::uint64_t size_changed = 0;
    std::uint64_t content_damaged = 0;
    std::uint64_t metadata_only = 0;
    std::uint64_t unreadable = 0;
    std::uint64_t damaged_blocks = 0;
    std::uint64_t planned_files = 0; // files that need restoring (also set by dry runs)
    std::uint64_t planned_bytes = 0;
    std::uint64_t restored_files = 0;
    std::uint64_t restored_bytes = 0;
    std::uint64_t failed_files = 0;
    std::uint64_t verify_bad = 0;
    std::string json;
};

RecoverResult run_recover(const RecoverOptions& options);

} // namespace ipbrs
