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

// Backup layout: a file of at most segment_blocks blocks is stored whole under
// files/<path>; a larger file is stored as segment objects
// segments/<path>/<index>, each holding segment_blocks blocks. Concurrent
// writers therefore never share a destination file (NTFS serialises writes
// that extend one file).
struct Catalog {
    std::uint32_t block_size = 1u << 20;
    std::uint32_t segment_blocks = 16;
    std::vector<CatalogFile> files;
    std::uint64_t total_bytes() const;
    bool segmented(const CatalogFile& file) const { return file.leaves.size() > segment_blocks; }
};

// Text format, written atomically (temp file + rename):
//   IPBRS-CATALOG 1
//   block_size <n>
//   segment_blocks <n>
//   files <count>
//   F <size> <mtime> <blocks> <root-hex> <path>
//   <leaf-hex leaf-hex ...>      (concatenated, one line per file)
void save_catalog(const fs::path& path, const Catalog& catalog);
Catalog load_catalog(const fs::path& path);

// Rejects absolute paths, drive letters, backslashes and ".."/"." components.
bool is_safe_relative_path(const std::string& path);

inline fs::path catalog_path(const fs::path& backup) { return backup / "catalog.ipbrs"; }
inline fs::path payload_root(const fs::path& backup) { return backup / "files"; }
inline fs::path segment_root(const fs::path& backup) { return backup / "segments"; }

// Stored location of one block of a cataloged file inside a backup.
fs::path stored_path(const fs::path& backup, const Catalog& catalog, const CatalogFile& file,
                     std::uint32_t block, std::uint64_t& offset);

} // namespace ipbrs
