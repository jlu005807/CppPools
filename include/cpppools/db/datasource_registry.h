#pragma once

#include "cpppools/config/config.h"

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cpppools::db {

class DbConnectionPool;
class RowCache;

// 路由结果（spec §4.1 五种情形）。
enum class RouteStatus {
    kOk,                 // 命中唯一数据源（自动或显式）
    kUnknownTable,       // 表不在任何（或指定的）数据源 → HTTP 400
    kAmbiguous,          // 表在多个数据源且未显式指定 → HTTP 409（不兜底猜）
    kUnknownDatasource,  // 显式指定的数据源不存在 → HTTP 400
};

struct RouteResult {
    RouteStatus status{RouteStatus::kUnknownTable};
    // kOk 时非空。**非拥有**：生命周期等同 registry，调用方不得 delete、
    // 不得在 registry 析构后持有（池地址在构建后稳定）。
    DbConnectionPool* pool{nullptr};
    std::string datasource;                // kOk 时命中的数据源名（响应要回传）
    std::vector<std::string> candidates;   // kAmbiguous 时的候选（配置顺序，稳定）
    std::string message;                   // 非 kOk 时的可读消息（含上下文）
};

struct RegistryBuildResult {
    std::unique_ptr<class DataSourceRegistry> registry;
    std::string error;  // registry 为空时非空
};

// 多数据源注册表：每数据源一个连接池 + table→datasource 索引。
//
// 构建（build）：逐源 create_database 探连（可连 + 可自省）→ 记录表清单
// → 每源建池（factory 每槽位新建 Database 实例）。任一源失败整体 fail-fast。
//
// 路由语义（spec §4.1）：
//   - 显式 datasource 优先级最高；
//   - 表名唯一出现 → 自动路由；
//   - 表名跨源歧义且未指定 → kAmbiguous（绝不按配置顺序兜底，
//     防止 affected_rows:1 的静默写错库）；
//   - 歧义表在构建期打印 stderr 警告（spec：歧义必须在启动时被发现）。
//
// 路由索引是构建期 probe 的**快照**：运行期 schema 变更不重新自省
// （关系模型不变是本项目前提）。若索引与池内快照漂移，执行期校验管道
// 以池内快照为准兜底 —— 失败形态是干净的“表不存在”拒绝，而非错执行。
//
// 构建完成后全只读，route() 可被多线程并发调用。
class DataSourceRegistry {
public:
    // cache_cfg.enabled=true 时为每个数据源创建一份**共享** RowCache，
    // 注入该源全部槽位的 CachingDatabase 装饰器（spec §4.1.1）。
    static RegistryBuildResult build(
        const std::vector<config::DataSourceConfig>& sources,
        const config::AppConfig::Cache& cache_cfg = {});

    ~DataSourceRegistry();
    DataSourceRegistry(const DataSourceRegistry&) = delete;
    DataSourceRegistry& operator=(const DataSourceRegistry&) = delete;

    RouteResult route(const std::string& table, const std::string& datasource_hint) const;

    DbConnectionPool* find_pool(const std::string& datasource_name) const;
    // 默认源（is_default 或唯一源；构建时已校验）。可能为 nullptr 仅当构建失败路径。
    DbConnectionPool* default_pool() const;
    const std::string& default_name() const { return default_name_; }

    std::vector<std::string> datasource_names() const;
    // 全部连接池（配置顺序）：指标/统计聚合用。指针生命周期同 registry。
    std::vector<const DbConnectionPool*> pools() const;
    // 指定源的表清单（/api/v1/schema 用）。未知源返回空。
    std::vector<std::string> tables_of(const std::string& datasource_name) const;
    // 单个数据源的配置摘要（跨节点一致性校验用，spec §6.3）。
    // digest = murmur3_32(LP(type) ‖ LP(目标标识各段)) 的 hex，**不含凭据**：
    // 哈希输入用 LP 长度前缀（与 §3.1 同原则，禁止分隔符拼接）。
    struct DataSourceDigest {
        std::string name;
        std::string type;
        std::string digest;
    };
    std::vector<DataSourceDigest> datasource_digests() const;
    // 各数据源的共享缓存（cache 关闭时为空）：cache_* 指标聚合用。
    std::vector<std::pair<std::string, const RowCache*>> caches() const;

private:
    DataSourceRegistry() = default;

    struct Entry {
        config::DataSourceConfig config;
        std::vector<std::string> tables;               // 自省顺序
        std::unique_ptr<DbConnectionPool> pool;
        std::shared_ptr<RowCache> cache;               // enabled 时非空，槽位共享
    };

    const Entry* find_entry(const std::string& name) const;

    std::vector<Entry> entries_;                              // 配置顺序
    std::unordered_map<std::string, std::size_t> by_name_;    // 名称 → entries_ 下标
    std::unordered_map<std::string, std::vector<std::string>> table_index_;
    std::string default_name_;
};

}  // namespace cpppools::db
