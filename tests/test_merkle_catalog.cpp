#include "testing.hpp"
#include "temp_dir.hpp"

#include "catalog.hpp"
#include "merkle.hpp"

#include <string>
#include <vector>

using namespace ipbrs;

namespace {

Digest leaf_of(int n) {
    const std::string text = "block-" + std::to_string(n);
    return leaf_hash(text.data(), text.size());
}

Digest node(const Digest& left, const Digest& right) {
    std::vector<std::uint8_t> bytes(1, 0x01);
    bytes.insert(bytes.end(), left.begin(), left.end());
    bytes.insert(bytes.end(), right.begin(), right.end());
    return sha256(bytes.data(), bytes.size());
}

CatalogFile catalog_file(const std::string& path, std::uint32_t blocks, int seed) {
    CatalogFile f;
    f.path = path;
    f.size = blocks == 0 ? 0 : static_cast<std::uint64_t>(blocks) * 4096 - 7;
    f.mtime = 1234567 + seed;
    for (std::uint32_t b = 0; b < std::max<std::uint32_t>(1, blocks); ++b) f.leaves.push_back(leaf_of(seed * 100 + static_cast<int>(b)));
    f.root = merkle_root(f.leaves);
    return f;
}

Catalog sample_catalog() {
    Catalog c;
    c.block_size = 4096;
    c.segment_blocks = 4;
    c.files.push_back(catalog_file("empty.bin", 0, 1));
    c.files.push_back(catalog_file("dir/one block.txt", 1, 2)); // space in path
    c.files.push_back(catalog_file("dir/sub/many.dat", 9, 3));   // segmented (9 > 4)
    return c;
}

} // namespace

TEST(merkle_root_structure) {
    const Digest a = leaf_of(1), b = leaf_of(2), c = leaf_of(3);
    CHECK(merkle_root({a}) == a);
    CHECK(merkle_root({a, b}) == node(a, b));
    CHECK(merkle_root({a, b, c}) == node(node(a, b), c)); // odd node promoted
    CHECK(merkle_root({}) == leaf_hash(nullptr, 0));
}

TEST(merkle_leaf_is_domain_separated) {
    const std::string data = "payload";
    CHECK(leaf_hash(data.data(), data.size()) != sha256(data.data(), data.size()));
}

TEST(merkle_any_changed_leaf_changes_root) {
    for (int count = 1; count <= 9; ++count) {
        std::vector<Digest> leaves;
        for (int i = 0; i < count; ++i) leaves.push_back(leaf_of(i));
        const Digest root = merkle_root(leaves);
        for (int i = 0; i < count; ++i) {
            std::vector<Digest> changed = leaves;
            changed[static_cast<std::size_t>(i)] = leaf_of(1000 + i);
            CHECK(merkle_root(changed) != root);
        }
        if (count > 1) { // order matters
            std::vector<Digest> swapped = leaves;
            std::swap(swapped[0], swapped[1]);
            CHECK(merkle_root(swapped) != root);
        }
    }
}

TEST(block_geometry) {
    CHECK_EQ(block_count(0, 4096), 1u);
    CHECK_EQ(block_length(0, 4096, 0), 0u);
    CHECK_EQ(block_count(4096, 4096), 1u);
    CHECK_EQ(block_count(4097, 4096), 2u);
    CHECK_EQ(block_length(4097, 4096, 0), 4096u);
    CHECK_EQ(block_length(4097, 4096, 1), 1u);
    CHECK_EQ(block_length(4097, 4096, 2), 0u);
}

TEST(catalog_round_trip) {
    testing::TempDir dir;
    const Catalog original = sample_catalog();
    save_catalog(dir / "catalog.ipbrs", original);
    CHECK(!fs::exists(dir / "catalog.ipbrs.tmp"));
    const Catalog loaded = load_catalog(dir / "catalog.ipbrs");
    CHECK_EQ(loaded.block_size, original.block_size);
    CHECK_EQ(loaded.segment_blocks, original.segment_blocks);
    CHECK_EQ(loaded.files.size(), original.files.size());
    for (std::size_t i = 0; i < original.files.size(); ++i) {
        CHECK_EQ(loaded.files[i].path, original.files[i].path);
        CHECK_EQ(loaded.files[i].size, original.files[i].size);
        CHECK_EQ(loaded.files[i].mtime, original.files[i].mtime);
        CHECK(loaded.files[i].root == original.files[i].root);
        CHECK(loaded.files[i].leaves == original.files[i].leaves);
    }
}

TEST(catalog_rejects_tampered_leaf) {
    testing::TempDir dir;
    save_catalog(dir / "catalog.ipbrs", sample_catalog());
    std::string text = testing::read_file(dir / "catalog.ipbrs");
    const std::size_t line = text.find("many.dat\n") + 9; // start of that file's leaf line
    text[line] = text[line] == '0' ? '1' : '0';
    testing::write_file(dir / "catalog.ipbrs", text);
    CHECK_THROWS(load_catalog(dir / "catalog.ipbrs"));
}

TEST(catalog_rejects_unsafe_paths) {
    CHECK(is_safe_relative_path("a/b.txt"));
    CHECK(is_safe_relative_path("a b/c"));
    const char* unsafe[] = {"", "/abs", "a/../b", "..", "./a", "a//b", "a/", "C:/x", "a\\b", "a\nb"};
    for (const char* path : unsafe) CHECK(!is_safe_relative_path(path));

    testing::TempDir dir;
    Catalog c = sample_catalog();
    c.files[0].path = "../escape.bin";
    CHECK_THROWS(save_catalog(dir / "catalog.ipbrs", c));
    save_catalog(dir / "catalog.ipbrs", sample_catalog());
    std::string text = testing::read_file(dir / "catalog.ipbrs");
    text.replace(text.find("empty.bin"), 9, "../ab.bin"); // same length, only the path changes
    testing::write_file(dir / "catalog.ipbrs", text);
    CHECK_THROWS(load_catalog(dir / "catalog.ipbrs"));
}

TEST(stored_path_layout) {
    const Catalog c = sample_catalog();
    const fs::path backup = "B";
    std::uint64_t offset = 99;
    CHECK(stored_path(backup, c, c.files[1], 0, offset) == fs::path("B") / "files" / fs::u8path("dir/one block.txt"));
    CHECK_EQ(offset, 0u);
    // many.dat: 9 blocks, segment_blocks 4 -> block 6 lives in segment 1 at block offset 2.
    CHECK(stored_path(backup, c, c.files[2], 6, offset) == fs::path("B") / "segments" / fs::u8path("dir/sub/many.dat") / "000001");
    CHECK_EQ(offset, 2u * 4096);
    CHECK(stored_path(backup, c, c.files[2], 8, offset).filename() == "000002");
    CHECK_EQ(offset, 0u);
}
