#include "server/hash_ring.h"

namespace cpppools::server {
namespace hash_ring {

namespace {

inline uint32_t rotl32(uint32_t x, int r) {
    return (x << r) | (x >> (32 - r));
}

inline uint32_t fmix32(uint32_t h) {
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

inline uint32_t load_le32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

// murmur3 x86 32-bit reference 实现（Austin Appleby, public domain）。
// 输出视为**无符号** 32 位 —— spec §3.1 钉死（Python 侧必须 signed=False，
// 否则两端按符号性排序会建出不同的环，且失败是静默的）。
uint32_t murmur3_32(const void* data, std::size_t len, uint32_t seed) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    const std::size_t nblocks = len / 4;

    uint32_t h1 = seed;
    constexpr uint32_t c1 = 0xcc9e2d51u;
    constexpr uint32_t c2 = 0x1b873593u;

    // body：按 4 字节小端块处理。
    for (std::size_t i = 0; i < nblocks; ++i) {
        uint32_t k1 = load_le32(bytes + i * 4);
        k1 *= c1;
        k1 = rotl32(k1, 15);
        k1 *= c2;
        h1 ^= k1;
        h1 = rotl32(h1, 13);
        h1 = h1 * 5 + 0xe6546b64u;
    }

    // tail：剩余 0-3 字节。
    const unsigned char* tail = bytes + nblocks * 4;
    uint32_t k1 = 0;
    switch (len & 3u) {
        case 3:
            k1 ^= static_cast<uint32_t>(tail[2]) << 16;
            [[fallthrough]];
        case 2:
            k1 ^= static_cast<uint32_t>(tail[1]) << 8;
            [[fallthrough]];
        case 1:
            k1 ^= static_cast<uint32_t>(tail[0]);
            k1 *= c1;
            k1 = rotl32(k1, 15);
            k1 *= c2;
            h1 ^= k1;
            break;
        default:
            break;
    }

    // finalization：混入长度 + 雪崩。
    h1 ^= static_cast<uint32_t>(len);
    h1 = fmix32(h1);
    return h1;
}

std::string le32(uint32_t value) {
    std::string out(4, '\0');
    out[0] = static_cast<char>(value & 0xffu);
    out[1] = static_cast<char>((value >> 8) & 0xffu);
    out[2] = static_cast<char>((value >> 16) & 0xffu);
    out[3] = static_cast<char>((value >> 24) & 0xffu);
    return out;
}

std::string lp(const std::string& s) {
    std::string out = le32(static_cast<uint32_t>(s.size()));
    out.append(s);
    return out;
}

std::string vnode_key(const std::string& node_id, uint32_t vnode_index) {
    return lp(node_id) + le32(vnode_index);
}

std::string query_key(const std::string& datasource, const std::string& table,
                      const std::string& column, const std::string& value,
                      uint32_t attempt) {
    return lp(datasource) + lp(table) + lp(column) + lp(value) + le32(attempt);
}

}  // namespace hash_ring

using hash_ring::murmur3_32;
using hash_ring::vnode_key;

void HashRing::add_node(const std::string& node_id, int vnode_count) {
    if (vnode_count <= 0) {
        return;
    }
    for (uint32_t i = 0; i < static_cast<uint32_t>(vnode_count); ++i) {
        const uint32_t h = murmur3_32(vnode_key(node_id, i));
        const auto it = ring_.find(h);
        if (it == ring_.end()) {
            ring_.emplace(h, std::make_pair(node_id, i));
        } else {
            // 哈希冲突：按 (node_id, vnode_index) 字典序保留最小者。
            // 确定性 tie-break —— 不依赖插入顺序，两端建环结果一致。
            const auto candidate = std::make_pair(node_id, i);
            if (candidate < it->second) {
                it->second = candidate;
            }
        }
    }
}

std::string HashRing::owner(const std::string& hash_input) const {
    if (ring_.empty()) {
        return "";
    }
    const uint32_t h = murmur3_32(hash_input);
    auto it = ring_.lower_bound(h);
    if (it == ring_.end()) {
        it = ring_.begin();  // 越界回绕到环首（顺时针语义）
    }
    return it->second.first;
}

}  // namespace cpppools::server
