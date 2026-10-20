#include "catalog.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ipbrs {

std::uint64_t Catalog::total_bytes() const {
    std::uint64_t total = 0;
    for (const CatalogFile& f : files) total += f.size;
    return total;
}

fs::path stored_path(const fs::path& backup, const Catalog& catalog, const CatalogFile& file,
                     std::uint32_t block, std::uint64_t& offset) {
    if (!catalog.segmented(file)) {
        offset = static_cast<std::uint64_t>(block) * catalog.block_size;
        return payload_root(backup) / fs::u8path(file.path);
    }
    offset = static_cast<std::uint64_t>(block % catalog.segment_blocks) * catalog.block_size;
    char name[16];
    std::snprintf(name, sizeof(name), "%06u", block / catalog.segment_blocks);
    return segment_root(backup) / fs::u8path(file.path) / name;
}

bool is_safe_relative_path(const std::string& path) {
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string::npos ||
        path.find(':') != std::string::npos || path.find('\n') != std::string::npos)
        return false;
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const std::size_t end = std::min(path.find('/', begin), path.size());
        const std::string part = path.substr(begin, end - begin);
        if (part.empty() || part == "." || part == "..") return false;
        begin = end + 1;
    }
    return true;
}

void save_catalog(const fs::path& path, const Catalog& catalog) {
    const fs::path temporary = path.u8string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write catalog: " + temporary.u8string());
        out << "IPBRS-CATALOG 1\n"
            << "block_size " << catalog.block_size << "\n"
            << "segment_blocks " << catalog.segment_blocks << "\n"
            << "files " << catalog.files.size() << "\n";
        std::string line;
        for (const CatalogFile& f : catalog.files) {
            if (!is_safe_relative_path(f.path)) throw std::runtime_error("unsafe catalog path: " + f.path);
            out << "F " << f.size << ' ' << f.mtime << ' ' << f.leaves.size() << ' '
                << to_hex(f.root) << ' ' << f.path << '\n';
            line.clear();
            line.reserve(f.leaves.size() * 64 + 1);
            for (const Digest& leaf : f.leaves) line += to_hex(leaf);
            line += '\n';
            out << line;
        }
        out.flush();
        if (!out) throw std::runtime_error("failed writing catalog: " + temporary.u8string());
    }
    File(temporary, File::Mode::WriteExisting).sync();
    fs::rename(temporary, path);
}

Catalog load_catalog(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open catalog: " + path.u8string());
    auto fail = [&](const std::string& why) {
        return std::runtime_error("invalid catalog " + path.u8string() + ": " + why);
    };

    std::string line, word;
    Catalog catalog;
    std::size_t count = 0;
    if (!std::getline(in, line) || line != "IPBRS-CATALOG 1") throw fail("bad header");
    if (!std::getline(in, line)) throw fail("missing block_size");
    {
        std::istringstream fields(line);
        if (!(fields >> word >> catalog.block_size) || word != "block_size" || catalog.block_size == 0)
            throw fail("bad block_size");
    }
    if (!std::getline(in, line)) throw fail("missing segment_blocks");
    {
        std::istringstream fields(line);
        if (!(fields >> word >> catalog.segment_blocks) || word != "segment_blocks" || catalog.segment_blocks == 0)
            throw fail("bad segment_blocks");
    }
    if (!std::getline(in, line)) throw fail("missing file count");
    {
        std::istringstream fields(line);
        if (!(fields >> word >> count) || word != "files") throw fail("bad file count");
    }

    catalog.files.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        CatalogFile& f = catalog.files[i];
        if (!std::getline(in, line)) throw fail("truncated");
        std::istringstream fields(line);
        std::size_t blocks = 0;
        std::string root;
        if (!(fields >> word >> f.size >> f.mtime >> blocks >> root) || word != "F" ||
            !parse_hex(root, 0, f.root))
            throw fail("bad entry line " + std::to_string(i));
        fields.get(); // the single space before the path
        std::getline(fields, f.path);
        if (!is_safe_relative_path(f.path)) throw fail("unsafe path " + f.path);
        if (blocks != block_count(f.size, catalog.block_size)) throw fail("block count mismatch " + f.path);
        if (!std::getline(in, line) || line.size() != blocks * 64) throw fail("bad leaves for " + f.path);
        f.leaves.resize(blocks);
        for (std::size_t b = 0; b < blocks; ++b)
            if (!parse_hex(line, b * 64, f.leaves[b])) throw fail("bad leaf hex for " + f.path);
        if (merkle_root(f.leaves) != f.root) throw fail("root does not match leaves for " + f.path);
    }
    return catalog;
}

} // namespace ipbrs
