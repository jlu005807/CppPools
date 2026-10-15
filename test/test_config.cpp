#include "cpppools/config/config.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

using namespace cpppools::config;

namespace {

int test_mkdir(const char* path) {
#ifdef _WIN32
    return _mkdir(path);
#else
    return ::mkdir(path, 0755);
#endif
}

void set_test_environment(const char* key, const char* value) {
#ifdef _WIN32
    (void)_putenv_s(key, value);
#else
    (void)::setenv(key, value, 1);
#endif
}

void unset_test_environment(const char* key) {
#ifdef _WIN32
    (void)_putenv_s(key, "");
#else
    (void)::unsetenv(key);
#endif
}

// CTest 从 build/ 目录运行时 output/b/ 不存在，自建之（幂等）。
void ensure_tmp_dir() {
    (void)test_mkdir("output");
    (void)test_mkdir("output/b");
}

// 写临时配置文件，返回路径。测试结束自行删除。
std::string write_tmp_config(const std::string& content, const char* name) {
    ensure_tmp_dir();
    const std::string path = std::string("output/b/") + name;
    std::ofstream out(path);
    assert(out);
    out << content;
    return path;
}

constexpr const char* kValidTwoSources = R"({
  "server": { "port": 8080, "worker_threads": 6, "latency_window_size": 1024 },
  "datasources": [
    { "name": "main", "type": "sqlite", "default": true, "pool_size": 4,
      "sqlite": { "path": "data/main.db" } },
    { "name": "archive", "type": "mysql", "pool_size": 8,
      "mysql": { "host": "127.0.0.1", "port": 3306, "user": "root",
                 "password": "plaintext-ok-for-test", "database": "archive" } }
  ]
})";

// password_env 专用 fixture：依赖环境变量，只在对应的两个用例里使用。
constexpr const char* kPasswordEnvConfig = R"({
  "datasources": [
    { "name": "archive", "type": "mysql",
      "mysql": { "host": "127.0.0.1", "user": "root",
                 "password_env": "CPPPOOLS_TEST_PASSWORD", "database": "archive" } }
  ]
})";

}  // namespace

static void test_load_valid_config() {
    const std::string path = write_tmp_config(kValidTwoSources, "cfg_valid.json");
    const auto result = load_config(path);
    assert(result.ok);

    const AppConfig& cfg = result.config;
    assert(cfg.server.port == 8080);
    assert(cfg.server.worker_threads == 6);
    assert(cfg.server.latency_window_size == 1024);
    assert(cfg.datasources.size() == 2);

    const DataSourceConfig& a = cfg.datasources[0];
    assert(a.name == "main");
    assert(a.type == "sqlite");
    assert(a.is_default);
    assert(a.pool_size == 4);
    assert(a.sqlite_path == "data/main.db");

    const DataSourceConfig& b = cfg.datasources[1];
    assert(b.name == "archive");
    assert(b.type == "mysql");
    assert(!b.is_default);
    assert(b.pool_size == 8);
    assert(b.mysql_host == "127.0.0.1");
    assert(b.mysql_port == 3306);
    assert(b.mysql_user == "root");
    assert(b.mysql_database == "archive");
    assert(b.mysql_password == "plaintext-ok-for-test");

    std::remove(path.c_str());
}

static void test_load_missing_file_fails() {
    const auto result = load_config("output/b/definitely_not_exist.json");
    assert(!result.ok);
    assert(!result.error.empty());
}

static void test_load_malformed_json_fails() {
    const std::string path = write_tmp_config("{ not json", "cfg_bad.json");
    const auto result = load_config(path);
    assert(!result.ok);
    assert(!result.error.empty());
    std::remove(path.c_str());
}

static void test_missing_datasources_fails() {
    const std::string path =
        write_tmp_config(R"({ "server": { "port": 8080 } })", "cfg_no_ds.json");
    const auto result = load_config(path);
    assert(!result.ok);
    std::remove(path.c_str());
}

static void test_empty_datasources_fails() {
    const std::string path =
        write_tmp_config(R"({ "datasources": [] })", "cfg_empty_ds.json");
    const auto result = load_config(path);
    assert(!result.ok);
    std::remove(path.c_str());
}

static void test_duplicate_names_fail() {
    const std::string path = write_tmp_config(R"({
      "datasources": [
        { "name": "x", "type": "sqlite", "sqlite": { "path": "a.db" } },
        { "name": "x", "type": "sqlite", "sqlite": { "path": "b.db" } }
      ]
    })", "cfg_dup.json");
    const auto result = load_config(path);
    assert(!result.ok);
    std::remove(path.c_str());
}

static void test_unknown_type_fails() {
    const std::string path = write_tmp_config(R"({
      "datasources": [ { "name": "x", "type": "oracle" } ]
    })", "cfg_unknown_type.json");
    const auto result = load_config(path);
    assert(!result.ok);
    std::remove(path.c_str());
}

static void test_sqlite_missing_path_fails() {
    const std::string path = write_tmp_config(R"({
      "datasources": [ { "name": "x", "type": "sqlite" } ]
    })", "cfg_sqlite_nopath.json");
    const auto result = load_config(path);
    assert(!result.ok);
    std::remove(path.c_str());
}

static void test_mysql_missing_required_fields_fails() {
    // 缺 user。
    const std::string path = write_tmp_config(R"({
      "datasources": [ { "name": "x", "type": "mysql",
                         "mysql": { "host": "h", "database": "d" } } ]
    })", "cfg_mysql_nouser.json");
    const auto result = load_config(path);
    assert(!result.ok);
    std::remove(path.c_str());
}

static void test_mysql_password_env_resolved() {
    set_test_environment("CPPPOOLS_TEST_PASSWORD", "secret123");
    const std::string path = write_tmp_config(kPasswordEnvConfig, "cfg_pwdenv.json");
    const auto result = load_config(path);
    assert(result.ok);
    assert(result.config.datasources[0].mysql_password == "secret123");
    unset_test_environment("CPPPOOLS_TEST_PASSWORD");
    std::remove(path.c_str());
}

static void test_mysql_password_env_unset_fails() {
    unset_test_environment("CPPPOOLS_TEST_PASSWORD");
    const std::string path = write_tmp_config(kPasswordEnvConfig, "cfg_pwdenv_missing.json");
    const auto result = load_config(path);
    assert(!result.ok);  // 环境变量未设置 = 配置不完整
    std::remove(path.c_str());
}

static void test_multiple_defaults_fail() {
    const std::string path = write_tmp_config(R"({
      "datasources": [
        { "name": "a", "type": "sqlite", "default": true, "sqlite": { "path": "a.db" } },
        { "name": "b", "type": "sqlite", "default": true, "sqlite": { "path": "b.db" } }
      ]
    })", "cfg_two_defaults.json");
    const auto result = load_config(path);
    assert(!result.ok);
    std::remove(path.c_str());
}

static void test_server_defaults_apply() {
    const std::string path = write_tmp_config(R"({
      "datasources": [ { "name": "x", "type": "sqlite", "sqlite": { "path": "a.db" } } ]
    })", "cfg_defaults.json");
    const auto result = load_config(path);
    assert(result.ok);
    assert(result.config.server.port == 8080);
    assert(result.config.server.worker_threads == 6);
    assert(result.config.datasources[0].pool_size == 8);  // 数据源默认池大小
    assert(!result.config.datasources[0].is_default);     // 未声明 default 即 false
    std::remove(path.c_str());
}

static void test_cache_redis_defaults_and_validation() {
    const std::string path = write_tmp_config(R"({
      "cache": {"enabled": true, "backend": "auto",
                "redis": {"host": "localhost", "port": 6380,
                          "timeout_ms": 25, "max_reply_bytes": 4096}},
      "datasources": [ { "name": "x", "type": "sqlite", "sqlite": { "path": "a.db" } } ]
    })", "cfg_redis.json");
    const auto result = load_config(path);
    assert(result.ok);
    assert(result.config.cache.backend == "auto");
    assert(result.config.cache.redis_host == "localhost");
    assert(result.config.cache.redis_port == 6380);
    assert(result.config.cache.redis_timeout_ms == 25);
    assert(result.config.cache.redis_max_reply_bytes == 4096);
    std::remove(path.c_str());

    const std::string bad = write_tmp_config(R"({
      "cache": {"backend": "bogus"},
      "datasources": [ { "name": "x", "type": "sqlite", "sqlite": { "path": "a.db" } } ]
    })", "cfg_redis_bad.json");
    const auto invalid = load_config(bad);
    assert(!invalid.ok);
    assert(invalid.error.find("cache.backend") != std::string::npos);
    std::remove(bad.c_str());
}

static void test_resolve_config_path_precedence() {
    // 显式参数 > 环境变量 > 默认路径。
    const char* argv_with_flag[] = {"cpppools_server", "--config", "custom.json"};
    assert(resolve_config_path(3, argv_with_flag) == "custom.json");

    set_test_environment("CPPPOOLS_CONFIG", "env.json");
    assert(resolve_config_path(1, argv_with_flag) == "env.json");  // 无 --config 时看环境变量
    assert(resolve_config_path(3, argv_with_flag) == "custom.json");  // 显式参数仍优先
    unset_test_environment("CPPPOOLS_CONFIG");

    assert(resolve_config_path(1, argv_with_flag) == "config/cpppools.json");
}
static void test_resolve_config_path_missing_arg_throws() {
    const char* argv[] = {"cpppools_server", "--config"};
    bool threw = false;
    try {
        resolve_config_path(2, argv);
    } catch (const std::invalid_argument& ex) {
        threw = true;
        assert(std::string(ex.what()).find("--config") != std::string::npos);
        assert(std::string(ex.what()).find("path") != std::string::npos);
    }
    assert(threw && "--config without a path must throw invalid_argument");
}

static void test_zero_pool_size_fails() {
    const std::string path = write_tmp_config(R"({
      "datasources": [ { "name": "x", "type": "sqlite", "pool_size": 0,
                         "sqlite": { "path": "a.db" } } ]
    })", "cfg_zero_pool.json");
    const auto result = load_config(path);
    assert(!result.ok);
    std::remove(path.c_str());
}

static void test_field_type_errors_fail_with_message() {
    // B1 回归：字段类型错误必须是可读的 fail，绝不能 abort。
    const char* cases[] = {
        R"({ "server": { "port": "abc" }, "datasources": [ { "name": "x", "type": "sqlite", "sqlite": { "path": "a.db" } } ] })",
        R"({ "server": { "port": 8080.5 }, "datasources": [ { "name": "x", "type": "sqlite", "sqlite": { "path": "a.db" } } ] })",
        R"({ "datasources": [ { "name": 5, "type": "sqlite", "sqlite": { "path": "a.db" } } ] })",
        R"({ "datasources": [ { "name": "x", "type": "sqlite", "default": 1, "sqlite": { "path": "a.db" } } ] })",
        R"({ "datasources": [ { "name": "x", "type": "sqlite", "pool_size": "big", "sqlite": { "path": "a.db" } } ] })",
        R"({ "datasources": [ { "name": "x", "type": "sqlite", "sqlite": { "path": 5 } } ] })",
    };
    for (const char* raw : cases) {
        const std::string path = write_tmp_config(raw, "cfg_type.json");
        const auto result = load_config(path);
        assert(!result.ok);
        // fail-fast 的核心卖点是可读错误：消息必须非空且指明字段。
        assert(!result.error.empty());
        assert(result.error.find("must be") != std::string::npos ||
               result.error.find("invalid") != std::string::npos);
        std::remove(path.c_str());
    }
}

static void test_negative_numbers_fail() {
    // I1 回归：负数不得静默变成巨值或退化默认。
    const std::string p1 = write_tmp_config(
        R"({ "datasources": [ { "name": "x", "type": "sqlite", "pool_size": -5, "sqlite": { "path": "a.db" } } ] })",
        "cfg_neg_pool.json");
    const auto r1 = load_config(p1);
    assert(!r1.ok);
    assert(r1.error.find("pool_size") != std::string::npos);
    std::remove(p1.c_str());

    const std::string p2 = write_tmp_config(
        R"({ "server": { "worker_threads": -1 }, "datasources": [ { "name": "x", "type": "sqlite", "sqlite": { "path": "a.db" } } ] })",
        "cfg_neg_workers.json");
    const auto r2 = load_config(p2);
    assert(!r2.ok);
    assert(r2.error.find("worker_threads") != std::string::npos);
    std::remove(p2.c_str());
}

// ---------------------------------------------------------------------------
// cluster / cache 段（分布式集群与行级缓存，spec §6.1）
// ---------------------------------------------------------------------------

// cluster 段与 cache 段均合法的完整配置。
constexpr const char* kValidClusterConfig = R"({
  "cluster": {
    "node_id": "node-a",
    "epoch": 3,
    "nodes": [
      { "node_id": "node-a", "host": "10.0.0.1", "port": 8080 },
      { "node_id": "node-b", "host": "10.0.0.2", "port": 8080 }
    ]
  },
  "cache": {
    "enabled": true,
    "max_entries": 50000,
    "max_bytes": 1048576,
    "max_entry_bytes": 4096,
    "ttl_ms": 30000,
    "blacklist_ttl_ms": 60000,
    "tables": ["users", "orders"]
  },
  "datasources": [
    { "name": "main", "type": "sqlite", "sqlite": { "path": "data/main.db" } }
  ]
})";

// 包一层最小 datasources，只换中间段。
std::string wrap_datasources(const std::string& middle) {
    return "{" + middle + ", \"datasources\": ["
           "{ \"name\": \"main\", \"type\": \"sqlite\","
           " \"sqlite\": { \"path\": \"data/main.db\" } }]}";
}

static void test_cluster_cache_sections() {
    // 1. 完整合法配置：全字段解析正确。
    {
        const std::string path = write_tmp_config(kValidClusterConfig, "cfg_cluster.json");
        const auto result = load_config(path);
        assert(result.ok);
        assert(result.config.has_cluster);
        assert(result.config.cluster.node_id == "node-a");
        assert(result.config.cluster.epoch == 3);
        assert(result.config.cluster.nodes.size() == 2);
        assert(result.config.cluster.nodes[0].host == "10.0.0.1");
        assert(result.config.cluster.nodes[0].port == 8080);
        assert(result.config.cluster.nodes[1].node_id == "node-b");
        assert(result.config.cache.enabled);
        assert(result.config.cache.max_entries == 50000);
        assert(result.config.cache.max_bytes == 1048576);
        assert(result.config.cache.max_entry_bytes == 4096);
        assert(result.config.cache.ttl_ms == 30000);
        assert(result.config.cache.blacklist_ttl_ms == 60000);
        assert(result.config.cache.tables.size() == 2);
        assert(result.config.cache.tables[0] == "users");
        std::remove(path.c_str());
    }

    // 2. 两段缺省 = 单节点模式（向后兼容：既有配置不受影响）。
    {
        const std::string path = write_tmp_config(kValidTwoSources, "cfg_nocluster.json");
        const auto result = load_config(path);
        assert(result.ok);
        assert(!result.config.has_cluster);
        assert(!result.config.cache.enabled);  // 默认关闭
        std::remove(path.c_str());
    }

    // 3. node_id 不在 nodes 列表中 → fail-fast（spec §6.1）。
    {
        const std::string cfg = wrap_datasources(
            "\"cluster\": { \"node_id\": \"ghost\", "
            "\"nodes\": [ { \"node_id\": \"node-a\", \"host\": \"10.0.0.1\", \"port\": 8080 } ] }");
        const std::string path = write_tmp_config(cfg, "cfg_badnodeid.json");
        const auto result = load_config(path);
        assert(!result.ok);
        assert(result.error.find("node_id") != std::string::npos);
        std::remove(path.c_str());
    }

    // 4. nodes 空 → 失败；nodes 内重复 node_id → 失败。
    {
        const std::string cfg = wrap_datasources(
            "\"cluster\": { \"node_id\": \"node-a\", \"nodes\": [] }");
        const std::string path = write_tmp_config(cfg, "cfg_emptynodes.json");
        assert(!load_config(path).ok);
        std::remove(path.c_str());
    }
    {
        const std::string cfg = wrap_datasources(
            "\"cluster\": { \"node_id\": \"node-a\", \"nodes\": ["
            "{ \"node_id\": \"node-a\", \"host\": \"10.0.0.1\", \"port\": 8080 },"
            "{ \"node_id\": \"node-a\", \"host\": \"10.0.0.2\", \"port\": 8080 } ] }");
        const std::string path = write_tmp_config(cfg, "cfg_dupnodes.json");
        const auto result = load_config(path);
        assert(!result.ok);
        assert(result.error.find("node") != std::string::npos);
        std::remove(path.c_str());
    }

    // 5. nodes 端口越界 / host 缺失 → 失败。
    {
        const std::string cfg = wrap_datasources(
            "\"cluster\": { \"node_id\": \"node-a\", \"nodes\": ["
            "{ \"node_id\": \"node-a\", \"host\": \"10.0.0.1\", \"port\": 99999 } ] }");
        const std::string path = write_tmp_config(cfg, "cfg_badport.json");
        const auto result = load_config(path);
        assert(!result.ok);
        assert(result.error.find("port") != std::string::npos);
        std::remove(path.c_str());
    }
    {
        const std::string cfg = wrap_datasources(
            "\"cluster\": { \"node_id\": \"node-a\", \"nodes\": ["
            "{ \"node_id\": \"node-a\", \"port\": 8080 } ] }");
        const std::string path = write_tmp_config(cfg, "cfg_nohost.json");
        const auto result = load_config(path);
        assert(!result.ok);
        assert(result.error.find("host") != std::string::npos);
        std::remove(path.c_str());
    }

    // 6. 不变量 I1：blacklist_ttl_ms 必须 ≥ ttl_ms（spec §5.2）。
    {
        const std::string cfg = wrap_datasources(
            "\"cache\": { \"enabled\": true, \"ttl_ms\": 60000, \"blacklist_ttl_ms\": 10000 }");
        const std::string path = write_tmp_config(cfg, "cfg_badttl.json");
        const auto result = load_config(path);
        assert(!result.ok);
        assert(result.error.find("blacklist_ttl_ms") != std::string::npos);
        std::remove(path.c_str());
    }

    // 7. cache 非法取值：ttl ≤ 0、容量 ≤ 0。
    {
        const std::string cfg = wrap_datasources(
            "\"cache\": { \"enabled\": true, \"ttl_ms\": 0 }");
        const std::string path = write_tmp_config(cfg, "cfg_zerottl.json");
        assert(!load_config(path).ok);
        std::remove(path.c_str());
    }
    {
        const std::string cfg = wrap_datasources(
            "\"cache\": { \"enabled\": true, \"max_entries\": 0 }");
        const std::string path = write_tmp_config(cfg, "cfg_zeroentries.json");
        assert(!load_config(path).ok);
        std::remove(path.c_str());
    }

    // 8. 字段类型错（epoch 给字符串）→ 带字段名的可读错误。
    {
        const std::string cfg = wrap_datasources(
            "\"cluster\": { \"node_id\": \"node-a\", \"epoch\": \"three\", \"nodes\": ["
            "{ \"node_id\": \"node-a\", \"host\": \"10.0.0.1\", \"port\": 8080 } ] }");
        const std::string path = write_tmp_config(cfg, "cfg_badepoch.json");
        const auto result = load_config(path);
        assert(!result.ok);
        assert(result.error.find("cluster.epoch") != std::string::npos);
        std::remove(path.c_str());
    }

    // 9. cache.tables 非数组 / 非字符串元素 → 失败。
    {
        const std::string cfg = wrap_datasources(
            "\"cache\": { \"enabled\": true, \"tables\": \"users\" }");
        const std::string path = write_tmp_config(cfg, "cfg_badtables.json");
        assert(!load_config(path).ok);
        std::remove(path.c_str());
    }
}

int main() {
    test_load_valid_config();
    test_load_missing_file_fails();
    test_load_malformed_json_fails();
    test_missing_datasources_fails();
    test_empty_datasources_fails();
    test_duplicate_names_fail();
    test_unknown_type_fails();
    test_sqlite_missing_path_fails();
    test_mysql_missing_required_fields_fails();
    test_mysql_password_env_resolved();
    test_mysql_password_env_unset_fails();
    test_multiple_defaults_fail();
    test_server_defaults_apply();
    test_cache_redis_defaults_and_validation();
    test_resolve_config_path_precedence();
    test_resolve_config_path_missing_arg_throws();
    test_zero_pool_size_fails();
    test_field_type_errors_fail_with_message();
    test_negative_numbers_fail();
    test_cluster_cache_sections();
    return 0;
}
