// test_database_conformance：真实 SQLite 后端的一致性测试（spec §12）。
//
// 这是本设计最重要的验证手段：同一批 CRUD 断言跑真实 SQL 生成、
// 真实 prepared statement、真实自省。期 6 的 MySQL 后端将复用同一批断言。
//
// fixture 里的建表用**裸 sqlite3_exec**：中间件不建表（类型级保证），
// 但测试必须自己造被测库。

#include "cpppools/db/database.h"
#include "cpppools/db/row_request.h"
#include "db/caching_database.h"
#include "db/sqlite_database.h"
#include "cpppools/db/value.h"
#include "test_temp_file.h"

#include <sqlite3.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <atomic>
#include <thread>

using namespace cpppools::db;
namespace config = cpppools::config;

namespace {

// ---------------------------------------------------------------------------
// fixture：临时文件库 + 建表
// ---------------------------------------------------------------------------

std::string make_temp_path(const char* tag) {
    return cpppools::testing::make_test_temp_file("cpppools_conformance_" +
                                                  std::string(tag));
}

// 用裸连接建被测 schema。返回 0 表示成功。
int seed_schema(const std::string& path) {
    sqlite3* raw = nullptr;
    if (sqlite3_open(path.c_str(), &raw) != SQLITE_OK) {
        return -1;
    }
    const char* ddl =
        "CREATE TABLE users("
        "  id INTEGER PRIMARY KEY,"
        "  name TEXT NOT NULL,"
        "  age INTEGER,"
        "  score REAL,"
        "  tag TEXT UNIQUE"
        ");"
        "CREATE TABLE pairs(a INTEGER, b INTEGER, UNIQUE(a, b));"
        "CREATE TABLE pk_pairs(a INTEGER, b INTEGER, PRIMARY KEY(a, b));"
        "CREATE TABLE nums(id INTEGER PRIMARY KEY, v NUMERIC);";
    char* err = nullptr;
    const int rc = sqlite3_exec(raw, ddl, nullptr, nullptr, &err);
    if (err != nullptr) {
        sqlite3_free(err);
    }
    sqlite3_close(raw);
    return rc == SQLITE_OK ? 0 : -1;
}

struct TempDb {
    std::string path;
    std::unique_ptr<Database> db;

    TempDb() = default;
    TempDb(TempDb&&) noexcept = default;
    TempDb& operator=(TempDb&&) noexcept = default;

    ~TempDb() {
        db.reset();
        if (!path.empty()) {
            std::remove(path.c_str());
            std::remove((path + "-wal").c_str());
            std::remove((path + "-shm").c_str());
        }
    }
};

// 装饰器开关（cluster-cache spec §9 的 conformance 参数化）：main 跑两遍，
// 第二遍把每个 open_seeded_db 的库包上 CachingDatabase，验证 CRUD 语义
// 子集在缓存层下逐条不变。排除清单（直接构造后端或断言后端内部）的用例
// 在 g_with_cache=true 时自行跳过。
bool g_with_cache = false;

// 打开一个建好 users/pairs 表的临时库。
TempDb open_seeded_db(const std::string& tag) {
    TempDb t;
    t.path = make_temp_path(tag.c_str());
    assert(seed_schema(t.path) == 0);

    config::DataSourceConfig cfg;
    cfg.name = tag;
    cfg.type = "sqlite";
    cfg.sqlite_path = t.path;
    DatabaseCreateResult result = make_sqlite_database(cfg);
    if (!result.error.empty()) {
        std::fprintf(stderr, "make_sqlite_database failed: %s\n", result.error.c_str());
    }
    assert(result.db != nullptr);
    if (g_with_cache) {
        cpppools::db::RowCacheConfig rc;
        rc.ttl = std::chrono::seconds(60);
        t.db = std::make_unique<cpppools::db::CachingDatabase>(
            std::move(result.db),
            std::make_shared<cpppools::db::RowCache>(rc));
    } else {
        t.db = std::move(result.db);
    }
    return t;
}

WhereClause where_id(std::int64_t v) {
    return WhereClause{"id", Value{v}};
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

InsertRequest make_user(const std::string& name, std::int64_t age, double score,
                        const std::string& tag) {
    return InsertRequest{"users", {{"name", Value{name}},
                                   {"age", Value{age}},
                                   {"score", Value{score}},
                                   {"tag", Value{tag}}}};
}

// ---------------------------------------------------------------------------
// 自省
// ---------------------------------------------------------------------------

static void test_introspection() {
    TempDb t = open_seeded_db("intro");
    const Schema& s = t.db->schema();

    assert(s.has_table("users"));
    assert(s.has_table("pairs"));
    assert(s.table_names().size() == 4);

    // id: INTEGER PRIMARY KEY = rowid 别名 → 口径 D5：
    // nullable=false（拒绝显式 NULL）、autoincrement=true、unique。
    const ColumnInfo* id = s.find_column("users", "id");
    assert(id != nullptr);
    assert(id->kind == ValueKind::kInteger);
    assert(id->primary_key);
    assert(id->unique);
    assert(id->autoincrement);
    assert(!id->nullable);

    const ColumnInfo* name = s.find_column("users", "name");
    assert(name->kind == ValueKind::kText);
    assert(!name->nullable);
    assert(!name->unique);

    const ColumnInfo* age = s.find_column("users", "age");
    assert(age->kind == ValueKind::kInteger);
    assert(age->nullable);
    assert(!age->unique);

    const ColumnInfo* score = s.find_column("users", "score");
    assert(score->kind == ValueKind::kReal);

    // TEXT UNIQUE → unique=true、nullable=true。
    const ColumnInfo* tag = s.find_column("users", "tag");
    assert(tag->kind == ValueKind::kText);
    assert(tag->unique);
    assert(tag->nullable);

    // 联合唯一（UNIQUE(a,b)）不使任何单列唯一（口径 D6）。
    const ColumnInfo* a = s.find_column("pairs", "a");
    const ColumnInfo* b = s.find_column("pairs", "b");
    assert(a != nullptr && b != nullptr);
    assert(!a->unique);
    assert(!b->unique);
}

static void test_blob_column_rejects_datasource() {
    if (g_with_cache) return;  // 排除清单：直接构造后端、断言创建失败
    // 口径 D7：BLOB 亲和列 → 创建失败，error 指明 table.column。
    const std::string path = make_temp_path("blob");
    sqlite3* raw = nullptr;
    assert(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
    assert(sqlite3_exec(raw, "CREATE TABLE bins(data BLOB)", nullptr, nullptr, nullptr)
           == SQLITE_OK);
    sqlite3_close(raw);

    config::DataSourceConfig cfg;
    cfg.type = "sqlite";
    cfg.sqlite_path = path;
    DatabaseCreateResult result = make_sqlite_database(cfg);
    assert(result.db == nullptr);
    assert(result.error.find("bins.data") != std::string::npos);

    std::remove(path.c_str());
}

static void test_open_missing_file_fails() {
    if (g_with_cache) return;  // 排除清单：直接构造后端、断言创建失败
    // 口径 D11：不带 CREATE —— 库文件不存在则失败（中间件不建库）。
    config::DataSourceConfig cfg;
    cfg.type = "sqlite";
    cfg.sqlite_path = "/tmp/cpppools_conformance_does_not_exist_xyz.db";
    std::remove(cfg.sqlite_path.c_str());
    DatabaseCreateResult result = make_sqlite_database(cfg);
    assert(result.db == nullptr);
    assert(!result.error.empty());
}

// ---------------------------------------------------------------------------
// insert：往返 / inserted_id / 显式 PK / 唯一冲突 / NULL / REAL 收整数
// ---------------------------------------------------------------------------

static void test_insert_roundtrip_and_inserted_id() {
    TempDb t = open_seeded_db("ins");

    const auto r1 = t.db->insert(make_user("alice", 30, 91.5, "a-1"));
    assert(r1.status == DbStatus::kOk);
    assert(r1.value.affected_rows == 1);
    assert(r1.value.inserted_id.has_value());
    assert(*r1.value.inserted_id == 1);

    const auto r2 = t.db->insert(make_user("bob", 25, 70.0, "b-1"));
    assert(r2.status == DbStatus::kOk);
    assert(*r2.value.inserted_id == 2);

    // 往返：按生成的 id 查回，全列一致。
    SelectRequest sel{"users", {}, where_id(1)};
    const auto got = t.db->select(sel);
    assert(got.status == DbStatus::kOk);
    assert(got.value.columns.size() == 5);
    assert(got.value.rows.size() == 1);
    const auto& row = got.value.rows[0];
    assert(std::get<std::int64_t>(row[0]) == 1);
    assert(std::get<std::string>(row[1]) == "alice");
    assert(std::get<std::int64_t>(row[2]) == 30);
    assert(std::get<double>(row[3]) == 91.5);
    assert(std::get<std::string>(row[4]) == "a-1");
}

static void test_insert_explicit_pk() {
    TempDb t = open_seeded_db("expk");

    InsertRequest ins{"users", {{"id", Value{std::int64_t{100}}},
                                {"name", Value{std::string("carol")}}}};
    const auto r = t.db->insert(ins);
    assert(r.status == DbStatus::kOk);
    // 显式 rowid 也被 last_insert_rowid 记录（口径 D12）。
    assert(r.value.inserted_id.has_value());
    assert(*r.value.inserted_id == 100);

    const auto got = t.db->select(SelectRequest{"users", {"name"}, where_id(100)});
    assert(got.status == DbStatus::kOk);
    assert(got.value.rows.size() == 1);
    assert(std::get<std::string>(got.value.rows[0][0]) == "carol");
}

static void test_insert_unique_conflict() {
    TempDb t = open_seeded_db("uniq");

    assert(t.db->insert(make_user("alice", 30, 1.0, "same")).status == DbStatus::kOk);
    // tag 重复 → kAlreadyExists（口径 D13 → HTTP 409）。
    const auto r = t.db->insert(make_user("bob", 20, 2.0, "same"));
    assert(r.status == DbStatus::kAlreadyExists);
    assert(!r.message.empty());

    // PK 重复同理。
    InsertRequest dup_pk{"users", {{"id", Value{std::int64_t{1}}},
                                   {"name", Value{std::string("x")}}}};
    assert(t.db->insert(dup_pk).status == DbStatus::kAlreadyExists);
}

static void test_insert_null_and_real_accepts_integer() {
    TempDb t = open_seeded_db("null");

    // age 可空：显式 NULL 往返。
    InsertRequest ins{"users", {{"name", Value{std::string("dave")}},
                                {"age", Value{}},
                                {"score", Value{std::int64_t{3}}}}};  // REAL 收整数
    assert(t.db->insert(ins).status == DbStatus::kOk);

    const auto got = t.db->select(SelectRequest{"users", {"age", "score"}, where_id(1)});
    assert(got.status == DbStatus::kOk);
    const auto& row = got.value.rows[0];
    assert(is_null(row[0]));
    assert(std::get<double>(row[1]) == 3.0);
}

static void test_insert_missing_notnull_is_invalid_request() {
    // 缺省 NOT NULL 列（无 DEFAULT）是客户端契约违反（漏传必填列），
    // 不是后端故障：映射 kInvalidRequest → HTTP 400（spec §7.3 的 400/500 分界）。
    TempDb t = open_seeded_db("notnull");

    InsertRequest ins{"users", {{"age", Value{std::int64_t{1}}}}};  // 缺 name
    const auto r = t.db->insert(ins);
    assert(r.status == DbStatus::kInvalidRequest);
    assert(contains(r.message, "constraint violation"));
}

// ---------------------------------------------------------------------------
// select：投影 / 全列顺序 / 0 行
// ---------------------------------------------------------------------------

static void test_select_projection_and_empty() {
    TempDb t = open_seeded_db("sel");
    assert(t.db->insert(make_user("alice", 30, 91.5, "a-1")).status == DbStatus::kOk);

    // 投影：请求顺序 = 返回顺序（口径 D4：取回后由适配层完成）。
    const auto proj = t.db->select(SelectRequest{"users", {"age", "name"}, where_id(1)});
    assert(proj.status == DbStatus::kOk);
    assert(proj.value.columns.size() == 2);
    assert(proj.value.columns[0] == "age");
    assert(proj.value.columns[1] == "name");
    assert(std::get<std::int64_t>(proj.value.rows[0][0]) == 30);
    assert(std::get<std::string>(proj.value.rows[0][1]) == "alice");

    // 全列：schema 顺序。
    const auto full = t.db->select(SelectRequest{"users", {}, where_id(1)});
    assert(full.value.columns.size() == 5);
    assert(full.value.columns[0] == "id");
    assert(full.value.columns[4] == "tag");

    // 0 行：kOk + 空集（SQL 语义，决策②）。
    const auto none = t.db->select(SelectRequest{"users", {}, where_id(999)});
    assert(none.status == DbStatus::kOk);
    assert(none.value.rows.empty());
    assert(none.value.columns.size() == 5 && "空集仍带列名");
}

// ---------------------------------------------------------------------------
// update：读改写合并 / 0 行
// ---------------------------------------------------------------------------

static void test_update_merges_untouched_columns() {
    TempDb t = open_seeded_db("upd");
    assert(t.db->insert(make_user("alice", 30, 91.5, "a-1")).status == DbStatus::kOk);

    // 只改 score；其余列必须保持（口径 D2 读改写）。
    UpdateRequest upd{"users", {{"score", Value{99.0}}}, where_id(1)};
    const auto r = t.db->update(upd);
    assert(r.status == DbStatus::kOk);
    assert(r.value == 1);

    const auto got = t.db->select(SelectRequest{"users", {}, where_id(1)});
    const auto& row = got.value.rows[0];
    assert(std::get<std::string>(row[1]) == "alice");      // 未动
    assert(std::get<std::int64_t>(row[2]) == 30);          // 未动
    assert(std::get<double>(row[3]) == 99.0);              // 已改
    assert(std::get<std::string>(row[4]) == "a-1");        // 未动
}

static void test_update_zero_rows_is_ok() {
    TempDb t = open_seeded_db("upd0");
    UpdateRequest upd{"users", {{"score", Value{1.0}}}, where_id(404)};
    const auto r = t.db->update(upd);
    assert(r.status == DbStatus::kOk);
    assert(r.value == 0);  // 决策②：0 行影响不是错误
}

// ---------------------------------------------------------------------------
// delete
// ---------------------------------------------------------------------------

static void test_delete_and_zero_rows() {
    TempDb t = open_seeded_db("del");
    assert(t.db->insert(make_user("alice", 30, 1.0, "a-1")).status == DbStatus::kOk);

    const auto r = t.db->remove(DeleteRequest{"users", where_id(1)});
    assert(r.status == DbStatus::kOk);
    assert(r.value == 1);

    const auto gone = t.db->select(SelectRequest{"users", {}, where_id(1)});
    assert(gone.status == DbStatus::kOk);
    assert(gone.value.rows.empty());

    // 再删：0 行，kOk。
    const auto r2 = t.db->remove(DeleteRequest{"users", where_id(1)});
    assert(r2.status == DbStatus::kOk);
    assert(r2.value == 0);
}

// ---------------------------------------------------------------------------
// :memory: shared-cache：两条连接共享同一内存库（spec §10.1 的正解）
// ---------------------------------------------------------------------------

static void test_shared_memory_two_connections() {
    if (g_with_cache) return;  // 排除清单：直接构造两条裸连接断言共享内存库
    const std::string uri = "file:cpppools_conformance_mem?mode=memory&cache=shared";

    // 裸连接先建表并保持打开（共享内存库随最后一个连接关闭而消失）。
    sqlite3* raw = nullptr;
    assert(sqlite3_open_v2(uri.c_str(), &raw,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI, nullptr) == SQLITE_OK);
    assert(sqlite3_exec(raw,
                        "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL,"
                        " age INTEGER, score REAL, tag TEXT UNIQUE)",
                        nullptr, nullptr, nullptr) == SQLITE_OK);

    config::DataSourceConfig cfg_a;
    cfg_a.name = "mem-a";
    cfg_a.type = "sqlite";
    cfg_a.sqlite_path = uri;
    DatabaseCreateResult a = make_sqlite_database(cfg_a);
    assert(a.db != nullptr);

    config::DataSourceConfig cfg_b = cfg_a;
    cfg_b.name = "mem-b";
    DatabaseCreateResult b = make_sqlite_database(cfg_b);
    assert(b.db != nullptr);

    // 连接 A 写、连接 B 读 —— 互见即共享成功。
    assert(a.db->insert(make_user("eve", 22, 5.0, "e-1")).status == DbStatus::kOk);
    const auto got = b.db->select(SelectRequest{"users", {"name"}, where_id(1)});
    assert(got.status == DbStatus::kOk);
    assert(got.value.rows.size() == 1);
    assert(std::get<std::string>(got.value.rows[0][0]) == "eve");

    // healthy() 走真实 SELECT 1（口径 D14）。
    assert(a.db->healthy());

    b.db.reset();
    a.db.reset();
    sqlite3_close(raw);
}

// ---------------------------------------------------------------------------
// statement cache 真实生效：同一实例重复操作命中缓存（行为不变 + 句柄复用）
// ---------------------------------------------------------------------------

static void test_repeated_ops_stay_correct() {
    if (g_with_cache) return;  // 排除清单：statement-cache 系列（断言句柄复用语境）
    TempDb t = open_seeded_db("repeat");
    for (std::int64_t i = 1; i <= 20; ++i) {
        InsertRequest ins{"users", {{"name", Value{"u" + std::to_string(i)}},
                                    {"age", Value{i}}}};
        const auto r = t.db->insert(ins);
        assert(r.status == DbStatus::kOk);
        assert(r.value.inserted_id.has_value());
        assert(*r.value.inserted_id == i);
    }
    for (std::int64_t i = 1; i <= 20; ++i) {
        const auto got = t.db->select(SelectRequest{"users", {"age"}, where_id(i)});
        assert(got.status == DbStatus::kOk);
        assert(got.value.rows.size() == 1);
        assert(std::get<std::int64_t>(got.value.rows[0][0]) == i);
    }
    // 改一半：statement 复用下状态不能串（reset/clear 正确性）。
    for (std::int64_t i = 1; i <= 10; ++i) {
        UpdateRequest upd{"users", {{"score", Value{static_cast<double>(i)}}}, where_id(i)};
        assert(t.db->update(upd).status == DbStatus::kOk);
    }
    const auto check = t.db->select(SelectRequest{"users", {"score"}, where_id(3)});
    assert(std::get<double>(check.value.rows[0][0]) == 3.0);
    const auto untouched = t.db->select(SelectRequest{"users", {"score"}, where_id(15)});
    assert(is_null(untouched.value.rows[0][0]));
}

// ---------------------------------------------------------------------------
// 评审修正批补充用例
// ---------------------------------------------------------------------------

static void test_update_unique_conflict_maps_to_already_exists() {
    TempDb t = open_seeded_db("upduniq");
    assert(t.db->insert(make_user("alice", 30, 1.0, "a-1")).status == DbStatus::kOk);
    assert(t.db->insert(make_user("bob", 20, 2.0, "b-1")).status == DbStatus::kOk);

    // 把 bob 的 tag 改成 alice 已占用的值 → UNIQUE 冲突 → kAlreadyExists。
    UpdateRequest upd{"users", {{"tag", Value{std::string("a-1")}}}, where_id(2)};
    const auto r = t.db->update(upd);
    assert(r.status == DbStatus::kAlreadyExists);
    assert(!r.message.empty());
}

static void test_update_can_change_where_column_itself() {
    TempDb t = open_seeded_db("updwhere");
    assert(t.db->insert(make_user("alice", 30, 1.0, "a-1")).status == DbStatus::kOk);

    // SET 全集包含 where 列本身：改主键 id 1 → 200。
    UpdateRequest upd{"users", {{"id", Value{std::int64_t{200}}}}, where_id(1)};
    const auto r = t.db->update(upd);
    assert(r.status == DbStatus::kOk);
    assert(r.value == 1);

    assert(t.db->select(SelectRequest{"users", {}, where_id(1)}).value.rows.empty());
    const auto got = t.db->select(SelectRequest{"users", {"name"}, where_id(200)});
    assert(got.status == DbStatus::kOk);
    assert(got.value.rows.size() == 1);
    assert(std::get<std::string>(got.value.rows[0][0]) == "alice");
}

static void test_projection_duplicate_columns_pinned() {
    // 行为钉住：请求重复列名时返回重复列（校验层不去重，投影按请求展开）。
    TempDb t = open_seeded_db("dupproj");
    assert(t.db->insert(make_user("alice", 30, 1.0, "a-1")).status == DbStatus::kOk);

    const auto r = t.db->select(SelectRequest{"users", {"age", "age"}, where_id(1)});
    assert(r.status == DbStatus::kOk);
    assert(r.value.columns.size() == 2);
    assert(r.value.columns[0] == "age" && r.value.columns[1] == "age");
    assert(std::get<std::int64_t>(r.value.rows[0][0]) == 30);
    assert(std::get<std::int64_t>(r.value.rows[0][1]) == 30);
}

static void test_rowid_alias_explicit_null_rejected_end_to_end() {
    // D5 端到端：真实自省出 id nullable=false → 显式 NULL 被校验层拒绝，
    // 不让 SQLite 静默换成 rowid。
    TempDb t = open_seeded_db("nullpk");
    InsertRequest ins{"users", {{"id", Value{}}, {"name", Value{std::string("x")}}}};
    const auto r = t.db->insert(ins);
    assert(r.status == DbStatus::kInvalidRequest);
    assert(contains(r.message, "does not accept NULL"));
}

static void test_partial_unique_index_not_marked_unique() {
    if (g_with_cache) return;  // 排除清单：直接构造后端、断言自省口径
    // partial UNIQUE 索引只在谓词子集上唯一 —— 不得标列级 unique，
    // 否则击穿「每操作至多一行」的安全不变量。
    const std::string path = make_temp_path("partial");
    sqlite3* raw = nullptr;
    assert(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
    assert(sqlite3_exec(raw,
                        "CREATE TABLE items(id INTEGER PRIMARY KEY, code TEXT, active INTEGER);"
                        "CREATE UNIQUE INDEX uq_active_code ON items(code) WHERE active = 1;",
                        nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);

    config::DataSourceConfig cfg;
    cfg.name = "partial";
    cfg.type = "sqlite";
    cfg.sqlite_path = path;
    DatabaseCreateResult result = make_sqlite_database(cfg);
    assert(result.db != nullptr);

    const ColumnInfo* code = result.db->schema().find_column("items", "code");
    assert(code != nullptr);
    assert(!code->unique && "partial unique 索引不得标列级 unique");

    // 以其为 where 被校验层拒绝。
    SelectRequest sel{"items", {"id"}, WhereClause{"code", Value{std::string("c1")}}};
    assert(result.db->select(sel).status == DbStatus::kInvalidRequest);

    result.db.reset();
    std::remove(path.c_str());
}

static void test_foreign_data_read_is_controlled() {
    // 读侧类型规范化：外部（绕过校验管道）写入的异构数据不得把
    // bad_variant_access 留给下游 —— 可无损提升的修正，否则明确报错。
    TempDb t = open_seeded_db("foreign");

    // NUMERIC 亲和列存整数：NUMERIC 亲和保留整数存储类，
    // 读回必须按 schema 口径提升为 double（与 MySQL REAL 恒回 double 一致）。
    sqlite3* raw = nullptr;
    assert(sqlite3_open(t.path.c_str(), &raw) == SQLITE_OK);
    assert(sqlite3_exec(raw, "INSERT INTO nums(id, v) VALUES(1, 7)", nullptr, nullptr, nullptr)
           == SQLITE_OK);
    // REAL 列塞文本：SQLite 动态类型允许，中间件必须报可读的错。
    assert(sqlite3_exec(raw,
                        "INSERT INTO users(id, name, score) VALUES(1, 'x', 'oops')",
                        nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);

    const auto nums = t.db->select(SelectRequest{"nums", {"v"}, where_id(1)});
    assert(nums.status == DbStatus::kOk);
    assert(nums.value.rows.size() == 1);
    assert(std::holds_alternative<double>(nums.value.rows[0][0]) &&
           "NUMERIC 列的整数存储必须提升为 double");
    assert(std::get<double>(nums.value.rows[0][0]) == 7.0);

    const auto bad = t.db->select(SelectRequest{"users", {"score"}, where_id(1)});
    assert(bad.status == DbStatus::kBackendError);
    assert(contains(bad.message, "foreign data of wrong type"));
    assert(contains(bad.message, "score"));
}


// Schema change after a prepare failure: cached invalid statement must be
// recovered (re-prepared) when the schema is restored.
static void test_schema_change_recovers_invalid_statement() {
    if (g_with_cache) return;  // CachingDatabase wraps select, skip under cache

    TempDb t = open_seeded_db("schema_change");

    // Insert a row first so select has something to find.
    InsertRequest ins{"users", {{"name", Value{std::string("alice")}}, {"age", Value{std::int64_t(30)}}}};
    const auto ir = t.db->insert(ins);
    assert(ir.status == DbStatus::kOk);

    // First select: primes the statement cache with a valid prepared statement.
    const auto r1 = t.db->select(SelectRequest{"users", {}, where_id(1)});
    assert(r1.status == DbStatus::kOk);
    assert(r1.value.rows.size() == 1);

    // Drop the table via a raw connection: cached SELECT is now stale.
    sqlite3* raw = nullptr;
    assert(sqlite3_open(t.path.c_str(), &raw) == SQLITE_OK);
    assert(sqlite3_exec(raw, "DROP TABLE users", nullptr, nullptr, nullptr) == SQLITE_OK);
    assert(sqlite3_exec(raw,
        "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL, age INTEGER, score REAL, tag TEXT UNIQUE)",
        nullptr, nullptr, nullptr) == SQLITE_OK);
    assert(sqlite3_exec(raw,
        "INSERT INTO users(id, name, age) VALUES(1, 'bob', 25)",
        nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);

    // Select again: the cached statement referenced the dropped table.
    // It should have been invalidated; the new select should re-prepare
    // against the restored schema and return the new row.
    const auto r2 = t.db->select(SelectRequest{"users", {}, where_id(1)});
    assert(r2.status == DbStatus::kOk);
    assert(r2.value.rows.size() == 1);
    // Verify we got the new data (name = "bob"), not a stale cached error.
    // Column index: id=0, name=1
    assert(std::holds_alternative<std::string>(r2.value.rows[0][1]));
    assert(std::get<std::string>(r2.value.rows[0][1]) == "bob");
}


// Composite primary key: columns should have primary_key=true but unique=false.
// Selecting by a single PK column should fail (not a unique where clause).
static void test_composite_primary_key_not_unique_per_column() {
    if (g_with_cache) return;

    TempDb t = open_seeded_db("compound_pk");
    const Schema& sch = t.db->schema();
    const TableInfo* tbl = sch.find_table("pk_pairs");
    assert(tbl != nullptr);
    for (const auto& col : tbl->columns) {
        assert(col.primary_key);
        assert(!col.unique && "composite PK columns must not be marked unique per column");
    }
    // Selecting by only column "a" should be rejected: not a unique where clause.
    const auto r = t.db->select(SelectRequest{"pk_pairs", {}, WhereClause{"a", Value{std::int64_t(1)}}});
    assert(r.status == DbStatus::kInvalidRequest);
}


// Two-connection concurrent update: both column changes must be preserved.
// SQLite in WAL mode allows concurrent readers, but do_update does a
// read-then-write. BEGIN IMMEDIATE serializes the read-modify-write so
// neither update is lost.
static void test_concurrent_update_preserves_both_columns() {
    if (g_with_cache) return;

    TempDb t = open_seeded_db("conc");
    assert(t.db->insert(make_user("alice", 30, 90.0, "t1")).status == DbStatus::kOk);

    // Open a second independent connection to the same file.
    config::DataSourceConfig cfg2;
    cfg2.name = "conc2";
    cfg2.type = "sqlite";
    cfg2.sqlite_path = t.path;
    DatabaseCreateResult r2 = make_sqlite_database(cfg2);
    assert(r2.db != nullptr);
    std::unique_ptr<Database> db2 = std::move(r2.db);

    std::atomic<bool> go{false};
    auto worker = [&go](Database* db, const std::string& col, const Value& v) {
        while (!go.load()) { /* spin-wait on barrier */ }
        UpdateRequest upd{"users", {{col, v}}, where_id(1)};
        db->update(upd);
    };

    std::thread t1(worker, t.db.get(), "age", Value{std::int64_t{31}});
    std::thread t2(worker, db2.get(), "score", Value{95.5});
    go.store(true);
    t1.join();
    t2.join();

    // Read the final row and verify both columns reflect their updates.
    const auto result = t.db->select(SelectRequest{"users", {}, where_id(1)});
    assert(result.status == DbStatus::kOk);
    assert(!result.value.rows.empty());
    const auto& row = result.value.rows[0];
    // Find the age and score columns.
    std::int64_t age = 0;
    double score = 0.0;
    for (std::size_t i = 0; i < result.value.columns.size(); ++i) {
        if (result.value.columns[i] == "age" && i < row.size()) {
            if (auto* p = std::get_if<std::int64_t>(&row[i])) age = *p;
        }
        if (result.value.columns[i] == "score" && i < row.size()) {
            if (auto* p = std::get_if<double>(&row[i])) score = *p;
        }
    }
    assert(age == 31 && "age column must reflect update from connection 1");
    assert(score == 95.5 && "score column must reflect update from connection 2");
}

}  // namespace

// CRUD 语义子集：两遍跑同一批用例（裸后端 / 套装饰器，spec §9）。
static void run_all_cases() {
    test_introspection();
    test_blob_column_rejects_datasource();
    test_open_missing_file_fails();
    test_insert_roundtrip_and_inserted_id();
    test_insert_explicit_pk();
    test_insert_unique_conflict();
    test_insert_null_and_real_accepts_integer();
    test_insert_missing_notnull_is_invalid_request();
    test_select_projection_and_empty();
    test_update_merges_untouched_columns();
    test_update_zero_rows_is_ok();
    test_delete_and_zero_rows();
    test_shared_memory_two_connections();
    test_repeated_ops_stay_correct();
    test_update_unique_conflict_maps_to_already_exists();
    test_update_can_change_where_column_itself();
    test_projection_duplicate_columns_pinned();
    test_rowid_alias_explicit_null_rejected_end_to_end();
    test_partial_unique_index_not_marked_unique();
    test_foreign_data_read_is_controlled();
    test_schema_change_recovers_invalid_statement();
    test_composite_primary_key_not_unique_per_column();
    test_concurrent_update_preserves_both_columns();
}

int main() {
    g_with_cache = false;
    run_all_cases();
    g_with_cache = true;
    run_all_cases();
    return 0;
}
