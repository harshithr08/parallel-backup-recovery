#include "merkle.hpp"

namespace ipbrs {

Digest leaf_hash(const void* data, std::size_t size) {
    Sha256 hasher;
    const std::uint8_t prefix = 0x00;
    hasher.update(&prefix, 1);
    hasher.update(data, size);
    return hasher.finish();
}

Digest merkle_root(const std::vector<Digest>& leaves) {
    if (leaves.empty()) return leaf_hash(nullptr, 0);
    std::vector<Digest> level = leaves;
    while (level.size() > 1) {
        std::vector<Digest> next;
        next.reserve((level.size() + 1) / 2);
        for (std::size_t i = 0; i + 1 < level.size(); i += 2) {
            Sha256 hasher;
            const std::uint8_t prefix = 0x01;
            hasher.update(&prefix, 1);
            hasher.update(level[i].data(), level[i].size());
            hasher.update(level[i + 1].data(), level[i + 1].size());
            next.push_back(hasher.finish());
        }
        if (level.size() % 2) next.push_back(level.back());
        level.swap(next);
    }
    return level.front();
}

} // namespace ipbrs
