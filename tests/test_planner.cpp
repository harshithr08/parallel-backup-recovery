#include "testing.hpp"

#include "merkle.hpp"
#include "workload.hpp"

#include <algorithm>

using namespace ipbrs;

namespace {

const std::uint32_t kBlock = 1u << 20;

Range whole(std::uint32_t file, std::uint64_t bytes) {
    Range r;
    r.file = file;
    r.block_count = block_count(bytes, kBlock);
    r.bytes = bytes;
    return r;
}

PlanConfig config(int threads, Split split = Split::Adaptive, std::uint32_t align = 1) {
    PlanConfig c;
    c.split = split;
    c.threads = threads;
    c.block_size = kBlock;
    c.split_factor = 8;
    c.align_blocks = align;
    return c;
}

// 1,000 small files and two large ones: the "few huge files" shape.
std::vector<Range> mixed_ranges() {
    std::vector<Range> ranges;
    for (std::uint32_t i = 0; i < 1000; ++i) ranges.push_back(whole(i, 8192 + i));
    ranges.push_back(whole(1000, 400ull << 20));
    ranges.push_back(whole(1001, (100ull << 20) + 12345)); // partial last block
    return ranges;
}

// Pieces of each range must tile it exactly: no gaps, no overlap, same bytes.
void check_tiling(const std::vector<Range>& ranges, const std::vector<Task>& tasks) {
    for (const Range& r : ranges) {
        std::vector<const Task*> pieces;
        for (const Task& t : tasks)
            if (t.file == r.file && t.first_block >= r.first_block && t.first_block < r.first_block + r.block_count)
                pieces.push_back(&t);
        std::sort(pieces.begin(), pieces.end(),
                  [](const Task* a, const Task* b) { return a->first_block < b->first_block; });
        std::uint32_t next = r.first_block;
        for (const Task* t : pieces) {
            CHECK_EQ(t->first_block, next);
            CHECK(t->block_count > 0);
            next += t->block_count;
        }
        CHECK_EQ(next, r.first_block + r.block_count);
    }
}

} // namespace

TEST(planner_without_splitting_keeps_one_task_per_range) {
    const std::vector<Range> ranges = mixed_ranges();
    for (const PlanConfig& c : {config(1), config(8, Split::None)}) {
        PlanStats stats;
        const std::vector<Task> tasks = plan_tasks(ranges, c, &stats);
        CHECK_EQ(tasks.size(), ranges.size());
        CHECK_EQ(stats.split_threshold, 0u);
        CHECK(stats.split_files.empty());
        for (std::size_t i = 0; i < tasks.size(); ++i) {
            CHECK_EQ(tasks[i].block_count, ranges[i].block_count);
            CHECK(tasks[i].whole_file);
        }
    }
}

TEST(planner_splits_only_large_ranges_and_tiles_them) {
    const std::vector<Range> ranges = mixed_ranges();
    PlanStats stats;
    const std::vector<Task> tasks = plan_tasks(ranges, config(8), &stats);
    CHECK(tasks.size() > ranges.size());
    CHECK(stats.split_files == std::vector<std::uint32_t>({1000, 1001}));
    check_tiling(ranges, tasks);
    std::uint64_t total = 0;
    for (const Task& t : tasks) {
        if (t.file < 1000) CHECK(t.whole_file); // small files are never split
        total += t.cost;
    }
    CHECK_EQ(total, stats.total_cost);
    CHECK(stats.max_task_cost <= stats.split_threshold + 64u * 1024); // no task above the bound
    // Bytes are preserved: each split file's piece costs add back to its size.
    std::uint64_t bytes = 0;
    for (const Task& t : tasks)
        if (t.file == 1001) bytes += t.cost - 64u * 1024;
    CHECK_EQ(bytes, ranges[1001].bytes);
}

TEST(planner_threshold_shrinks_with_more_threads) {
    const std::vector<Range> ranges = mixed_ranges();
    PlanStats four, sixteen;
    plan_tasks(ranges, config(4), &four);
    plan_tasks(ranges, config(16), &sixteen);
    CHECK(sixteen.split_threshold < four.split_threshold);
    CHECK(sixteen.tasks > four.tasks);
}

TEST(planner_handles_zero_byte_and_single_block_files) {
    std::vector<Range> ranges = {whole(0, 0), whole(1, 1), whole(2, kBlock), whole(3, 900ull << 20)};
    PlanConfig c = config(16);
    c.per_task_cost = 0; // costs must still be positive
    const std::vector<Task> tasks = plan_tasks(ranges, c, nullptr);
    for (const Task& t : tasks) CHECK(t.cost >= 1);
    for (std::uint32_t f = 0; f < 3; ++f) {
        const auto n = std::count_if(tasks.begin(), tasks.end(), [&](const Task& t) { return t.file == f; });
        CHECK_EQ(n, 1); // one block can never be split
    }
    check_tiling(ranges, tasks);
}

TEST(planner_aligns_pieces_to_segments) {
    const std::uint32_t align = 16;
    // A whole file and a mid-file damaged run that starts off a segment boundary.
    Range run;
    run.file = 1;
    run.first_block = 37;
    run.block_count = 300;
    run.bytes = 300ull * kBlock;
    run.whole_file = false;
    const std::vector<Range> ranges = {whole(0, 700ull << 20), run};
    const std::vector<Task> tasks = plan_tasks(ranges, config(16, Split::Adaptive, align), nullptr);
    check_tiling(ranges, tasks);
    CHECK(tasks.size() > ranges.size());
    for (const Task& t : tasks) {
        const Range& r = ranges[t.file];
        const std::uint32_t begin = t.first_block, end = t.first_block + t.block_count;
        CHECK(begin == r.first_block || begin % align == 0);
        CHECK(end == r.first_block + r.block_count || end % align == 0);
    }
}
