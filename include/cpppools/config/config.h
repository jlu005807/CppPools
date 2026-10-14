#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace cpppools::config {

// HTTP 服务层配置（对应 JSON 的 "server" 段；缺省时取默认值）。
struct ServerConfig {
    int port{8080};
    std::size_t worker_threads{6};
    std::size_t latency_window_size{1024};
};

// 一个后端数据源的完整描述。type-specific 字段平铺：
// type=="sqlite" 时 sqlite_path 有效；type=="mysql" 时 mysql_* 有效。
// 平铺而非 variant：字段少且固定，平铺更直观、序列化代码更短。
struct DataSourceConfig {
    std::string name;        // 必需，全配置内唯一
    std::string type;        // "sqlite" | "mysql"
    bool is_default{false};  // "default": true；至多一个
    std::size_t pool_size{8};

    // sqlite
    std::string sqlite_path;

    // mysql
    std::string mysql_host;
    int mysql_port{3306};
    std::string mysql_user;
    std::string mysql_password;   // password_env 解析后的结果；或直接 password
    std::string mysql_database;
};

struct AppConfig {
    ServerConfig server;
    std::vector<DataSourceConfig> datasources;

    // 集群与缓存段（可选；两段缺省 = 单节点模式，行为与改造前完全一致）。
    // 字段定义见 cluster-cache spec §6.1。
    struct ClusterNode {
        std::string node_id;  // 全集群唯一
        std::string host;
        int port{8080};
    };
    struct Cluster {
        std::string node_id;              // 本节点身份，必须出现在 nodes 中
        long long epoch{0};               // 拓扑版本，变更时递增
        std::vector<ClusterNode> nodes;   // 全集群节点列表
    };
    struct Cache {
        bool enabled{false};
        std::string backend{"memory"}; // memory | redis | auto
        std::string redis_host{"127.0.0.1"};
        int redis_port{6379};
        long long redis_timeout_ms{100};
        std::size_t redis_max_reply_bytes{1024 * 1024};
        std::size_t max_entries{100000};
        std::size_t max_bytes{static_cast<std::size_t>(256) * 1024 * 1024};
        std::size_t max_entry_bytes{65536};
        long long ttl_ms{60000};
        long long blacklist_ttl_ms{60000};  // 不变量 I1：必须 ≥ ttl_ms
        std::vector<std::string> tables;    // 空 = 全部表可缓存
    };

    bool has_cluster{false};
    Cluster cluster;
    Cache cache;
};

// 加载结果：错误按值返回（项目约定：状态按值返回，不用异常做控制流）。
struct ConfigLoadResult {
    bool ok{false};
    AppConfig config;
    std::string error;  // ok==false 时非空，可直接对用户展示
};

// 配置文件路径解析：显式参数 > 环境变量 CPPPOOLS_CONFIG > 默认 config/cpppools.json。
// argc/argv 直接透传：argv[1]=="--config" 时取 argv[2]。
std::string resolve_config_path(int argc, const char* const* argv);

// 读取并校验配置文件。任何校验失败都返回 ok=false + 可读 error。
ConfigLoadResult load_config(const std::string& path);

}  // namespace cpppools::config
