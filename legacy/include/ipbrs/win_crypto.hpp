#ifndef IPBRS_WIN_CRYPTO_HPP
#define IPBRS_WIN_CRYPTO_HPP

#include <cstdint>
#include <string>

namespace ipbrs {
namespace wincrypto {

struct FileIdentity {
    std::uint64_t volume_serial;
    std::uint64_t file_index;
    std::uint64_t size_bytes;
    std::uint64_t last_write_time_100ns;

    FileIdentity() : volume_serial(0), file_index(0), size_bytes(0), last_write_time_100ns(0) {}
};

bool sha256_file(const std::wstring& path, std::string& digest_hex,
                 std::uint64_t& bytes_read, std::string& error,
                 FileIdentity* identity = 0);

} // namespace wincrypto
} // namespace ipbrs

#endif
