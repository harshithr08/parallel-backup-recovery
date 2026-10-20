#include "engine.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <set>

namespace ipbrs {
namespace {

using Flags = std::unique_ptr<std::atomic<std::uint8_t>[]>;

Flags make_flags(std::size_t count) {
    Flags flags(new std::atomic<std::uint8_t>[count]);
    for (std::size_t i = 0; i < count; ++i) flags[i].store(0);
    return flags;
}

PlanConfig plan_config(const RunConfig& run, std::uint32_t block_size) {
    PlanConfig config;
    config.split = run.split;
    config.threads = run.threads();
    config.block_size = block_size;
    config.split_factor = run.split_factor;
    config.per_task_cost = run.per_task_cost;
    return config;
}

std::vector<std::vector<char>> worker_buffers(int threads, std::uint32_t block_size) {
    return std::vector<std::vector<char>>(static_cast<std::size_t>(threads), std::vector<char>(block_size));
}

std::string parent_of(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// Directory creation is metadata-bound and cheap relative to file work, so it
// runs serially before scheduling.
void create_parents(const fs::path& root, const Catalog& catalog, const std::vector<std::uint32_t>& files) {
    std::set<std::string> parents;
    for (std::uint32_t f : files) parents.insert(parent_of(catalog.files[f].path));
    for (const std::string& dir : parents)
        if (!dir.empty()) fs::create_directories(native_path(root, dir));
}

void presize(const fs::path& root, const Catalog& catalog, const std::vector<std::uint32_t>& files, int threads) {
    parallel_for(files.size(), threads, [&](std::size_t i, int) {
        const CatalogFile& c = catalog.files[files[i]];
        File(native_path(root, c.path), File::Mode::WriteCreate).resize(c.size);
    });
}

// Reads blocks of one cataloged file either from a plain tree (root/path) or
// from a backup's files/segments layout, keeping the current file open.
class BlockSource {
public:
    BlockSource(const fs::path& root, bool is_backup, const Catalog& catalog, const CatalogFile& file)
        : root_(root), backup_(is_backup), catalog_(catalog), file_(file) {}

    std::size_t read(std::uint32_t block, char* buffer) {
        std::uint64_t offset = static_cast<std::uint64_t>(block) * catalog_.block_size;
        fs::path path = backup_ ? stored_path(root_, catalog_, file_, block, offset)
                                : native_path(root_, file_.path);
        if (!open_ || path != current_) {
            open_.reset(new File(path, File::Mode::Read));
            current_ = std::move(path);
        }
        const std::uint64_t length = block_length(file_.size, catalog_.block_size, block);
        return open_->read_at(buffer, static_cast<std::size_t>(length), offset);
    }

private:
    const fs::path& root_;
    bool backup_;
    const Catalog& catalog_;
    const CatalogFile& file_;
    std::unique_ptr<File> open_;
    fs::path current_;
};

enum class State : std::uint8_t { Ok, Missing, SizeChanged, Damaged, MetadataOnly, Unreadable };

struct Scan {
    std::vector<State> state;
    std::vector<std::vector<std::uint8_t>> bad_blocks; // per file, only for hashed files
    std::uint64_t stat_ns = 0;
    std::uint64_t hash_ns = 0;
    std::uint64_t hashed_files = 0;
    std::uint64_t damaged_blocks = 0;
    PlanStats plan;
    SchedulerStats hash;
};

// Classifies cataloged files in two parallel phases: metadata (exists, size,
// mtime) and block hashing through the shared scheduler. With is_backup, root
// is a backup directory; segmented files are then checked by hashing alone.
Scan scan_tree(const fs::path& root, bool is_backup, const Catalog& catalog,
               const std::vector<std::uint32_t>& subset, bool trust_mtime, const RunConfig& run) {
    const std::size_t count = catalog.files.size();
    const int threads = run.threads();
    Scan scan;
    scan.state.assign(count, State::Ok);
    scan.bad_blocks.resize(count);
    std::vector<std::uint8_t> needs_hash(count, 0), mtime_differs(count, 0);

    std::uint64_t start = now_ns();
    parallel_for(subset.size(), threads, [&](std::size_t i, int) {
        const std::uint32_t f = subset[i];
        const CatalogFile& c = catalog.files[f];
        if (is_backup && catalog.segmented(c)) {
            needs_hash[f] = 1;
            return;
        }
        PathInfo info;
        try {
            info = stat_path(is_backup ? payload_root(root) / fs::u8path(c.path) : native_path(root, c.path));
        } catch (const std::exception&) {
            scan.state[f] = State::Unreadable;
            return;
        }
        if (info.kind == PathKind::Missing) scan.state[f] = State::Missing;
        else if (info.kind == PathKind::Other) scan.state[f] = State::Unreadable;
        else if (info.size != c.size) scan.state[f] = State::SizeChanged;
        else {
            mtime_differs[f] = info.mtime != c.mtime;
            if (!(trust_mtime && !mtime_differs[f])) needs_hash[f] = 1;
        }
    });
    scan.stat_ns = now_ns() - start;

    start = now_ns();
    std::vector<Range> ranges;
    for (std::uint32_t f : subset) {
        if (!needs_hash[f]) continue;
        const CatalogFile& c = catalog.files[f];
        Range r;
        r.file = f;
        r.block_count = static_cast<std::uint32_t>(c.leaves.size());
        r.bytes = c.size;
        r.whole_file = false;
        ranges.push_back(r);
        scan.bad_blocks[f].assign(c.leaves.size(), 0);
    }
    scan.hashed_files = ranges.size();
    const std::vector<Task> tasks = plan_tasks(ranges, plan_config(run, catalog.block_size), &scan.plan);
    Flags unreadable = make_flags(count);
    auto buffers = worker_buffers(threads, catalog.block_size);
    scan.hash = run_tasks(tasks, run.sched, [&](const Task& t, int worker) -> std::uint64_t {
        const CatalogFile& c = catalog.files[t.file];
        std::vector<char>& buffer = buffers[static_cast<std::size_t>(worker)];
        try {
            BlockSource in(root, is_backup, catalog, c);
            std::uint64_t bytes = 0;
            for (std::uint32_t b = t.first_block; b < t.first_block + t.block_count; ++b) {
                const std::uint64_t length = block_length(c.size, catalog.block_size, b);
                const std::size_t got = in.read(b, buffer.data());
                if (got != length || leaf_hash(buffer.data(), got) != c.leaves[b]) scan.bad_blocks[t.file][b] = 1;
                bytes += got;
            }
            return bytes;
        } catch (...) {
            unreadable[t.file].store(1);
            throw;
        }
    });
    for (const Range& r : ranges) {
        const std::uint32_t f = r.file;
        const std::uint64_t bad = static_cast<std::uint64_t>(
            std::count(scan.bad_blocks[f].begin(), scan.bad_blocks[f].end(), std::uint8_t(1)));
        if (unreadable[f].load()) scan.state[f] = State::Unreadable;
        else if (bad) { scan.state[f] = State::Damaged; scan.damaged_blocks += bad; }
        else if (mtime_differs[f]) scan.state[f] = State::MetadataOnly;
    }
    scan.hash_ns = now_ns() - start;
    return scan;
}

void write_scan_json(Json& json, const Scan& scan) {
    json.begin_object()
        .field("stat_ns", scan.stat_ns)
        .field("hash_ns", scan.hash_ns)
        .field("hashed_files", scan.hashed_files)
        .field("damaged_blocks", scan.damaged_blocks);
    json.key("plan");
    scan.plan.write_json(json);
    json.key("scheduler");
    scan.hash.write_json(json);
    json.end_object();
}

std::vector<std::uint32_t> all_files(std::size_t count) {
    std::vector<std::uint32_t> files(count);
    for (std::size_t i = 0; i < count; ++i) files[i] = static_cast<std::uint32_t>(i);
    return files;
}

Range whole_range(const Catalog& catalog, std::uint32_t f) {
    Range r;
    r.file = f;
    r.block_count = static_cast<std::uint32_t>(catalog.files[f].leaves.size());
    r.bytes = catalog.files[f].size;
    r.whole_file = true;
    return r;
}

bool is_inside(const fs::path& candidate, const fs::path& root) {
    const std::string c = fs::weakly_canonical(candidate).generic_u8string() + "/";
    const std::string r = fs::weakly_canonical(root).generic_u8string() + "/";
    return c.compare(0, r.size(), r) == 0;
}

} // namespace

void RunConfig::write_json(Json& json) const {
    json.begin_object()
        .field("policy", policy_name(sched.policy))
        .field("threads", threads())
        .field("chunk", static_cast<std::uint64_t>(sched.chunk))
        .field("split", split_name(split))
        .field("split_factor", split_factor)
        .field("per_task_cost", per_task_cost)
        .end_object();
}

BackupResult run_backup(const BackupOptions& options) {
    if (options.block_size < 4096 || options.block_size > (64u << 20))
        throw std::invalid_argument("block size must be between 4 KiB and 64 MiB");
    const RunConfig& run = options.run;
    const int threads = run.threads();
    const std::uint64_t start = now_ns();

    if (is_inside(options.backup, options.source) || is_inside(options.source, options.backup))
        throw std::invalid_argument("source and backup directories must not overlap");
    if (fs::exists(options.backup) && !fs::is_empty(options.backup))
        throw std::invalid_argument("backup directory must be new or empty: " + options.backup.u8string());

    DiscoveryStats discovery;
    const std::vector<FileEntry> entries = discover_files(options.source, threads, &discovery);

    std::uint64_t phase = now_ns();
    Catalog catalog;
    catalog.block_size = options.block_size;
    catalog.files.resize(entries.size());
    std::vector<Range> ranges(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        CatalogFile& c = catalog.files[i];
        c.path = entries[i].path;
        c.size = entries[i].size;
        c.mtime = entries[i].mtime;
        c.leaves.resize(block_count(c.size, catalog.block_size));
        ranges[i] = whole_range(catalog, static_cast<std::uint32_t>(i));
    }
    PlanConfig plan_cfg = plan_config(run, catalog.block_size);
    plan_cfg.align_blocks = catalog.segment_blocks; // a piece owns whole segment objects
    PlanStats plan;
    const std::vector<Task> tasks = plan_tasks(ranges, plan_cfg, &plan);
    const fs::path payload = payload_root(options.backup);
    const fs::path segments = segment_root(options.backup);
    fs::create_directories(payload);
    std::vector<std::uint32_t> plain, segmented;
    for (std::uint32_t f = 0; f < catalog.files.size(); ++f)
        (catalog.segmented(catalog.files[f]) ? segmented : plain).push_back(f);
    create_parents(payload, catalog, plain);
    for (std::uint32_t f : segmented) fs::create_directories(native_path(segments, catalog.files[f].path));
    const std::uint64_t prepare_ns = now_ns() - phase;

    // One pass per block: read source, hash, write destination. Every
    // destination object (plain file or segment) is created by exactly one task.
    Flags failed = make_flags(catalog.files.size());
    auto buffers = worker_buffers(threads, catalog.block_size);
    const SchedulerStats copy = run_tasks(tasks, run.sched, [&](const Task& t, int worker) -> std::uint64_t {
        CatalogFile& c = catalog.files[t.file];
        std::vector<char>& buffer = buffers[static_cast<std::size_t>(worker)];
        try {
            File in(native_path(options.source, c.path), File::Mode::Read);
            std::unique_ptr<File> out;
            fs::path out_path;
            std::uint64_t bytes = 0;
            for (std::uint32_t b = t.first_block; b < t.first_block + t.block_count; ++b) {
                const std::uint64_t length = block_length(c.size, catalog.block_size, b);
                const std::uint64_t offset = static_cast<std::uint64_t>(b) * catalog.block_size;
                const std::size_t got = in.read_at(buffer.data(), static_cast<std::size_t>(length), offset);
                if (got != length) throw std::runtime_error("source shrank during backup: " + c.path);
                c.leaves[b] = leaf_hash(buffer.data(), got);
                std::uint64_t stored_offset = 0;
                fs::path target = stored_path(options.backup, catalog, c, b, stored_offset);
                if (!out || target != out_path) {
                    if (out && options.sync) out->sync();
                    out.reset(new File(target, File::Mode::WriteCreate));
                    out_path = std::move(target);
                }
                out->write_at(buffer.data(), got, stored_offset);
                bytes += got;
            }
            if (t.whole_file) {
                char probe;
                if (in.read_at(&probe, 1, c.size) != 0) throw std::runtime_error("source grew during backup: " + c.path);
                if (!catalog.segmented(c)) out->set_mtime(c.mtime);
            }
            if (options.sync) out->sync();
            return bytes;
        } catch (...) {
            failed[t.file].store(1);
            throw;
        }
    });

    // Merkle roots and source-stability check.
    phase = now_ns();
    std::atomic<std::uint64_t> changed(0);
    parallel_for(catalog.files.size(), threads, [&](std::size_t f, int) {
        if (failed[f].load()) return;
        CatalogFile& c = catalog.files[f];
        c.root = merkle_root(c.leaves);
        const PathInfo now = stat_path(native_path(options.source, c.path));
        if (now.kind != PathKind::File || now.size != c.size || now.mtime != c.mtime) {
            failed[f].store(1);
            changed.fetch_add(1);
        }
    });
    BackupResult result;
    result.files = catalog.files.size();
    result.bytes = catalog.total_bytes();
    for (std::size_t f = 0; f < catalog.files.size(); ++f) result.failed_files += failed[f].load();
    if (result.failed_files == 0) {
        save_catalog(catalog_path(options.backup), catalog);
        result.committed = true;
    }
    const std::uint64_t finalize_ns = now_ns() - phase;
    const std::uint64_t total_ns = now_ns() - start;

    Json json;
    json.begin_object()
        .field("operation", "backup")
        .field("committed", result.committed)
        .field("files", result.files)
        .field("bytes", result.bytes)
        .field("failed_files", result.failed_files)
        .field("changed_during_backup", changed.load())
        .field("sync", options.sync)
        .field("block_size", catalog.block_size)
        .field("total_ns", total_ns)
        .field("discovery_ns", discovery.ns)
        .field("prepare_ns", prepare_ns)
        .field("copy_ns", copy.wall_ns)
        .field("finalize_ns", finalize_ns)
        .field("directories", discovery.directories);
    json.key("run");
    run.write_json(json);
    json.key("plan");
    plan.write_json(json);
    json.key("scheduler");
    copy.write_json(json);
    json.end_object();
    result.json = json.str();
    return result;
}

VerifyResult run_verify(const VerifyOptions& options) {
    const std::uint64_t start = now_ns();
    const Catalog catalog = load_catalog(catalog_path(options.backup));
    const std::uint64_t load_ns = now_ns() - start;
    const Scan scan = scan_tree(options.backup, true, catalog, all_files(catalog.files.size()),
                                false, options.run);
    VerifyResult result;
    result.files = catalog.files.size();
    std::uint64_t counts[6] = {0, 0, 0, 0, 0, 0};
    for (State s : scan.state) ++counts[static_cast<int>(s)];
    result.bad = result.files - counts[static_cast<int>(State::Ok)];

    Json json;
    json.begin_object()
        .field("operation", "verify")
        .field("files", result.files)
        .field("bytes", catalog.total_bytes())
        .field("ok", counts[static_cast<int>(State::Ok)])
        .field("bad", result.bad)
        .field("missing", counts[static_cast<int>(State::Missing)])
        .field("size_changed", counts[static_cast<int>(State::SizeChanged)])
        .field("content_damaged", counts[static_cast<int>(State::Damaged)])
        .field("metadata_only", counts[static_cast<int>(State::MetadataOnly)])
        .field("unreadable", counts[static_cast<int>(State::Unreadable)])
        .field("total_ns", now_ns() - start)
        .field("load_ns", load_ns);
    json.key("run");
    options.run.write_json(json);
    json.key("scan");
    write_scan_json(json, scan);
    json.end_object();
    result.json = json.str();
    return result;
}

RecoveryMode parse_recovery_mode(const std::string& name) {
    if (name == "block") return RecoveryMode::Block;
    if (name == "file") return RecoveryMode::File;
    if (name == "full") return RecoveryMode::Full;
    throw std::invalid_argument("unknown recovery mode: " + name);
}

const char* recovery_mode_name(RecoveryMode mode) {
    switch (mode) {
        case RecoveryMode::Block: return "block";
        case RecoveryMode::File: return "file";
        case RecoveryMode::Full: return "full";
    }
    return "unknown";
}

RecoverResult run_recover(const RecoverOptions& options) {
    const RunConfig& run = options.run;
    const int threads = run.threads();
    const std::uint64_t start = now_ns();
    const Catalog catalog = load_catalog(catalog_path(options.backup));
    const std::size_t count = catalog.files.size();
    const std::uint64_t load_ns = now_ns() - start;

    RecoverResult result;
    result.catalog_files = count;

    // 1. Classify (skipped by the full-recovery baseline).
    Scan scan;
    if (options.mode == RecoveryMode::Full) {
        scan.state.assign(count, State::SizeChanged);
    } else {
        scan = scan_tree(options.source, false, catalog, all_files(count), options.trust_mtime, run);
    }

    // 2. Decide what to restore.
    std::uint64_t phase = now_ns();
    std::vector<Range> ranges;
    std::vector<std::uint8_t> creates(count, 0);
    std::vector<std::uint32_t> touched, created;
    for (std::uint32_t f = 0; f < count; ++f) {
        const State state = scan.state[f];
        if (options.mode != RecoveryMode::Full) {
            if (state == State::Missing) ++result.missing;
            else if (state == State::SizeChanged) ++result.size_changed;
            else if (state == State::Damaged) ++result.content_damaged;
            else if (state == State::MetadataOnly) ++result.metadata_only;
            else if (state == State::Unreadable) ++result.unreadable;
        }
        if (state == State::Ok) continue;
        touched.push_back(f);
        if (state == State::MetadataOnly) continue;
        if (state == State::Damaged && options.mode == RecoveryMode::Block) {
            const std::vector<std::uint8_t>& bad = scan.bad_blocks[f];
            for (std::uint32_t b = 0; b < bad.size();) {
                if (!bad[b]) { ++b; continue; }
                Range r;
                r.file = f;
                r.first_block = b;
                r.block_count = 0;
                r.whole_file = false;
                while (b < bad.size() && bad[b]) { // run of consecutive damaged blocks
                    r.bytes += block_length(catalog.files[f].size, catalog.block_size, b);
                    ++r.block_count;
                    ++b;
                }
                ranges.push_back(r);
            }
            continue;
        }
        ranges.push_back(whole_range(catalog, f));
        creates[f] = 1;
        created.push_back(f);
    }
    result.damaged_blocks = scan.damaged_blocks;

    PlanStats plan;
    const std::vector<Task> tasks = plan_tasks(ranges, plan_config(run, catalog.block_size), &plan);
    create_parents(options.source, catalog, created);
    std::vector<std::uint32_t> presized;
    for (std::uint32_t f : plan.split_files)
        if (creates[f]) presized.push_back(f);
    presize(options.source, catalog, presized, threads);
    const std::uint64_t prepare_ns = now_ns() - phase;

    // 3. Restore: every block read from the backup is checked against the
    //    catalog before it is written, so a damaged backup copy is never applied.
    Flags failed = make_flags(count);
    Flags timestamped = make_flags(count);
    auto buffers = worker_buffers(threads, catalog.block_size);
    const SchedulerStats restore = run_tasks(tasks, run.sched, [&](const Task& t, int worker) -> std::uint64_t {
        const CatalogFile& c = catalog.files[t.file];
        std::vector<char>& buffer = buffers[static_cast<std::size_t>(worker)];
        try {
            BlockSource in(options.backup, true, catalog, c);
            File out(native_path(options.source, c.path),
                     t.whole_file ? File::Mode::WriteCreate : File::Mode::WriteExisting);
            std::uint64_t bytes = 0;
            for (std::uint32_t b = t.first_block; b < t.first_block + t.block_count; ++b) {
                const std::uint64_t length = block_length(c.size, catalog.block_size, b);
                const std::uint64_t offset = static_cast<std::uint64_t>(b) * catalog.block_size;
                const std::size_t got = in.read(b, buffer.data());
                if (got != length || leaf_hash(buffer.data(), got) != c.leaves[b])
                    throw std::runtime_error("backup copy is damaged: " + c.path);
                out.write_at(buffer.data(), got, offset);
                bytes += got;
            }
            if (t.whole_file) {
                out.set_mtime(c.mtime);
                timestamped[t.file].store(1);
            }
            if (options.sync) out.sync();
            return bytes;
        } catch (...) {
            failed[t.file].store(1);
            throw;
        }
    });

    // 4. Timestamps for block-restored, split and metadata-only files.
    phase = now_ns();
    parallel_for(touched.size(), threads, [&](std::size_t i, int) {
        const std::uint32_t f = touched[i];
        if (failed[f].load() || timestamped[f].load()) return;
        File(native_path(options.source, catalog.files[f].path), File::Mode::WriteExisting)
            .set_mtime(catalog.files[f].mtime);
    });
    const std::uint64_t finalize_ns = now_ns() - phase;
    const std::uint64_t total_ns = now_ns() - start;

    for (std::uint32_t f : touched) {
        if (failed[f].load()) ++result.failed_files;
        else if (scan.state[f] != State::MetadataOnly) ++result.restored_files;
    }
    for (const WorkerStats& w : restore.workers) result.restored_bytes += w.bytes;

    // 5. Independent post-check of every touched file (outside total_ns).
    std::uint64_t verify_ns = 0;
    if (options.post_verify && !touched.empty()) {
        phase = now_ns();
        const Scan check = scan_tree(options.source, false, catalog, touched, false, run);
        for (std::uint32_t f : touched)
            if (check.state[f] != State::Ok) ++result.verify_bad;
        verify_ns = now_ns() - phase;
    }

    const std::uint64_t catalog_bytes = catalog.total_bytes();
    Json json;
    json.begin_object()
        .field("operation", "recover")
        .field("mode", recovery_mode_name(options.mode))
        .field("trust_mtime", options.trust_mtime)
        .field("sync", options.sync)
        .field("catalog_files", count)
        .field("catalog_bytes", catalog_bytes)
        .field("missing", result.missing)
        .field("size_changed", result.size_changed)
        .field("content_damaged", result.content_damaged)
        .field("metadata_only", result.metadata_only)
        .field("unreadable", result.unreadable)
        .field("damaged_blocks", result.damaged_blocks)
        .field("restored_files", result.restored_files)
        .field("restored_bytes", result.restored_bytes)
        .field("bytes_avoided", catalog_bytes - std::min(catalog_bytes, result.restored_bytes))
        .field("files_avoided", count - std::min<std::uint64_t>(count, result.restored_files))
        .field("failed_files", result.failed_files)
        .field("verified", options.post_verify)
        .field("verify_bad", result.verify_bad)
        .field("total_ns", total_ns)
        .field("load_ns", load_ns)
        .field("prepare_ns", prepare_ns)
        .field("restore_ns", restore.wall_ns)
        .field("finalize_ns", finalize_ns)
        .field("verify_ns", verify_ns);
    json.key("run");
    run.write_json(json);
    if (options.mode != RecoveryMode::Full) {
        json.key("scan");
        write_scan_json(json, scan);
    }
    json.key("plan");
    plan.write_json(json);
    json.key("scheduler");
    restore.write_json(json);
    json.end_object();
    result.json = json.str();
    return result;
}

} // namespace ipbrs
