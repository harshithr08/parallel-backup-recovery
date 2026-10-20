#include "testing.hpp"

#include "scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using namespace ipbrs;

namespace {

std::vector<Task> random_tasks(std::size_t count, std::uint64_t seed) {
    Rng rng(seed);
    std::vector<Task> tasks(count);
    for (std::size_t i = 0; i < count; ++i) {
        tasks[i].file = static_cast<std::uint32_t>(i);
        // Heavy-tailed costs: mostly small, a few very large.
        tasks[i].cost = 1 + (rng.below(20) == 0 ? rng.below(1000000) : rng.below(1000));
    }
    return tasks;
}

const Policy kPolicies[] = {Policy::Sequential, Policy::Static, Policy::Dynamic, Policy::Steal};
const int kThreads[] = {1, 2, 3, 8, 16};

} // namespace

TEST(scheduler_runs_every_task_exactly_once) {
    const std::vector<Task> tasks = random_tasks(3000, 11);
    for (Policy policy : kPolicies) {
        for (int threads : kThreads) {
            for (std::size_t chunk : {std::size_t(1), std::size_t(7), std::size_t(64)}) {
                if (policy != Policy::Dynamic && chunk != 1) continue;
                std::unique_ptr<std::atomic<int>[]> runs(new std::atomic<int>[tasks.size()]);
                for (std::size_t i = 0; i < tasks.size(); ++i) runs[i].store(0);
                SchedulerConfig config;
                config.policy = policy;
                config.threads = threads;
                config.chunk = chunk;
                const SchedulerStats stats = run_tasks(tasks, config, [&](const Task& t, int worker) -> std::uint64_t {
                    CHECK(worker >= 0 && worker < threads);
                    runs[t.file].fetch_add(1);
                    return t.cost;
                });
                for (std::size_t i = 0; i < tasks.size(); ++i) CHECK_EQ(runs[i].load(), 1);
                CHECK_EQ(stats.threads, policy == Policy::Sequential ? 1 : threads);
                CHECK_EQ(stats.workers.size(), static_cast<std::size_t>(stats.threads));
                std::uint64_t executed = 0, bytes = 0, cost = 0, expected_cost = 0;
                for (const WorkerStats& w : stats.workers) {
                    executed += w.tasks;
                    bytes += w.bytes;
                    cost += w.cost;
                }
                for (const Task& t : tasks) expected_cost += t.cost;
                CHECK_EQ(executed, static_cast<std::uint64_t>(tasks.size()));
                CHECK_EQ(bytes, expected_cost);
                CHECK_EQ(cost, expected_cost);
                CHECK_EQ(stats.failed_tasks, 0u);
                if (policy != Policy::Steal) CHECK_EQ(stats.steals(), 0u);
            }
        }
    }
}

TEST(scheduler_counts_failures_and_keeps_running) {
    const std::vector<Task> tasks = random_tasks(500, 5);
    for (Policy policy : kPolicies) {
        std::atomic<int> executed(0);
        SchedulerConfig config;
        config.policy = policy;
        config.threads = 4;
        const SchedulerStats stats = run_tasks(tasks, config, [&](const Task& t, int) -> std::uint64_t {
            executed.fetch_add(1);
            if (t.file % 10 == 0) throw std::runtime_error("task " + std::to_string(t.file) + " failed");
            return 1;
        });
        CHECK_EQ(executed.load(), 500);
        CHECK_EQ(stats.failed_tasks, 50u);
        CHECK_EQ(stats.errors.size(), 20u); // capped
    }
}

// Equal estimated costs, but every task that starts on worker 0 is slow:
// the other workers must steal from worker 0's queue.
TEST(steal_rebalances_when_estimates_are_wrong) {
    std::vector<Task> tasks(400);
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        tasks[i].file = static_cast<std::uint32_t>(i);
        tasks[i].cost = 100;
    }
    for (Policy policy : {Policy::Static, Policy::Steal}) {
        SchedulerConfig config;
        config.policy = policy;
        config.threads = 4;
        const SchedulerStats stats = run_tasks(tasks, config, [&](const Task&, int worker) -> std::uint64_t {
            if (worker == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return 1;
        });
        if (policy == Policy::Static) {
            CHECK_EQ(stats.workers[0].tasks, 100u); // fixed partition, no rebalancing
            CHECK_EQ(stats.steals(), 0u);
        } else {
            CHECK(stats.steals() > 0);
            CHECK(stats.workers[0].tasks < 50u); // most of its queue was taken by others
        }
    }
}

TEST(lpt_partition_is_complete_and_balanced) {
    const std::vector<Task> tasks = random_tasks(1000, 21);
    std::uint64_t max_cost = 0;
    for (const Task& t : tasks) max_cost = std::max(max_cost, t.cost);
    for (int workers : {1, 2, 5, 16}) {
        const auto parts = lpt_partition(tasks, workers);
        CHECK_EQ(parts.size(), static_cast<std::size_t>(workers));
        std::vector<int> seen(tasks.size(), 0);
        std::uint64_t lo = UINT64_MAX, hi = 0;
        for (const auto& part : parts) {
            std::uint64_t load = 0;
            for (std::size_t j = 0; j < part.size(); ++j) {
                ++seen[part[j]];
                load += tasks[part[j]].cost;
                if (j > 0) CHECK(tasks[part[j - 1]].cost >= tasks[part[j]].cost); // largest first
            }
            lo = std::min(lo, load);
            hi = std::max(hi, load);
        }
        for (int s : seen) CHECK_EQ(s, 1);
        CHECK(hi - lo <= max_cost); // greedy list-scheduling bound
    }
}

TEST(scheduler_rejects_invalid_config_and_handles_empty_input) {
    const std::vector<Task> tasks = random_tasks(10, 1);
    auto noop = [](const Task&, int) -> std::uint64_t { return 0; };
    SchedulerConfig config;
    config.threads = 0;
    CHECK_THROWS(run_tasks(tasks, config, noop));
    config.threads = 2;
    config.chunk = 0;
    CHECK_THROWS(run_tasks(tasks, config, noop));
    config.chunk = 1;
    const SchedulerStats empty = run_tasks({}, config, noop);
    CHECK_EQ(empty.failed_tasks, 0u);
    CHECK_EQ(empty.workers.size(), 2u);
    for (Policy p : kPolicies) CHECK(parse_policy(policy_name(p)) == p);
    CHECK_THROWS(parse_policy("ipbrs-turbo"));
}
