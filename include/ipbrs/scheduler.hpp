#ifndef IPBRS_SCHEDULER_HPP
#define IPBRS_SCHEDULER_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ipbrs {

struct Task {
    std::string id;
    std::string relative_path;
    std::uint64_t size_bytes;
    double estimated_cost;
    std::uint64_t source_last_write_time_100ns;
    std::string file_type;

    Task() : size_bytes(0), estimated_cost(0.0), source_last_write_time_100ns(0) {}
    Task(const std::string& task_id, const std::string& path,
         std::uint64_t bytes, double cost)
        : id(task_id), relative_path(path), size_bytes(bytes), estimated_cost(cost),
          source_last_write_time_100ns(0) {}
};

enum class Policy {
    Sequential,
    StaticCostBased,
    DynamicFixedChunk,
    CostAwareWorkStealing,
    CostAwareAdaptiveWorkStealing
};

struct SchedulerConfig {
    Policy policy;
    std::size_t worker_count;
    std::size_t chunk_size;
    std::size_t min_chunk_size;
    std::size_t max_chunk_size;

    SchedulerConfig() : policy(Policy::Sequential), worker_count(1), chunk_size(1),
                        min_chunk_size(1), max_chunk_size(64) {}
};

struct TaskOutcome {
    bool success;
    std::uint64_t bytes_processed;
    std::string error_code;
    std::string error_message;

    TaskOutcome() : success(true), bytes_processed(0) {}
};

struct TaskResult {
    std::string task_id;
    std::size_t worker_id;
    bool success;
    std::uint64_t bytes_processed;
    std::uint64_t duration_ns;
    std::string error_code;
    std::string error_message;

    TaskResult()
        : worker_id(0), success(false), bytes_processed(0), duration_ns(0) {}
};

struct WorkerMetrics {
    std::size_t tasks_completed;
    std::size_t tasks_failed;
    std::uint64_t bytes_processed;
    std::uint64_t busy_ns;
    std::size_t steal_attempts;
    std::size_t steals_succeeded;

    WorkerMetrics()
        : tasks_completed(0), tasks_failed(0), bytes_processed(0), busy_ns(0),
          steal_attempts(0), steals_succeeded(0) {}
};

struct GranularityDecision {
    std::size_t worker_id;
    std::size_t chunk_size;
    std::size_t unfinished_tasks;
    bool stealing;

    GranularityDecision() : worker_id(0), chunk_size(0), unfinished_tasks(0), stealing(false) {}
};

struct SchedulerResult {
    Policy policy;
    std::size_t requested_workers;
    std::size_t workers_used;
    std::uint64_t elapsed_ns;
    std::uint64_t scheduling_overhead_ns;
    std::vector<WorkerMetrics> workers;
    std::vector<TaskResult> tasks;
    double estimated_cost_cv;
    std::size_t initial_adaptive_chunk;
    std::vector<GranularityDecision> granularity_decisions;

    SchedulerResult()
        : policy(Policy::Sequential), requested_workers(1), workers_used(0),
          elapsed_ns(0), scheduling_overhead_ns(0), estimated_cost_cv(0.0),
          initial_adaptive_chunk(0) {}
};

// The callback must be thread-safe whenever worker_count > 1. Exceptions are
// caught by the scheduler and returned as failed TaskResult records.
typedef std::function<TaskOutcome(const Task&, std::size_t)> TaskCallback;

SchedulerResult run_scheduler(const std::vector<Task>& tasks,
                              const SchedulerConfig& config,
                              const TaskCallback& callback);

const char* policy_name(Policy policy);

} // namespace ipbrs

#endif
