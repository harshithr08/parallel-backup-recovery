#include "fileio.hpp"

#include <algorithm>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ipbrs {

#ifdef _WIN32

namespace {

std::runtime_error io_error(const char* what, const std::string& name) {
    return std::runtime_error(std::string(what) + " failed (error " +
                              std::to_string(GetLastError()) + "): " + name);
}

OVERLAPPED at_offset(std::uint64_t offset) {
    OVERLAPPED overlapped = {};
    overlapped.Offset = static_cast<DWORD>(offset);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
    return overlapped;
}

} // namespace

File::File(const fs::path& path, Mode mode) : handle_(INVALID_HANDLE_VALUE), name_(path.u8string()) {
    const DWORD access = mode == Mode::Read ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE;
    const DWORD disposition = mode == Mode::WriteCreate ? CREATE_ALWAYS : OPEN_EXISTING;
    const DWORD flags = FILE_ATTRIBUTE_NORMAL | (mode == Mode::Read ? FILE_FLAG_SEQUENTIAL_SCAN : 0);
    handle_ = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr, disposition, flags, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) throw io_error("open", name_);
}

File::~File() {
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
}

std::size_t File::read_at(void* buffer, std::size_t size, std::uint64_t offset) {
    std::size_t done = 0;
    while (done < size) {
        OVERLAPPED overlapped = at_offset(offset + done);
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(size - done, 1u << 30));
        DWORD got = 0;
        if (!ReadFile(handle_, static_cast<char*>(buffer) + done, request, &got, &overlapped)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            throw io_error("read", name_);
        }
        if (got == 0) break;
        done += got;
    }
    return done;
}

void File::write_at(const void* buffer, std::size_t size, std::uint64_t offset) {
    std::size_t done = 0;
    while (done < size) {
        OVERLAPPED overlapped = at_offset(offset + done);
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(size - done, 1u << 30));
        DWORD wrote = 0;
        if (!WriteFile(handle_, static_cast<const char*>(buffer) + done, request, &wrote, &overlapped) ||
            wrote == 0)
            throw io_error("write", name_);
        done += wrote;
    }
}

void File::resize(std::uint64_t size) {
    FILE_END_OF_FILE_INFO info = {};
    info.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
    if (!SetFileInformationByHandle(handle_, FileEndOfFileInfo, &info, sizeof(info)))
        throw io_error("resize", name_);
}

// MSVC's file_time_type counts 100 ns ticks since 1601, exactly a FILETIME.
void File::set_mtime(FileTime mtime) {
    FILETIME time;
    time.dwLowDateTime = static_cast<DWORD>(static_cast<std::uint64_t>(mtime));
    time.dwHighDateTime = static_cast<DWORD>(static_cast<std::uint64_t>(mtime) >> 32);
    if (!SetFileTime(handle_, nullptr, nullptr, &time)) throw io_error("set mtime", name_);
}

PathInfo stat_path(const fs::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    PathInfo info;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return info;
        throw io_error("stat", path.u8string());
    }
    if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
        info.kind = PathKind::Other;
        return info;
    }
    info.kind = PathKind::File;
    info.size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    info.mtime = static_cast<FileTime>((static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                                       data.ftLastWriteTime.dwLowDateTime);
    return info;
}

#else

namespace {

std::runtime_error io_error(const char* what, const std::string& name) {
    return std::runtime_error(std::string(what) + " failed: " + name);
}

} // namespace

void File::set_mtime(FileTime mtime) {
    fs::last_write_time(path_, from_ticks(mtime));
}

PathInfo stat_path(const fs::path& path) {
    PathInfo info;
    std::error_code error;
    const fs::file_status status = fs::symlink_status(path, error);
    if (status.type() == fs::file_type::not_found) return info;
    if (error) throw io_error("stat", path.string());
    if (status.type() != fs::file_type::regular) {
        info.kind = PathKind::Other;
        return info;
    }
    info.kind = PathKind::File;
    info.size = fs::file_size(path);
    info.mtime = to_ticks(fs::last_write_time(path));
    return info;
}

File::File(const fs::path& path, Mode mode) : fd_(-1), path_(path), name_(path.string()) {
    const int flags = mode == Mode::Read ? O_RDONLY
                    : mode == Mode::WriteExisting ? O_RDWR : O_RDWR | O_CREAT | O_TRUNC;
    fd_ = ::open(path.c_str(), flags, 0644);
    if (fd_ < 0) throw io_error("open", name_);
}

File::~File() {
    if (fd_ >= 0) ::close(fd_);
}

std::size_t File::read_at(void* buffer, std::size_t size, std::uint64_t offset) {
    std::size_t done = 0;
    while (done < size) {
        const ssize_t got = ::pread(fd_, static_cast<char*>(buffer) + done, size - done,
                                    static_cast<off_t>(offset + done));
        if (got < 0) throw io_error("read", name_);
        if (got == 0) break;
        done += static_cast<std::size_t>(got);
    }
    return done;
}

void File::write_at(const void* buffer, std::size_t size, std::uint64_t offset) {
    std::size_t done = 0;
    while (done < size) {
        const ssize_t wrote = ::pwrite(fd_, static_cast<const char*>(buffer) + done, size - done,
                                       static_cast<off_t>(offset + done));
        if (wrote <= 0) throw io_error("write", name_);
        done += static_cast<std::size_t>(wrote);
    }
}

void File::resize(std::uint64_t size) {
    if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) throw io_error("resize", name_);
}

#endif

} // namespace ipbrs
