// test_routing：多数据源注册表 + table→datasource 路由（spec §4.1）。
//
// 用两个真实 SQLite 文件数据源本机实测（spec §11 期4 的可验证性要求）：
//   shard0: users + orders
//   shard1: users + logs
// users 跨源歧义：不指定 datasource 时必须 kAmbiguous（HTTP 409 语义），
// 绝不按配置顺序兜底猜（spec §4.1 的静默数据错乱论证）。

#include "cpppools/config/config.h"
#include "cpppools/db/datasource_registry.h"
#include "cpppools/db/db_connection_pool.h"
#include "cpppools/db/row_request.h"
#include "cpppools/db/value.h"
#include "db/sqlite_database.h"

#include <sqlite3.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>

using namespace cpppools::db;
namespace config = cpppools::config;

namespace {

std::string make_temp_path(const char* tag) {
    std::string path = "/tmp/cpppools_routing_" + std::string(tag) + "_XXXXXX";
    std::vector<char> buf(path.begin(), path.end());
    buf.push_back('\0');
    const int fd = mkstemp(buf.data());
    assert(fd >= 0);
    close(fd);
    return std::string(buf.data());
}

void seed(const std::string& path, const std::vector<std::string>& ddls) {
    sqlite3* raw = nullptr;
    assert(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
    for (const auto& ddl : ddls) {
        char* err = nullptr;
        assert(sqlite3_exec(raw, ddl.c_str(), nullptr, nullptr, &err) == SQLITE_OK);
    }
    sqlite3_close(raw);
}

config::DataSourceConfig make_source(const std::string& name, const std::string& path,
                                     bool is_default = false) {
    config::DataSourceConfig cfg;
    cfg.name = name;
    cfg.type = "sqlite";
    cfg.is_default = is_default;
    cfg.pool_size = 2;
    cfg.sqlite_path = path;
    return cfg;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// 双源 fixture：shard0(users,orders) + shard1(users,logs)。
struct TwoShards {
    std::string path_a;
    std::string path_b;
    std::unique_ptr<DataSourceRegistry> reg;

    TwoShards() {
        path_a = make_temp_path("shard0");
        path_b = make_temp_path("shard1");
        seed(path_a, {"CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL)",
                      "CREATE TABLE orders(id INTEGER PRIMARY KEY, amount INTEGER)"});
        seed(path_b, {"CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL)",
                      "CREATE TABLE logs(id INTEGER PRIMARY KEY, msg TEXT)"});
        // 注册 sqlite 后端（幂等）。
        register_sqlite_backend();
        RegistryBuildResult result = DataSourceRegistry::build(
            {make_source("shard0", path_a, true), make_source("shard1", path_b)});
        if (!result.error.empty()) {
            std::fprintf(stderr, "registry build failed: %s\n", result.error.c_str());
        }
        assert(result.registry != nullptr);
        reg = std::move(result.registry);
    }

    ~TwoShards() {
        reg.reset();
        for (const auto& p : {path_a, path_b}) {
            std::remove(p.c_str());
            std::remove((p + "-wal").c_str());
            std::remove((p + "-shm").c_str());
        }
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// 构建：清单、默认源
// ---------------------------------------------------------------------------

static void test_build_lists_sources_and_default() {
    TwoShards f;
    const auto names = f.reg->datasource_names();
    assert(names.size() == 2);
    assert(names[0] == "shard0" && names[1] == "shard1");
    assert(f.reg->default_name() == "shard0");
    assert(f.reg->default_pool() != nullptr);
    assert(f.reg->default_pool() == f.reg->find_pool("shard0"));
    assert(f.reg->find_pool("shard1") != nullptr);
    assert(f.reg->find_pool("ghost") == nullptr);

    // 每源的表清单（/api/v1/schema 期5 要用）。
    const auto tables_a = f.reg->tables_of("shard0");
    assert(tables_a.size() == 2);
    assert(std::find(tables_a.begin(), tables_a.end(), "orders") != tables_a.end());
}

static void test_build_errors() {
    // 空清单。
    auto r0 = DataSourceRegistry::build({});
    assert(r0.registry == nullptr && !r0.error.empty());

    // 重名。
    TwoShards seed_srcs;  // 复用已建好的库文件
    auto r1 = DataSourceRegistry::build(
        {make_source("dup", seed_srcs.path_a, true), make_source("dup", seed_srcs.path_b)});
    assert(r1.registry == nullptr);
    assert(contains(r1.error, "dup"));

    // 库文件不存在（READWRITE 不带 CREATE）。
    auto r2 = DataSourceRegistry::build(
        {make_source("bad", "/tmp/cpppools_routing_missing_xyz.db")});
    assert(r2.registry == nullptr);
    assert(contains(r2.error, "bad"));

    // 多源无 default / 多 default → 配置错。
    auto r3 = DataSourceRegistry::build(
        {make_source("s0", seed_srcs.path_a), make_source("s1", seed_srcs.path_b)});
    assert(r3.registry == nullptr);
    assert(contains(r3.error, "\"default\""));
    auto r4 = DataSourceRegistry::build(
        {make_source("s0", seed_srcs.path_a, true), make_source("s1", seed_srcs.path_b, true)});
    assert(r4.registry == nullptr);
    assert(contains(r4.error, "\"default\""));

    // 双源中途失败：第二源库缺失 → 错误带第二源名（部分构建的池须正常析构）。
    auto r6 = DataSourceRegistry::build(
        {make_source("ok", seed_srcs.path_a, true),
         make_source("broken", "/tmp/cpppools_routing_missing_second_xyz.db")});
    assert(r6.registry == nullptr);
    assert(contains(r6.error, "broken"));

    // 单源无需 default 标记。
    auto r5 = DataSourceRegistry::build({make_source("solo", seed_srcs.path_a)});
    assert(r5.registry != nullptr);
    assert(r5.registry->default_name() == "solo");
}

// ---------------------------------------------------------------------------
// 路由（spec §4.1 五种情形）
// ---------------------------------------------------------------------------

static void test_unique_table_routes_automatically() {
    TwoShards f;

    const RouteResult orders = f.reg->route("orders", "");
    assert(orders.status == RouteStatus::kOk);
    assert(orders.datasource == "shard0");
    assert(orders.pool == f.reg->find_pool("shard0"));

    const RouteResult logs = f.reg->route("logs", "");
    assert(logs.status == RouteStatus::kOk);
    assert(logs.datasource == "shard1");
    assert(logs.pool == f.reg->find_pool("shard1"));
}

static void test_ambiguous_table_requires_explicit_datasource() {
    TwoShards f;

    // users 两源都有、未指定 → 歧义（HTTP 层映射 409，绝不兜底猜）。
    const RouteResult r = f.reg->route("users", "");
    assert(r.status == RouteStatus::kAmbiguous);
    assert(r.pool == nullptr);
    assert(r.candidates.size() == 2);
    // 候选顺序 = 配置顺序（对外可见性质，钉住）。
    assert(r.candidates[0] == "shard0" && r.candidates[1] == "shard1");
    assert(contains(r.message, "shard0"));
    assert(contains(r.message, "shard1"));

    // 显式指定 → 消歧（优先级最高）。
    const RouteResult one = f.reg->route("users", "shard1");
    assert(one.status == RouteStatus::kOk);
    assert(one.datasource == "shard1");
}

static void test_unknown_table_and_unknown_datasource() {
    TwoShards f;

    const RouteResult no_table = f.reg->route("ghost", "");
    assert(no_table.status == RouteStatus::kUnknownTable);
    assert(contains(no_table.message, "ghost"));

    const RouteResult no_ds = f.reg->route("users", "ghost");
    assert(no_ds.status == RouteStatus::kUnknownDatasource);
    assert(contains(no_ds.message, "ghost"));

    // 指定的数据源存在但没有该表 → 也是 unknown table（带数据源上下文）。
    const RouteResult not_in = f.reg->route("orders", "shard1");
    assert(not_in.status == RouteStatus::kUnknownTable);
    assert(contains(not_in.message, "shard1"));
}

// 真实 SQL 跨源隔离：路由到的池写入的数据，另一源读不到 ——
// 证明两个池确实是两条独立的库，而不是共享了什么。
static void test_routed_crud_is_isolated_between_sources() {
    TwoShards f;

    const RouteResult one = f.reg->route("users", "shard1");
    assert(one.status == RouteStatus::kOk);

    // 经 shard1 的池借连接插入一行。
    {
        DbConnGuard guard(*one.pool, std::chrono::milliseconds(100));
        assert(guard.valid());
        InsertRequest ins{"users", {{"id", Value{std::int64_t{7}}},
                                    {"name", Value{std::string("eve")}}}};
        assert(guard.get()->insert(ins).status == DbStatus::kOk);
    }

    // shard1 读得到。
    {
        DbConnGuard guard(*one.pool, std::chrono::milliseconds(100));
        const auto got = guard.get()->select(
            SelectRequest{"users", {"name"}, WhereClause{"id", Value{std::int64_t{7}}}});
        assert(got.status == DbStatus::kOk);
        assert(got.value.rows.size() == 1);
        assert(std::get<std::string>(got.value.rows[0][0]) == "eve");
    }

    // shard0 读不到（同名表，不同的库）。
    {
        DbConnGuard guard(*f.reg->find_pool("shard0"), std::chrono::milliseconds(100));
        const auto got = guard.get()->select(
            SelectRequest{"users", {}, WhereClause{"id", Value{std::int64_t{7}}}});
        assert(got.status == DbStatus::kOk);
        assert(got.value.rows.empty());
    }
}

// :memory: 陷阱（spec §10.1）：字面 :memory: 每 open 一个独立内存库
// → 强制单槽；file::memory:?cache=shared 共享内存库则支持多槽，
// 且池内两条连接必须互见数据（否则就是陷阱本身）。
static void test_memory_datasource_handling() {
    register_sqlite_backend();

    // 字面 :memory: → pool_size 强制降为 1。
    auto r_plain = DataSourceRegistry::build(
        {make_source("plain", ":memory:", true)});
    assert(r_plain.registry != nullptr);
    assert(r_plain.registry->find_pool("plain")->total_count() == 1);

    // shared-cache：多槽合法，且池内两条连接共享同一内存库。
    const std::string uri = "file:cpppools_routing_mem?mode=memory&cache=shared";
    // 先建表并保持一个裸连接（共享内存库随最后一个连接关闭而消失）。
    sqlite3* raw = nullptr;
    assert(sqlite3_open_v2(uri.c_str(), &raw,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI, nullptr) == SQLITE_OK);
    assert(sqlite3_exec(raw, "CREATE TABLE t(id INTEGER PRIMARY KEY, v INTEGER)",
                        nullptr, nullptr, nullptr) == SQLITE_OK);

    config::DataSourceConfig cfg = make_source("mem", uri, true);
    cfg.pool_size = 2;
    auto r = DataSourceRegistry::build({cfg});
    assert(r.registry != nullptr);
    assert(r.registry->find_pool("mem")->total_count() == 2);

    // 同时借出两条连接：A 写、B 读 —— 互见即共享成功，
    // 也证明这不是“N 个互不相干空库”的陷阱形态。
    DbConnectionPool* pool = r.registry->find_pool("mem");
    Database* a = pool->acquire(std::chrono::milliseconds(100));
    Database* b = pool->acquire(std::chrono::milliseconds(100));
    assert(a != nullptr && b != nullptr && a != b);
    assert(a->insert(InsertRequest{"t", {{"id", Value{std::int64_t{1}}},
                                         {"v", Value{std::int64_t{9}}}}})
               .status == DbStatus::kOk);
    const auto got = b->select(SelectRequest{"t", {"v"}, WhereClause{"id", Value{std::int64_t{1}}}});
    assert(got.status == DbStatus::kOk);
    assert(got.value.rows.size() == 1);
    assert(std::get<std::int64_t>(got.value.rows[0][0]) == 9);
    pool->release(a);
    pool->release(b);

    r.registry.reset();
    sqlite3_close(raw);
}

int main() {
    test_build_lists_sources_and_default();
    test_build_errors();
    test_unique_table_routes_automatically();
    test_ambiguous_table_requires_explicit_datasource();
    test_unknown_table_and_unknown_datasource();
    test_routed_crud_is_isolated_between_sources();
    test_memory_datasource_handling();
    return 0;
}
