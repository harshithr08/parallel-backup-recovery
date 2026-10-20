#include "dataset.hpp"

#include "scheduler.hpp"
#include "util.hpp"
#include "workload.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>

namespace ipbrs {
namespace {

std::uint64_t log_uniform(Rng& rng, double low, double high) {
    return static_cast<std::uint64_t>(std::exp(std::log(low) + rng.unit() * (std::log(high) - std::log(low))));
}

std::uint64_t sample_size(const std::string& profile, Rng& rng) {
    if (profile == "low") return 8192 + rng.below(8193);
    if (profile == "medium") return log_uniform(rng, 1024.0, 1048576.0);
    if (profile == "high") return log_uniform(rng, 256.0, 4194304.0);
    throw std::invalid_argument("unknown profile (low|medium|high): " + profile);
}

void write_random_file(const fs::path& path, std::uint64_t size, std::uint64_t content_seed) {
    File out(path, File::Mode::WriteCreate);
    Rng rng(content_seed);
    std::vector<std::uint64_t> words((1u << 20) / sizeof(std::uint64_t));
    for (std::uint64_t offset = 0; offset < size;) {
        for (std::uint64_t& w : words) w = rng.next();
        const std::uint64_t n = std::min<std::uint64_t>(size - offset, words.size() * sizeof(std::uint64_t));
        out.write_at(words.data(), static_cast<std::size_t>(n), offset);
        offset += n;
    }
}

} // namespace

std::string generate_dataset(const GenOptions& options) {
    const std::uint64_t start = now_ns();
    if (fs::exists(options.out) && !fs::is_empty(options.out))
        throw std::invalid_argument("output directory must be new or empty: " + options.out.u8string());
    if (options.files_per_dir == 0) throw std::invalid_argument("files_per_dir must be positive");

    struct Spec { std::string path; std::uint64_t size; std::uint64_t seed; };
    std::vector<Spec> specs;
    char path_buffer[64];
    Rng rng(options.seed);
    for (std::uint64_t i = 0; i < options.files; ++i) {
        std::snprintf(path_buffer, sizeof(path_buffer), "d%04llu/f%07llu.dat",
                      static_cast<unsigned long long>(i / options.files_per_dir),
                      static_cast<unsigned long long>(i));
        Spec spec{path_buffer, sample_size(options.profile, rng), mix_seed(options.seed, i)};
        if (i > 0 && rng.unit() < options.duplicate_ratio) {
            const Spec& original = specs[static_cast<std::size_t>(rng.below(i))];
            spec.size = original.size;
            spec.seed = original.seed; // identical content
        }
        specs.push_back(spec);
    }
    for (std::uint64_t i = 0; i < options.big_files; ++i) {
        std::snprintf(path_buffer, sizeof(path_buffer), "big/b%03llu.dat", static_cast<unsigned long long>(i));
        specs.push_back(Spec{path_buffer, options.big_mib << 20, mix_seed(options.seed ^ 0xB16ull, i)});
    }

    std::map<std::string, int> dirs;
    for (const Spec& s : specs) dirs[s.path.substr(0, s.path.find('/'))] = 1;
    for (const auto& d : dirs) fs::create_directories(options.out / d.first);

    // Largest first so a big file does not start last.
    std::vector<std::size_t> order(specs.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) { return specs[a].size > specs[b].size; });
    parallel_for(order.size(), std::max(1, options.threads), [&](std::size_t i, int) {
        const Spec& s = specs[order[i]];
        write_random_file(native_path(options.out, s.path), s.size, s.seed);
    });

    std::uint64_t bytes = 0;
    for (const Spec& s : specs) bytes += s.size;
    Json json;
    json.begin_object()
        .field("operation", "generate")
        .field("profile", options.profile)
        .field("files", static_cast<std::uint64_t>(specs.size()))
        .field("bytes", bytes)
        .field("big_files", options.big_files)
        .field("big_mib", options.big_mib)
        .field("seed", options.seed)
        .field("duplicate_ratio", options.duplicate_ratio)
        .field("total_ns", now_ns() - start)
        .end_object();
    return json.str();
}

std::string inject_damage(const DamageOptions& options) {
    if (options.ratio < 0.0 || options.ratio > 1.0) throw std::invalid_argument("ratio must be in [0, 1]");
    if (options.types.empty()) throw std::invalid_argument("at least one damage type is required");
    for (const std::string& t : options.types)
        if (t != "missing" && t != "modified" && t != "corrupted" && t != "truncated")
            throw std::invalid_argument("unknown damage type: " + t);

    std::vector<FileEntry> files = discover_files(options.target, 1, nullptr);
    Rng rng(options.seed);
    for (std::size_t i = files.size(); i > 1; --i)
        std::swap(files[i - 1], files[static_cast<std::size_t>(rng.below(i))]);
    std::size_t count = static_cast<std::size_t>(std::llround(options.ratio * static_cast<double>(files.size())));
    if (options.ratio > 0.0) count = std::max<std::size_t>(count, 1);
    count = std::min(count, files.size());

    std::ofstream log;
    if (!options.log.empty()) {
        log.open(options.log, std::ios::binary | std::ios::trunc);
        if (!log) throw std::runtime_error("cannot write damage log: " + options.log.u8string());
    }
    std::map<std::string, std::uint64_t> applied;
    std::uint64_t damaged_bytes = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const FileEntry& f = files[i];
        std::string type = options.types[i % options.types.size()];
        if (f.size == 0) type = "missing";
        const fs::path path = native_path(options.target, f.path);
        if (type == "missing") {
            fs::remove(path);
        } else if (type == "modified") {
            const std::uint64_t length = std::min<std::uint64_t>(4096, f.size);
            const std::uint64_t offset = rng.below(f.size - length + 1);
            std::vector<std::uint64_t> noise(static_cast<std::size_t>((length + 7) / 8));
            for (std::uint64_t& w : noise) w = rng.next();
            File(path, File::Mode::WriteExisting).write_at(noise.data(), static_cast<std::size_t>(length), offset);
            damaged_bytes += length;
        } else if (type == "corrupted") {
            File file(path, File::Mode::WriteExisting);
            const std::uint64_t offset = rng.below(f.size);
            unsigned char byte = 0;
            file.read_at(&byte, 1, offset);
            byte ^= static_cast<unsigned char>(1u << rng.below(8));
            file.write_at(&byte, 1, offset);
            file.set_mtime(f.mtime);
            damaged_bytes += 1;
        } else {
            File(path, File::Mode::WriteExisting).resize(f.size / 2);
            damaged_bytes += f.size - f.size / 2;
        }
        ++applied[type];
        if (log) log << type << '\t' << f.path << '\n';
    }

    Json json;
    json.begin_object()
        .field("operation", "damage")
        .field("files", static_cast<std::uint64_t>(files.size()))
        .field("damaged", static_cast<std::uint64_t>(count))
        .field("damaged_bytes", damaged_bytes)
        .field("seed", options.seed);
    json.key("by_type").begin_object();
    for (const auto& entry : applied) json.field(entry.first, entry.second);
    json.end_object().end_object();
    return json.str();
}

} // namespace ipbrs
