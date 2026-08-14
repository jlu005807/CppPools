#pragma once

#include "server/hash_ring.h"

#include "cpppools/db/value.h"
#include "cpppools/server/http_server.h"  // ClusterView

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace cpppools::server {

// 归属决策（spec §3.6 决策矩阵的输出）。
struct OwnershipDecision {
    enum class Action { kLocal, kRedirect };

    Action action{Action::kLocal};
    // false = bypass 读命中与回填（失效不受影响 —— 不变量 I0）。
    bool cache_allowed{true};
    // true = 响应必须回 X-CppPools-Epoch: <self>（客户端立即刷新拓扑，§5.4）。
    bool epoch_mismatch{false};
    // kRedirect 时：owner 的基址（http://host:port），由 handler 追加原 path。
    std::string location;
};

// 归属路由器：服务端权威自判 is_owner = (ring.owner(key) == self)（spec §3.6，
// 评审 B3：不依赖客户端申报）。构造时从 ClusterView 建环；decide 实现
// §3.6 决策矩阵五行，并内置 §8.1 的决策计数器。
//
// 单节点退化（nodes 空）：恒 kLocal + 允许缓存 —— epoch 门控只服务多节点
// 滚动重启窗口，单节点缓存是唯一副本，bypass 只会白白回源。
//
// 线程模型：decide 为 const，多线程（worker）并发调用安全；
// 计数器为原子量（指标取数口径：计数点 = 决策点）。
class OwnershipRouter {
public:
    explicit OwnershipRouter(ClusterView view);

    // 路由 4 元组按 D1 口径（客户端可见 datasource：显式值或空串）。
    // epoch_header 空 = 客户端未携带（不比对，curl 兼容）。
    // pool_label = 请求解析出的数据源名：bypass 计数的 {pool} 标签（§8.1）。
    OwnershipDecision decide(const std::string& key_datasource,
                             const std::string& table,
                             const std::string& column,
                             const db::Value& value,
                             bool has_fallback,
                             const std::string& epoch_header,
                             const std::string& pool_label) const;

    // §8.1 决策计数器：redirects 全局；bypass 按 {pool} 拆分
    //（同一节点多数据源时定位"哪个源的键在漂流"）。
    // 归因不合并（spec §8.1）：not_owner = 配置错位退化（owner 不在
    // nodes 表）；fallback = 客户端申报绕行（owner 不可达的故障信号）。
    struct Stats {
        std::uint64_t redirects{0};
        std::map<std::string, std::uint64_t> bypass_not_owner;
        std::map<std::string, std::uint64_t> bypass_fallback;
        std::map<std::string, std::uint64_t> bypass_epoch;
    };
    Stats stats() const;

    const std::string& self_node_id() const { return view_.node_id; }
    long long self_epoch() const { return view_.epoch; }

private:
    ClusterView view_;
    HashRing ring_;
    bool single_node_{false};
    mutable std::atomic<std::uint64_t> redirects_{0};
    // bypass 计数按 {pool} 拆分：键空间 = 数据源名（启动后固定），有界。
    mutable std::mutex stats_mu_;
    mutable std::map<std::string, std::uint64_t> bypass_not_owner_;
    mutable std::map<std::string, std::uint64_t> bypass_fallback_;
    mutable std::map<std::string, std::uint64_t> bypass_epoch_;
};

}  // namespace cpppools::server
