#include "testing.hpp"
#include "temp_dir.hpp"

#include "catalog.hpp"

using namespace ipbrs;

namespace {

CatalogFile entry(const std::string& path, std::uint64_t size, int seed) {
    CatalogFile f;
    f.path = path;
    f.size = size;
    f.mtime = 100 + seed;
    for (std::uint32_t b = 0; b < block_count(size, 4096); ++b) {
        const std::string text = path + std::to_string(b);
        f.leaves.push_back(leaf_hash(text.data(), text.size()));
    }
    f.root = merkle_root(f.leaves);
    return f;
}

Catalog small_catalog() {
    Catalog c;
    c.block_size = 4096;
    c.segment_blocks = 4;
    c.files = {entry("a.txt", 10, 1), entry("dir/b.bin", 9000, 2), entry("dir/c.bin", 0, 3)};
    return c;
}

std::string valid_text(const testing::TempDir& dir) {
    save_catalog(dir / "good.ipbrs", small_catalog());
    return testing::read_file(dir / "good.ipbrs");
}

std::string replace_once(std::string text, const std::string& from, const std::string& to) {
    const std::size_t at = text.find(from);
    if (at == std::string::npos) throw std::runtime_error("test setup: '" + from + "' not found");
    return text.replace(at, from.size(), to);
}

// Writes the mutated text and expects load_catalog to reject it.
void check_rejected(const testing::TempDir& dir, const std::string& text) {
    testing::write_file(dir / "bad.ipbrs", text);
    CHECK_THROWS(load_catalog(dir / "bad.ipbrs"));
}

} // namespace

TEST(catalog_rejects_bad_header_and_geometry) {
    testing::TempDir dir;
    const std::string good = valid_text(dir);
    CHECK_EQ(load_catalog(dir / "good.ipbrs").files.size(), 3u);
    check_rejected(dir, replace_once(good, "IPBRS-CATALOG 1", "IPBRS-CATALOG 2"));
    check_rejected(dir, replace_once(good, "block_size 4096", "block_size 1"));
    check_rejected(dir, replace_once(good, "block_size 4096", "block_size 134217728")); // 128 MiB
    check_rejected(dir, replace_once(good, "segment_blocks 4", "segment_blocks 0"));
    check_rejected(dir, "");
    CHECK_THROWS(load_catalog(dir / "missing.ipbrs"));
}

TEST(catalog_rejects_truncation_and_bad_counts) {
    testing::TempDir dir;
    const std::string good = valid_text(dir);
    check_rejected(dir, good.substr(0, good.size() / 2));
    check_rejected(dir, good.substr(0, good.size() - 3)); // last leaf hash cut short
    testing::write_file(dir / "no-newline.ipbrs", good.substr(0, good.size() - 1));
    CHECK_EQ(load_catalog(dir / "no-newline.ipbrs").files.size(), 3u); // a missing final newline is harmless
    check_rejected(dir, replace_once(good, "files 3", "files 4"));
    check_rejected(dir, replace_once(good, "files 3", "files 999999999999")); // no huge allocation
    check_rejected(dir, replace_once(good, "files 3", "files 2"));            // leftover entry
    check_rejected(dir, good + "junk\n");
}

TEST(catalog_rejects_inconsistent_entries) {
    testing::TempDir dir;
    const std::string good = valid_text(dir);
    check_rejected(dir, replace_once(good, "F 9000 ", "F 90000 "));  // size no longer matches block count
    check_rejected(dir, replace_once(good, "F 9000 ", "F 9x00 "));   // not a number
    check_rejected(dir, replace_once(good, "F 10 ", "F 5000 "));     // size implies 2 blocks, 1 leaf stored
    check_rejected(dir, replace_once(good, " a.txt\n", " dir/b.bin\n")); // duplicate path
}

TEST(catalog_save_rejects_duplicates_and_file_directory_conflicts) {
    testing::TempDir dir;
    Catalog duplicate = small_catalog();
    duplicate.files.push_back(entry("a.txt", 3, 9));
    CHECK_THROWS(save_catalog(dir / "x.ipbrs", duplicate));

    Catalog conflict = small_catalog();
    conflict.files.push_back(entry("a.txt/inner", 3, 9)); // a.txt is a file and a directory
    CHECK_THROWS(save_catalog(dir / "x.ipbrs", conflict));

    Catalog geometry = small_catalog();
    geometry.block_size = 100;
    CHECK_THROWS(save_catalog(dir / "x.ipbrs", geometry));
    CHECK(!fs::exists(dir / "x.ipbrs"));
}
