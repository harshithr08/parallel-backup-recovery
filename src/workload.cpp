#include "workload.hpp"

#include <algorithm>

namespace ipbrs {

std::vector<FileEntry> discover_files(const fs::path& root, int threads, DiscoveryStats* stats) {
    const std::uint64_t start = now_ns();
    if (!fs::is_directory(root)) throw std::runtime_error("not a directory: " + root.u8string());
    threads = std::max(1, threads);

    std::vector<FileEntry> files;
    std::vector<std::string> frontier(1);
    std::uint64_t directories = 0;
    while (!frontier.empty()) {
        directories += frontier.size();
        std::vector<std::vector<FileEntry>> found(static_cast<std::size_t>(threads));
        std::vector<std::vector<std::string>> subdirs(static_cast<std::size_t>(threads));
        parallel_for(frontier.size(), threads, [&](std::size_t i, int worker) {
            const std::string& dir = frontier[i];
            const fs::path native = dir.empty() ? root : native_path(root, dir);
            for (const fs::directory_entry& entry : fs::directory_iterator(native)) {
                const std::string name = entry.path().filename().u8string();
                const std::string relative = dir.empty() ? name : dir + "/" + name;
                if (entry.is_symlink()) continue;
                if (entry.is_directory()) {
                    subdirs[static_cast<std::size_t>(worker)].push_back(relative);
                } else if (entry.is_regular_file()) {
                    FileEntry file;
                    file.path = relative;
                    file.size = entry.file_size();
                    file.mtime = to_ticks(entry.last_write_time());
                    found[static_cast<std::size_t>(worker)].push_back(std::move(file));
                }
            }
        });
        frontier.clear();
        for (auto& list : subdirs) frontier.insert(frontier.end(), list.begin(), list.end());
        for (auto& list : found)
            files.insert(files.end(), std::make_move_iterator(list.begin()), std::make_move_iterator(list.end()));
    }
    std::sort(files.begin(), files.end(),
              [](const FileEntry& a, const FileEntry& b) { return a.path < b.path; });
    if (stats) {
        stats->ns = now_ns() - start;
        stats->directories = directories;
        stats->threads = threads;
    }
    return files;
}

Split parse_split(const std::string& name) {
    if (name == "none" || name == "file") return Split::None;
    if (name == "adaptive" || name == "block") return Split::Adaptive;
    throw std::invalid_argument("unknown split mode: " + name);
}

const char* split_name(Split split) { return split == Split::None ? "none" : "adaptive"; }

void PlanStats::write_json(Json& json) const {
    json.begin_object()
        .field("ranges", ranges)
        .field("tasks", tasks)
        .field("total_cost", total_cost)
        .field("max_task_cost", max_task_cost)
        .field("split_threshold", split_threshold)
        .field("split_files", static_cast<std::uint64_t>(split_files.size()))
        .end_object();
}

std::vector<Task> plan_tasks(const std::vector<Range>& ranges, const PlanConfig& config, PlanStats* stats) {
    PlanStats local;
    PlanStats& out = stats ? *stats : local;
    out = PlanStats();
    out.ranges = ranges.size();

    std::uint64_t total = 0;
    for (const Range& r : ranges) total += config.per_task_cost + r.bytes;

    std::uint64_t threshold = 0;
    std::uint32_t blocks_per_piece = 0;
    if (config.split == Split::Adaptive && config.threads > 1) {
        const std::uint64_t pieces = static_cast<std::uint64_t>(config.threads) * std::max<std::uint64_t>(1, config.split_factor);
        threshold = std::max<std::uint64_t>(config.block_size, total / pieces);
        blocks_per_piece = static_cast<std::uint32_t>(std::max<std::uint64_t>(1, threshold / config.block_size));
    }
    out.split_threshold = threshold;

    std::vector<Task> tasks;
    tasks.reserve(ranges.size());
    for (const Range& r : ranges) {
        if (threshold == 0 || r.bytes <= threshold || r.block_count <= 1) {
            Task t;
            t.file = r.file;
            t.first_block = r.first_block;
            t.block_count = r.block_count;
            t.whole_file = r.whole_file;
            t.cost = config.per_task_cost + r.bytes;
            tasks.push_back(t);
            continue;
        }
        out.split_files.push_back(r.file);
        std::uint64_t remaining = r.bytes;
        for (std::uint32_t done = 0; done < r.block_count; done += blocks_per_piece) {
            Task t;
            t.file = r.file;
            t.first_block = r.first_block + done;
            t.block_count = std::min(blocks_per_piece, r.block_count - done);
            t.whole_file = false; // pieces never create the file; the caller pre-sizes it
            const std::uint64_t piece = std::min<std::uint64_t>(remaining,
                static_cast<std::uint64_t>(t.block_count) * config.block_size);
            remaining -= piece;
            t.cost = config.per_task_cost + piece;
            tasks.push_back(t);
        }
    }
    out.tasks = tasks.size();
    out.total_cost = 0;
    for (const Task& t : tasks) {
        out.total_cost += t.cost;
        out.max_task_cost = std::max(out.max_task_cost, t.cost);
    }
    std::sort(out.split_files.begin(), out.split_files.end());
    out.split_files.erase(std::unique(out.split_files.begin(), out.split_files.end()), out.split_files.end());
    return tasks;
}

} // namespace ipbrs
