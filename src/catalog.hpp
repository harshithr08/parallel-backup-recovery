#pragma once

#include "fileio.hpp"
#include "merkle.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ipbrs {

struct CatalogFile {
    std::string path;
    std::uint64_t size = 0;
    FileTime mtime = 0;
    Digest root{};
    std::vector<Digest> leaves; // one per block
};

struct Catalog {
    std::uint32_t block_size = 1u << 20;
    std::vector<CatalogFile> files;
    std::uint64_t total_bytes() const;
};

// Text format, written atomically (temp file + rename):
//   IPBRS-CATALOG 1
//   block_size <n>
//   files <count>
//   F <size> <mtime> <blocks> <root-hex> <path>
//   <leaf-hex leaf-hex ...>      (concatenated, one line per file)
void save_catalog(const fs::path& path, const Catalog& catalog);
Catalog load_catalog(const fs::path& path);

// Rejects absolute paths, drive letters, backslashes and ".."/"." components.
bool is_safe_relative_path(const std::string& path);

inline fs::path catalog_path(const fs::path& backup) { return backup / "catalog.ipbrs"; }
inline fs::path payload_root(const fs::path& backup) { return backup / "files"; }

} // namespace ipbrs
