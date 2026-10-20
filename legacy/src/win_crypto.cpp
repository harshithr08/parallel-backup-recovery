#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <sstream>
#include <vector>

#include "ipbrs/win_crypto.hpp"

#ifndef CALG_SHA_256
// ALG_CLASS_HASH | ALG_TYPE_ANY | ALG_SID_SHA_256 (CryptoAPI identifier).
#define CALG_SHA_256 (ALG_CLASS_HASH | ALG_TYPE_ANY | 12)
#endif

namespace ipbrs {
namespace wincrypto {
namespace {

std::string win_error(const char* operation, DWORD code) {
    std::ostringstream buffer;
    buffer << operation << " failed (Windows error " << static_cast<unsigned long>(code) << ")";
    return buffer.str();
}

std::string to_hex(const BYTE* bytes, DWORD length) {
    static const char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(static_cast<std::size_t>(length) * 2);
    for (DWORD i = 0; i < length; ++i) {
        result.push_back(digits[(bytes[i] >> 4) & 0x0f]);
        result.push_back(digits[bytes[i] & 0x0f]);
    }
    return result;
}

} // namespace

bool sha256_file(const std::wstring& path, std::string& digest_hex,
                 std::uint64_t& bytes_read, std::string& error,
                 FileIdentity* identity) {
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    HANDLE file = INVALID_HANDLE_VALUE;
    bool success = false;
    bytes_read = 0;

    if (!CryptAcquireContextW(&provider, 0, L"Microsoft Enhanced RSA and AES Cryptographic Provider",
                              PROV_RSA_AES, CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        error = win_error("CryptAcquireContextW", GetLastError());
        return false;
    }
    if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        error = win_error("CryptCreateHash(SHA-256)", GetLastError());
        CryptReleaseContext(provider, 0);
        return false;
    }
    file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, 0);
    if (file == INVALID_HANDLE_VALUE) {
        error = win_error("CreateFileW(SHA-256)", GetLastError());
        CryptDestroyHash(hash);
        CryptReleaseContext(provider, 0);
        return false;
    }

    std::vector<BYTE> buffer(1024 * 1024);
    BY_HANDLE_FILE_INFORMATION before;
    if (!GetFileInformationByHandle(file, &before)) {
        error = win_error("GetFileInformationByHandle(SHA-256)", GetLastError());
        goto cleanup;
    }

    while (true) {
        DWORD count = 0;
        if (!ReadFile(file, &buffer[0], static_cast<DWORD>(buffer.size()), &count, 0)) {
            error = win_error("ReadFile(SHA-256)", GetLastError());
            goto cleanup;
        }
        if (count > 0) {
            if (!CryptHashData(hash, &buffer[0], count, 0)) {
                error = win_error("CryptHashData", GetLastError());
                goto cleanup;
            }
            bytes_read += static_cast<std::uint64_t>(count);
        }
        if (count < buffer.size()) break;
    }

    {
        BYTE digest[64];
        DWORD digest_length = sizeof(digest);
        if (!CryptGetHashParam(hash, HP_HASHVAL, digest, &digest_length, 0)) {
            error = win_error("CryptGetHashParam(HP_HASHVAL)", GetLastError());
            goto cleanup;
        }
        digest_hex = to_hex(digest, digest_length);
    }
    {
        BY_HANDLE_FILE_INFORMATION after;
        if (!GetFileInformationByHandle(file, &after)) {
            error = win_error("GetFileInformationByHandle(SHA-256 final)", GetLastError());
            goto cleanup;
        }
        const std::uint64_t before_size =
            (static_cast<std::uint64_t>(before.nFileSizeHigh) << 32) | before.nFileSizeLow;
        const std::uint64_t after_size =
            (static_cast<std::uint64_t>(after.nFileSizeHigh) << 32) | after.nFileSizeLow;
        const std::uint64_t before_index =
            (static_cast<std::uint64_t>(before.nFileIndexHigh) << 32) | before.nFileIndexLow;
        const std::uint64_t after_index =
            (static_cast<std::uint64_t>(after.nFileIndexHigh) << 32) | after.nFileIndexLow;
        const std::uint64_t before_time =
            (static_cast<std::uint64_t>(before.ftLastWriteTime.dwHighDateTime) << 32) |
            before.ftLastWriteTime.dwLowDateTime;
        const std::uint64_t after_time =
            (static_cast<std::uint64_t>(after.ftLastWriteTime.dwHighDateTime) << 32) |
            after.ftLastWriteTime.dwLowDateTime;
        if (before.dwVolumeSerialNumber != after.dwVolumeSerialNumber || before_index != after_index ||
            before_size != after_size || before_time != after_time) {
            error = "file identity or metadata changed while SHA-256 was computed";
            goto cleanup;
        }
        if (identity) {
            identity->volume_serial = after.dwVolumeSerialNumber;
            identity->file_index = after_index;
            identity->size_bytes = after_size;
            identity->last_write_time_100ns = after_time;
        }
    }
    success = true;

cleanup:
    if (file != INVALID_HANDLE_VALUE && !CloseHandle(file) && success) {
        error = win_error("CloseHandle(SHA-256)", GetLastError());
        success = false;
    }
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    return success;
}

} // namespace wincrypto
} // namespace ipbrs
