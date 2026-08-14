// 一致性哈希环与 murmur3_32 的跨语言黄金向量测试（cluster-cache spec §3.1/§3.2）。
//
// 黄金向量由 Python mmh3（seed=0, signed=False）+ 与 §3.1 相同的 LP 编码计算，
// 在 spec §3.2 冻结（V1-V8）。两端测试引用同一组值 ——
// 任何一端的实现漂移都会撞红。

#include "server/hash_ring.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

using cpppools::server::HashRing;
using cpppools::server::hash_ring::le32;
using cpppools::server::hash_ring::lp;
using cpppools::server::hash_ring::murmur3_32;
using cpppools::server::hash_ring::query_key;
using cpppools::server::hash_ring::vnode_key;

namespace {

// spec §3.2 黄金向量（冻结值，禁止随实现改动）。
constexpr uint32_t kGoldenV1 = 1664544994u;   // vnode(node-a, 0)
constexpr uint32_t kGoldenV2 = 3870970589u;   // vnode(节点-b, 99)（非 ASCII）
constexpr uint32_t kGoldenV3 = 931942866u;    // query(main,users,id,42,attempt=0)
constexpr uint32_t kGoldenV4 = 2309089622u;   // query(main,users,tag,a/b,attempt=0)
constexpr uint32_t kGoldenV5 = 4213239566u;   // query(main,users,id,-7,attempt=3)
constexpr uint32_t kGoldenEmpty = 0u;         // murmur3 公知：mmh3("", seed=0) = 0
constexpr uint32_t kGoldenTest = 3127628307u; // murmur3 公知：mmh3("test", 0)

}  // namespace

// ---------------------------------------------------------------------------
// murmur3_32 基准值（防实现错位）
// ---------------------------------------------------------------------------

static void test_murmur3_known_vectors() {
    assert(murmur3_32("", 0, 0) == kGoldenEmpty);
    assert(murmur3_32("test", 4, 0) == kGoldenTest);
    // seed 不同结果不同（排除 seed 被忽略的实现错误）。
    assert(murmur3_32("test", 4, 1) != kGoldenTest);
    // V7：tail=3 字节基准（覆盖 body 循环后剩 3 字节的分支）。
    assert(murmur3_32("abc", 3, 0) == 3017643002u);
}

// ---------------------------------------------------------------------------
// LP 编码：长度前缀，无分隔符注入（spec §3.1）
// ---------------------------------------------------------------------------

static void test_lp_encoding_no_separator_collision() {
    // "a/b"+"c" 与 "a"+"b/c" 若用分隔符拼接会撞键；LP 编码必须不同。
    const std::string k1 = lp("a/b") + lp("c");
    const std::string k2 = lp("a") + lp("b/c");
    assert(k1 != k2);

    // LP 前 4 字节 = 长度（小端）。
    const std::string encoded = lp("abc");
    assert(encoded.size() == 4 + 3);
    assert(encoded[0] == 3 && encoded[1] == 0 && encoded[2] == 0 && encoded[3] == 0);
    assert(encoded.substr(4) == "abc");

    // LE32 小端。
    const std::string n = le32(0x01020304u);
    assert(n.size() == 4);
    assert(static_cast<unsigned char>(n[0]) == 0x04);
    assert(static_cast<unsigned char>(n[3]) == 0x01);
}

// ---------------------------------------------------------------------------
// 黄金向量（vnode / query）
// ---------------------------------------------------------------------------

static void test_golden_vectors() {
    // V1 / V2：vnode 哈希输入 = LP(node_id) ‖ LE32(index)。
    assert(murmur3_32(vnode_key("node-a", 0)) == kGoldenV1);
    assert(murmur3_32(vnode_key("节点-b", 99)) == kGoldenV2);
    // V3 / V4 / V5：query 哈希输入 = LP(ds)‖LP(table)‖LP(col)‖LP(val)‖LE32(attempt)。
    assert(murmur3_32(query_key("main", "users", "id", "42", 0)) == kGoldenV3);
    assert(murmur3_32(query_key("main", "users", "tag", "a/b", 0)) == kGoldenV4);
    assert(murmur3_32(query_key("main", "users", "id", "-7", 3)) == kGoldenV5);
}

// ---------------------------------------------------------------------------
// 环：确定性 / 回绕 / 归属
// ---------------------------------------------------------------------------

static void test_ring_deterministic_and_wraparound() {
    HashRing ring_a;
    HashRing ring_b;
    for (const std::string& node : {"node-a", "node-b", "node-c"}) {
        ring_a.add_node(node, 100);
    }
    // 乱序插入必须建出相同的环（tie-break 确定性 + 排序与插入顺序无关）。
    for (const std::string& node : {"node-c", "node-a", "node-b"}) {
        ring_b.add_node(node, 100);
    }
    for (int i = 0; i < 200; ++i) {
        const std::string key = "key-" + std::to_string(i);
        assert(ring_a.owner(lp(key)) == ring_b.owner(lp(key)));
        assert(!ring_a.owner(lp(key)).empty());
    }

    // 空环：无节点时 owner 返回空串（调用方 fail-fast，不能崩）。
    HashRing empty;
    assert(empty.owner(lp("anything")).empty());
}

static void test_ring_v6_end_to_end_ownership() {
    // V6：三节点 × 100 vnodes，查询键 = V3 的输入，期望 owner = node-a。
    HashRing ring;
    for (const std::string& node : {"node-a", "node-b", "node-c"}) {
        ring.add_node(node, 100);
    }
    const std::string key = query_key("main", "users", "id", "42", 0);
    assert(ring.owner(key) == "node-a");
    // attempt 改变 → 大概率落不同节点（绕行机制的基础）。
    int distinct = 0;
    for (uint32_t attempt = 0; attempt < 8; ++attempt) {
        const std::string k = query_key("main", "users", "id", "42", attempt);
        if (ring.owner(k) != "node-a") {
            ++distinct;
        }
    }
    assert(distinct > 0 && "attempt 绕行必须能落到其他节点");
}

// ---------------------------------------------------------------------------
// vnode 哈希碰撞的确定性 tie-break（spec §3.2 V8，离线搜索冻结的碰撞对）。
// 300 个 vnode 在 2^32 空间的自然碰撞概率仅 ~1e-5 —— 不冻结真实碰撞对，
// tie-break 分支永远不会被执行到，测试通过纯属"没撞上"。
// ---------------------------------------------------------------------------

static void test_ring_collision_tie_break_is_deterministic() {
    // 冻结事实：vnode(node-8, 1591) 与 vnode(node-21, 173) 的哈希同为
    // 2079710457；tie-break 按 (node_id, vnode_index) 字典序取最小 → node-21。
    assert(murmur3_32(vnode_key("node-8", 1591)) == 2079710457u);
    assert(murmur3_32(vnode_key("node-21", 173)) == 2079710457u);

    // 两种插入顺序建出的环必须逐键一致（tie-break 不依赖插入顺序）。
    HashRing ring1;
    ring1.add_node("node-8", 1592);
    ring1.add_node("node-21", 174);
    HashRing ring2;
    ring2.add_node("node-21", 174);
    ring2.add_node("node-8", 1592);
    for (int i = 0; i < 200; ++i) {
        const std::string key = lp("probe-" + std::to_string(i));
        assert(ring1.owner(key) == ring2.owner(key));
    }

    // 碰撞点归属 = 字典序赢家 node-21：查询键哈希 2078835168 落在碰撞点前
    // 一个 vnode(2076313318) 与碰撞点(2079710457) 之间 ⇒ lower_bound 恰好
    // 命中碰撞槽，环上该槽存的是 tie-break 赢家。
    const std::string key = query_key("main", "users", "id", "1099", 0);
    assert(murmur3_32(key) == 2078835168u);
    assert(ring1.owner(key) == "node-21");
    assert(ring2.owner(key) == "node-21");
}

// ---------------------------------------------------------------------------
// 确定性回绕：查询哈希 > 环上最大 vnode 哈希 ⇒ 回绕到环首。
// ---------------------------------------------------------------------------

static void test_ring_wraparound_is_deterministic() {
    // 冻结事实（离线搜索）：node-a/node-b × 100 vnodes 的环上最大 vnode
    // 哈希为 4279750736；查询键 (main,users,id,78,0) 的哈希 4282887048
    // 大于它 ⇒ lower_bound 越界 ⇒ 必须回绕到环首节点。
    HashRing ring;
    ring.add_node("node-a", 100);
    ring.add_node("node-b", 100);
    const std::string key = query_key("main", "users", "id", "78", 0);
    const uint32_t h = murmur3_32(key);
    assert(h == 4282887048u);
    // 环首节点 = 最小 vnode 哈希的归属；用空串查询（murmur3("")=0，哈希 0
    // 必落环首）由环自身求出环首归属，避免硬编码节点名
    // （冻结的是"回绕行为"本身，不是环首是谁）。
    const std::string head_owner = ring.owner("");
    assert(ring.owner(key) == head_owner);
    // 对照：未越界的键不走回绕（哈希 0 的 owner 与越界键一致 = 都回绕到环首）。
    // 另取一个中间区间的键，归属应与环上分布一致（不为空即可，
    // 精确归属由 V6/V8 用例钉住）。
    const std::string mid_key = query_key("main", "users", "id", "42", 0);
    assert(!ring.owner(mid_key).empty());
}

// ---------------------------------------------------------------------------
// 分布均匀性（统计界）：3 节点 × 100 vnodes，1 万键，
// 每节点份额应在 1/3 ± 6% 以内（100 vnodes 下经验界，防严重倾斜）。
// ---------------------------------------------------------------------------

static void test_ring_distribution_balance() {
    HashRing ring;
    for (const std::string& node : {"node-a", "node-b", "node-c"}) {
        ring.add_node(node, 100);
    }
    std::unordered_map<std::string, int> counts;
    const int kKeys = 10000;
    for (int i = 0; i < kKeys; ++i) {
        const std::string key = query_key("main", "t" + std::to_string(i % 10),
                                          "id", std::to_string(i), 0);
        ++counts[ring.owner(key)];
    }
    assert(counts.size() == 3);
    for (const auto& [node, count] : counts) {
        const double share = static_cast<double>(count) / kKeys;
        assert(share > 0.333 - 0.06 && share < 0.333 + 0.06);
        (void)node;
    }
}

int main() {
    test_murmur3_known_vectors();
    test_lp_encoding_no_separator_collision();
    test_golden_vectors();
    test_ring_deterministic_and_wraparound();
    test_ring_collision_tie_break_is_deterministic();
    test_ring_wraparound_is_deterministic();
    test_ring_v6_end_to_end_ownership();
    test_ring_distribution_balance();
    return 0;
}
