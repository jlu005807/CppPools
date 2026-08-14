#include "server/ownership_router.h"

#include <utility>
#include <variant>

namespace cpppools::server {

namespace {

// 路由键值的字符串化（与 SDK 同口径，spec §3.1/§3.3）：
// int64 → 十进制文本；string → 原样。REAL 键与 NULL 不参与环路由
// （spec 决策⑫/§3.6 无键条款），handler 层已拦在 decide 之前 ——
// 其余类型到这里是防御兜底，不构成路由语义。
std::string value_to_routing_string(const db::Value& value) {
    if (const auto* i = std::get_if<std::int64_t>(&value)) {
        return std::to_string(*i);
    }
    if (const auto* s = std::get_if<std::string>(&value)) {
        return *s;
    }
    return std::string{};
}

}  // namespace

OwnershipRouter::OwnershipRouter(ClusterView view) : view_(std::move(view)) {
    // 单节点模式：nodes 空 → 不建环，decide 恒本地（行为与期1 逐字节一致）。
    single_node_ = view_.nodes.empty();
    for (const auto& node : view_.nodes) {
        ring_.add_node(node.node_id, view_.vnodes_per_node);
    }
}

OwnershipDecision OwnershipRouter::decide(const std::string& key_datasource,
                                          const std::string& table,
                                          const std::string& column,
                                          const db::Value& value,
                                          bool has_fallback,
                                          const std::string& epoch_header,
                                          const std::string& pool_label) const {
    OwnershipDecision d;
    if (single_node_) {
        // 恒本地 + 允许缓存：epoch 门控只服务多节点滚动重启窗口。
        return d;
    }

    // 服务端权威自判（评审 B3）：不看客户端申报，只信自己的环。
    const std::string hash_input = hash_ring::query_key(
        key_datasource, table, column, value_to_routing_string(value),
        /*attempt=*/0);
    const std::string owner = ring_.owner(hash_input);

    if (owner == view_.node_id) {
        // epoch 头缺失不比对（curl 兼容，矩阵 "—" 列）。
        if (!epoch_header.empty() &&
            epoch_header != std::to_string(view_.epoch)) {
            d.cache_allowed = false;
            d.epoch_mismatch = true;
            std::lock_guard<std::mutex> lock(stats_mu_);
            ++bypass_epoch_[pool_label];
        }
        // owner 上的 Fallback 头不改变行为（提示单调性：它给不出
        // "更保守"的理由 —— 节点自己就是权威归属方）。
        (void)has_fallback;
        return d;
    }

    // 非 owner。
    d.cache_allowed = false;
    if (!has_fallback) {
        // 307 而非代理转发：零出向依赖、零线程占用（spec §3.6 决策⑧）。
        d.action = OwnershipDecision::Action::kRedirect;
        for (const auto& node : view_.nodes) {
            if (node.node_id == owner) {
                d.location = "http://" + node.host + ":" + std::to_string(node.port);
                break;
            }
        }
        if (d.location.empty()) {
            // owner 不在 nodes 表 = 配置错位：退化为就地处理不缓存。
            // 提示单调性 —— 永远选更保守的一侧。
            d.action = OwnershipDecision::Action::kLocal;
            std::lock_guard<std::mutex> lock(stats_mu_);
            ++bypass_not_owner_[pool_label];
        } else {
            redirects_.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        // 客户端已判定 owner 不可达（attempt 绕行，§3.5）：就地处理，
        // bypass 缓存（规则 A），失效由写路径照常执行（I0）。
        // spec §8.1：fallback（客户端申报）与 not_owner（配置错位退化）
        // 是不同信号，分开归因 —— 前者是"owner 不可达"的故障信号。
        std::lock_guard<std::mutex> lock(stats_mu_);
        ++bypass_fallback_[pool_label];
    }
    return d;
}

OwnershipRouter::Stats OwnershipRouter::stats() const {
    Stats s;
    s.redirects = redirects_.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(stats_mu_);
    s.bypass_not_owner = bypass_not_owner_;
    s.bypass_fallback = bypass_fallback_;
    s.bypass_epoch = bypass_epoch_;
    return s;
}

}  // namespace cpppools::server
