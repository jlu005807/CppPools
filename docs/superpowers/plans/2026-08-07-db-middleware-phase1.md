# DB 中间件改造实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 CppPools 从 score 专用 CRUD 服务改造为对客户端透明的多数据源数据库中间件（SQLite + MySQL，可扩展）。

**Architecture:** 客户端发通用行级 CRUD（JSON 请求体）→ 校验管道（schema 白名单）→ `Database` 抽象基类的子类实现（SQLite/MySQL）→ 行级结果。多数据源按表名路由，歧义时要求显式 `datasource`（不兜底）。三大池子（已加固）作为运行时骨架不动。

**Tech Stack:** C++17、vendored nlohmann/json（`third_party/nlohmann/json.hpp`，已就位）、SQLite3（brew/SDK 已就位）、MySQL C API（可选分支）、assert + CTest。

**设计依据：** `docs/superpowers/specs/2026-08-06-db-middleware-design.md`（本地文件，未入 git —— 属刻意决定）。

**执行纪律（沿用池加固阶段）：**
- 每个 Task 一组 RED→GREEN：先写失败测试、确认失败原因正确、最小实现、确认通过
- 每期末：`output/t.sh` 全量（真查告警）+ CMake 全量重编 + CTest + e2e + TSan（涉并发改动时）
- 每期末 code-reviewer 评审 → 修正 → 复验 → 提交（作者 `jlu005807 <1289406660@qq.com>`，用 `git -c user.name=... -c user.email=...`，不改全局配置）
- 测试断言不得依赖时序侥幸；涉及「新行为」必须有反向实证（变异或反向用例）

## 总体路线（spec §11，每期一提交）

| 期 | 内容 | 里程碑 |
|---|---|---|
| 1 | JSON 配置加载 + HTTP 请求体读取（**本计划详述**） | 配置错误 fail-fast，CRUD 旧端点不受影响 |
| 2 | 适配层骨架：value/schema/row_request/Database 基类（NVI 校验）/factory/statement_cache | 校验管道单测 |
| 3 | SQLite 后端 + 连接池元素切换 | conformance 测试 |
| 4 | 多数据源：registry + table→datasource 路由 + 歧义 409 | 路由测试（双 SQLite） |
| 5 | 通用 CRUD 端点 + `/api/v1/schema` + 删除 score 遗留 | **可对接 SDK** |
| 6 | MySQL 后端 | 静态审查 + 外部环境回归 |
| 7 | 文档：backend_api.md 重写 + README | — |

---

# 期 1：JSON 配置加载 + HTTP 请求体读取

**目标**：服务从 JSON 配置文件读数据源清单；HTTP 层能读带 Content-Length 的请求体。本期**不改变任何既有端点行为**（score CRUD 照常工作），为后续各期铺路。

**文件结构**：

| 文件 | 职责 | 动作 |
|---|---|---|
| `include/cpppools/config/config.h` | `ServerConfig` / `DataSourceConfig` / `AppConfig` 类型 + `resolve_config_path` / `load_config` 声明 | 新建 |
| `src/config/config.cpp` | nlohmann/json 解析 + 校验（nlohmann 只进 .cpp） | 新建 |
| `src/server/http_util.h/.cpp` | 新增 `find_content_length`（头区纯解析） | 修改 |
| `include/cpppools/server/http_server.h` | `RequestContext` 加 `body` 字段 + `max_body_bytes` 常量 | 修改 |
| `src/server/http_server.cpp` | `handle_client` 读请求体（400/413 分支） | 修改 |
| `src/main.cpp` | `--config` 参数、加载配置、启动横幅回显数据源清单 | 修改 |
| `config/cpppools.example.json` | 示例配置（入库） | 新建 |
| `test/test_config.cpp` | 配置解析/校验/路径解析测试 | 新建 |
| `test/test_http_util.cpp` | Content-Length 解析用例 | 修改 |
| `test/test_http_server.cpp` | 请求体读取端到端（socketpair） | 修改 |
| `CMakeLists.txt` | 新源文件 + test_config 注册 | 修改 |
| `output/t.sh` | 编译清单加 `src/config/config.cpp` | 修改 |

---

### Task 1: 配置类型 + JSON 解析与校验

**Files:**
- Create: `include/cpppools/config/config.h`
- Create: `src/config/config.cpp`
- Test: `test/test_config.cpp`

- [x] **Step 1: 写失败测试** `test/test_config.cpp`

```cpp
#include "cpppools/config/config.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace cpppools::config;

namespace {

// 写临时配置文件，返回路径。测试结束自行删除。
std::string write_tmp_config(const std::string& content, const char* name) {
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
    const std::string path = write_tmp_config(kValidTwoSources, "cfg_pwdenv.json");
    const auto result = load_config(path);
    assert(result.ok);
    assert(result.config.datasources[1].mysql_password == "secret123");
    ::unsetenv("CPPPOOLS_TEST_PASSWORD");
    std::remove(path.c_str());
}

static void test_mysql_password_env_unset_fails() {
    ::unsetenv("CPPPOOLS_TEST_PASSWORD");
    const std::string path = write_tmp_config(kValidTwoSources, "cfg_pwdenv_missing.json");
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
    return 0;
}
```

- [x] **Step 2: 运行确认编译失败**（config.h 尚不存在）

先把 `test_config.cpp` 放到位并直接编译该测试（t.sh 源清单此时还没 config.cpp，
编译 test_config.cpp 本身即可验证 RED）：
Run: `output/t.sh test_config`
Expected: 编译失败 `'cpppools/config/config.h' file not found`（失败原因是缺头文件，符合预期）

- [x] **Step 3: 实现** `include/cpppools/config/config.h`

```cpp
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
```

- [x] **Step 4: 实现** `src/config/config.cpp`（nlohmann 只在本文件 include）

```cpp
#include "cpppools/config/config.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace cpppools::config {

namespace {

using njson = nlohmann::json;

// 从 JSON 对象里取字段；缺失时用 fallback。模板只对数值/字符串使用。
template <typename T>
T get_or(const njson& obj, const char* key, T fallback) {
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return fallback;
    }
    return it->get<T>();
}

std::string get_string_or(const njson& obj, const char* key, const std::string& fallback) {
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return fallback;
    }
    return it->get<std::string>();
}

ConfigLoadResult fail(std::string message) {
    ConfigLoadResult r;
    r.ok = false;
    r.error = std::move(message);
    return r;
}

}  // namespace

std::string resolve_config_path(int argc, const char* const* argv) {
    if (argc >= 3 && std::string(argv[1]) == "--config") {
        return argv[2];
    }
    if (const char* env = std::getenv("CPPPOOLS_CONFIG"); env != nullptr && env[0] != '\0') {
        return env;
    }
    return "config/cpppools.json";
}

ConfigLoadResult load_config(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        return fail("config file not found: " + path +
                    " (use --config <path> or CPPPOOLS_CONFIG; see config/cpppools.example.json)");
    }
    std::ostringstream buf;
    buf << in.rdbuf();

    njson root;
    try {
        root = njson::parse(buf.str());
    } catch (const njson::exception& ex) {
        return fail(std::string("malformed JSON in ") + path + ": " + ex.what());
    }

    AppConfig cfg;

    // ---- server 段（可选，全字段有默认值）----
    if (const auto it = root.find("server"); it != root.end()) {
        if (!it->is_object()) {
            return fail("\"server\" must be an object");
        }
        cfg.server.port = get_or<int>(*it, "port", 8080);
        cfg.server.worker_threads = static_cast<std::size_t>(
            get_or<int>(*it, "worker_threads", 6));
        cfg.server.latency_window_size = static_cast<std::size_t>(
            get_or<int>(*it, "latency_window_size", 1024));
    }
    if (cfg.server.port <= 0 || cfg.server.port > 65535) {
        return fail("server.port must be in [1, 65535]");
    }
    if (cfg.server.worker_threads == 0) {
        return fail("server.worker_threads must be > 0");
    }

    // ---- datasources 段（必需，非空数组）----
    const auto ds_it = root.find("datasources");
    if (ds_it == root.end() || !ds_it->is_array()) {
        return fail("\"datasources\" is required and must be an array");
    }
    if (ds_it->empty()) {
        return fail("\"datasources\" must not be empty");
    }

    std::unordered_set<std::string> seen_names;
    std::size_t default_count = 0;

    for (std::size_t i = 0; i < ds_it->size(); ++i) {
        const njson& item = (*ds_it)[i];
        if (!item.is_object()) {
            return fail("datasources[" + std::to_string(i) + "] must be an object");
        }
        DataSourceConfig ds;
        ds.name = get_string_or(item, "name", "");
        if (ds.name.empty()) {
            return fail("datasources[" + std::to_string(i) + "] missing \"name\"");
        }
        if (!seen_names.insert(ds.name).second) {
            return fail("duplicate datasource name: \"" + ds.name + "\"");
        }
        ds.type = get_string_or(item, "type", "");
        ds.is_default = get_or<bool>(item, "default", false);
        ds.pool_size = static_cast<std::size_t>(get_or<int>(item, "pool_size", 8));
        if (ds.pool_size == 0) {
            return fail("datasource \"" + ds.name + "\": pool_size must be > 0");
        }
        if (ds.is_default) {
            ++default_count;
        }

        if (ds.type == "sqlite") {
            const auto sq = item.find("sqlite");
            if (sq == item.end() || !sq->is_object()) {
                return fail("datasource \"" + ds.name + "\": missing \"sqlite\" section");
            }
            ds.sqlite_path = get_string_or(*sq, "path", "");
            if (ds.sqlite_path.empty()) {
                return fail("datasource \"" + ds.name + "\": sqlite.path is required");
            }
        } else if (ds.type == "mysql") {
            const auto my = item.find("mysql");
            if (my == item.end() || !my->is_object()) {
                return fail("datasource \"" + ds.name + "\": missing \"mysql\" section");
            }
            ds.mysql_host = get_string_or(*my, "host", "127.0.0.1");
            ds.mysql_port = get_or<int>(*my, "port", 3306);
            ds.mysql_user = get_string_or(*my, "user", "");
            ds.mysql_database = get_string_or(*my, "database", "");
            if (ds.mysql_user.empty()) {
                return fail("datasource \"" + ds.name + "\": mysql.user is required");
            }
            if (ds.mysql_database.empty()) {
                return fail("datasource \"" + ds.name + "\": mysql.database is required");
            }
            // 密码：password_env 优先（安全实践），其次明文 password。
            const std::string env_name = get_string_or(*my, "password_env", "");
            if (!env_name.empty()) {
                const char* env_val = std::getenv(env_name.c_str());
                if (env_val == nullptr) {
                    return fail("datasource \"" + ds.name + "\": environment variable \"" +
                                env_name + "\" (mysql.password_env) is not set");
                }
                ds.mysql_password = env_val;
            } else {
                ds.mysql_password = get_string_or(*my, "password", "");
            }
        } else {
            return fail("datasource \"" + ds.name + "\": unknown type \"" + ds.type +
                        "\" (supported: sqlite, mysql)");
        }

        cfg.datasources.push_back(std::move(ds));
    }

    if (default_count > 1) {
        return fail("at most one datasource may have \"default\": true");
    }

    ConfigLoadResult result;
    result.ok = true;
    result.config = std::move(cfg);
    return result;
}

}  // namespace cpppools::config
```

- [x] **Step 5: 接线构建系统**

`CMakeLists.txt`：`add_library(cpppools STATIC ...)` 源列表加一行 `src/config/config.cpp`；
测试注册区加 `cpppools_add_test(test_config)`。

`output/t.sh`：c++ 源列表加一行 `src/config/config.cpp \`。

- [x] **Step 6: 运行确认通过**

Run: `output/t.sh test_config`
Expected: 编译零告警 + 15 个用例全过

- [x] **Step 7: 提交**

```bash
git add -A
git -c user.name='jlu005807' -c user.email='1289406660@qq.com' commit -m "期1-1：JSON配置加载与校验"
```

---

### Task 2: HTTP 请求体读取（Content-Length）

**Files:**
- Modify: `src/server/http_util.h` / `src/server/http_util.cpp`
- Modify: `include/cpppools/server/http_server.h`（RequestContext.body + kMaxBodyBytes）
- Modify: `src/server/http_server.cpp`（handle_client）
- Test: `test/test_http_util.cpp`、`test/test_http_server.cpp`

- [x] **Step 1: 写 http_util 失败测试**（追加到 `test/test_http_util.cpp`）

```cpp
static void test_find_content_length_parses_value() {
    // 头区以空行（\r\n\r\n）结束；Content-Length 大小写不敏感（RFC 7230 头名不区分大小写）。
    assert(find_content_length("POST /x HTTP/1.1\r\nContent-Length: 17\r\n\r\n{}") == 17);
    assert(find_content_length("POST /x HTTP/1.1\r\ncontent-length: 5\r\n\r\nabcde") == 5);
    assert(find_content_length("POST /x HTTP/1.1\r\nContent-Length: 0\r\n\r\n") == 0);
    // 无头区终结符：视为请求不完整。
    assert(!find_content_length("POST /x HTTP/1.1\r\nContent-Length: 17").has_value());
    // 无 Content-Length 头：GET 类请求的正常形态。
    assert(!find_content_length("GET /x HTTP/1.1\r\nHost: a\r\n\r\n").has_value());
    // 非法值：非数字 / 负数。
    assert(!find_content_length("POST /x HTTP/1.1\r\nContent-Length: abc\r\n\r\n").has_value());
    assert(!find_content_length("POST /x HTTP/1.1\r\nContent-Length: -1\r\n\r\n").has_value());
}
```

并在其 `main()` 里调用 `test_find_content_length_parses_value();`。

- [x] **Step 2: 运行确认失败**（函数不存在）

Run: `output/t.sh test_http_util`
Expected: 编译失败 `no member named 'find_content_length'`

- [x] **Step 3: 实现** `find_content_length`

`src/server/http_util.h` 追加声明：

```cpp
// 从原始请求文本的头区解析 Content-Length。
// 头区以 "\r\n\r\n" 结束；找不到终结符返回 nullopt（请求不完整，
// 与请求行的完整性闸门同理 —— 防止半截头部被当作完整请求处理）。
// 无 Content-Length 头同样返回 nullopt（GET 请求的正常形态）。
// 值必须是纯非负十进制数，否则 nullopt（400 由调用方负责）。
std::optional<std::size_t> find_content_length(std::string_view raw);
```

`src/server/http_util.cpp` 追加实现：

```cpp
std::optional<std::size_t> find_content_length(std::string_view raw) {
    // 头区终结符是头部完整的闸门（同 parse_request_line 对请求行的处理）。
    const auto header_end = raw.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view headers = raw.substr(0, header_end);

    // 逐行扫描，头名大小写不敏感（RFC 7230 §3.2）。
    std::size_t pos = 0;
    while (pos < headers.size()) {
        const auto line_end = headers.find("\r\n", pos);
        const std::string_view line =
            headers.substr(pos, line_end == std::string_view::npos
                                    ? std::string_view::npos
                                    : line_end - pos);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            std::string_view name = line.substr(0, colon);
            // 就地小写比较：头名仅 ASCII 字母/数字/连字符，逐字符 tolower 安全。
            bool match = name.size() == 14;
            if (match) {
                static constexpr char kLower[] = "content-length";
                for (std::size_t i = 0; i < 14; ++i) {
                    const char c = name[i];
                    const char lower = (c >= 'A' && c <= 'Z')
                                           ? static_cast<char>(c + ('a' - 'A'))
                                           : c;
                    if (lower != kLower[i]) {
                        match = false;
                        break;
                    }
                }
            }
            if (match) {
                // 值：跳过冒号后的空白，要求全为非负数字。
                std::string_view value = line.substr(colon + 1);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                    value.remove_prefix(1);
                }
                while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                    value.remove_suffix(1);
                }
                if (value.empty()) {
                    return std::nullopt;
                }
                std::size_t result = 0;
                for (const char c : value) {
                    if (c < '0' || c > '9') {
                        return std::nullopt;
                    }
                    // 溢出保护：超过 1GB 的 body 必然非法（服务端有 kMaxBodyBytes）。
                    if (result > (std::size_t{1} << 30)) {
                        return std::nullopt;
                    }
                    result = result * 10 + static_cast<std::size_t>(c - '0');
                }
                return result;
            }
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        pos = line_end + 2;
    }
    return std::nullopt;
}
```

- [x] **Step 4: 运行确认通过**

Run: `output/t.sh test_http_util`
Expected: PASS

- [x] **Step 5: 写 handle_client 读体失败测试**（追加到 `test/test_http_server.cpp`）

```cpp
static void test_request_body_is_read_when_content_length_present() {
    // 带 Content-Length 的请求：handle_client 应把 body 读进 ctx。
    // 当前 score 端点不用 body，本测试验证的是：带 body 的请求仍能正常处理
    // （不会把 body 误读进请求行/头部导致 400），且无 Content-Length 的请求不受影响。
    Fixture f(2, 4);
    const std::string body = R"({"hello":"world"})";
    const std::string req = "POST /health HTTP/1.1\r\n"
                            "Content-Length: " + std::to_string(body.size()) + "\r\n"
                            "\r\n" + body;
    const std::string resp = f.round_trip(req);
    // /health 只接受 GET → 405；关键断言是"走到了路由"而非 400 请求格式错。
    assert(resp.find("405") != std::string::npos);
}

static void test_body_over_limit_returns_413() {
    Fixture f(2, 4);
    // Content-Length 超过 kMaxBodyBytes → 413，且不尝试读入。
    const std::string req = "POST /health HTTP/1.1\r\n"
                            "Content-Length: 999999\r\n"
                            "\r\n";
    const std::string resp = f.round_trip(req);
    assert(resp.find("413") != std::string::npos);
}
```

并在 `main()` 追加两个调用。

- [x] **Step 6: 运行确认失败**（当前不读 body，Content-Length 头会让请求行走 405/400 之外的错误路径；413 不存在）

Run: `output/t.sh test_http_server`
Expected: FAIL（断言失败，非编译错）

- [x] **Step 7: 实现 handle_client 读体**

`include/cpppools/server/http_server.h`：

```cpp
// 请求体上限。超过返回 413。JSON 行级 CRUD 的 body 是 KB 级，
// 64KB 留足余量；配置化留给后续（本期固定）。
inline constexpr std::size_t kMaxBodyBytes = 65536;

struct RequestContext {
    ...（现有字段不动）
    std::string body;   // 请求体（Content-Length 读入；reset() 清空保留 capacity）
    ...
    void reset() {
        request_id = 0;
        response.clear();
        body.clear();
    }
};
```

`src/server/http_server.cpp` 的 `handle_client`：在首次 recv 之后、路由之前插入请求体读取段。
要点：首次 recv 可能已包含部分/全部 body（一次 recv 的结果）——先截取已到达部分，
不足则继续 recv 直到读满；读完后把 `request` 视图截到头区为止（路由只看头区，
现有逻辑不变）。必须在路由之前读：否则 body 字节残留在 socket 上，
若将来支持 keep-alive 会污染下一个请求。

实现（插入在 `const bool request_complete = ...; std::string_view request(...);` 之后）：

```cpp
    const auto received = static_cast<std::size_t>(n);
    const bool request_complete = received < kRequestBufferBytes;
    std::string_view request(ctx->buffer.data(), received);

    // --- 请求体读取 ---
    const auto content_length = find_content_length(request);
    if (content_length.has_value()) {
        const std::size_t want = *content_length;
        if (want > kMaxBodyBytes) {
            const ApiResponse too_large =
                make_error_response(413, "Payload Too Large", "request body too large");
            std::string response_buf;
            append_http_response(too_large, response_buf);
            (void)send_all(guard.get(), response_buf.data(), response_buf.size());
            return;
        }
        const auto header_end = request.find("\r\n\r\n");
        const std::size_t body_offset = header_end + 4;
        const std::size_t have = received > body_offset ? received - body_offset : 0;
        ctx->body.assign(request.data() + body_offset,
                         std::min(have, want));
        // 不足则继续 recv（单次 recv 未必读全 body）。
        while (ctx->body.size() < want) {
            char chunk[4096];
            SsizeType m = 0;
            do {
                m = recv(guard.get(), chunk, sizeof(chunk), 0);
            } while (m < 0 && errno == EINTR);
            if (m <= 0) {
                break;  // 对端断开：body 不完整，路由层会按现有语义处理
            }
            const std::size_t take = std::min(want - ctx->body.size(),
                                              static_cast<std::size_t>(m));
            ctx->body.append(chunk, take);
        }
        // 头区视图截到头区终结符为止：路由层不应看到 body 字节。
        request = request.substr(0, body_offset);
    }
```

（保留原有 `request_complete` 判断对头区的语义：`received < kRequestBufferBytes` 仍基于首次 recv。）

- [x] **Step 8: 运行确认通过 + 全量回归**

Run: `output/t.sh test_http_server && output/t.sh test_http_util`
Expected: PASS；其余测试不回归

- [x] **Step 9: 提交**

```bash
git add -A
git -c user.name='jlu005807' -c user.email='1289406660@qq.com' commit -m "期1-2：HTTP请求体读取（Content-Length）"
```

---

### Task 3: main.cpp 接线 + 示例配置 + 端到端验证

**Files:**
- Modify: `src/main.cpp`
- Create: `config/cpppools.example.json`
- Test: e2e（`output/e2e.sh` 扩展）

- [x] **Step 1: 写示例配置** `config/cpppools.example.json`

```json
{
  "server": {
    "port": 8080,
    "worker_threads": 6,
    "latency_window_size": 1024
  },
  "datasources": [
    {
      "name": "main",
      "type": "sqlite",
      "default": true,
      "pool_size": 4,
      "sqlite": { "path": "data/main.db" }
    }
  ]
}
```

- [x] **Step 2: 改 main.cpp**

命令行处理改为：支持 `--config <path>`（与位置参数端口/worker 共存：
若 `argv[1]=="--config"` 则端口/worker 走环境变量或默认值，本期不做混合解析，
`--config` 与位置参数二选一，混用时以 `--config` 优先并忽略位置参数）。

```cpp
    const std::string config_path = config::resolve_config_path(argc, argv);
    const config::ConfigLoadResult cfg_result = config::load_config(config_path);
    if (!cfg_result.ok) {
        std::cerr << "config error: " << cfg_result.error << std::endl;
        std::cerr << "see config/cpppools.example.json for the expected format" << std::endl;
        return 1;
    }
    const config::AppConfig& app_cfg = cfg_result.config;

    // 本期过渡：server 端口/worker 取自配置；数据源清单只回显，
    // 实际建连仍走旧的 DbPoolConfig（mock/mysql 环境变量）—— 期 3 才切换。
    const int port = app_cfg.server.port;
    const int worker_count = static_cast<int>(app_cfg.server.worker_threads);
```

（原有的 `argv[1]/argv[2]` 位置参数解析删除，环境变量 `CPPPOOLS_DB_*` 保留不动。）

启动横幅追加数据源清单回显：

```cpp
    std::cout << "config file   : " << config_path << std::endl;
    for (const auto& ds : app_cfg.datasources) {
        std::cout << "datasource    : " << ds.name << " (" << ds.type << ")"
                  << (ds.is_default ? " [default]" : "") << std::endl;
    }
```

注意：密码绝不回显（`mysql_password` 不进任何日志/端点）。

- [x] **Step 3: 扩展 e2e** —— 用配置文件启动，验证横幅与既有 CRUD 不回归

`output/e2e.sh` 起服务命令前写入临时配置（端口 18099、worker 1、一个 sqlite 数据源），
启动时不带位置参数；断言既有行为（/health 200、背压 37×503）不变。

- [x] **Step 4: 全量验证**

Run: 全部 `output/t.sh` + CMake 全量重编（零告警）+ CTest + e2e
Expected: 全绿

- [x] **Step 5: 提交**

```bash
git add -A
git -c user.name='jlu005807' -c user.email='1289406660@qq.com' commit -m "期1-3：配置文件接线与示例"
```

- [x] **Step 6: 期末评审** —— 派 code-reviewer 评审批 1 全部三个提交，修正后复验，
必要时追加修正提交。

---

## 已知取舍（期 1）

- 配置文件的 `datasources` 本期只加载与回显，**不建连** —— 实际切换在期 3/4。
  期间服务行为与改造前完全一致（score CRUD 照常），保证每期可回滚。
- 请求体只支持 Content-Length 模型（chunked 明确不支持，400 语义由路由层既有逻辑覆盖）。
- `resolve_config_path` 的 `--config` 与位置参数互斥：混用场景超出本期 YAGNI。
