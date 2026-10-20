#ifndef IPBRS_BACKUP_HPP
#define IPBRS_BACKUP_HPP

#include "ipbrs/scheduler.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ipbrs {

struct BackupConfig {
    std::string source_root_utf8;
    std::string backup_root_utf8;
    SchedulerConfig scheduler;
};

struct BackupSummary {
    bool success;
    std::string backup_id;
    std::string snapshot_path_utf8;
    std::string catalog_path_utf8;
    std::string error_message;
    std::uint64_t total_elapsed_ns;
    std::uint64_t discovery_elapsed_ns;
    std::size_t discovery_workers_used;
    SchedulerResult scheduling;

    BackupSummary() : success(false), total_elapsed_ns(0), discovery_elapsed_ns(0), discovery_workers_used(0) {}
};

struct RecoveryConfig {
    std::string source_root_utf8;
    std::string backup_root_utf8;
    std::string catalog_path_utf8;
    bool full_recovery;
    SchedulerConfig scheduler;

    RecoveryConfig() : full_recovery(false) {}
};

struct RecoverySummary {
    bool success;
    std::string status;
    std::uint64_t total_elapsed_ns;
    std::string backup_id;
    std::string error_message;
    std::size_t catalog_files;
    std::size_t restore_tasks;
    std::size_t files_missing;
    std::size_t files_modified;
    std::size_t files_inaccessible;
    std::size_t files_extra;
    std::size_t files_restored;
    std::size_t files_skipped;
    std::size_t files_failed;
    std::uint64_t bytes_restored;
    std::uint64_t bytes_avoided;
    SchedulerResult scheduling;

    RecoverySummary()
        : success(false), status("FAILED"), total_elapsed_ns(0), catalog_files(0), restore_tasks(0), files_missing(0),
          files_modified(0), files_inaccessible(0), files_extra(0), files_restored(0),
          files_skipped(0), files_failed(0), bytes_restored(0), bytes_avoided(0) {}
};

struct VerificationSummary {
    bool success;
    std::string backup_id;
    std::string error_message;
    std::uint64_t total_elapsed_ns;
    std::size_t catalog_files;
    std::size_t files_verified;
    std::size_t files_failed;
    std::size_t files_missing;
    std::size_t files_size_mismatch;
    std::size_t files_timestamp_mismatch;
    std::size_t files_digest_mismatch;
    std::size_t files_inaccessible;
    std::size_t files_unsafe;
    SchedulerResult scheduling;
    std::vector<TaskResult> files;

    VerificationSummary()
        : success(false), total_elapsed_ns(0), catalog_files(0), files_verified(0), files_failed(0),
          files_missing(0), files_size_mismatch(0), files_timestamp_mismatch(0), files_digest_mismatch(0),
          files_inaccessible(0), files_unsafe(0) {}
};

// Windows implementation: discovers regular non-symlink files, copies each
// task through the selected scheduler, verifies destination SHA-256, and emits
// a catalog only when every task completes successfully.
BackupSummary run_backup(const BackupConfig& config);

// Restores missing or content-changed files from a verified C++ JSON catalog.
// The backup root is the parent containing snapshot directories.
RecoverySummary run_recovery(const RecoveryConfig& config);

// Independently hashes every cataloged file in a C++ JSON snapshot using the
// selected scheduler (sequential by default).
VerificationSummary verify_backup(const std::string& backup_root_utf8,
                                  const std::string& catalog_path_utf8,
                                  const SchedulerConfig& scheduler = SchedulerConfig());

} // namespace ipbrs

#endif
