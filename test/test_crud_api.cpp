// test_crud_api：通用 CRUD 端点的状态码矩阵（spec §7.2/§7.3，取代 test_score_api）。
//
// 双源 fixture：main(users,orders) + aux(logs,users) —— users 跨源歧义。
// route_request 不碰 socket，纯函数式验证"请求文本→响应文本"。

#include "cpppools/db/datasource_registry.h"
#include "cpppools/pool/object_pool.h"
#include "cpppools/pool/thread_pool.h"
#include "cpppools/server/http_server.h"
#include "db/sqlite_database.h"

#include <sqlite3.h>

#include <cassert>
#include <cstdio>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

using namespace cpppools;

namespace {

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string make_temp_path(const char* tag) {
    std::string path = "/tmp/cpppools_crudapi_" + std::string(tag) + "_XXXXXX";
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
        assert(sqlite3_exec(raw, ddl.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    }
    sqlite3_close(raw);
}

config::DataSourceConfig make_source(const std::string& name, const std::string& path,
                                     bool is_default) {
    config::DataSourceConfig cfg;
    cfg.name = name;
    cfg.type = "sqlite";
    cfg.is_default = is_default;
    cfg.pool_size = 2;
    cfg.sqlite_path = path;
    return cfg;
}

struct Fixture {
    std::string path_main;
    std::string path_aux;
    pool::ThreadPool thread_pool{[] {
        pool::ThreadPoolConfig c;
        c.name = "worker";
        c.worker_count = 1;
        return c;
    }()};
    pool::ObjectPool<server::RequestContext> request_pool{"request", 4, 8};
    std::unique_ptr<db::DataSourceRegistry> registry;
    server::HttpServer server;

    Fixture() : server(0, thread_pool, request_pool, make_registry()) {}

    db::DataSourceRegistry& make_registry() {
        db::register_sqlite_backend();
        path_main = make_temp_path("main");
        path_aux = make_temp_path("aux");
        seed(path_main,
             {"CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL, tag TEXT UNIQUE)",
              "CREATE TABLE orders(id INTEGER PRIMARY KEY, amount INTEGER)"});
        seed(path_aux,
             {"CREATE TABLE logs(id INTEGER PRIMARY KEY, msg TEXT)",
              "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL, tag TEXT UNIQUE)"});
        auto result = db::DataSourceRegistry::build(
            {make_source("main", path_main, true), make_source("aux", path_aux, false)});
        assert(result.registry != nullptr);
        registry = std::move(result.registry);
        return *registry;
    }

    ~Fixture() {
        // server/thread_pool/request_pool 先于本析构体逆序销毁；
        // registry 在成员析构链里晚于 server（声明在 server 之前）。
        registry.reset();
        for (const auto& p : {path_main, path_aux}) {
            std::remove(p.c_str());
            std::remove((p + "-wal").c_str());
            std::remove((p + "-shm").c_str());
        }
    }

    server::ApiResponse post(const std::string& path, const std::string& body) {
        const std::string req = "POST " + path + " HTTP/1.1\r\n" +
                                "Content-Length: " + std::to_string(body.size()) +
                                "\r\n\r\n";
        return server.route_request(req, true, body);
    }

    server::ApiResponse get(const std::string& path) {
        return server.route_request("GET " + path + " HTTP/1.1\r\n", true);
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// insert / select / update / delete 基本往返（响应回传 datasource）
// ---------------------------------------------------------------------------

static void test_insert_select_roundtrip() {
    Fixture f;
    const auto ins = f.post("/api/v1/insert",
                            R"({"table":"orders","values":{"id":1,"amount":500}})");
    assert(ins.status_code == 201);
    assert(contains(ins.body, "\"datasource\":\"main\""));
    assert(contains(ins.body, "\"affected_rows\":1"));
    assert(contains(ins.body, "\"inserted_id\":1"));

    const auto sel = f.post("/api/v1/select",
                            R"({"table":"orders","columns":["amount"],)"
                            R"("where":{"column":"id","value":1}})");
    assert(sel.status_code == 200);
    assert(contains(sel.body, "\"datasource\":\"main\""));
    assert(contains(sel.body, "\"columns\":[\"amount\"]"));
    assert(contains(sel.body, "\"rows\":[[500]]"));
    assert(contains(sel.body, "\"row_count\":1"));
}

static void test_select_empty_is_200_not_404() {
    // SQL 语义（决策②）：0 行是成功，不是"资源不存在"。
    Fixture f;
    const auto sel = f.post("/api/v1/select",
                            R"({"table":"orders","where":{"column":"id","value":999}})");
    assert(sel.status_code == 200);
    assert(contains(sel.body, "\"rows\":[]"));
    assert(contains(sel.body, "\"row_count\":0"));
}

static void test_update_and_delete_affected_rows() {
    Fixture f;
    assert(f.post("/api/v1/insert",
                  R"({"table":"orders","values":{"id":1,"amount":100}})")
               .status_code == 201);

    const auto upd = f.post("/api/v1/update",
                            R"({"table":"orders","set":{"amount":200},)"
                            R"("where":{"column":"id","value":1}})");
    assert(upd.status_code == 200);
    assert(contains(upd.body, "\"affected_rows\":1"));
    assert(contains(upd.body, "\"datasource\":\"main\""));

    // 0 行影响：200 + affected_rows:0（不是 404）。
    const auto upd0 = f.post("/api/v1/update",
                             R"({"table":"orders","set":{"amount":1},)"
                             R"("where":{"column":"id","value":404}})");
    assert(upd0.status_code == 200);
    assert(contains(upd0.body, "\"affected_rows\":0"));

    const auto del = f.post("/api/v1/delete",
                            R"({"table":"orders","where":{"column":"id","value":1}})");
    assert(del.status_code == 200);
    assert(contains(del.body, "\"affected_rows\":1"));

    const auto del0 = f.post("/api/v1/delete",
                             R"({"table":"orders","where":{"column":"id","value":1}})");
    assert(del0.status_code == 200);
    assert(contains(del0.body, "\"affected_rows\":0"));
}

static void test_unique_conflict_is_409() {
    Fixture f;
    assert(f.post("/api/v1/insert",
                  R"({"table":"users","datasource":"main",)"
                  R"("values":{"name":"a","tag":"t1"}})")
               .status_code == 201);
    const auto dup = f.post("/api/v1/insert",
                            R"({"table":"users","datasource":"main",)"
                            R"("values":{"name":"b","tag":"t1"}})");
    assert(dup.status_code == 409);
    assert(contains(dup.body, "\"message\":"));
}

// ---------------------------------------------------------------------------
// 路由：自动 / 歧义 409 / 显式消歧 / 未知表与源
// ---------------------------------------------------------------------------

static void test_routing_auto_ambiguous_explicit() {
    Fixture f;
    // orders 只在 main → 自动路由。
    const auto orders = f.post("/api/v1/insert",
                               R"({"table":"orders","values":{"id":2,"amount":1}})");
    assert(orders.status_code == 201);
    assert(contains(orders.body, "\"datasource\":\"main\""));

    // logs 只在 aux → 自动路由到 aux。
    const auto logs = f.post("/api/v1/insert",
                             R"({"table":"logs","values":{"id":1,"msg":"hi"}})");
    assert(logs.status_code == 201);
    assert(contains(logs.body, "\"datasource\":\"aux\""));

    // users 两源都有、未指定 → 409，消息列出候选。
    const auto amb = f.post("/api/v1/select",
                            R"({"table":"users","where":{"column":"id","value":1}})");
    assert(amb.status_code == 409);
    assert(contains(amb.body, "main"));
    assert(contains(amb.body, "aux"));

    // 显式 datasource 消歧（优先级最高）。
    const auto explicit_ds = f.post("/api/v1/insert",
                                    R"({"table":"users","datasource":"aux",)"
                                    R"("values":{"name":"eve","tag":"e1"}})");
    assert(explicit_ds.status_code == 201);
    assert(contains(explicit_ds.body, "\"datasource\":\"aux\""));

    // main 里确实没有这行（跨源隔离）。
    const auto from_main = f.post("/api/v1/select",
                                  R"({"table":"users","datasource":"main",)"
                                  R"("where":{"column":"tag","value":"e1"}})");
    assert(from_main.status_code == 200);
    assert(contains(from_main.body, "\"row_count\":0"));
}

static void test_unknown_table_and_datasource_are_400() {
    Fixture f;
    const auto no_table = f.post("/api/v1/select",
                                 R"({"table":"ghost","where":{"column":"id","value":1}})");
    assert(no_table.status_code == 400);
    assert(contains(no_table.body, "ghost"));

    const auto no_ds = f.post("/api/v1/select",
                              R"({"table":"orders","datasource":"ghost",)"
                              R"("where":{"column":"id","value":1}})");
    assert(no_ds.status_code == 400);
    assert(contains(no_ds.body, "ghost"));

    // aux 存在但没有 orders 表。
    const auto not_in = f.post("/api/v1/select",
                               R"({"table":"orders","datasource":"aux",)"
                               R"("where":{"column":"id","value":1}})");
    assert(not_in.status_code == 400);
}

// ---------------------------------------------------------------------------
// 契约违反：400 矩阵（JSON 格式 / 缺字段 / 校验管道拒绝）
// ---------------------------------------------------------------------------

static void test_malformed_requests_are_400() {
    Fixture f;
    // 非法 JSON。
    assert(f.post("/api/v1/select", "{not json").status_code == 400);
    // 空 body。
    assert(f.post("/api/v1/select", "").status_code == 400);
    // 缺 table。
    assert(f.post("/api/v1/select",
                  R"({"where":{"column":"id","value":1}})").status_code == 400);
    // 缺 where。
    assert(f.post("/api/v1/select", R"({"table":"orders"})").status_code == 400);
    // insert 缺 values。
    assert(f.post("/api/v1/insert", R"({"table":"orders"})").status_code == 400);
    // 值类型不支持（布尔）。
    const auto bad_val = f.post("/api/v1/insert",
                                R"({"table":"orders","values":{"id":true}})");
    assert(bad_val.status_code == 400);
}

static void test_validation_pipeline_rejections_are_400() {
    Fixture f;
    // where 列非唯一（name 不是 PK/UNIQUE）。
    const auto non_unique = f.post("/api/v1/select",
                                   R"({"table":"users","datasource":"main",)"
                                   R"("where":{"column":"name","value":"x"}})");
    assert(non_unique.status_code == 400);
    assert(contains(non_unique.body, "primary key or unique"));

    // 类型不匹配（严格校验，不做隐式转换）。
    const auto type_mm = f.post("/api/v1/select",
                                R"({"table":"orders","where":{"column":"id","value":"1"}})");
    assert(type_mm.status_code == 400);
    assert(contains(type_mm.body, "expects"));

    // 未知列。
    const auto no_col = f.post("/api/v1/select",
                               R"({"table":"orders","columns":["ghost"],)"
                               R"("where":{"column":"id","value":1}})");
    assert(no_col.status_code == 400);
}

// ---------------------------------------------------------------------------
// 评审修正批回归：解析异常面 / 约束违例口径 / 脏数据容错
// ---------------------------------------------------------------------------

static void test_overflow_number_is_400_not_500() {
    // 1e999 是合法 JSON 语法但超出 double 表示范围：nlohmann 抛
    // out_of_range.406 —— 必须按输入问题回 400，不得穿出 handler 变 500。
    Fixture f;
    const auto resp = f.post("/api/v1/insert",
                             R"({"table":"orders","values":{"amount":1e999}})");
    assert(resp.status_code == 400);
    assert(contains(resp.body, "malformed JSON"));
}

static void test_missing_notnull_column_is_400() {
    // 漏传必填列（name NOT NULL）是客户端契约违反 → 400，不是后端故障 500。
    Fixture f;
    const auto resp = f.post("/api/v1/insert",
                             R"({"table":"users","datasource":"main",)"
                             R"("values":{"tag":"only-tag"}})");
    assert(resp.status_code == 400);
    assert(contains(resp.body, "constraint violation"));
}

static void test_invalid_utf8_text_is_replaced_not_500() {
    // 外部写入的非法 UTF-8 TEXT：查询必须仍可用（替换为 U+FFFD），
    // 不得让一条脏数据把整行查询永久变 500。
    Fixture f;
    sqlite3* raw = nullptr;
    assert(sqlite3_open(f.path_main.c_str(), &raw) == SQLITE_OK);
    assert(sqlite3_exec(raw,
                        "INSERT INTO users(id, name, tag) "
                        "VALUES(501, CAST(X'61FF62' AS TEXT), 'dirty')",
                        nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);

    const auto resp = f.post("/api/v1/select",
                             R"({"table":"users","datasource":"main",)"
                             R"("where":{"column":"id","value":501}})");
    assert(resp.status_code == 200);
    assert(contains(resp.body, "\ufffd") && "非法字节应替换为 U+FFFD");
}

static void test_non_string_datasource_is_400() {
    // 严格口径：datasource 给非字符串是契约违反，不得静默当"未指定"。
    Fixture f;
    const auto resp = f.post("/api/v1/select",
                             R"({"table":"orders","datasource":123,)"
                             R"("where":{"column":"id","value":1}})");
    assert(resp.status_code == 400);
    assert(contains(resp.body, "datasource"));
}

// ---------------------------------------------------------------------------
// /api/v1/schema：自省快照（不暴露后端 type）
// ---------------------------------------------------------------------------

static void test_unsigned_overflow_is_400() {
    // 9223372036854775808 == INT64_MAX + 1; exceeds int64_t range.
    Fixture f;
    const auto resp = f.post("/api/v1/insert",
                             R"({"table":"orders","values":{"amount":9223372036854775808}})");
    assert(resp.status_code == 400);
    assert(contains(resp.body, "out of range"));
}

static void test_schema_endpoint() {
    Fixture f;
    const auto resp = f.get("/api/v1/schema");
    assert(resp.status_code == 200);
    assert(contains(resp.body, "\"datasources\":["));
    assert(contains(resp.body, "\"name\":\"main\""));
    assert(contains(resp.body, "\"name\":\"aux\""));
    assert(contains(resp.body, "\"name\":\"orders\""));
    assert(contains(resp.body, "\"name\":\"logs\""));
    // 列元信息。
    assert(contains(resp.body, "\"name\":\"amount\""));
    assert(contains(resp.body, "\"type\":\"integer\""));
    assert(contains(resp.body, "\"unique\":true"));
    // 不暴露后端类型（SQLite/MySQL 是客户端不该依赖的实现细节）。
    assert(!contains(resp.body, "sqlite"));
    assert(!contains(resp.body, "mysql"));
}

int main() {
    test_insert_select_roundtrip();
    test_select_empty_is_200_not_404();
    test_update_and_delete_affected_rows();
    test_unique_conflict_is_409();
    test_routing_auto_ambiguous_explicit();
    test_unknown_table_and_datasource_are_400();
    test_malformed_requests_are_400();
    test_validation_pipeline_rejections_are_400();
    test_overflow_number_is_400_not_500();
    test_unsigned_overflow_is_400();
    test_missing_notnull_column_is_400();
    test_invalid_utf8_text_is_replaced_not_500();
    test_non_string_datasource_is_400();
    test_schema_endpoint();
    return 0;
}
