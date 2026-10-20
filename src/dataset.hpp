#pragma once

#include "fileio.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ipbrs {

// Synthetic heterogeneous workload. Sizes per profile:
//   low    uniform 8-16 KiB
//   medium log-uniform 1 KiB - 1 MiB
//   high   log-uniform 256 B - 4 MiB
// plus big_files files of big_mib MiB each (the "few huge files" case).
struct GenOptions {
    fs::path out;
    std::uint64_t files = 1000;
    std::string profile = "medium";
    std::uint64_t seed = 1;
    double duplicate_ratio = 0.0;
    std::uint64_t big_files = 0;
    std::uint64_t big_mib = 256;
    std::uint64_t files_per_dir = 1000;
    int threads = 1;
};

std::string generate_dataset(const GenOptions& options); // JSON summary

// Damages ratio * N files of a tree, cycling through the requested types:
//   missing   delete the file
//   modified  overwrite up to 4 KiB in place (mtime changes)
//   corrupted flip one byte and restore the old mtime (silent bit rot)
//   truncated cut the file to half its size
struct DamageOptions {
    fs::path target;
    double ratio = 0.05;
    std::vector<std::string> types{"missing", "modified", "corrupted", "truncated"};
    std::uint64_t seed = 1;
    fs::path log; // optional TSV of type<TAB>path
};

std::string inject_damage(const DamageOptions& options); // JSON summary

} // namespace ipbrs
