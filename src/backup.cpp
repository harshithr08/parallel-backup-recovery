#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <stdio.h>
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "ipbrs/backup.hpp"
#include "ipbrs/win_crypto.hpp"

namespace ipbrs {
namespace {

std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) return std::wstring();
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(),
                                           static_cast<int>(value.size()), 0, 0);
    if (needed <= 0) throw std::runtime_error("input path is not valid UTF-8");
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(),
                            static_cast<int>(value.size()), &result[0], needed) != needed)
        throw std::runtime_error("could not convert UTF-8 path to Windows path");
    return result;
}

std::string wide_to_utf8(const std::wstring& value) {
    if (value.empty()) return std::string();
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
                                           static_cast<int>(value.size()), 0, 0, 0, 0);
    if (needed <= 0) throw std::runtime_error("could not encode Windows path as UTF-8");
    std::string result(static_cast<std::size_t>(needed), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
                            static_cast<int>(value.size()), &result[0], needed, 0, 0) != needed)
        throw std::runtime_error("could not encode Windows path as UTF-8");
    return result;
}

std::wstring full_path(const std::wstring& path) {
    DWORD needed = GetFullPathNameW(path.c_str(), 0, 0, 0);
    if (!needed) throw std::runtime_error("GetFullPathNameW failed");
    std::vector<wchar_t> buffer(needed + 1);
    const DWORD written = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), &buffer[0], 0);
    if (!written || written >= buffer.size()) throw std::runtime_error("could not resolve full path");
    std::wstring result(&buffer[0], written);
    while (result.size() > 3 && (result[result.size() - 1] == L'\\' || result[result.size() - 1] == L'/'))
        result.erase(result.size() - 1);
    return result;
}

std::wstring join_path(const std::wstring& left, const std::wstring& right) {
    if (left.empty()) return right;
    if (right.empty()) return left;
    if (left[left.size() - 1] == L'\\' || left[left.size() - 1] == L'/') return left + right;
    return left + L"\\" + right;
}

std::wstring normalized_for_compare(std::wstring value) {
    std::replace(value.begin(), value.end(), L'/', L'\\');
    while (value.size() > 1 && value[value.size() - 1] == L'\\') value.erase(value.size() - 1);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

bool is_same_or_child(const std::wstring& candidate, const std::wstring& root) {
    const std::wstring c = normalized_for_compare(candidate);
    const std::wstring r = normalized_for_compare(root);
    if (c == r) return true;
    return c.size() > r.size() && c.compare(0, r.size(), r) == 0 && c[r.size()] == L'\\';
}

std::string last_error_message(const char* context);

void validate_source_file_chain(const std::wstring& root, const std::wstring& relative) {
    DWORD attributes = GetFileAttributesW(root.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("source root changed or became a reparse point");

    std::wstring current = root;
    std::size_t begin = 0;
    while (begin <= relative.size()) {
        const std::size_t end = relative.find(L'\\', begin);
        const bool final_component = end == std::wstring::npos;
        const std::wstring component = relative.substr(
            begin, final_component ? std::wstring::npos : end - begin);
        if (component.empty() || component == L"." || component == L"..")
            throw std::runtime_error("discovered source path contains an unsafe component");
        current = join_path(current, component);
        attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
            throw std::runtime_error(last_error_message("GetFileAttributesW(source task)"));
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            throw std::runtime_error("source path changed to include a reparse point");
        if (!final_component && !(attributes & FILE_ATTRIBUTE_DIRECTORY))
            throw std::runtime_error("source path parent changed to a non-directory");
        if (final_component && (attributes & FILE_ATTRIBUTE_DIRECTORY))
            throw std::runtime_error("source file path changed to a directory");
        if (final_component) break;
        begin = end + 1;
    }
}

void ensure_directory(const std::wstring& path) {
    const std::wstring full = full_path(path);
    std::size_t start = 0;
    if (full.size() >= 3 && full[1] == L':') start = 3;
    else if (full.size() >= 2 && full[0] == L'\\' && full[1] == L'\\') {
        const std::size_t server = full.find(L'\\', 2);
        const std::size_t share = server == std::wstring::npos ? server : full.find(L'\\', server + 1);
        start = share == std::wstring::npos ? full.size() : share + 1;
    }
    for (std::size_t i = start; i <= full.size(); ++i) {
        if (i != full.size() && full[i] != L'\\' && full[i] != L'/') continue;
        const std::wstring partial = full.substr(0, i);
        if (partial.empty()) continue;
        if (!CreateDirectoryW(partial.c_str(), 0)) {
            const DWORD error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS) throw std::runtime_error("could not create output directory");
            const DWORD attrs = GetFileAttributesW(partial.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) ||
                (attrs & FILE_ATTRIBUTE_REPARSE_POINT))
                throw std::runtime_error("output path component is not a safe directory");
        }
    }
}

std::uint64_t file_size(const WIN32_FILE_ATTRIBUTE_DATA& data) {
    return (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

std::uint64_t file_size(const WIN32_FIND_DATAW& data) {
    return (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

std::uint64_t file_time(const FILETIME& time) {
    ULARGE_INTEGER value;
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

bool set_file_time(const std::wstring& path, std::uint64_t value, std::string& error) {
    HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) {
        error = last_error_message("CreateFileW(set backup timestamp)");
        return false;
    }
    ULARGE_INTEGER raw;
    raw.QuadPart = value;
    FILETIME timestamp;
    timestamp.dwLowDateTime = raw.LowPart;
    timestamp.dwHighDateTime = raw.HighPart;
    const bool ok = SetFileTime(file, 0, 0, &timestamp) != 0;
    if (!ok) error = last_error_message("SetFileTime(backup)");
    CloseHandle(file);
    return ok;
}

std::string file_type_for_path(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    const std::size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return "binary";
    std::wstring extension = path.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    const wchar_t* documents[] = {L".txt", L".doc", L".docx", L".pdf", L".rtf", L".docdata"};
    const wchar_t* images[] = {L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".imgdata"};
    const wchar_t* videos[] = {L".mp4", L".mov", L".avi", L".mkv", L".viddata"};
    const wchar_t* archives[] = {L".zip", L".7z", L".rar", L".tar", L".gz", L".arcdata"};
    for (std::size_t i = 0; i < sizeof(documents) / sizeof(documents[0]); ++i)
        if (extension == documents[i]) return "document";
    for (std::size_t i = 0; i < sizeof(images) / sizeof(images[0]); ++i)
        if (extension == images[i]) return "image";
    for (std::size_t i = 0; i < sizeof(videos) / sizeof(videos[0]); ++i)
        if (extension == videos[i]) return "video";
    for (std::size_t i = 0; i < sizeof(archives) / sizeof(archives[0]); ++i)
        if (extension == archives[i]) return "archive";
    return "binary";
}

std::string last_error_message(const char* context) {
    std::ostringstream message;
    message << context << " (Windows error " << static_cast<unsigned long>(GetLastError()) << ")";
    return message.str();
}

FILE* open_wide_file(const std::wstring& path, const wchar_t* mode) {
#ifdef _MSC_VER
    FILE* file = 0;
    if (_wfopen_s(&file, path.c_str(), mode) != 0) return 0;
    return file;
#else
    return _wfopen(path.c_str(), mode);
#endif
}

struct DirectoryEntry {
    std::wstring absolute_path;
    std::wstring relative_path;
};

class FindHandle {
public:
    explicit FindHandle(HANDLE handle) : handle_(handle) {}
    ~FindHandle() { if (handle_ != INVALID_HANDLE_VALUE) FindClose(handle_); }
    HANDLE get() const { return handle_; }
private:
    HANDLE handle_;
    FindHandle(const FindHandle&);
    FindHandle& operator=(const FindHandle&);
};

void scan_directory(const std::wstring& source_root, const DirectoryEntry& directory,
                    std::vector<DirectoryEntry>& child_directories,
                    std::vector<Task>& files) {
    std::wstring current = source_root;
    DWORD attributes = GetFileAttributesW(current.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("source directory is missing, unsafe, or not a directory");
    std::size_t begin = 0;
    while (begin < directory.relative_path.size()) {
        const std::size_t end = directory.relative_path.find(L'\\', begin);
        const std::wstring component = directory.relative_path.substr(
            begin, end == std::wstring::npos ? std::wstring::npos : end - begin);
        current = join_path(current, component);
        attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("source directory chain changed or contains a reparse point");
        if (end == std::wstring::npos) break;
        begin = end + 1;
    }
    const std::wstring pattern = join_path(directory.absolute_path, L"*");
    WIN32_FIND_DATAW data;
    HANDLE raw_handle = FindFirstFileW(pattern.c_str(), &data);
    if (raw_handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) return;
        throw std::runtime_error("directory discovery failed (Windows error " +
                                 std::to_string(error) + ")");
    }
    FindHandle handle(raw_handle);
    do {
        const std::wstring name(data.cFileName);
        if (name == L"." || name == L"..") continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        DirectoryEntry child;
        child.absolute_path = join_path(directory.absolute_path, name);
        child.relative_path = directory.relative_path.empty()
            ? name : join_path(directory.relative_path, name);
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            child_directories.push_back(child);
        } else {
            const std::string rel_utf8 = wide_to_utf8(child.relative_path);
            Task task;
            task.id = rel_utf8;
            task.relative_path = rel_utf8;
            task.size_bytes = file_size(data);
            task.estimated_cost = static_cast<double>(task.size_bytes ? task.size_bytes : 1);
            task.source_last_write_time_100ns = file_time(data.ftLastWriteTime);
            task.file_type = file_type_for_path(name);
            files.push_back(task);
        }
    } while (FindNextFileW(handle.get(), &data));
    const DWORD final_error = GetLastError();
    if (final_error != ERROR_NO_MORE_FILES)
        throw std::runtime_error("directory enumeration ended with an error (Windows error " +
                                 std::to_string(final_error) + ")");
}

struct DiscoveryResult {
    std::vector<Task> tasks;
    std::uint64_t elapsed_ns;
    std::size_t workers_used;
    DiscoveryResult() : elapsed_ns(0), workers_used(0) {}
};

DiscoveryResult discover_directory_tree(const std::wstring& source_root,
                                        std::size_t requested_workers) {
    if (requested_workers == 0)
        throw std::invalid_argument("discovery worker count must be greater than zero");
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    DiscoveryResult result;
    std::deque<DirectoryEntry> pending_directories;
    DirectoryEntry root;
    root.absolute_path = source_root;
    pending_directories.push_back(root);
    std::size_t outstanding = 1;
    bool done = false;
    std::exception_ptr failure;
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::size_t> directories_scanned(requested_workers, 0);
    std::vector<std::thread> workers;
    workers.reserve(requested_workers);

    const auto worker = [&](std::size_t worker_id) {
        while (true) {
            DirectoryEntry current;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [&]() { return done || !pending_directories.empty(); });
                if (done) return;
                current = pending_directories.front();
                pending_directories.pop_front();
            }
            std::vector<DirectoryEntry> children;
            std::vector<Task> files;
            try {
                scan_directory(source_root, current, children, files);
            } catch (...) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (!failure) failure = std::current_exception();
                    done = true;
                    pending_directories.clear();
                }
                changed.notify_all();
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (done) return;
                directories_scanned[worker_id] += 1;
                result.tasks.insert(result.tasks.end(), files.begin(), files.end());
                for (std::size_t i = 0; i < children.size(); ++i)
                    pending_directories.push_back(children[i]);
                outstanding += children.size();
                --outstanding;
                if (outstanding == 0) done = true;
            }
            changed.notify_all();
        }
    };

    try {
        for (std::size_t i = 0; i < requested_workers; ++i)
            workers.push_back(std::thread(worker, i));
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            done = true;
            failure = std::current_exception();
            pending_directories.clear();
        }
        changed.notify_all();
    }
    for (std::size_t i = 0; i < workers.size(); ++i) workers[i].join();
    if (failure) std::rethrow_exception(failure);

    for (std::size_t i = 0; i < directories_scanned.size(); ++i)
        if (directories_scanned[i]) ++result.workers_used;
    std::sort(result.tasks.begin(), result.tasks.end(), [](const Task& left, const Task& right) {
        return left.relative_path < right.relative_path;
    });
    result.elapsed_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count());
    return result;
}

wincrypto::FileIdentity identity_from_handle_info(const BY_HANDLE_FILE_INFORMATION& info) {
    wincrypto::FileIdentity identity;
    identity.volume_serial = info.dwVolumeSerialNumber;
    identity.file_index = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    identity.size_bytes = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    identity.last_write_time_100ns =
        (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
        info.ftLastWriteTime.dwLowDateTime;
    return identity;
}

bool same_file_identity(const wincrypto::FileIdentity& left,
                        const wincrypto::FileIdentity& right) {
    return left.volume_serial == right.volume_serial && left.file_index == right.file_index &&
           left.size_bytes == right.size_bytes &&
           left.last_write_time_100ns == right.last_write_time_100ns;
}

bool query_file_identity(const std::wstring& path, wincrypto::FileIdentity& identity,
                         std::string& error) {
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) {
        error = last_error_message("CreateFileW(source identity)");
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info;
    const bool inspected = GetFileInformationByHandle(file, &info) != 0;
    if (!inspected) error = last_error_message("GetFileInformationByHandle(source identity)");
    const bool closed = CloseHandle(file) != 0;
    if (!closed && inspected) error = last_error_message("CloseHandle(source identity)");
    if (!inspected || !closed) return false;
    identity = identity_from_handle_info(info);
    return true;
}

bool copy_file_stream(const std::wstring& source, const std::wstring& temporary,
                      std::uint64_t& bytes, std::string& error,
                      wincrypto::FileIdentity& copied_identity) {
    HANDLE input = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, 0);
    if (input == INVALID_HANDLE_VALUE) {
        error = last_error_message("CreateFileW(source copy)");
        return false;
    }
    BY_HANDLE_FILE_INFORMATION before_info;
    if (!GetFileInformationByHandle(input, &before_info)) {
        error = last_error_message("GetFileInformationByHandle(source copy)");
        CloseHandle(input);
        return false;
    }
    FILE* output = open_wide_file(temporary, L"wbx");
    if (!output) {
        CloseHandle(input);
        error = "could not create temporary backup file";
        return false;
    }
    std::vector<char> buffer(1024 * 1024);
    bool ok = true;
    bytes = 0;
    while (true) {
        DWORD read_count = 0;
        if (!ReadFile(input, &buffer[0], static_cast<DWORD>(buffer.size()), &read_count, 0)) {
            error = last_error_message("ReadFile(source copy)");
            ok = false;
            break;
        }
        if (read_count) {
            if (std::fwrite(&buffer[0], 1, static_cast<std::size_t>(read_count), output) != read_count) {
                error = "backup write failed";
                ok = false;
                break;
            }
            bytes += static_cast<std::uint64_t>(read_count);
        }
        if (read_count == 0) break;
    }
    BY_HANDLE_FILE_INFORMATION after_info;
    if (ok && !GetFileInformationByHandle(input, &after_info)) {
        error = last_error_message("GetFileInformationByHandle(source copy final)");
        ok = false;
    }
    if (ok) {
        const wincrypto::FileIdentity before_identity = identity_from_handle_info(before_info);
        const wincrypto::FileIdentity after_identity = identity_from_handle_info(after_info);
        if (!same_file_identity(before_identity, after_identity)) {
            error = "source identity or metadata changed while it was copied";
            ok = false;
        } else {
            copied_identity = after_identity;
        }
    }
    if (ok && std::fflush(output) != 0) {
        error = "could not flush temporary backup file";
        ok = false;
    }
    if (ok && _commit(_fileno(output)) != 0) {
        error = "could not flush temporary backup data to disk";
        ok = false;
    }
    if (!CloseHandle(input) && ok) {
        error = last_error_message("CloseHandle(source copy)");
        ok = false;
    }
    if (std::fclose(output) != 0 && ok) {
        error = "could not close temporary backup file";
        ok = false;
    }
    if (!ok) DeleteFileW(temporary.c_str());
    return ok;
}

std::string json_escape(const std::string& input) {
    std::ostringstream out;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(input[i]);
        if (ch == '"' || ch == '\\') out << '\\' << static_cast<char>(ch);
        else if (ch < 0x20) {
            out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned int>(ch)
                << std::dec << std::setfill(' ');
        } else out << static_cast<char>(ch);
    }
    return out.str();
}

std::string make_backup_id() {
    const std::uint64_t tick = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::ostringstream out;
    out << "backup-" << std::hex << GetCurrentProcessId() << "-" << tick;
    return out.str();
}

std::wstring unique_backup_temp_path(const std::wstring& destination) {
    static volatile LONG sequence = 0;
    const LONG value = InterlockedIncrement(&sequence);
    std::wostringstream suffix;
    suffix << L".ipbrs-backup-" << GetCurrentProcessId() << L"-" << GetTickCount64()
           << L"-" << value << L".tmp";
    return destination + suffix.str();
}

bool write_catalog(const std::wstring& catalog_path, const std::string& backup_id,
                   const std::string& source_root, const std::string& snapshot_root,
                   const std::vector<Task>& tasks, const std::vector<std::string>& digests,
                   std::string& error) {
    const std::wstring temp_path = catalog_path + L".tmp";
    FILE* file = open_wide_file(temp_path, L"wb");
    if (!file) {
        error = "could not create temporary catalog";
        return false;
    }
    std::ostringstream json;
    json << "{\n  \"schema_version\": 3,\n"
         << "  \"backup_id\": \"" << json_escape(backup_id) << "\",\n"
         << "  \"source_root\": \"" << json_escape(source_root) << "\",\n"
         << "  \"snapshot_root\": \"" << json_escape(snapshot_root) << "\",\n"
         << "  \"data_root\": \"files\",\n"
         << "  \"files\": [\n";
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        json << "    {\"relative_path\": \"" << json_escape(tasks[i].relative_path)
             << "\", \"size_bytes\": " << tasks[i].size_bytes
             << ", \"source_last_write_time_100ns\": " << tasks[i].source_last_write_time_100ns
             << ", \"file_type\": \"" << json_escape(tasks[i].file_type)
             << "\", \"backup_status\": \"COMPLETE\""
             << ", \"estimated_cost\": " << std::setprecision(17) << tasks[i].estimated_cost
             << ", \"sha256\": \"" << digests[i] << "\"}"
             << (i + 1 == tasks.size() ? "\n" : ",\n");
    }
    json << "  ]\n}\n";
    const std::string contents = json.str();
    const bool written = std::fwrite(contents.data(), 1, contents.size(), file) == contents.size();
    const bool flushed = written && std::fflush(file) == 0 && _commit(_fileno(file)) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!written || !flushed || !closed) {
        error = "could not write complete catalog";
        DeleteFileW(temp_path.c_str());
        return false;
    }
    if (!MoveFileExW(temp_path.c_str(), catalog_path.c_str(), MOVEFILE_WRITE_THROUGH)) {
        error = last_error_message("MoveFileExW(catalog)");
        DeleteFileW(temp_path.c_str());
        return false;
    }
    return true;
}

} // namespace

BackupSummary run_backup(const BackupConfig& config) {
    const std::chrono::steady_clock::time_point operation_started = std::chrono::steady_clock::now();
    BackupSummary summary;
    try {
        const std::wstring source_root = full_path(utf8_to_wide(config.source_root_utf8));
        const std::wstring backup_root = full_path(utf8_to_wide(config.backup_root_utf8));
        const DWORD source_attrs = GetFileAttributesW(source_root.c_str());
        if (source_attrs == INVALID_FILE_ATTRIBUTES || !(source_attrs & FILE_ATTRIBUTE_DIRECTORY))
            throw std::runtime_error("source root is not an accessible directory");
        if (source_attrs & FILE_ATTRIBUTE_REPARSE_POINT)
            throw std::runtime_error("source root cannot be a reparse point");
        if (is_same_or_child(backup_root, source_root) || is_same_or_child(source_root, backup_root))
            throw std::runtime_error("source and backup roots must not overlap");

        ensure_directory(backup_root);
        summary.backup_id = make_backup_id();
        std::wstring snapshot_root = join_path(backup_root, utf8_to_wide(summary.backup_id));
        if (!CreateDirectoryW(snapshot_root.c_str(), 0)) {
            summary.backup_id += "-1";
            snapshot_root = join_path(backup_root, utf8_to_wide(summary.backup_id));
            if (!CreateDirectoryW(snapshot_root.c_str(), 0))
                throw std::runtime_error("could not create a unique backup snapshot directory");
        }
        const std::wstring catalog_path = join_path(snapshot_root, L"catalog.json");
        const std::wstring data_root = join_path(snapshot_root, L"files");
        ensure_directory(data_root);
        summary.snapshot_path_utf8 = wide_to_utf8(snapshot_root);
        summary.catalog_path_utf8 = wide_to_utf8(catalog_path);

        const std::size_t discovery_workers =
            config.scheduler.policy == Policy::Sequential ? 1 : config.scheduler.worker_count;
        DiscoveryResult discovery = discover_directory_tree(source_root, discovery_workers);
        summary.discovery_elapsed_ns = discovery.elapsed_ns;
        summary.discovery_workers_used = discovery.workers_used;
        std::vector<Task>& tasks = discovery.tasks;
        std::unordered_map<std::string, std::size_t> task_index;
        std::vector<std::string> digests(tasks.size());
        for (std::size_t i = 0; i < tasks.size(); ++i) task_index[tasks[i].relative_path] = i;

        SchedulerConfig scheduler = config.scheduler;
        if (scheduler.policy != Policy::Sequential && scheduler.worker_count == 0)
            throw std::runtime_error("worker count must be greater than zero");
        TaskCallback callback = [&](const Task& task, std::size_t) -> TaskOutcome {
            TaskOutcome outcome;
            const std::wstring relative = utf8_to_wide(task.relative_path);
            const std::wstring source = join_path(source_root, relative);
            const std::wstring destination = join_path(data_root, relative);
            const std::wstring temporary = unique_backup_temp_path(destination);
            std::string source_digest;
            std::string destination_digest;
            std::string error;
            std::uint64_t source_bytes = 0;
            std::uint64_t copied_bytes = 0;
            wincrypto::FileIdentity hashed_identity;
            wincrypto::FileIdentity copied_identity;
            wincrypto::FileIdentity final_identity;
            WIN32_FILE_ATTRIBUTE_DATA before;
            WIN32_FILE_ATTRIBUTE_DATA after;
            try {
                validate_source_file_chain(source_root, relative);
            } catch (const std::exception& exception) {
                outcome.success = false;
                outcome.error_code = "SOURCE_PATH_UNSAFE";
                outcome.error_message = exception.what();
                return outcome;
            }
            if (!GetFileAttributesExW(source.c_str(), GetFileExInfoStandard, &before)) {
                outcome.success = false;
                outcome.error_code = "SOURCE_STAT";
                outcome.error_message = last_error_message("GetFileAttributesExW(source)");
                return outcome;
            }
            if (!wincrypto::sha256_file(source, source_digest, source_bytes, error, &hashed_identity)) {
                outcome.success = false;
                outcome.error_code = "SOURCE_SHA256";
                outcome.error_message = error;
                return outcome;
            }
            ensure_directory(destination.substr(0, destination.find_last_of(L"\\/")));
            if (!copy_file_stream(source, temporary, copied_bytes, error, copied_identity)) {
                outcome.success = false;
                outcome.error_code = "COPY";
                outcome.error_message = error;
                return outcome;
            }
            bool source_chain_valid = true;
            try {
                validate_source_file_chain(source_root, relative);
            } catch (const std::exception& exception) {
                source_chain_valid = false;
                error = exception.what();
            }
            if (!source_chain_valid ||
                !GetFileAttributesExW(source.c_str(), GetFileExInfoStandard, &after) ||
                (before.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                (after.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                file_size(before) != file_size(after) ||
                file_time(before.ftLastWriteTime) != file_time(after.ftLastWriteTime) ||
                file_time(before.ftLastWriteTime) != task.source_last_write_time_100ns ||
                hashed_identity.size_bytes != file_size(before) ||
                hashed_identity.last_write_time_100ns != file_time(before.ftLastWriteTime) ||
                hashed_identity.last_write_time_100ns != task.source_last_write_time_100ns ||
                !same_file_identity(hashed_identity, copied_identity) ||
                !query_file_identity(source, final_identity, error) ||
                !same_file_identity(hashed_identity, final_identity)) {
                DeleteFileW(temporary.c_str());
                outcome.success = false;
                outcome.error_code = "SOURCE_CHANGED";
                outcome.error_message = error.empty()
                    ? "source file identity or metadata changed while the backup was being copied"
                    : error;
                return outcome;
            }
            if (copied_bytes != source_bytes || copied_bytes != file_size(before) ||
                copied_bytes != task.size_bytes) {
                DeleteFileW(temporary.c_str());
                outcome.success = false;
                outcome.error_code = "COPY_SIZE_MISMATCH";
                outcome.error_message = "source size changed after discovery or copied byte count differs";
                return outcome;
            }
            std::uint64_t destination_bytes = 0;
            if (!wincrypto::sha256_file(temporary, destination_digest, destination_bytes, error) ||
                destination_bytes != copied_bytes || destination_digest != source_digest) {
                DeleteFileW(temporary.c_str());
                outcome.success = false;
                outcome.error_code = "VERIFY_TEMP";
                outcome.error_message = error.empty() ? "temporary backup SHA-256 mismatch" : error;
                return outcome;
            }
            if (!set_file_time(temporary, task.source_last_write_time_100ns, error)) {
                DeleteFileW(temporary.c_str());
                outcome.success = false;
                outcome.error_code = "SET_BACKUP_TIMESTAMP";
                outcome.error_message = error;
                return outcome;
            }
            if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
                DeleteFileW(temporary.c_str());
                outcome.success = false;
                outcome.error_code = "COMMIT_FILE";
                outcome.error_message = last_error_message("MoveFileExW(file)");
                return outcome;
            }
            std::uint64_t final_bytes = 0;
            if (!wincrypto::sha256_file(destination, destination_digest, final_bytes, error) ||
                final_bytes != copied_bytes || destination_digest != source_digest) {
                outcome.success = false;
                outcome.error_code = "VERIFY_FINAL";
                outcome.error_message = error.empty() ? "final backup SHA-256 mismatch" : error;
                return outcome;
            }
            const std::unordered_map<std::string, std::size_t>::const_iterator task_position =
                task_index.find(task.relative_path);
            if (task_position == task_index.end()) {
                outcome.success = false;
                outcome.error_code = "TASK_INDEX_MISSING";
                outcome.error_message = "scheduler returned a task absent from the backup manifest";
                return outcome;
            }
            digests[task_position->second] = source_digest;
            outcome.success = true;
            outcome.bytes_processed = copied_bytes;
            return outcome;
        };

        summary.scheduling = run_scheduler(tasks, scheduler, callback);
        bool all_ok = summary.scheduling.tasks.size() == tasks.size();
        for (std::size_t i = 0; i < summary.scheduling.tasks.size(); ++i)
            if (!summary.scheduling.tasks[i].success) all_ok = false;
        if (!all_ok) {
            summary.error_message = "one or more backup tasks failed; no catalog was committed";
            summary.total_elapsed_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - operation_started).count());
            return summary;
        }
        if (!write_catalog(catalog_path, summary.backup_id, wide_to_utf8(source_root),
                           wide_to_utf8(snapshot_root), tasks, digests, summary.error_message)) {
            summary.total_elapsed_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - operation_started).count());
            return summary;
        }
        summary.success = true;
    } catch (const std::exception& error) {
        summary.error_message = error.what();
    } catch (...) {
        summary.error_message = "unknown backup failure";
    }
    summary.total_elapsed_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - operation_started).count());
    return summary;
}

} // namespace ipbrs
