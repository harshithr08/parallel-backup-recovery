#pragma once

#include "util.hpp"

#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace ipbrs {

enum class Policy {
    Sequential, // one worker, input order: the speedup reference
    Static,     // cost-aware LPT partition, no runtime balancing
    Dynamic,    // conventional self-scheduling: fixed chunks from a shared counter
    Steal       // LPT partition seeds per-worker queues; idle workers steal half a victim's work
};

Policy parse_policy(const std::string& name);
const char* policy_name(Policy policy);

// One unit of schedulable work: a contiguous run of blocks of one file.
// A whole_file task also owns creating its destination file.
struct Task {
    std::uint32_t file = 0;
    std::uint32_t first_block = 0;
    std::uint32_t block_count = 0;
    bool whole_file = true;
    std::uint64_t cost = 1;
};

struct SchedulerConfig {
    Policy policy = Policy::Steal;
    int threads = 1;
    std::size_t chunk = 1; // tasks per claim for Policy::Dynamic
};

struct WorkerStats {
    std::uint64_t busy_ns = 0;        // inside task callbacks
    std::uint64_t claim_ns = 0;       // taking local work or stealing
    std::uint64_t finish_ns = 0;      // when the worker ran out of work, from run start
    std::uint64_t tasks = 0;
    std::uint64_t bytes = 0;
    std::uint64_t cost = 0;
    std::uint64_t steal_attempts = 0; // victim scans, including unsuccessful ones
    std::uint64_t steals = 0;         // successful steals
    std::uint64_t tasks_stolen = 0;
    std::uint64_t cost_stolen = 0;
};

struct SchedulerStats {
    Policy policy = Policy::Sequential;
    int threads = 1;
    std::uint64_t wall_ns = 0;
    std::uint64_t plan_ns = 0;
    std::uint64_t failed_tasks = 0;
    std::vector<WorkerStats> workers;
    std::vector<std::string> errors; // first few failure messages

    // (max busy - mean busy) / mean busy over workers.
    double load_imbalance() const;
    // 1 - total busy time / (workers * wall time).
    double idle_fraction() const;
    // (plan + total claim time) / (workers * wall time).
    double overhead_fraction() const;
    // (latest - earliest worker finish) / wall: the tail where some workers idle.
    double finish_spread() const;
    std::uint64_t claim_ns() const;
    std::uint64_t steals() const;
    std::uint64_t tasks_stolen() const;
    void write_json(Json& json) const;
};

// Returns bytes processed. Exceptions mark the task failed and are recorded.
using Work = std::function<std::uint64_t(const Task&, int worker)>;

SchedulerStats run_tasks(const std::vector<Task>& tasks, const SchedulerConfig& config, const Work& work);

// Longest-processing-time-first greedy partition of task indices by cost.
std::vector<std::vector<std::uint32_t>> lpt_partition(const std::vector<Task>& tasks, int workers);

// Data-parallel loop for setup/finalize phases. body(index, worker). The first
// exception thrown by any iteration is rethrown after the loop.
template <class Body>
void parallel_for(std::size_t count, int threads, Body&& body) {
    if (threads <= 1 || count < 2) {
        for (std::size_t i = 0; i < count; ++i) body(i, 0);
        return;
    }
#ifdef _OPENMP
    std::exception_ptr error;
    std::mutex error_mutex;
    const std::int64_t n = static_cast<std::int64_t>(count);
#pragma omp parallel for schedule(dynamic, 16) num_threads(threads)
    for (std::int64_t i = 0; i < n; ++i) {
        try {
            body(static_cast<std::size_t>(i), omp_get_thread_num());
        } catch (...) {
            std::lock_guard<std::mutex> lock(error_mutex);
            if (!error) error = std::current_exception();
        }
    }
    if (error) std::rethrow_exception(error);
#else
    throw std::runtime_error("parallel execution requested, but this build lacks OpenMP");
#endif
}

} // namespace ipbrs
