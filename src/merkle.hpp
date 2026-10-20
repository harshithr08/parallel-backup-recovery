#pragma once

#include "sha256.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ipbrs {

// A file is a sequence of fixed-size blocks; each block has an independent leaf
// hash, so blocks of one file can be hashed, copied and verified in parallel.
// leaf = SHA256(0x00 || block), node = SHA256(0x01 || left || right); an odd
// node is promoted unchanged. An empty file has one empty block.
Digest leaf_hash(const void* data, std::size_t size);
Digest merkle_root(const std::vector<Digest>& leaves);

inline std::uint32_t block_count(std::uint64_t size, std::uint32_t block_size) {
    return size == 0 ? 1u : static_cast<std::uint32_t>((size + block_size - 1) / block_size);
}

inline std::uint64_t block_length(std::uint64_t size, std::uint32_t block_size, std::uint32_t block) {
    const std::uint64_t offset = static_cast<std::uint64_t>(block) * block_size;
    return offset >= size ? 0 : std::min<std::uint64_t>(block_size, size - offset);
}

} // namespace ipbrs
