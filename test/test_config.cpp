#include "cpppools/config/config.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>

using namespace cpppools::config;

namespace {

// CTest 从 build/ 目录运行时 output/b/ 不存在，自建之（幂等）。
void ensure_tmp_dir() {
    ::mkdir("output", 0755);
    ::mkdir("output/b", 0755);
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
    ::setenv("CPPPOOLS_TEST_PASSWORD", "secret123", 1);
    const std::string path = write_tmp_config(kPasswordEnvConfig, "cfg_pwdenv.json");
    const auto result = load_config(path);
    assert(result.ok);
    assert(result.config.datasources[0].mysql_password == "secret123");
    ::unsetenv("CPPPOOLS_TEST_PASSWORD");
    std::remove(path.c_str());
}

static void test_mysql_password_env_unset_fails() {
    ::unsetenv("CPPPOOLS_TEST_PASSWORD");
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

static void test_resolve_config_path_precedence() {
    // 显式参数 > 环境变量 > 默认路径。
    const char* argv_with_flag[] = {"cpppools_server", "--config", "custom.json"};
    assert(resolve_config_path(3, argv_with_flag) == "custom.json");

    ::setenv("CPPPOOLS_CONFIG", "env.json", 1);
    assert(resolve_config_path(1, argv_with_flag) == "env.json");  // 无 --config 时看环境变量
    assert(resolve_config_path(3, argv_with_flag) == "custom.json");  // 显式参数仍优先
    ::unsetenv("CPPPOOLS_CONFIG");

    assert(resolve_config_path(1, argv_with_flag) == "config/cpppools.json");
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
    test_resolve_config_path_precedence();
    test_zero_pool_size_fails();
    test_field_type_errors_fail_with_message();
    test_negative_numbers_fail();
    return 0;
}
