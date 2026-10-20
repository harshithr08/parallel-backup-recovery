#include "testing.hpp"
#include "temp_dir.hpp"

#include "dataset.hpp"
#include "engine.hpp"

using namespace ipbrs;

namespace {

const std::uint32_t kBlock = 64 * 1024; // 16 blocks per segment -> files > 1 MiB are segmented

// The same seed always produces the same tree, so a second generation is the
// ground truth for byte-for-byte comparison after recovery.
void make_dataset(const fs::path& out) {
    GenOptions gen;
    gen.out = out;
    gen.files = 80;
    gen.profile = "medium";
    gen.seed = 2026;
    gen.duplicate_ratio = 0.1;
    gen.big_files = 2;
    gen.big_mib = 3;
    gen.files_per_dir = 25;
    gen.threads = 4;
    generate_dataset(gen);
}

RunConfig ipbrs_run(Policy policy = Policy::Steal, Split split = Split::Adaptive, int threads = 4) {
    RunConfig run;
    run.sched.policy = policy;
    run.sched.threads = threads;
    run.split = split;
    run.split_factor = 8;
    return run;
}

BackupResult backup(const fs::path& source, const fs::path& dest, const RunConfig& run) {
    BackupOptions options;
    options.source = source;
    options.backup = dest;
    options.block_size = kBlock;
    options.run = run;
    return run_backup(options);
}

RecoverResult recover(const fs::path& source, const fs::path& dest, RecoveryMode mode, bool trust_mtime = false) {
    RecoverOptions options;
    options.source = source;
    options.backup = dest;
    options.mode = mode;
    options.trust_mtime = trust_mtime;
    options.run = ipbrs_run();
    return run_recover(options);
}

std::uint64_t verify_bad(const fs::path& dest) {
    VerifyOptions options;
    options.backup = dest;
    options.run = ipbrs_run();
    return run_verify(options).bad;
}

// Same relative paths, contents and modification times.
void check_same_tree(const fs::path& actual, const fs::path& expected) {
    const auto a = discover_files(actual, 1, nullptr);
    const auto e = discover_files(expected, 1, nullptr);
    CHECK_EQ(a.size(), e.size());
    for (std::size_t i = 0; i < a.size() && i < e.size(); ++i) {
        CHECK_EQ(a[i].path, e[i].path);
        CHECK_EQ(a[i].size, e[i].size);
        CHECK(testing::read_file(native_path(actual, a[i].path)) == testing::read_file(native_path(expected, e[i].path)));
    }
}

void flip_byte(const fs::path& path, std::uint64_t offset, bool keep_mtime) {
    const FileTime mtime = stat_path(path).mtime;
    File file(path, File::Mode::WriteExisting);
    unsigned char byte = 0;
    file.read_at(&byte, 1, offset);
    byte ^= 0x5A;
    file.write_at(&byte, 1, offset);
    if (keep_mtime) file.set_mtime(mtime);
}

} // namespace

TEST(e2e_every_policy_produces_a_verified_backup) {
    testing::TempDir dir;
    make_dataset(dir / "src");
    int index = 0;
    for (Policy policy : {Policy::Sequential, Policy::Static, Policy::Dynamic, Policy::Steal}) {
        for (Split split : {Split::None, Split::Adaptive}) {
            const fs::path dest = dir / ("bk" + std::to_string(index++));
            const BackupResult result = backup(dir / "src", dest, ipbrs_run(policy, split));
            CHECK(result.committed);
            CHECK_EQ(result.files, 82u);
            CHECK_EQ(result.failed_files, 0u);
            CHECK(fs::is_directory(segment_root(dest) / "big" / "b000.dat")); // 3 MiB > 1 MiB segment
            CHECK_EQ(verify_bad(dest), 0u);
        }
    }
}

TEST(e2e_block_recovery_repairs_all_damage_types) {
    testing::TempDir dir;
    make_dataset(dir / "src");
    make_dataset(dir / "expected");
    CHECK(backup(dir / "src", dir / "bk", ipbrs_run()).committed);

    DamageOptions damage;
    damage.target = dir / "src";
    damage.ratio = 0.25;
    damage.seed = 9;
    inject_damage(damage);
    flip_byte(dir / "src" / "big" / "b001.dat", 2 * 1024 * 1024 + 17, true); // one block, silent

    const RecoverResult r = recover(dir / "src", dir / "bk", RecoveryMode::Block);
    CHECK(r.missing > 0);
    CHECK(r.size_changed > 0);
    CHECK(r.content_damaged > 0);
    CHECK_EQ(r.failed_files, 0u);
    CHECK_EQ(r.verify_bad, 0u);
    std::uint64_t tree_bytes = 0;
    for (const FileEntry& f : discover_files(dir / "expected", 1, nullptr)) tree_bytes += f.size;
    CHECK(r.restored_bytes < tree_bytes / 2); // selective: well under a full restore
    check_same_tree(dir / "src", dir / "expected");

    const RecoverResult again = recover(dir / "src", dir / "bk", RecoveryMode::Block);
    CHECK_EQ(again.restored_files, 0u); // nothing left to repair
}

TEST(e2e_file_and_full_recovery_restore_identical_trees) {
    testing::TempDir dir;
    make_dataset(dir / "src");
    make_dataset(dir / "expected");
    CHECK(backup(dir / "src", dir / "bk", ipbrs_run(Policy::Dynamic)).committed);
    for (RecoveryMode mode : {RecoveryMode::File, RecoveryMode::Full}) {
        DamageOptions damage;
        damage.target = dir / "src";
        damage.ratio = 0.2;
        damage.seed = mode == RecoveryMode::File ? 1 : 2;
        inject_damage(damage);
        flip_byte(dir / "src" / "big" / "b000.dat", 100, true);
        const RecoverResult r = recover(dir / "src", dir / "bk", mode);
        CHECK_EQ(r.failed_files, 0u);
        CHECK_EQ(r.verify_bad, 0u);
        if (mode == RecoveryMode::Full) CHECK_EQ(r.restored_files, 82u);
        check_same_tree(dir / "src", dir / "expected");
    }
}

TEST(e2e_block_mode_restores_less_than_file_mode) {
    testing::TempDir dir;
    make_dataset(dir / "src");
    CHECK(backup(dir / "src", dir / "bk", ipbrs_run()).committed);
    std::uint64_t restored[2] = {0, 0};
    int i = 0;
    for (RecoveryMode mode : {RecoveryMode::Block, RecoveryMode::File}) {
        flip_byte(dir / "src" / "big" / "b000.dat", 5, false);
        flip_byte(dir / "src" / "big" / "b001.dat", 3 * 1024 * 1024 - 1, false);
        const RecoverResult r = recover(dir / "src", dir / "bk", mode);
        CHECK_EQ(r.content_damaged, 2u);
        CHECK_EQ(r.verify_bad, 0u);
        restored[i++] = r.restored_bytes;
    }
    CHECK_EQ(restored[0], 2u * kBlock);               // one 64 KiB block per file
    CHECK_EQ(restored[1], 2u * 3u * 1024u * 1024u);   // both whole files
}

TEST(e2e_trust_mtime_misses_silent_corruption) {
    testing::TempDir dir;
    make_dataset(dir / "src");
    CHECK(backup(dir / "src", dir / "bk", ipbrs_run()).committed);
    flip_byte(dir / "src" / "d0000" / "f0000003.dat", 0, true); // bit rot, mtime unchanged
    CHECK_EQ(recover(dir / "src", dir / "bk", RecoveryMode::Block, true).content_damaged, 0u);
    CHECK_EQ(recover(dir / "src", dir / "bk", RecoveryMode::Block, false).content_damaged, 1u);
}

TEST(e2e_damaged_backup_copy_is_never_applied) {
    testing::TempDir dir;
    make_dataset(dir / "src");
    CHECK(backup(dir / "src", dir / "bk", ipbrs_run()).committed);
    flip_byte(segment_root(dir / "bk") / "big" / "b000.dat" / "000000", 10, false); // bad backup block 0
    CHECK(verify_bad(dir / "bk") == 1u);

    flip_byte(dir / "src" / "big" / "b000.dat", 10, false); // same block damaged at the source
    const std::string damaged = testing::read_file(dir / "src" / "big" / "b000.dat");
    const RecoverResult r = recover(dir / "src", dir / "bk", RecoveryMode::Block);
    CHECK_EQ(r.failed_files, 1u);
    CHECK(testing::read_file(dir / "src" / "big" / "b000.dat") == damaged); // left as it was
}

TEST(e2e_backup_rejects_unsafe_destinations) {
    testing::TempDir dir;
    make_dataset(dir / "src");
    CHECK_THROWS(backup(dir / "src", dir / "src" / "inner-backup", ipbrs_run())); // inside source
    testing::write_file(dir / "used" / "x.txt", "x");
    CHECK_THROWS(backup(dir / "src", dir / "used", ipbrs_run()));                  // not empty
}
