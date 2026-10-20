#include "ipbrs/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <queue>
#include <stdexcept>
#include <mutex>
#include <utility>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace ipbrs {
namespace {

typedef std::chrono::steady_clock Clock;

std::uint64_t elapsed_ns(const Clock::time_point& start, const Clock::time_point& finish) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
}

std::size_t actual_workers(const std::vector<Task>& tasks, const SchedulerConfig& config) {
    if (tasks.empty()) return 0;
    if (config.policy == Policy::Sequential) return 1;
    if (config.worker_count == 0) throw std::invalid_argument("worker_count must be greater than zero");
    const std::size_t workers = std::min(config.worker_count, tasks.size());
    if (workers > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("worker_count exceeds OpenMP's supported integer range");
    return workers;
}

bool claim_chunk(std::atomic<std::size_t>& next_index, std::size_t task_count,
                 std::size_t requested_chunk, std::size_t& begin, std::size_t& end) {
    std::size_t current = next_index.load();
    while (current < task_count) {
        const std::size_t amount = std::min(requested_chunk, task_count - current);
        const std::size_t next = current + amount;
        if (next_index.compare_exchange_weak(current, next)) {
            begin = current;
            end = next;
            return true;
        }
    }
    return false;
}

struct WorkQueue {
    std::vector<std::size_t> items;
    std::size_t head;
    std::size_t tail;
    std::mutex mutex;
    WorkQueue() : head(0), tail(0) {}
};

bool take_local_batch(WorkQueue& queue, std::size_t amount, std::vector<std::size_t>& batch) {
    std::lock_guard<std::mutex> lock(queue.mutex);
    if (queue.head >= queue.tail) return false;
    const std::size_t end = std::min(queue.tail, queue.head + amount);
    batch.insert(batch.end(), queue.items.begin() + queue.head, queue.items.begin() + end);
    queue.head = end;
    return true;
}

bool steal_batch(WorkQueue& queue, std::size_t amount, std::vector<std::size_t>& batch) {
    std::lock_guard<std::mutex> lock(queue.mutex);
    if (queue.tail <= queue.head) return false;
    const std::size_t begin = queue.head + std::max<std::size_t>(1, (queue.tail - queue.head + 1) / 2);
    if (begin >= queue.tail) return false;
    const std::size_t available = queue.tail - begin;
    const std::size_t stolen = std::min(amount, available);
    const std::size_t new_tail = queue.tail - stolen;
    batch.insert(batch.end(), queue.items.begin() + new_tail, queue.items.begin() + queue.tail);
    queue.tail = new_tail;
    return true;
}

TaskResult invoke(const Task& task, std::size_t worker_id, const TaskCallback& callback) {
    TaskResult result;
    result.task_id = task.id;
    result.worker_id = worker_id;
    const Clock::time_point start = Clock::now();
    try {
        const TaskOutcome outcome = callback(task, worker_id);
        result.success = outcome.success;
        result.bytes_processed = outcome.bytes_processed;
        result.error_code = outcome.error_code;
        result.error_message = outcome.error_message;
    } catch (const std::exception& error) {
        result.success = false;
        result.error_code = "WORKER_EXCEPTION";
        result.error_message = error.what();
    } catch (...) {
        result.success = false;
        result.error_code = "UNKNOWN_WORKER_EXCEPTION";
        result.error_message = "worker callback threw a non-standard exception";
    }
    result.duration_ns = elapsed_ns(start, Clock::now());
    return result;
}

void add_worker_result(WorkerMetrics& metrics, const TaskResult& result) {
    ++metrics.tasks_completed;
    if (!result.success) ++metrics.tasks_failed;
    metrics.bytes_processed += result.bytes_processed;
    metrics.busy_ns += result.duration_ns;
}

std::vector<std::vector<std::size_t> > make_lpt_partitions(
        const std::vector<Task>& tasks, std::size_t workers) {
    std::vector<std::size_t> order(tasks.size());
    for (std::size_t i = 0; i < tasks.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&tasks](std::size_t left, std::size_t right) {
        if (tasks[left].estimated_cost != tasks[right].estimated_cost)
            return tasks[left].estimated_cost > tasks[right].estimated_cost;
        return tasks[left].id < tasks[right].id;
    });

    typedef std::pair<double, std::size_t> HeapEntry;
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, std::greater<HeapEntry> > loads;
    std::vector<std::vector<std::size_t> > partitions(workers);
    for (std::size_t worker = 0; worker < workers; ++worker)
        loads.push(std::make_pair(0.0, worker));

    for (std::size_t i = 0; i < order.size(); ++i) {
        const HeapEntry least = loads.top();
        loads.pop();
        partitions[least.second].push_back(order[i]);
        loads.push(std::make_pair(least.first + tasks[order[i]].estimated_cost, least.second));
    }
    return partitions;
}

} // namespace

const char* policy_name(Policy policy) {
    switch (policy) {
        case Policy::Sequential: return "sequential";
        case Policy::StaticCostBased: return "static_cost_based";
        case Policy::DynamicFixedChunk: return "dynamic_fixed_chunk";
        case Policy::CostAwareWorkStealing: return "cost_aware_work_stealing";
        case Policy::CostAwareAdaptiveWorkStealing: return "cost_aware_adaptive_work_stealing";
    }
    return "unknown";
}

SchedulerResult run_scheduler(const std::vector<Task>& tasks,
                              const SchedulerConfig& config,
                              const TaskCallback& callback) {
    if (!callback) throw std::invalid_argument("task callback must be set");
    if (config.policy != Policy::Sequential &&
        config.policy != Policy::StaticCostBased &&
        config.policy != Policy::DynamicFixedChunk &&
        config.policy != Policy::CostAwareWorkStealing &&
        config.policy != Policy::CostAwareAdaptiveWorkStealing)
        throw std::invalid_argument("unknown scheduling policy");
    if ((config.policy == Policy::DynamicFixedChunk || config.policy == Policy::CostAwareWorkStealing ||
         config.policy == Policy::CostAwareAdaptiveWorkStealing) && config.chunk_size == 0)
        throw std::invalid_argument("dynamic and stealing chunk_size must be greater than zero");
    if (config.policy == Policy::CostAwareAdaptiveWorkStealing &&
        (config.min_chunk_size == 0 || config.max_chunk_size < config.min_chunk_size))
        throw std::invalid_argument("adaptive chunk bounds must satisfy 0 < min_chunk_size <= max_chunk_size");
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        if (!std::isfinite(tasks[i].estimated_cost) || tasks[i].estimated_cost < 0.0)
            throw std::invalid_argument("task estimated_cost must be finite and non-negative");
    }
#ifndef _OPENMP
    if (config.policy != Policy::Sequential &&
        std::min(config.worker_count, tasks.size()) > 1)
        throw std::runtime_error("parallel scheduling requested, but this build lacks OpenMP");
#endif

    SchedulerResult result;
    result.policy = config.policy;
    result.requested_workers = config.policy == Policy::Sequential ? 1 : config.worker_count;
    const std::size_t workers = actual_workers(tasks, config);
    result.workers_used = workers;
    result.workers.resize(workers);
    result.tasks.resize(tasks.size());
    if (tasks.empty()) return result;

    const Clock::time_point whole_start = Clock::now();
    if (config.policy == Policy::Sequential) {
        for (std::size_t i = 0; i < tasks.size(); ++i) {
            result.tasks[i] = invoke(tasks[i], 0, callback);
            add_worker_result(result.workers[0], result.tasks[i]);
        }
    } else if (config.policy == Policy::StaticCostBased) {
        const Clock::time_point planning_start = Clock::now();
        const std::vector<std::vector<std::size_t> > partitions = make_lpt_partitions(tasks, workers);
        result.scheduling_overhead_ns += elapsed_ns(planning_start, Clock::now());
#ifdef _OPENMP
        const int omp_workers = static_cast<int>(workers);
        std::atomic<std::size_t> observed_workers(0);
#pragma omp parallel for num_threads(omp_workers) schedule(static, 1)
        for (long long partition = 0; partition < static_cast<long long>(workers); ++partition) {
            const std::size_t worker = static_cast<std::size_t>(omp_get_thread_num());
            std::size_t observed = observed_workers.load();
            while (observed < worker + 1 &&
                   !observed_workers.compare_exchange_weak(observed, worker + 1)) {}
            if (worker < workers) {
                const std::size_t partition_index = static_cast<std::size_t>(partition);
                for (std::size_t j = 0; j < partitions[partition_index].size(); ++j) {
                    const std::size_t index = partitions[partition_index][j];
                    result.tasks[index] = invoke(tasks[index], worker, callback);
                    add_worker_result(result.workers[worker], result.tasks[index]);
                }
            }
        }
        result.workers_used = observed_workers.load();
#else
        for (std::size_t worker = 0; worker < workers; ++worker) {
            for (std::size_t j = 0; j < partitions[worker].size(); ++j) {
                const std::size_t index = partitions[worker][j];
                result.tasks[index] = invoke(tasks[index], worker, callback);
                add_worker_result(result.workers[worker], result.tasks[index]);
            }
        }
#endif
    } else if (config.policy == Policy::DynamicFixedChunk) {
        std::atomic<std::size_t> next_index(0);
        std::atomic<std::uint64_t> claim_overhead(0);
#ifdef _OPENMP
        const int omp_workers = static_cast<int>(workers);
#pragma omp parallel num_threads(omp_workers)
        {
#pragma omp single
            result.workers_used = static_cast<std::size_t>(omp_get_num_threads());
            const std::size_t worker = static_cast<std::size_t>(omp_get_thread_num());
            if (worker < workers) {
                while (true) {
                    const Clock::time_point claim_start = Clock::now();
                    std::size_t begin = 0;
                    std::size_t end = 0;
                    const bool claimed = claim_chunk(next_index, tasks.size(), config.chunk_size, begin, end);
                    const std::uint64_t claim_ns = elapsed_ns(claim_start, Clock::now());
                    claim_overhead.fetch_add(claim_ns);
                    if (!claimed) break;
                    for (std::size_t index = begin; index < end; ++index) {
                        result.tasks[index] = invoke(tasks[index], worker, callback);
                        add_worker_result(result.workers[worker], result.tasks[index]);
                    }
                }
            }
        }
#else
        std::size_t worker = 0;
        while (true) {
            const Clock::time_point claim_start = Clock::now();
            std::size_t begin = 0;
            std::size_t end = 0;
            const bool claimed = claim_chunk(next_index, tasks.size(), config.chunk_size, begin, end);
            claim_overhead.fetch_add(elapsed_ns(claim_start, Clock::now()));
            if (!claimed) break;
            for (std::size_t index = begin; index < end; ++index) {
                result.tasks[index] = invoke(tasks[index], worker, callback);
                add_worker_result(result.workers[worker], result.tasks[index]);
            }
            worker = (worker + 1) % workers;
        }
#endif
        result.scheduling_overhead_ns += claim_overhead.load();
    } else if (config.policy == Policy::CostAwareWorkStealing) {
        const Clock::time_point planning_start = Clock::now();
        const std::vector<std::vector<std::size_t> > partitions = make_lpt_partitions(tasks, workers);
        std::vector<WorkQueue> queues(workers);
        for (std::size_t w = 0; w < workers; ++w) {
            queues[w].items = partitions[w];
            queues[w].tail = partitions[w].size();
        }
        result.scheduling_overhead_ns += elapsed_ns(planning_start, Clock::now());
        std::atomic<std::uint64_t> claim_overhead(0);
        std::atomic<std::size_t> observed_workers(0);
#ifdef _OPENMP
        const int omp_workers = static_cast<int>(workers);
#pragma omp parallel num_threads(omp_workers)
        {
            const std::size_t worker = static_cast<std::size_t>(omp_get_thread_num());
            std::size_t observed = observed_workers.load();
            while (observed < worker + 1 &&
                   !observed_workers.compare_exchange_weak(observed, worker + 1)) {}
            std::vector<std::size_t> batch;
            while (true) {
                batch.clear();
                Clock::time_point claim_start = Clock::now();
                bool found = take_local_batch(queues[worker], config.chunk_size, batch);
                if (!found) {
                    for (std::size_t offset = 1; offset < workers; ++offset) {
                        const std::size_t victim = (worker + offset) % workers;
                        ++result.workers[worker].steal_attempts;
                        if (steal_batch(queues[victim], config.chunk_size, batch)) {
                            ++result.workers[worker].steals_succeeded;
                            found = true;
                            break;
                        }
                    }
                }
                claim_overhead.fetch_add(elapsed_ns(claim_start, Clock::now()));
                if (!found) break;
                for (std::size_t i = 0; i < batch.size(); ++i) {
                    const std::size_t index = batch[i];
                    result.tasks[index] = invoke(tasks[index], worker, callback);
                    add_worker_result(result.workers[worker], result.tasks[index]);
                }
            }
        }
        result.workers_used = observed_workers.load();
#else
        for (std::size_t worker = 0; worker < workers; ++worker) {
            std::vector<std::size_t> batch;
            while (true) {
                batch.clear();
                const Clock::time_point claim_start = Clock::now();
                bool found = take_local_batch(queues[worker], config.chunk_size, batch);
                if (!found) {
                    for (std::size_t offset = 1; offset < workers; ++offset) {
                        const std::size_t victim = (worker + offset) % workers;
                        ++result.workers[worker].steal_attempts;
                        if (steal_batch(queues[victim], config.chunk_size, batch)) {
                            ++result.workers[worker].steals_succeeded; found = true; break;
                        }
                    }
                }
                claim_overhead.fetch_add(elapsed_ns(claim_start, Clock::now()));
                if (!found) break;
                for (std::size_t i = 0; i < batch.size(); ++i) {
                    const std::size_t index = batch[i];
                    result.tasks[index] = invoke(tasks[index], worker, callback);
                    add_worker_result(result.workers[worker], result.tasks[index]);
                }
            }
        }
#endif
        result.scheduling_overhead_ns += claim_overhead.load();
    } else if (config.policy == Policy::CostAwareAdaptiveWorkStealing) {
        double mean = 0.0;
        for (std::size_t i = 0; i < tasks.size(); ++i) mean += tasks[i].estimated_cost;
        mean /= static_cast<double>(tasks.size());
        double variance = 0.0;
        if (mean > 0.0) {
            for (std::size_t i = 0; i < tasks.size(); ++i) {
                const double delta = tasks[i].estimated_cost - mean;
                variance += delta * delta;
            }
            variance /= static_cast<double>(tasks.size());
            result.estimated_cost_cv = std::sqrt(variance) / mean;
        }
        const std::size_t bounded_base = static_cast<std::size_t>(std::max<double>(
            static_cast<double>(config.min_chunk_size),
            std::min<double>(static_cast<double>(config.max_chunk_size),
                             static_cast<double>(config.chunk_size))));
        result.initial_adaptive_chunk = bounded_base;

        const Clock::time_point planning_start = Clock::now();
        const std::vector<std::vector<std::size_t> > partitions = make_lpt_partitions(tasks, workers);
        std::vector<WorkQueue> queues(workers);
        for (std::size_t w = 0; w < workers; ++w) {
            queues[w].items = partitions[w];
            queues[w].tail = partitions[w].size();
        }
        result.scheduling_overhead_ns += elapsed_ns(planning_start, Clock::now());
        std::atomic<std::size_t> unfinished(tasks.size());
        std::atomic<std::uint64_t> claim_overhead(0);
        std::atomic<std::size_t> observed_workers(0);
        std::mutex decision_mutex;
        const auto requested_chunk = [&]() -> std::size_t {
            const double remaining_ratio = static_cast<double>(unfinished.load()) /
                                           static_cast<double>(tasks.size());
            const double scaled = std::ceil(static_cast<double>(bounded_base) * remaining_ratio);
            return static_cast<std::size_t>(std::max<double>(
                static_cast<double>(config.min_chunk_size),
                std::min<double>(static_cast<double>(config.max_chunk_size), scaled)));
        };
        const auto record_decision = [&](std::size_t worker, std::size_t amount, bool stealing) {
            GranularityDecision decision;
            decision.worker_id = worker;
            decision.chunk_size = amount;
            decision.unfinished_tasks = unfinished.load();
            decision.stealing = stealing;
            std::lock_guard<std::mutex> lock(decision_mutex);
            result.granularity_decisions.push_back(decision);
        };
#ifdef _OPENMP
        const int omp_workers = static_cast<int>(workers);
#pragma omp parallel num_threads(omp_workers)
        {
            const std::size_t worker = static_cast<std::size_t>(omp_get_thread_num());
            std::size_t observed = observed_workers.load();
            while (observed < worker + 1 &&
                   !observed_workers.compare_exchange_weak(observed, worker + 1)) {}
            std::vector<std::size_t> batch;
            std::size_t last_amount = 0;
            while (true) {
                batch.clear();
                const Clock::time_point claim_start = Clock::now();
                const std::size_t amount = requested_chunk();
                bool found = take_local_batch(queues[worker], amount, batch);
                bool stolen = false;
                if (!found) {
                    for (std::size_t offset = 1; offset < workers; ++offset) {
                        const std::size_t victim = (worker + offset) % workers;
                        ++result.workers[worker].steal_attempts;
                        if (steal_batch(queues[victim], amount, batch)) {
                            ++result.workers[worker].steals_succeeded;
                            found = true;
                            stolen = true;
                            break;
                        }
                    }
                }
                claim_overhead.fetch_add(elapsed_ns(claim_start, Clock::now()));
                if (!found) break;
                if (amount != last_amount) {
                    record_decision(worker, amount, stolen);
                    last_amount = amount;
                }
                for (std::size_t i = 0; i < batch.size(); ++i) {
                    const std::size_t index = batch[i];
                    result.tasks[index] = invoke(tasks[index], worker, callback);
                    add_worker_result(result.workers[worker], result.tasks[index]);
                    unfinished.fetch_sub(1);
                }
            }
        }
        result.workers_used = observed_workers.load();
#else
        for (std::size_t worker = 0; worker < workers; ++worker) {
            std::vector<std::size_t> batch;
            std::size_t last_amount = 0;
            while (true) {
                batch.clear();
                const Clock::time_point claim_start = Clock::now();
                const std::size_t amount = requested_chunk();
                bool found = take_local_batch(queues[worker], amount, batch);
                bool stolen = false;
                if (!found) {
                    for (std::size_t offset = 1; offset < workers; ++offset) {
                        const std::size_t victim = (worker + offset) % workers;
                        ++result.workers[worker].steal_attempts;
                        if (steal_batch(queues[victim], amount, batch)) {
                            ++result.workers[worker].steals_succeeded;
                            found = true;
                            stolen = true;
                            break;
                        }
                    }
                }
                claim_overhead.fetch_add(elapsed_ns(claim_start, Clock::now()));
                if (!found) break;
                if (amount != last_amount) {
                    record_decision(worker, amount, stolen);
                    last_amount = amount;
                }
                for (std::size_t i = 0; i < batch.size(); ++i) {
                    const std::size_t index = batch[i];
                    result.tasks[index] = invoke(tasks[index], worker, callback);
                    add_worker_result(result.workers[worker], result.tasks[index]);
                    unfinished.fetch_sub(1);
                }
            }
        }
#endif
        result.scheduling_overhead_ns += claim_overhead.load();
    }

    result.elapsed_ns = elapsed_ns(whole_start, Clock::now());
    return result;
}

} // namespace ipbrs




