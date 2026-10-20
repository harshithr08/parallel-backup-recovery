#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace ipbrs {

namespace fs = std::filesystem;

// Positioned file I/O (pread/pwrite semantics). Several handles may write
// disjoint ranges of one file concurrently, which block-level tasks rely on.
// Modification time in std::filesystem::file_time_type ticks.
using FileTime = std::int64_t;

class File {
public:
    enum class Mode { Read, WriteExisting, WriteCreate };

    File(const fs::path& path, Mode mode);
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    // Reads up to size bytes; returns fewer only at end of file.
    std::size_t read_at(void* buffer, std::size_t size, std::uint64_t offset);
    void write_at(const void* buffer, std::size_t size, std::uint64_t offset);
    void resize(std::uint64_t size);
    void set_mtime(FileTime mtime);
    void sync(); // flush written data to stable storage

private:
#ifdef _WIN32
    void* handle_;
#else
    int fd_;
    fs::path path_;
#endif
    std::string name_;
};

enum class PathKind { Missing, File, Other };

struct PathInfo {
    PathKind kind = PathKind::Missing;
    std::uint64_t size = 0;
    FileTime mtime = 0;
};

// One metadata lookup; throws on errors other than "not found".
PathInfo stat_path(const fs::path& path);

inline FileTime to_ticks(fs::file_time_type time) { return time.time_since_epoch().count(); }
inline fs::file_time_type from_ticks(FileTime ticks) {
    return fs::file_time_type(fs::file_time_type::duration(ticks));
}

} // namespace ipbrs
