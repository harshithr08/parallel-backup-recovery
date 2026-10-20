#include "scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <queue>

namespace ipbrs {
namespace {

// Runs body(worker) on exactly `threads` OpenMP threads. body must not throw.
template <class Body>
void parallel_region(int threads, Body&& body) {
#ifdef _OPENMP
    omp_set_dynamic(0);
    int granted = 0;
#pragma omp parallel num_threads(threads)
    {
#pragma omp single
        granted = omp_get_num_threads();
        if (granted == threads) body(omp_get_thread_num());
    }
    if (granted != threads)
        throw std::runtime_error("OpenMP granted " + std::to_string(granted) + " of " +
                                 std::to_string(threads) + " threads");
#else
    if (threads != 1) throw std::runtime_error("parallel execution requested, but this build lacks OpenMP");
    body(0);
#endif
}

struct StealQueue {
    std::mutex mutex;
    std::vector<std::uint32_t> items; // largest cost first
    std::size_t head = 0;
    std::atomic<std::uint64_t> remaining{0}; // estimated cost not yet claimed
};

} // namespace

Policy parse_policy(const std::string& name) {
    if (name == "seq" || name == "sequential") return Policy::Sequential;
    if (name == "static") return Policy::Static;
    if (name == "dynamic") return Policy::Dynamic;
    if (name == "steal") return Policy::Steal;
    throw std::invalid_argument("unknown scheduler policy: " + name);
}

const char* policy_name(Policy policy) {
    switch (policy) {
        case Policy::Sequential: return "seq";
        case Policy::Static: return "static";
        case Policy::Dynamic: return "dynamic";
        case Policy::Steal: return "steal";
    }
    return "unknown";
}

double SchedulerStats::load_imbalance() const {
    if (workers.empty()) return 0.0;
    std::uint64_t max_busy = 0, total = 0;
    for (const WorkerStats& w : workers) {
        max_busy = std::max(max_busy, w.busy_ns);
        total += w.busy_ns;
    }
    const double mean = static_cast<double>(total) / static_cast<double>(workers.size());
    return mean > 0.0 ? (static_cast<double>(max_busy) - mean) / mean : 0.0;
}

double SchedulerStats::idle_fraction() const {
    if (workers.empty() || wall_ns == 0) return 0.0;
    std::uint64_t total = 0;
    for (const WorkerStats& w : workers) total += w.busy_ns;
    const double capacity = static_cast<double>(wall_ns) * static_cast<double>(workers.size());
    return std::max(0.0, 1.0 - static_cast<double>(total) / capacity);
}

double SchedulerStats::overhead_fraction() const {
    if (workers.empty() || wall_ns == 0) return 0.0;
    const double capacity = static_cast<double>(wall_ns) * static_cast<double>(workers.size());
    return static_cast<double>(plan_ns + claim_ns()) / capacity;
}

double SchedulerStats::finish_spread() const {
    if (workers.empty() || wall_ns == 0) return 0.0;
    std::uint64_t first = UINT64_MAX, last = 0;
    for (const WorkerStats& w : workers) {
        first = std::min(first, w.finish_ns);
        last = std::max(last, w.finish_ns);
    }
    return static_cast<double>(last - first) / static_cast<double>(wall_ns);
}

std::uint64_t SchedulerStats::tasks_stolen() const {
    std::uint64_t total = 0;
    for (const WorkerStats& w : workers) total += w.tasks_stolen;
    return total;
}

std::uint64_t SchedulerStats::claim_ns() const {
    std::uint64_t total = 0;
    for (const WorkerStats& w : workers) total += w.claim_ns;
    return total;
}

std::uint64_t SchedulerStats::steals() const {
    std::uint64_t total = 0;
    for (const WorkerStats& w : workers) total += w.steals;
    return total;
}

void SchedulerStats::write_json(Json& json) const {
    json.begin_object()
        .field("policy", policy_name(policy))
        .field("threads", threads)
        .field("wall_ns", wall_ns)
        .field("plan_ns", plan_ns)
        .field("claim_ns", claim_ns())
        .field("failed_tasks", failed_tasks)
        .field("load_imbalance", load_imbalance())
        .field("idle_fraction", idle_fraction())
        .field("overhead_fraction", overhead_fraction())
        .field("finish_spread", finish_spread())
        .field("steals", steals())
        .field("tasks_stolen", tasks_stolen());
    json.key("workers").begin_array();
    for (const WorkerStats& w : workers) {
        json.begin_object()
            .field("busy_ns", w.busy_ns)
            .field("idle_ns", wall_ns > w.busy_ns ? wall_ns - w.busy_ns : 0)
            .field("claim_ns", w.claim_ns)
            .field("finish_ns", w.finish_ns)
            .field("tasks", w.tasks)
            .field("bytes", w.bytes)
            .field("cost", w.cost)
            .field("steal_attempts", w.steal_attempts)
            .field("steals", w.steals)
            .field("tasks_stolen", w.tasks_stolen)
            .field("cost_stolen", w.cost_stolen)
            .end_object();
    }
    json.end_array();
    json.key("errors").begin_array();
    for (const std::string& error : errors) json.value(error);
    json.end_array().end_object();
}

std::vector<std::vector<std::uint32_t>> lpt_partition(const std::vector<Task>& tasks, int workers) {
    std::vector<std::uint32_t> order(tasks.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<std::uint32_t>(i);
    std::stable_sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
        return tasks[a].cost > tasks[b].cost;
    });
    typedef std::pair<std::uint64_t, int> Load;
    std::priority_queue<Load, std::vector<Load>, std::greater<Load>> loads;
    for (int w = 0; w < workers; ++w) loads.push(Load(0, w));
    std::vector<std::vector<std::uint32_t>> parts(static_cast<std::size_t>(workers));
    for (std::uint32_t index : order) {
        Load least = loads.top();
        loads.pop();
        parts[static_cast<std::size_t>(least.second)].push_back(index);
        least.first += tasks[index].cost;
        loads.push(least);
    }
    return parts;
}

SchedulerStats run_tasks(const std::vector<Task>& tasks, const SchedulerConfig& config, const Work& work) {
    if (config.threads < 1) throw std::invalid_argument("threads must be at least 1");
    if (config.chunk < 1) throw std::invalid_argument("chunk must be at least 1");

    SchedulerStats stats;
    stats.policy = config.policy;
    const int threads = config.policy == Policy::Sequential ? 1 : config.threads;
    stats.threads = threads;
    stats.workers.assign(static_cast<std::size_t>(threads), WorkerStats());
    if (tasks.empty()) return stats;

    std::atomic<std::uint64_t> failed(0);
    std::mutex error_mutex;
    auto execute = [&](std::uint32_t index, int worker) {
        WorkerStats& w = stats.workers[static_cast<std::size_t>(worker)];
        const std::uint64_t start = now_ns();
        std::string error;
        try {
            w.bytes += work(tasks[index], worker);
        } catch (const std::exception& e) {
            error = e.what();
        } catch (...) {
            error = "unknown exception";
        }
        if (!error.empty()) {
            failed.fetch_add(1);
            std::lock_guard<std::mutex> lock(error_mutex);
            if (stats.errors.size() < 20) stats.errors.push_back(error);
        }
        w.busy_ns += now_ns() - start;
        w.cost += tasks[index].cost;
        ++w.tasks;
    };

    const std::uint64_t start = now_ns();
    auto finished = [&](int worker) { stats.workers[static_cast<std::size_t>(worker)].finish_ns = now_ns() - start; };
    switch (config.policy) {
    case Policy::Sequential:
        for (std::size_t i = 0; i < tasks.size(); ++i) execute(static_cast<std::uint32_t>(i), 0);
        finished(0);
        break;

    case Policy::Static: {
        const std::uint64_t plan_start = now_ns();
        const auto parts = lpt_partition(tasks, threads);
        stats.plan_ns = now_ns() - plan_start;
        parallel_region(threads, [&](int worker) {
            for (std::uint32_t index : parts[static_cast<std::size_t>(worker)]) execute(index, worker);
            finished(worker);
        });
        break;
    }

    case Policy::Dynamic: {
        std::atomic<std::size_t> next(0);
        const std::size_t total = tasks.size();
        parallel_region(threads, [&](int worker) {
            WorkerStats& w = stats.workers[static_cast<std::size_t>(worker)];
            while (true) {
                const std::uint64_t claim_start = now_ns();
                const std::size_t begin = next.fetch_add(config.chunk);
                w.claim_ns += now_ns() - claim_start;
                if (begin >= total) break;
                const std::size_t end = std::min(total, begin + config.chunk);
                for (std::size_t i = begin; i < end; ++i) execute(static_cast<std::uint32_t>(i), worker);
            }
            finished(worker);
        });
        break;
    }

    case Policy::Steal: {
        const std::uint64_t plan_start = now_ns();
        const auto parts = lpt_partition(tasks, threads);
        std::unique_ptr<StealQueue[]> queues(new StealQueue[static_cast<std::size_t>(threads)]);
        for (int w = 0; w < threads; ++w) {
            StealQueue& q = queues[static_cast<std::size_t>(w)];
            q.items = parts[static_cast<std::size_t>(w)];
            std::uint64_t cost = 0;
            for (std::uint32_t index : q.items) cost += tasks[index].cost;
            q.remaining.store(cost);
        }
        stats.plan_ns = now_ns() - plan_start;

        // Move roughly half of the richest victim's unclaimed cost (taken from
        // its cheap tail) into the thief's empty queue.
        auto steal = [&](int thief) -> bool {
            WorkerStats& w = stats.workers[static_cast<std::size_t>(thief)];
            while (true) {
                ++w.steal_attempts;
                int victim = -1;
                std::uint64_t best = 0;
                for (int v = 0; v < threads; ++v) {
                    if (v == thief) continue;
                    const std::uint64_t r = queues[static_cast<std::size_t>(v)].remaining.load();
                    if (r > best) { best = r; victim = v; }
                }
                if (victim < 0) return false;
                std::vector<std::uint32_t> stolen;
                std::uint64_t got = 0;
                {
                    StealQueue& q = queues[static_cast<std::size_t>(victim)];
                    std::lock_guard<std::mutex> lock(q.mutex);
                    const std::uint64_t target = q.remaining.load() / 2;
                    while (q.items.size() > q.head && (stolen.empty() || got < target)) {
                        const std::uint32_t index = q.items.back();
                        q.items.pop_back();
                        stolen.push_back(index);
                        got += tasks[index].cost;
                    }
                    q.remaining.fetch_sub(got);
                }
                if (stolen.empty()) continue; // victim drained meanwhile; rescan
                std::reverse(stolen.begin(), stolen.end());
                StealQueue& own = queues[static_cast<std::size_t>(thief)];
                std::lock_guard<std::mutex> lock(own.mutex);
                own.items.swap(stolen);
                own.head = 0;
                own.remaining.store(got);
                ++w.steals;
                w.tasks_stolen += own.items.size();
                w.cost_stolen += got;
                return true;
            }
        };

        parallel_region(threads, [&](int worker) {
            WorkerStats& w = stats.workers[static_cast<std::size_t>(worker)];
            StealQueue& own = queues[static_cast<std::size_t>(worker)];
            while (true) {
                const std::uint64_t claim_start = now_ns();
                bool have = false;
                std::uint32_t index = 0;
                {
                    std::lock_guard<std::mutex> lock(own.mutex);
                    if (own.head < own.items.size()) {
                        index = own.items[own.head++];
                        own.remaining.fetch_sub(tasks[index].cost);
                        have = true;
                    }
                }
                if (!have) {
                    const bool stole = steal(worker);
                    w.claim_ns += now_ns() - claim_start;
                    if (!stole) break;
                    continue;
                }
                w.claim_ns += now_ns() - claim_start;
                execute(index, worker);
            }
            finished(worker);
        });
        break;
    }
    }
    stats.wall_ns = now_ns() - start;
    stats.failed_tasks = failed.load();
    return stats;
}

} // namespace ipbrs
