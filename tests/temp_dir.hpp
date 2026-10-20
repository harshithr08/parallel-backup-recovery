#pragma once

#include "util.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace testing {

namespace fs = std::filesystem;

// A fresh directory under the system temp folder, removed on destruction.
class TempDir {
public:
    TempDir() {
        ipbrs::Rng rng(ipbrs::now_ns());
        path_ = fs::temp_directory_path() / ("ipbrs-test-" + std::to_string(rng.next()));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const fs::path& path() const { return path_; }
    fs::path operator/(const std::string& name) const { return path_ / name; }

private:
    fs::path path_;
};

inline std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline void write_file(const fs::path& path, const std::string& data) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << data;
}

} // namespace testing
