#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ipbrs {

using Digest = std::array<std::uint8_t, 32>;

// Portable streaming SHA-256 (FIPS 180-4).
class Sha256 {
public:
    Sha256();
    void update(const void* data, std::size_t size);
    Digest finish();

private:
    void compress(const std::uint8_t* block);
    std::uint32_t state_[8];
    std::uint8_t buffer_[64];
    std::size_t buffered_ = 0;
    std::uint64_t total_ = 0;
};

Digest sha256(const void* data, std::size_t size);
std::string to_hex(const Digest& digest);
bool parse_hex(const std::string& text, std::size_t offset, Digest& digest);

} // namespace ipbrs
