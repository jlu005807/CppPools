#pragma once

#include <cstdint>
#include <cstddef>
#include <map>
#include <string>
#include <utility>

namespace cpppools::server {

// 一致性哈希环与配套哈希工具（cluster-cache spec §3.1/§3.2）。
//
// 关键口径（与 Python SDK 必须逐字节一致，由黄金向量测试钉住）：
// - murmur3 32-bit，seed = 0，**无符号** 32 位输出；
// - 字符串一律 UTF-8；哈希输入用 **LP 长度前缀编码**（禁止分隔符拼接）；
// - 环按哈希值升序，查询取顺时针第一个 vnode，越界回绕到首元素；
// - vnode 哈希冲突按 (node_id, vnode_index) 字典序确定性 tie-break，
//   不依赖插入顺序（否则两端建出的环可能不同）。
namespace hash_ring {

// murmur3 x86 32-bit（reference 实现，与 python mmh3.hash(seed=0, signed=False) 一致）。
uint32_t murmur3_32(const void* data, std::size_t len, uint32_t seed);
inline uint32_t murmur3_32(const std::string& data, uint32_t seed = 0) {
    return murmur3_32(data.data(), data.size(), seed);
}

// LE32：小端无符号 4 字节。
std::string le32(uint32_t value);

// LP：长度前缀编码 LE32(len(utf8(s))) ‖ utf8(s)。
std::string lp(const std::string& s);

// vnode 哈希输入：LP(node_id) ‖ LE32(vnode_index)。
std::string vnode_key(const std::string& node_id, uint32_t vnode_index);

// 查询哈希输入：LP(datasource) ‖ LP(table) ‖ LP(column) ‖ LP(value) ‖ LE32(attempt)。
std::string query_key(const std::string& datasource, const std::string& table,
                      const std::string& column, const std::string& value,
                      uint32_t attempt);

}  // namespace hash_ring

// 一致性哈希环。
//
// 建环期使用（启动时一次性构建，构建后只读）—— 故不加锁；
// 运行期若需并发读，由持有方保证"构建完成后不再修改"（与 spec §7.2
// 客户端"构建新环后原子替换引用"的用法一致）。
class HashRing {
public:
    // 添加节点：生成 vnode_count 个 vnode 并入环。vnode_count <= 0 时忽略。
    void add_node(const std::string& node_id, int vnode_count);

    // 归属判定：哈希输入的 owner 节点名。空环返回空串（调用方 fail-fast）。
    // hash_input 是**已编码**的字节串（vnode_key/query_key/lp 的产物）。
    std::string owner(const std::string& hash_input) const;

    bool empty() const { return ring_.empty(); }
    std::size_t vnode_count() const { return ring_.size(); }

private:
    // 哈希值 → (node_id, vnode_index)。std::map 天然升序；
    // 同一哈希值冲突时保留 (node_id, vnode_index) 字典序最小者（确定性）。
    std::map<uint32_t, std::pair<std::string, uint32_t>> ring_;
};

}  // namespace cpppools::server
