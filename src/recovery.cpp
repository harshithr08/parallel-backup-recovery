#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <set>
#include <string>
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
    if (needed <= 0) throw std::runtime_error("input path or catalog text is not valid UTF-8");
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(),
                            static_cast<int>(value.size()), &result[0], needed) != needed)
        throw std::runtime_error("could not convert UTF-8 value to Windows text");
    return result;
}

std::wstring full_path(const std::wstring& path) {
    DWORD needed = GetFullPathNameW(path.c_str(), 0, 0, 0);
    if (!needed) throw std::runtime_error("could not resolve a full path");
    std::vector<wchar_t> buffer(static_cast<std::size_t>(needed) + 1);
    const DWORD written = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), &buffer[0], 0);
    if (!written || written >= buffer.size()) throw std::runtime_error("could not resolve a full path");
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

std::string windows_error(const char* operation) {
    std::ostringstream out;
    out << operation << " failed (Windows error " << static_cast<unsigned long>(GetLastError()) << ")";
    return out.str();
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
        error = windows_error("CreateFileW(set restore timestamp)");
        return false;
    }
    ULARGE_INTEGER raw;
    raw.QuadPart = value;
    FILETIME timestamp;
    timestamp.dwLowDateTime = raw.LowPart;
    timestamp.dwHighDateTime = raw.HighPart;
    const bool ok = SetFileTime(file, 0, 0, &timestamp) != 0;
    if (!ok) error = windows_error("SetFileTime(restore)");
    CloseHandle(file);
    return ok;
}

struct CatalogFile {
    std::string relative_path;
    std::uint64_t size_bytes;
    std::uint64_t source_last_write_time_100ns;
    std::string file_type;
    std::string backup_status;
    std::string sha256;
    bool has_source_time;
    bool has_file_type;
    bool has_backup_status;
    CatalogFile() : size_bytes(0), source_last_write_time_100ns(0), has_source_time(false),
                    has_file_type(false), has_backup_status(false) {}
};

struct Catalog {
    std::uint64_t schema_version;
    std::string backup_id;
    std::string source_root;
    std::string snapshot_root;
    std::string data_root;
    bool has_data_root;
    std::vector<CatalogFile> files;
    Catalog() : schema_version(0), has_data_root(false) {}
};

class JsonReader {
public:
    explicit JsonReader(const std::string& text) : text_(text), position_(0) {}

    Catalog read_catalog() {
        Catalog catalog;
        std::set<std::string> keys;
        bool has_files = false;
        expect('{');
        bool first = true;
        while (!consume('}')) {
            if (!first) expect(',');
            first = false;
            const std::string key = read_string();
            if (!keys.insert(key).second) fail("duplicate JSON catalog field");
            expect(':');
            if (key == "schema_version") catalog.schema_version = read_uint64();
            else if (key == "backup_id") catalog.backup_id = read_string();
            else if (key == "source_root") catalog.source_root = read_string();
            else if (key == "snapshot_root") catalog.snapshot_root = read_string();
            else if (key == "data_root") { catalog.data_root = read_string(); catalog.has_data_root = true; }
            else if (key == "files") { read_files(catalog.files); has_files = true; }
            else skip_value();
        }
        skip_space();
        if (position_ != text_.size()) fail("trailing content after JSON catalog");
        if (!has_files || (catalog.schema_version != 1 && catalog.schema_version != 2 &&
                           catalog.schema_version != 3) || catalog.backup_id.empty() ||
            catalog.source_root.empty() || catalog.snapshot_root.empty())
            fail("catalog is missing required fields or has an unsupported schema version");
        if (catalog.schema_version == 3 && (!catalog.has_data_root || catalog.data_root != "files"))
            fail("schema 3 catalog must declare data_root as 'files'");
        if (catalog.schema_version >= 2) {
            for (std::size_t i = 0; i < catalog.files.size(); ++i) {
                if (!catalog.files[i].has_source_time || !catalog.files[i].has_file_type ||
                    !catalog.files[i].has_backup_status || catalog.files[i].file_type.empty() ||
                    catalog.files[i].backup_status != "COMPLETE")
                    fail("schema 2 or later catalog entry is missing valid source metadata or completion status");
            }
        }
        return catalog;
    }

private:
    const std::string& text_;
    std::size_t position_;

    void fail(const char* message) const {
        std::ostringstream out;
        out << message << " at catalog byte " << position_;
        throw std::runtime_error(out.str());
    }

    void skip_space() {
        while (position_ < text_.size() &&
               (text_[position_] == ' ' || text_[position_] == '\t' ||
                text_[position_] == '\r' || text_[position_] == '\n')) ++position_;
    }

    bool consume(char expected) {
        skip_space();
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void expect(char expected) {
        if (!consume(expected)) fail("unexpected JSON token");
    }

    static void append_utf8(std::string& out, unsigned long codepoint) {
        if (codepoint <= 0x7f) out.push_back(static_cast<char>(codepoint));
        else if (codepoint <= 0x7ff) {
            out.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (codepoint <= 0xffff) {
            out.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        }
    }

    unsigned long read_hex4() {
        if (position_ + 4 > text_.size()) fail("incomplete JSON unicode escape");
        unsigned long value = 0;
        for (int i = 0; i < 4; ++i) {
            const char ch = text_[position_++];
            value <<= 4;
            if (ch >= '0' && ch <= '9') value |= static_cast<unsigned long>(ch - '0');
            else if (ch >= 'a' && ch <= 'f') value |= static_cast<unsigned long>(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') value |= static_cast<unsigned long>(ch - 'A' + 10);
            else fail("invalid JSON unicode escape");
        }
        return value;
    }

    std::string read_string() {
        expect('"');
        std::string out;
        while (position_ < text_.size()) {
            const unsigned char ch = static_cast<unsigned char>(text_[position_++]);
            if (ch == '"') return out;
            if (ch < 0x20) fail("unescaped control byte in JSON string");
            if (ch != '\\') { out.push_back(static_cast<char>(ch)); continue; }
            if (position_ >= text_.size()) fail("incomplete JSON escape");
            const char escaped = text_[position_++];
            switch (escaped) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned long codepoint = read_hex4();
                    if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                        if (position_ + 2 > text_.size() || text_[position_] != '\\' ||
                            text_[position_ + 1] != 'u') fail("invalid JSON surrogate pair");
                        position_ += 2;
                        const unsigned long low = read_hex4();
                        if (low < 0xdc00 || low > 0xdfff) fail("invalid JSON surrogate pair");
                        codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                    } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                        fail("unpaired JSON low surrogate");
                    }
                    append_utf8(out, codepoint);
                    break;
                }
                default: fail("invalid JSON escape");
            }
        }
        fail("unterminated JSON string");
        return out;
    }

    std::uint64_t read_uint64() {
        skip_space();
        const std::size_t begin = position_;
        while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
        if (begin == position_) fail("expected unsigned integer");
        const std::string digits = text_.substr(begin, position_ - begin);
        char* end = 0;
        errno = 0;
        const unsigned long long value = std::strtoull(digits.c_str(), &end, 10);
        if (!end || *end != '\0' || errno == ERANGE)
            fail("integer is outside the supported range");
        return static_cast<std::uint64_t>(value);
    }

    void skip_value() {
        skip_space();
        if (position_ >= text_.size()) fail("missing JSON value");
        if (text_[position_] == '"') { (void)read_string(); return; }
        if (text_[position_] == '{') {
            ++position_;
            if (consume('}')) return;
            do { (void)read_string(); expect(':'); skip_value(); } while (consume(','));
            expect('}');
            return;
        }
        if (text_[position_] == '[') {
            ++position_;
            if (consume(']')) return;
            do { skip_value(); } while (consume(','));
            expect(']');
            return;
        }
        const std::size_t begin = position_;
        while (position_ < text_.size() && text_[position_] != ',' && text_[position_] != ']' &&
               text_[position_] != '}' && text_[position_] != ' ' && text_[position_] != '\r' &&
               text_[position_] != '\n' && text_[position_] != '\t') ++position_;
        if (begin == position_) fail("invalid JSON value");
    }

    void read_files(std::vector<CatalogFile>& files) {
        expect('[');
        if (consume(']')) return;
        do {
            CatalogFile file;
            bool has_path = false, has_size = false, has_digest = false;
            std::set<std::string> keys;
            expect('{');
            bool first = true;
            while (!consume('}')) {
                if (!first) expect(',');
                first = false;
                const std::string key = read_string();
                if (!keys.insert(key).second) fail("duplicate JSON file field");
                expect(':');
                if (key == "relative_path") { file.relative_path = read_string(); has_path = true; }
                else if (key == "size_bytes") { file.size_bytes = read_uint64(); has_size = true; }
                else if (key == "source_last_write_time_100ns") {
                    file.source_last_write_time_100ns = read_uint64(); file.has_source_time = true;
                }
                else if (key == "file_type") { file.file_type = read_string(); file.has_file_type = true; }
                else if (key == "backup_status") { file.backup_status = read_string(); file.has_backup_status = true; }
                else if (key == "sha256") { file.sha256 = read_string(); has_digest = true; }
                else skip_value();
            }
            if (!has_path || !has_size || !has_digest) fail("catalog file entry is incomplete");
            if (file.sha256.size() != 64) fail("catalog SHA-256 digest has an invalid length");
            for (std::size_t i = 0; i < file.sha256.size(); ++i)
                if (!std::isxdigit(static_cast<unsigned char>(file.sha256[i])))
                    fail("catalog SHA-256 digest contains invalid characters");
            files.push_back(file);
        } while (consume(','));
        expect(']');
    }
};

Catalog load_catalog(const std::wstring& path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) throw std::runtime_error("could not open C++ backup catalog");
    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad()) throw std::runtime_error("could not read C++ backup catalog");
    return JsonReader(contents.str()).read_catalog();
}

std::wstring safe_relative_path(const std::string& relative_utf8) {
    std::wstring relative = utf8_to_wide(relative_utf8);
    std::replace(relative.begin(), relative.end(), L'/', L'\\');
    if (relative.empty() || relative[0] == L'\\' || relative.find(L':') != std::wstring::npos)
        throw std::runtime_error("catalog contains an absolute or drive-qualified relative path");
    std::size_t begin = 0;
    while (begin <= relative.size()) {
        const std::size_t end = relative.find(L'\\', begin);
        const std::wstring component = relative.substr(begin, end == std::wstring::npos ? end : end - begin);
        if (component.empty() || component == L"." || component == L"..")
            throw std::runtime_error("catalog contains an unsafe relative path component");
        if (end == std::wstring::npos) break;
        begin = end + 1;
    }
    return relative;
}

class UnsafeCatalogPathError : public std::runtime_error {
public:
    explicit UnsafeCatalogPathError(const std::string& message) : std::runtime_error(message) {}
};

std::wstring snapshot_data_root(const std::wstring& snapshot_root, const Catalog& catalog) {
    return catalog.schema_version >= 3 ? join_path(snapshot_root, L"files") : snapshot_root;
}

bool validate_existing_file_chain(const std::wstring& root, const std::wstring& relative) {
    std::wstring current = root;
    std::size_t begin = 0;
    while (begin <= relative.size()) {
        const std::size_t end = relative.find(L'\\', begin);
        const bool final_component = end == std::wstring::npos;
        const std::wstring component = relative.substr(
            begin, final_component ? std::wstring::npos : end - begin);
        current = join_path(current, component);
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
            throw std::runtime_error(windows_error("GetFileAttributesW(catalog path)"));
        }
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            throw UnsafeCatalogPathError("catalog path traverses a reparse point; refusing recovery");
        if (final_component) {
            if (attributes & FILE_ATTRIBUTE_DIRECTORY)
                throw UnsafeCatalogPathError("catalog file path resolves to a directory");
            return true;
        }
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
            throw UnsafeCatalogPathError("catalog path has a non-directory parent component");
        begin = end + 1;
    }
    return false;
}

void ensure_directory(const std::wstring& path) {
    const std::wstring full = full_path(path);
    std::size_t start = full.size() >= 3 && full[1] == L':' ? 3 : 0;
    for (std::size_t i = start; i <= full.size(); ++i) {
        if (i != full.size() && full[i] != L'\\' && full[i] != L'/') continue;
        const std::wstring partial = full.substr(0, i);
        if (partial.empty()) continue;
        if (!CreateDirectoryW(partial.c_str(), 0)) {
            const DWORD error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS) throw std::runtime_error(windows_error("CreateDirectoryW"));
            const DWORD attributes = GetFileAttributesW(partial.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
                throw std::runtime_error("restore destination contains a non-directory or reparse point");
        }
    }
}

class RecoveryFindHandle {
public:
    explicit RecoveryFindHandle(HANDLE handle) : handle_(handle) {}
    ~RecoveryFindHandle() { if (handle_ != INVALID_HANDLE_VALUE) FindClose(handle_); }
    HANDLE get() const { return handle_; }
private:
    HANDLE handle_;
    RecoveryFindHandle(const RecoveryFindHandle&);
    RecoveryFindHandle& operator=(const RecoveryFindHandle&);
};

void count_extra_files_in_directory(const std::wstring& absolute_directory,
                                    const std::wstring& relative_directory,
                                    const std::set<std::wstring>& catalog_paths,
                                    std::size_t& count) {
    struct PendingDirectory {
        std::wstring absolute_path;
        std::wstring relative_path;
    };
    std::vector<PendingDirectory> pending;
    PendingDirectory root;
    root.absolute_path = absolute_directory;
    root.relative_path = relative_directory;
    pending.push_back(root);
    while (!pending.empty()) {
        const PendingDirectory current = pending.back();
        pending.pop_back();
        WIN32_FIND_DATAW entry;
        const std::wstring search = join_path(current.absolute_path, L"*");
        RecoveryFindHandle handle(FindFirstFileW(search.c_str(), &entry));
        if (handle.get() == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND) continue;
            throw std::runtime_error(windows_error("FindFirstFileW(recovery classification)"));
        }
        do {
            const std::wstring name(entry.cFileName);
            if (name == L"." || name == L".." ||
                (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                continue;
            PendingDirectory child;
            child.absolute_path = join_path(current.absolute_path, name);
            child.relative_path = current.relative_path.empty()
                ? name : join_path(current.relative_path, name);
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                pending.push_back(child);
            } else if (catalog_paths.find(normalized_for_compare(child.relative_path)) == catalog_paths.end()) {
                ++count;
            }
        } while (FindNextFileW(handle.get(), &entry));
        const DWORD final_error = GetLastError();
        if (final_error != ERROR_NO_MORE_FILES)
            throw std::runtime_error("recovery directory enumeration ended with an error (Windows error " +
                                     std::to_string(final_error) + ")");
    }
}

bool copy_file_stream(const std::wstring& source, const std::wstring& temporary,
                      std::uint64_t& bytes, std::string& error) {
    FILE* input = 0;
    if (_wfopen_s(&input, source.c_str(), L"rb") != 0) input = 0;
    if (!input) { error = "could not open backup file"; return false; }
    FILE* output = 0;
    if (_wfopen_s(&output, temporary.c_str(), L"wbx") != 0) output = 0;
    if (!output) { std::fclose(input); error = "could not create a unique temporary restore file"; return false; }
    std::vector<char> buffer(1024 * 1024);
    bool ok = true;
    bytes = 0;
    while (true) {
        const std::size_t count = std::fread(&buffer[0], 1, buffer.size(), input);
        if (count && std::fwrite(&buffer[0], 1, count, output) != count) {
            error = "restore write failed"; ok = false; break;
        }
        bytes += static_cast<std::uint64_t>(count);
        if (count < buffer.size()) {
            if (std::ferror(input)) { error = "backup read failed"; ok = false; }
            break;
        }
    }
    if (ok && std::fflush(output) != 0) { error = "could not flush temporary restore file"; ok = false; }
    if (ok && _commit(_fileno(output)) != 0) { error = "could not flush restore data to disk"; ok = false; }
    if (std::fclose(input) != 0 && ok) { error = "could not close backup file"; ok = false; }
    if (std::fclose(output) != 0 && ok) { error = "could not close temporary restore file"; ok = false; }
    if (!ok) DeleteFileW(temporary.c_str());
    return ok;
}

std::wstring unique_temp_path(const std::wstring& destination) {
    static volatile LONG sequence = 0;
    const LONG value = InterlockedIncrement(&sequence);
    std::wostringstream suffix;
    suffix << L".ipbrs-restore-" << GetCurrentProcessId() << L"-" << GetTickCount64() << L"-" << value << L".tmp";
    return destination + suffix.str();
}

struct RecoveryEntry {
    CatalogFile file;
    std::wstring relative_path;
    bool target_existed;
    wincrypto::FileIdentity target_identity;
    std::string target_sha256;
    RecoveryEntry() : target_existed(false) {}
};

} // namespace

RecoverySummary run_recovery(const RecoveryConfig& config) {
    const std::chrono::steady_clock::time_point operation_started = std::chrono::steady_clock::now();
    RecoverySummary summary;
    try {
        const std::wstring source_root = full_path(utf8_to_wide(config.source_root_utf8));
        const std::wstring backup_root = full_path(utf8_to_wide(config.backup_root_utf8));
        const std::wstring catalog_path = full_path(utf8_to_wide(config.catalog_path_utf8));
        const DWORD source_attributes = GetFileAttributesW(source_root.c_str());
        const DWORD backup_attributes = GetFileAttributesW(backup_root.c_str());
        if (source_attributes == INVALID_FILE_ATTRIBUTES || !(source_attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (source_attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("source root must be an accessible, non-reparse directory");
        if (backup_attributes == INVALID_FILE_ATTRIBUTES || !(backup_attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (backup_attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("backup root must be an accessible, non-reparse directory");
        if (is_same_or_child(source_root, backup_root) || is_same_or_child(backup_root, source_root))
            throw std::runtime_error("source and backup roots must not overlap");
        const Catalog catalog = load_catalog(catalog_path);
        summary.backup_id = catalog.backup_id;
        const std::wstring recorded_source = full_path(utf8_to_wide(catalog.source_root));
        const std::wstring snapshot_root = full_path(utf8_to_wide(catalog.snapshot_root));
        if (normalized_for_compare(source_root) != normalized_for_compare(recorded_source))
            throw std::runtime_error("source root does not match the root recorded in the catalog");
        if (!is_same_or_child(snapshot_root, backup_root) ||
            normalized_for_compare(snapshot_root) == normalized_for_compare(backup_root))
            throw std::runtime_error("catalog snapshot is outside the supplied backup root");
        if (normalized_for_compare(catalog_path) !=
            normalized_for_compare(join_path(snapshot_root, L"catalog.json")))
            throw std::runtime_error("catalog path does not match the snapshot recorded inside it");
        const DWORD snapshot_attributes = GetFileAttributesW(snapshot_root.c_str());
        if (snapshot_attributes == INVALID_FILE_ATTRIBUTES || !(snapshot_attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (snapshot_attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("catalog snapshot directory is missing or unsafe");
        const std::wstring data_root = snapshot_data_root(snapshot_root, catalog);
        if (catalog.schema_version >= 3) {
            const DWORD data_attributes = GetFileAttributesW(data_root.c_str());
            if (data_attributes == INVALID_FILE_ATTRIBUTES || !(data_attributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (data_attributes & FILE_ATTRIBUTE_REPARSE_POINT))
                throw std::runtime_error("catalog payload directory is missing or unsafe");
        }

        summary.catalog_files = catalog.files.size();
        std::vector<RecoveryEntry> restore;
        std::set<std::wstring> seen_paths;
        for (std::size_t i = 0; i < catalog.files.size(); ++i) {
            RecoveryEntry entry;
            entry.file = catalog.files[i];
            entry.relative_path = safe_relative_path(entry.file.relative_path);
            if (!seen_paths.insert(normalized_for_compare(entry.relative_path)).second)
                throw std::runtime_error("catalog contains duplicate relative paths");
            const std::wstring destination = full_path(join_path(source_root, entry.relative_path));
            if (!is_same_or_child(destination, source_root) ||
                normalized_for_compare(destination) == normalized_for_compare(source_root))
                throw std::runtime_error("catalog path escapes the source root");
            bool source_file_exists = false;
            try {
                source_file_exists = validate_existing_file_chain(source_root, entry.relative_path);
            } catch (const std::exception&) {
                // An unreadable or unsafe source path cannot be safely classified
                // or replaced. Keep it visible as a failed recovery item.
                ++summary.files_inaccessible;
                ++summary.files_failed;
                continue;
            }
            if (!source_file_exists) ++summary.files_missing;

            entry.target_existed = source_file_exists;
            bool damaged = config.full_recovery || !source_file_exists;
            if (!damaged && entry.file.has_source_time) {
                WIN32_FILE_ATTRIBUTE_DATA current;
                if (!GetFileAttributesExW(destination.c_str(), GetFileExInfoStandard, &current)) {
                    ++summary.files_inaccessible;
                    ++summary.files_failed;
                    continue;
                }
                if (file_time(current.ftLastWriteTime) != entry.file.source_last_write_time_100ns)
                    damaged = true;
            }
            if (source_file_exists) {
                std::string digest, error;
                std::uint64_t bytes = 0;
                if (!wincrypto::sha256_file(destination, digest, bytes, error, &entry.target_identity)) {
                    ++summary.files_inaccessible;
                    ++summary.files_failed;
                    continue;
                }
                entry.target_sha256 = digest;
                if (entry.file.has_source_time &&
                    entry.target_identity.last_write_time_100ns != entry.file.source_last_write_time_100ns)
                    damaged = true;
                if (bytes != entry.file.size_bytes || digest != entry.file.sha256) damaged = true;
            }
            if (damaged) {
                if (source_file_exists && !config.full_recovery) ++summary.files_modified;
                restore.push_back(entry);
            }
            else {
                ++summary.files_skipped;
                summary.bytes_avoided += entry.file.size_bytes;
            }
        }
        count_extra_files_in_directory(source_root, L"", seen_paths, summary.files_extra);
        summary.restore_tasks = restore.size();
        std::vector<Task> tasks;
        tasks.reserve(restore.size());
        for (std::size_t i = 0; i < restore.size(); ++i) {
            Task task;
            task.id = restore[i].file.relative_path;
            task.relative_path = restore[i].file.relative_path;
            task.size_bytes = restore[i].file.size_bytes;
            task.estimated_cost = static_cast<double>(task.size_bytes ? task.size_bytes : 1);
            tasks.push_back(task);
        }
        std::unordered_map<std::string, std::size_t> indexes;
        for (std::size_t i = 0; i < restore.size(); ++i) indexes[restore[i].file.relative_path] = i;
        const TaskCallback callback = [&](const Task& task, std::size_t) -> TaskOutcome {
            TaskOutcome outcome;
            const std::unordered_map<std::string, std::size_t>::const_iterator found = indexes.find(task.id);
            if (found == indexes.end()) {
                outcome.success = false; outcome.error_code = "TASK_NOT_IN_CATALOG";
                outcome.error_message = "scheduler returned a task absent from the catalog";
                return outcome;
            }
            const RecoveryEntry& entry = restore[found->second];
            const std::wstring relative = entry.relative_path;
            const std::wstring backup_file = full_path(join_path(data_root, relative));
            const std::wstring destination = full_path(join_path(source_root, relative));
            std::string error, digest;
            std::uint64_t bytes = 0;
            if (!is_same_or_child(backup_file, data_root) ||
                normalized_for_compare(backup_file) == normalized_for_compare(data_root)) {
                outcome.success = false; outcome.error_code = "UNSAFE_BACKUP_PATH";
                outcome.error_message = "catalog path escapes the backup snapshot"; return outcome;
            }
            try {
                if (!validate_existing_file_chain(data_root, relative)) {
                    outcome.success = false; outcome.error_code = "BACKUP_FILE_MISSING";
                    outcome.error_message = "catalog backup file is missing"; return outcome;
                }
            } catch (const std::exception& exception) {
                outcome.success = false; outcome.error_code = "UNSAFE_BACKUP_PATH";
                outcome.error_message = exception.what(); return outcome;
            }
            wincrypto::FileIdentity verified_backup_identity;
            if (!wincrypto::sha256_file(backup_file, digest, bytes, error, &verified_backup_identity) ||
                bytes != entry.file.size_bytes || digest != entry.file.sha256) {
                outcome.success = false; outcome.error_code = "BACKUP_VERIFY";
                outcome.error_message = error.empty() ? "backup file does not match catalog SHA-256" : error;
                return outcome;
            }
            if (verified_backup_identity.size_bytes != entry.file.size_bytes) {
                outcome.success = false; outcome.error_code = "BACKUP_VERIFY";
                outcome.error_message = "backup file identity size differs from catalog";
                return outcome;
            }
            const std::size_t separator = destination.find_last_of(L"\\/");
            if (separator == std::wstring::npos) {
                outcome.success = false; outcome.error_code = "DESTINATION_PATH";
                outcome.error_message = "could not determine restore destination directory"; return outcome;
            }
            try { ensure_directory(destination.substr(0, separator)); }
            catch (const std::exception& exception) {
                outcome.success = false; outcome.error_code = "DESTINATION_DIRECTORY";
                outcome.error_message = exception.what(); return outcome;
            }
            const std::wstring temporary = unique_temp_path(destination);
            if (!copy_file_stream(backup_file, temporary, bytes, error)) {
                outcome.success = false; outcome.error_code = "RESTORE_COPY";
                outcome.error_message = error; return outcome;
            }
            wincrypto::FileIdentity copied_backup_identity;
            if (!wincrypto::sha256_file(backup_file, digest, bytes, error, &copied_backup_identity) ||
                bytes != entry.file.size_bytes || digest != entry.file.sha256 ||
                copied_backup_identity.volume_serial != verified_backup_identity.volume_serial ||
                copied_backup_identity.file_index != verified_backup_identity.file_index ||
                copied_backup_identity.size_bytes != verified_backup_identity.size_bytes ||
                copied_backup_identity.last_write_time_100ns != verified_backup_identity.last_write_time_100ns) {
                DeleteFileW(temporary.c_str());
                outcome.success = false; outcome.error_code = "BACKUP_CHANGED";
                outcome.error_message = error.empty() ? "backup file identity changed during recovery copy" : error;
                return outcome;
            }
            if (!wincrypto::sha256_file(temporary, digest, bytes, error) ||
                bytes != entry.file.size_bytes || digest != entry.file.sha256) {
                DeleteFileW(temporary.c_str());
                outcome.success = false; outcome.error_code = "RESTORE_VERIFY_TEMP";
                outcome.error_message = error.empty() ? "temporary restore failed catalog verification" : error;
                return outcome;
            }
            if (entry.file.has_source_time &&
                !set_file_time(temporary, entry.file.source_last_write_time_100ns, error)) {
                DeleteFileW(temporary.c_str());
                outcome.success = false; outcome.error_code = "RESTORE_TIMESTAMP";
                outcome.error_message = error; return outcome;
            }
            const DWORD current_attributes = GetFileAttributesW(destination.c_str());
            if (current_attributes != INVALID_FILE_ATTRIBUTES &&
                ((current_attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                 (current_attributes & FILE_ATTRIBUTE_DIRECTORY))) {
                DeleteFileW(temporary.c_str());
                outcome.success = false; outcome.error_code = "UNSAFE_DESTINATION";
                outcome.error_message = "refusing to replace a directory or reparse point"; return outcome;
            }
            bool destination_changed = false;
            try {
                const std::wstring temporary_relative =
                    relative + temporary.substr(destination.size());
                const std::wstring resolved_destination = full_path(destination);
                const std::wstring resolved_temporary = full_path(temporary);
                if (!is_same_or_child(resolved_destination, source_root) ||
                    !is_same_or_child(resolved_temporary, source_root) ||
                    !validate_existing_file_chain(source_root, temporary_relative))
                    throw UnsafeCatalogPathError("restore path changed or escaped the source root before commit");
                // A missing final destination is expected for deleted files;
                // existing parents and any existing target must still be safe.
                (void)validate_existing_file_chain(source_root, relative);
                if (entry.target_existed) {
                    wincrypto::FileIdentity current_identity;
                    std::string current_digest, current_error;
                    std::uint64_t current_bytes = 0;
                    if (!validate_existing_file_chain(source_root, relative) ||
                        !wincrypto::sha256_file(destination, current_digest, current_bytes,
                                                current_error, &current_identity) ||
                        current_bytes != entry.target_identity.size_bytes ||
                        current_digest != entry.target_sha256 ||
                        current_identity.volume_serial != entry.target_identity.volume_serial ||
                        current_identity.file_index != entry.target_identity.file_index ||
                        current_identity.size_bytes != entry.target_identity.size_bytes ||
                        current_identity.last_write_time_100ns != entry.target_identity.last_write_time_100ns) {
                        destination_changed = true;
                        throw std::runtime_error("destination changed after recovery classification; refusing to overwrite it");
                    }
                } else if (validate_existing_file_chain(source_root, relative)) {
                    destination_changed = true;
                    throw std::runtime_error("destination appeared after recovery classification; refusing to overwrite it");
                }
            } catch (const std::exception& exception) {
                DeleteFileW(temporary.c_str());
                outcome.success = false;
                outcome.error_code = destination_changed ? "DESTINATION_CHANGED" : "UNSAFE_DESTINATION";
                outcome.error_message = exception.what();
                return outcome;
            }
            if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                error = windows_error("MoveFileExW(restore)");
                DeleteFileW(temporary.c_str());
                outcome.success = false; outcome.error_code = "RESTORE_COMMIT";
                outcome.error_message = error; return outcome;
            }
            try {
                if (!validate_existing_file_chain(source_root, relative))
                    throw UnsafeCatalogPathError("restored destination disappeared before final verification");
            } catch (const std::exception& exception) {
                outcome.success = false;
                outcome.error_code = "UNSAFE_DESTINATION";
                outcome.error_message = exception.what();
                return outcome;
            }
            if (!wincrypto::sha256_file(destination, digest, bytes, error) ||
                bytes != entry.file.size_bytes || digest != entry.file.sha256) {
                outcome.success = false; outcome.error_code = "RESTORE_VERIFY_FINAL";
                outcome.error_message = error.empty() ? "final restored file failed catalog verification" : error;
                return outcome;
            }
            if (entry.file.has_source_time) {
                WIN32_FILE_ATTRIBUTE_DATA restored_attributes;
                if (!GetFileAttributesExW(destination.c_str(), GetFileExInfoStandard, &restored_attributes) ||
                    file_time(restored_attributes.ftLastWriteTime) != entry.file.source_last_write_time_100ns) {
                    outcome.success = false;
                    outcome.error_code = "RESTORE_VERIFY_TIMESTAMP";
                    outcome.error_message = "final restored file timestamp differs from catalog";
                    return outcome;
                }
            }
            outcome.success = true;
            outcome.bytes_processed = bytes;
            return outcome;
        };
        summary.scheduling = run_scheduler(tasks, config.scheduler, callback);
        for (std::size_t i = 0; i < summary.scheduling.tasks.size(); ++i) {
            if (summary.scheduling.tasks[i].success) {
                ++summary.files_restored;
                summary.bytes_restored += summary.scheduling.tasks[i].bytes_processed;
            } else {
                ++summary.files_failed;
            }
        }
        const bool all_tasks_accounted_for = summary.scheduling.tasks.size() == tasks.size();
        if (summary.files_failed == 0 && all_tasks_accounted_for) {
            summary.status = "COMPLETE";
            summary.success = true;
        } else {
            summary.status = "COMPLETE_WITH_FAILURES";
            summary.success = false;
            summary.error_message = "one or more recovery items failed";
        }
    } catch (const std::exception& error) {
        summary.error_message = error.what();
    } catch (...) {
        summary.error_message = "unknown recovery failure";
    }
    summary.total_elapsed_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - operation_started).count());
    return summary;
}

VerificationSummary verify_backup(const std::string& backup_root_utf8,
                                  const std::string& catalog_path_utf8,
                                  const SchedulerConfig& scheduler) {
    const std::chrono::steady_clock::time_point operation_started = std::chrono::steady_clock::now();
    VerificationSummary summary;
    try {
        const std::wstring backup_root = full_path(utf8_to_wide(backup_root_utf8));
        const std::wstring catalog_path = full_path(utf8_to_wide(catalog_path_utf8));
        const DWORD backup_attributes = GetFileAttributesW(backup_root.c_str());
        if (backup_attributes == INVALID_FILE_ATTRIBUTES ||
            !(backup_attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (backup_attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("backup root must be an accessible, non-reparse directory");
        const Catalog catalog = load_catalog(catalog_path);
        summary.backup_id = catalog.backup_id;
        const std::wstring snapshot_root = full_path(utf8_to_wide(catalog.snapshot_root));
        if (!is_same_or_child(snapshot_root, backup_root) ||
            normalized_for_compare(snapshot_root) == normalized_for_compare(backup_root))
            throw std::runtime_error("catalog snapshot is outside the supplied backup root");
        if (normalized_for_compare(catalog_path) !=
            normalized_for_compare(join_path(snapshot_root, L"catalog.json")))
            throw std::runtime_error("catalog path does not match the snapshot recorded inside it");
        const DWORD snapshot_attributes = GetFileAttributesW(snapshot_root.c_str());
        if (snapshot_attributes == INVALID_FILE_ATTRIBUTES ||
            !(snapshot_attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (snapshot_attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("catalog snapshot directory is missing or unsafe");
        const std::wstring data_root = snapshot_data_root(snapshot_root, catalog);
        if (catalog.schema_version >= 3) {
            const DWORD data_attributes = GetFileAttributesW(data_root.c_str());
            if (data_attributes == INVALID_FILE_ATTRIBUTES || !(data_attributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (data_attributes & FILE_ATTRIBUTE_REPARSE_POINT))
                throw std::runtime_error("catalog payload directory is missing or unsafe");
        }

        summary.catalog_files = catalog.files.size();
        std::set<std::wstring> seen_paths;
        std::vector<Task> tasks;
        tasks.reserve(catalog.files.size());
        std::unordered_map<std::string, std::size_t> indexes;
        for (std::size_t i = 0; i < catalog.files.size(); ++i) {
            const std::wstring relative = safe_relative_path(catalog.files[i].relative_path);
            if (!seen_paths.insert(normalized_for_compare(relative)).second)
                throw std::runtime_error("catalog contains duplicate relative paths");
            const std::wstring full_file = full_path(join_path(data_root, relative));
            if (!is_same_or_child(full_file, data_root) ||
                normalized_for_compare(full_file) == normalized_for_compare(data_root))
                throw std::runtime_error("catalog path escapes its snapshot");
            Task task;
            task.id = catalog.files[i].relative_path;
            task.relative_path = catalog.files[i].relative_path;
            task.size_bytes = catalog.files[i].size_bytes;
            task.estimated_cost = static_cast<double>(task.size_bytes ? task.size_bytes : 1);
            indexes[task.id] = tasks.size();
            tasks.push_back(task);
        }
        const TaskCallback callback = [&](const Task& task, std::size_t) -> TaskOutcome {
            TaskOutcome outcome;
            const std::unordered_map<std::string, std::size_t>::const_iterator found = indexes.find(task.id);
            if (found == indexes.end()) {
                outcome.success = false;
                outcome.error_code = "TASK_NOT_IN_CATALOG";
                outcome.error_message = "scheduler returned a task absent from the catalog";
                return outcome;
            }
            try {
                const std::wstring relative = safe_relative_path(task.relative_path);
                const std::wstring full_file = full_path(join_path(data_root, relative));
                if (!is_same_or_child(full_file, data_root) ||
                    normalized_for_compare(full_file) == normalized_for_compare(data_root)) {
                    outcome.success = false;
                    outcome.error_code = "UNSAFE_PATH";
                    outcome.error_message = "catalog path escapes its snapshot";
                    return outcome;
                }
                bool file_exists = false;
                try {
                    file_exists = validate_existing_file_chain(data_root, relative);
                } catch (const UnsafeCatalogPathError& error) {
                    outcome.success = false;
                    outcome.error_code = "UNSAFE_PATH";
                    outcome.error_message = error.what();
                    return outcome;
                } catch (const std::exception& error) {
                    outcome.success = false;
                    outcome.error_code = "ACCESS_ERROR";
                    outcome.error_message = error.what();
                    return outcome;
                }
                if (!file_exists) {
                    outcome.success = false;
                    outcome.error_code = "MISSING_FILE";
                    outcome.error_message = "catalog backup file is missing";
                    return outcome;
                }
                std::string digest, error;
                std::uint64_t bytes = 0;
                if (!wincrypto::sha256_file(full_file, digest, bytes, error)) {
                    outcome.success = false;
                    outcome.error_code = "ACCESS_ERROR";
                    outcome.error_message = error;
                    return outcome;
                }
                const CatalogFile& expected = catalog.files[found->second];
                if (bytes != expected.size_bytes) {
                    outcome.success = false;
                    outcome.error_code = "SIZE_MISMATCH";
                    outcome.error_message = "backup file size differs from catalog";
                    return outcome;
                }
                if (digest != expected.sha256) {
                    outcome.success = false;
                    outcome.error_code = "DIGEST_MISMATCH";
                    outcome.error_message = "backup file SHA-256 differs from catalog";
                    return outcome;
                }
                if (expected.has_source_time) {
                    WIN32_FILE_ATTRIBUTE_DATA attributes;
                    if (!GetFileAttributesExW(full_file.c_str(), GetFileExInfoStandard, &attributes)) {
                        outcome.success = false;
                        outcome.error_code = "ACCESS_ERROR";
                        outcome.error_message = windows_error("GetFileAttributesExW(verify timestamp)");
                        return outcome;
                    }
                    if (file_time(attributes.ftLastWriteTime) != expected.source_last_write_time_100ns) {
                        outcome.success = false;
                        outcome.error_code = "TIMESTAMP_MISMATCH";
                        outcome.error_message = "backup file modification time differs from catalog";
                        return outcome;
                    }
                }
                outcome.success = true;
                outcome.bytes_processed = bytes;
            } catch (const std::exception& error) {
                outcome.success = false;
                outcome.error_code = "UNSAFE_PATH";
                outcome.error_message = error.what();
            } catch (...) {
                outcome.success = false;
                outcome.error_code = "ACCESS_ERROR";
                outcome.error_message = "unknown file verification failure";
            }
            return outcome;
        };
        summary.scheduling = run_scheduler(tasks, scheduler, callback);
        summary.files = summary.scheduling.tasks;
        for (std::size_t i = 0; i < summary.files.size(); ++i) {
            if (summary.files[i].success) ++summary.files_verified;
            else {
                ++summary.files_failed;
                const std::string& code = summary.files[i].error_code;
                if (code == "MISSING_FILE") ++summary.files_missing;
                else if (code == "SIZE_MISMATCH") ++summary.files_size_mismatch;
                else if (code == "TIMESTAMP_MISMATCH") ++summary.files_timestamp_mismatch;
                else if (code == "DIGEST_MISMATCH") ++summary.files_digest_mismatch;
                else if (code == "ACCESS_ERROR") ++summary.files_inaccessible;
                else ++summary.files_unsafe;
            }
        }
        summary.success = summary.files_failed == 0 &&
                          summary.files_verified == summary.catalog_files;
    } catch (const std::exception& error) {
        summary.error_message = error.what();
    } catch (...) {
        summary.error_message = "unknown backup verification failure";
    }
    summary.total_elapsed_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - operation_started).count());
    return summary;
}
} // namespace ipbrs
