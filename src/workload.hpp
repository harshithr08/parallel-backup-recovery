#pragma once

#include "fileio.hpp"
#include "scheduler.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ipbrs {

struct FileEntry {
    std::string path; // relative, '/'-separated, UTF-8
    std::uint64_t size = 0;
    FileTime mtime = 0;
};

struct DiscoveryStats {
    std::uint64_t ns = 0;
    std::uint64_t directories = 0;
    int threads = 1;
};

// Level-synchronous parallel directory walk; result sorted by path.
// Symbolic links are skipped.
std::vector<FileEntry> discover_files(const fs::path& root, int threads, DiscoveryStats* stats);

inline fs::path native_path(const fs::path& root, const std::string& relative) {
    return root / fs::u8path(relative);
}

enum class Split {
    None,     // one task per file range (file-level scheduling)
    Adaptive  // split ranges larger than total_cost / (threads * split_factor)
};

Split parse_split(const std::string& name);
const char* split_name(Split split);

// A run of blocks of one file that must be processed.
struct Range {
    std::uint32_t file = 0;
    std::uint32_t first_block = 0;
    std::uint32_t block_count = 1;
    std::uint64_t bytes = 0;
    bool whole_file = true;
};

struct PlanConfig {
    Split split = Split::Adaptive;
    int threads = 1;
    std::uint32_t block_size = 1u << 20;
    std::uint64_t split_factor = 8;
    std::uint64_t per_task_cost = 64u * 1024; // fixed open/create overhead, in byte units
    std::uint32_t align_blocks = 1; // pieces are whole multiples of this (backup segments)
};

struct PlanStats {
    std::uint64_t ranges = 0;
    std::uint64_t tasks = 0;
    std::uint64_t total_cost = 0;
    std::uint64_t max_task_cost = 0;
    std::uint64_t split_threshold = 0; // 0 when splitting is off
    std::vector<std::uint32_t> split_files; // files whose ranges were divided
    void write_json(Json& json) const;
};

// Cost model: cost(task) = per_task_cost + bytes. With adaptive splitting no
// task exceeds max(block, total / (threads * split_factor)), which bounds the
// list-scheduling makespan by total/threads + that piece size.
std::vector<Task> plan_tasks(const std::vector<Range>& ranges, const PlanConfig& config, PlanStats* stats);

} // namespace ipbrs
