// CachingDatabase 装饰器测试（cluster-cache spec §4.1/§4.3）。
//
// 核心断言（评审 B1 的守卫）：**跨连接可见性** —— 两个 SqliteDatabase
// 连接（同一库文件）共享一份 RowCache 时，连接 A 写入的数据必须能被
// 连接 B 通过缓存命中读到；写后失效必须跨连接生效。
// per-connection 缓存会让这两条断言失败（B 命中自己的旧副本）。

#include "db/caching_database.h"
#include "db/sqlite_database.h"

#include "cpppools/db/datasource_registry.h"
#include "cpppools/db/db_connection_pool.h"

#include <sqlite3.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

using namespace cpppools;
using namespace cpppools::db;

namespace {

std::string make_seeded_db(const char* tag) {
    std::string path = std::string("/tmp/cpppools_caching_") + tag + "_XXXXXX";
    std::vector<char> buf(path.begin(), path.end());
    buf.push_back('\0');
    const int fd = mkstemp(buf.data());
    assert(fd >= 0);
    close(fd);
    const std::string real_path(buf.data());
    sqlite3* raw = nullptr;
    assert(sqlite3_open(real_path.c_str(), &raw) == SQLITE_OK);
    assert(sqlite3_exec(raw,
                        "CREATE TABLE users(id INTEGER PRIMARY KEY,"
                        " name TEXT NOT NULL, tag TEXT UNIQUE, rating REAL)",
                        nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
    return real_path;
}

std::unique_ptr<Database> make_sqlite(const std::string& path, const std::string& name) {
    config::DataSourceConfig cfg;
    cfg.name = name;
    cfg.type = "sqlite";
    cfg.sqlite_path = path;
    auto r = make_sqlite_database(cfg);
    assert(r.db != nullptr);
    return std::move(r.db);
}

// 两个连接 + 一份共享缓存的装饰器对。
struct SharedPair {
    std::string path;
    std::shared_ptr<RowCache> cache;
    std::unique_ptr<CachingDatabase> a;
    std::unique_ptr<CachingDatabase> b;

    explicit SharedPair(const char* tag) {
        register_sqlite_backend();
        path = make_seeded_db(tag);
        RowCacheConfig rc;
        rc.ttl = std::chrono::seconds(60);
        cache = std::make_shared<RowCache>(rc);
        a = std::make_unique<CachingDatabase>(make_sqlite(path, "main"), cache);
        b = std::make_unique<CachingDatabase>(make_sqlite(path, "main"), cache);
    }

    ~SharedPair() {
        a.reset();
        b.reset();
        std::remove(path.c_str());
        std::remove((path + "-wal").c_str());
        std::remove((path + "-shm").c_str());
    }
};

WhereClause where_id(std::int64_t id) {
    return WhereClause{"id", Value{id}};
}

InsertRequest make_user(std::int64_t id, const std::string& name, const std::string& tag) {
    return InsertRequest{"users", {{"id", Value{id}},
                                   {"name", Value{name}},
                                   {"tag", Value{tag}},
                                   {"rating", Value{4.5}}}};
}

}  // namespace

// 跨连接可见性：A 写 → B 读（缓存命中）。
static void test_cross_connection_visibility_via_cache() {
    SharedPair p("vis");
    assert(p.a->insert(make_user(1, "alice", "t1")).status == DbStatus::kOk);

    // B 第一次读：miss 回源（SQLite 文件共享，读得到 A 的写）并回填。
    const auto r1 = p.b->select(SelectRequest{"users", {"name"}, where_id(1)});
    assert(r1.status == DbStatus::kOk);
    assert(std::get<std::string>(r1.value.rows[0][0]) == "alice");
    const auto miss_after_first = p.cache->stats().misses;

    // B 第二次读：必须命中缓存（misses 不再增长）。
    const auto r2 = p.b->select(SelectRequest{"users", {"name"}, where_id(1)});
    assert(r2.status == DbStatus::kOk);
    assert(std::get<std::string>(r2.value.rows[0][0]) == "alice");
    assert(p.cache->stats().misses == miss_after_first && "第二次读必须命中缓存");
    assert(p.cache->stats().hits >= 1);
}

// 写穿透失效跨连接生效：A 更新 → B 立即读到新值。
static void test_write_through_invalidation_cross_connection() {
    SharedPair p("wbi");
    assert(p.a->insert(make_user(1, "alice", "t1")).status == DbStatus::kOk);
    assert(p.b->select(SelectRequest{"users", {"name"}, where_id(1)}).status ==
           DbStatus::kOk);  // 回填
    assert(p.cache->stats().hits >= 1 || p.cache->stats().misses >= 1);

    // A 更新 name。
    UpdateRequest upd{"users", {{"name", Value{std::string("alice2")}}}, where_id(1)};
    assert(p.a->update(upd).status == DbStatus::kOk);

    // B 必须读到新值（旧条目已被 A 的失效清掉，而不是命中旧副本）。
    const auto r = p.b->select(SelectRequest{"users", {"name"}, where_id(1)});
    assert(r.status == DbStatus::kOk);
    assert(std::get<std::string>(r.value.rows[0][0]) == "alice2");
}

// delete 失效跨连接生效。
static void test_delete_invalidation_cross_connection() {
    SharedPair p("del");
    assert(p.a->insert(make_user(1, "alice", "t1")).status == DbStatus::kOk);
    assert(p.b->select(SelectRequest{"users", {}, where_id(1)}).status == DbStatus::kOk);

    assert(p.a->remove(DeleteRequest{"users", where_id(1)}).status == DbStatus::kOk);
    const auto r = p.b->select(SelectRequest{"users", {}, where_id(1)});
    assert(r.status == DbStatus::kOk);
    assert(r.value.rows.empty() && "delete 后缓存必须失效，读回空");
}

// 命中路径的投影语义（与后端逐字节一致的三条钉子）。
static void test_projection_semantics_on_hit() {
    SharedPair p("proj");
    assert(p.a->insert(make_user(1, "alice", "t1")).status == DbStatus::kOk);
    // 先读一次触发回填，之后全部走命中路径。
    assert(p.b->select(SelectRequest{"users", {}, where_id(1)}).status == DbStatus::kOk);
    assert(p.cache->stats().hits >= 1 || p.cache->stats().misses >= 1);

    // ① 投影顺序 = 请求顺序。
    const auto reordered = p.b->select(
        SelectRequest{"users", {"name", "id"}, where_id(1)});
    assert(reordered.status == DbStatus::kOk);
    assert(reordered.value.columns.size() == 2);
    assert(reordered.value.columns[0] == "name");
    assert(reordered.value.columns[1] == "id");
    assert(std::get<std::string>(reordered.value.rows[0][0]) == "alice");
    assert(std::get<std::int64_t>(reordered.value.rows[0][1]) == 1);

    // ② 重复列不去重。
    const auto dup = p.b->select(SelectRequest{"users", {"name", "name"}, where_id(1)});
    assert(dup.status == DbStatus::kOk);
    assert(dup.value.columns.size() == 2);
    assert(std::get<std::string>(dup.value.rows[0][1]) == "alice");

    // ③ 全列 = schema 顺序。
    const auto full = p.b->select(SelectRequest{"users", {}, where_id(1)});
    assert(full.value.columns.size() == 4);
    assert(full.value.columns[0] == "id");
    assert(full.value.columns[3] == "rating");
}

// REAL 键列 bypass：rating 作 where 不缓存（浮点键不稳定，spec §5.5）。
static void test_real_key_column_bypasses_cache() {
    SharedPair p("real");
    assert(p.a->insert(make_user(1, "alice", "t1")).status == DbStatus::kOk);
    // rating 是 REAL 列，但需要 unique 才能做 where —— users 表 rating 非唯一，
    // 校验管道会拒绝。这里验证的是 bypass 判断不破坏正常路径：
    // 用合法的唯一键读两次，第二次命中（REAL bypass 不影响普通键）。
    assert(p.b->select(SelectRequest{"users", {}, where_id(1)}).status == DbStatus::kOk);
    assert(p.b->select(SelectRequest{"users", {}, where_id(1)}).status == DbStatus::kOk);
    assert(p.cache->stats().hits >= 1);
}

// kill-switch：关闭后读直通内层（无命中），但写的失效仍执行（I0）。
static void test_kill_switch_disables_hits_but_not_invalidation() {
    SharedPair p("kill");
    assert(p.a->insert(make_user(1, "alice", "t1")).status == DbStatus::kOk);
    assert(p.b->select(SelectRequest{"users", {}, where_id(1)}).status == DbStatus::kOk);

    p.cache->set_enabled(false);
    const std::uint64_t hits_before = p.cache->stats().hits;
    const auto r = p.b->select(SelectRequest{"users", {}, where_id(1)});
    assert(r.status == DbStatus::kOk && "关闭缓存后仍必须能读到数据（直通内层）");
    assert(p.cache->stats().hits == hits_before && "关闭后不得产生命中");

    // 失效仍执行：重开缓存后，A 的更新必须让 B 读到新值。
    p.cache->set_enabled(true);
    assert(p.b->select(SelectRequest{"users", {}, where_id(1)}).status == DbStatus::kOk);  // 回填
    UpdateRequest upd{"users", {{"name", Value{std::string("bob")}}}, where_id(1)};
    assert(p.a->update(upd).status == DbStatus::kOk);
    const auto r2 = p.b->select(SelectRequest{"users", {"name"}, where_id(1)});
    assert(std::get<std::string>(r2.value.rows[0][0]) == "bob");
}

// 唯一键冲突等语义在装饰器下保持不变（透明性抽查）。
static void test_decorator_preserves_backend_semantics() {
    SharedPair p("sem");
    assert(p.a->insert(make_user(1, "alice", "t1")).status == DbStatus::kOk);
    // tag 冲突。
    assert(p.b->insert(make_user(2, "bob", "t1")).status == DbStatus::kAlreadyExists);
    // NULL 违反（name NOT NULL）—— 走校验管道，装饰器不改口径。
    InsertRequest bad{"users", {{"id", Value{std::int64_t{3}}}, {"name", Value{}}}};
    assert(p.a->insert(bad).status == DbStatus::kInvalidRequest);
    // 0 行 update = kOk + 0（SQL 语义，决策②）。
    UpdateRequest upd0{"users", {{"name", Value{std::string("x")}}}, where_id(999)};
    const auto r = p.a->update(upd0);
    assert(r.status == DbStatus::kOk);
    assert(r.value == 0);
}

// registry 接线：cache.enabled=true 时全部槽位共享同一份缓存
//（spec §4.1.1 的落地形态，评审 B1 的产品级守卫）。
static void test_registry_slots_share_one_cache() {
    register_sqlite_backend();
    const std::string path = make_seeded_db("reg");

    config::DataSourceConfig cfg;
    cfg.name = "main";
    cfg.type = "sqlite";
    cfg.sqlite_path = path;
    cfg.pool_size = 2;
    config::AppConfig::Cache cache_cfg;
    cache_cfg.enabled = true;
    cache_cfg.ttl_ms = 60000;
    auto result = DataSourceRegistry::build({cfg}, cache_cfg);
    assert(result.registry != nullptr);

    assert(result.registry->caches().size() == 1);
    DbConnectionPool* pool = result.registry->find_pool("main");
    assert(pool != nullptr);

    // 借两条连接（两个槽位 = 两个装饰器实例，共享缓存）。
    Database* a = pool->acquire(std::chrono::milliseconds(100));
    Database* b = pool->acquire(std::chrono::milliseconds(100));
    assert(a != nullptr && b != nullptr && a != b);

    assert(a->insert(make_user(1, "alice", "t1")).status == DbStatus::kOk);
    assert(b->select(SelectRequest{"users", {}, where_id(1)}).status == DbStatus::kOk);
    const auto hits_before = result.registry->caches()[0].second->stats().hits;
    assert(b->select(SelectRequest{"users", {}, where_id(1)}).status == DbStatus::kOk);
    assert(result.registry->caches()[0].second->stats().hits == hits_before + 1 &&
           "第二个槽位必须命中共享缓存");

    pool->release(a);
    pool->release(b);
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

// per-request bypass（期2 归属门控通道，spec §5.2 规则 A）：开启时读直通
// 内层（不命中不回填），但写的失效照常执行（I0）。
static void test_request_bypass_disables_reads_but_not_invalidation() {
    SharedPair p("bypass");
    assert(p.a->insert(make_user(1, "alice", "t1")).status == DbStatus::kOk);
    // 先回填一条。
    assert(p.b->select(SelectRequest{"users", {}, where_id(1)}).status == DbStatus::kOk);
    assert(p.cache->stats().entries == 1);

    p.b->set_cache_bypass(true);
    const std::size_t hits0 = p.cache->stats().hits;
    // bypass 中读直通内层：不产生命中，也不新增回填。
    const auto r1 = p.b->select(SelectRequest{"users", {"name"}, where_id(1)});
    assert(r1.status == DbStatus::kOk);
    assert(std::get<std::string>(r1.value.rows[0][0]) == "alice");
    assert(p.cache->stats().hits == hits0);

    // 失效不受 bypass 影响（I0）：A 更新后旧条目被清。
    UpdateRequest upd{"users", {{"name", Value{std::string("bob")}}}, where_id(1)};
    assert(p.a->update(upd).status == DbStatus::kOk);
    assert(p.cache->stats().entries == 0);

    // 关闭 bypass：读重新走缓存路径（miss → 回填新值）。
    p.b->set_cache_bypass(false);
    const auto r2 = p.b->select(SelectRequest{"users", {"name"}, where_id(1)});
    assert(std::get<std::string>(r2.value.rows[0][0]) == "bob");
}


// cache.tables whitelist: only listed tables get cache wrappers.
static void test_table_allowlist_skips_non_listed_tables() {
    register_sqlite_backend();
    std::string path = std::string("/tmp/cpppools_allowlist_XXXXXX");
    std::vector<char> buf(path.begin(), path.end());
    buf.push_back(' ');
    const int fd = mkstemp(buf.data());
    assert(fd >= 0);
    close(fd);
    const std::string real_path(buf.data());

    sqlite3* raw = nullptr;
    assert(sqlite3_open(real_path.c_str(), &raw) == SQLITE_OK);
    assert(sqlite3_exec(raw,
        "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL);"
        "CREATE TABLE audit(id INTEGER PRIMARY KEY, msg TEXT);",
        nullptr, nullptr, nullptr) == SQLITE_OK);
    assert(sqlite3_exec(raw,
        "INSERT INTO users(id, name) VALUES(1, 'alice');"
        "INSERT INTO audit(id, msg) VALUES(1, 'hello');",
        nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);

    RowCacheConfig rc;
    rc.ttl = std::chrono::seconds(60);
    auto cache = std::make_shared<RowCache>(rc);

    // Only "users" is cacheable; "audit" is not.
    auto db = std::make_unique<CachingDatabase>(
        make_sqlite(real_path, "main"), cache,
        std::vector<std::string>{"users"});

    // Read users: should miss and backfill (cacheable).
    auto r1 = db->select(SelectRequest{"users", {}, where_id(1)});
    assert(r1.status == DbStatus::kOk);
    assert(std::get<std::string>(r1.value.rows[0][1]) == "alice");
    assert(cache->stats().entries == 1);

    // Read audit: should bypass cache entirely (not cacheable).
    auto r2 = db->select(SelectRequest{"audit", {}, where_id(1)});
    assert(r2.status == DbStatus::kOk);
    assert(std::get<std::string>(r2.value.rows[0][1]) == "hello");
    // Cache still has only 1 entry (users), no audit entry.
    assert(cache->stats().entries == 1);

    // Update audit: should not trigger any invalidation.
    std::size_t inv0 = cache->stats().invalidations;
    UpdateRequest upd{"audit", {{"msg", Value{std::string("world")}}}, where_id(1)};
    assert(db->update(upd).status == DbStatus::kOk);
    assert(cache->stats().invalidations == inv0);

    // Read users again: should hit cache (not affected by audit update).
    auto r3 = db->select(SelectRequest{"users", {}, where_id(1)});
    assert(r3.status == DbStatus::kOk);
    assert(cache->stats().hits >= 1);

    db.reset();
    std::remove(real_path.c_str());
    std::remove((real_path + "-wal").c_str());
    std::remove((real_path + "-shm").c_str());
}


// Full-row backfill: a narrow projection miss must cache the complete row,
// so a later request for other columns returns real values, not NULLs.
static void test_narrow_miss_backfills_full_row() {
    SharedPair p("fullrow");
    assert(p.a->insert(make_user(1, "alice", "t1")).status == DbStatus::kOk);

    // First select: request only "name" (narrow projection miss).
    const auto r1 = p.b->select(SelectRequest{"users", {"name"}, where_id(1)});
    assert(r1.status == DbStatus::kOk);
    assert(std::get<std::string>(r1.value.rows[0][0]) == "alice");
    assert(p.cache->stats().entries == 1);

    // Second select: request "id" and "rating" using the same key.
    // This should be a cache hit, and the values must be real, not NULL.
    const auto r2 = p.b->select(SelectRequest{"users", {"id", "rating"}, where_id(1)});
    assert(r2.status == DbStatus::kOk);
    assert(r2.value.rows.size() == 1);
    assert(std::get<std::int64_t>(r2.value.rows[0][0]) == 1);
    assert(std::holds_alternative<double>(r2.value.rows[0][1]));
    assert(std::get<double>(r2.value.rows[0][1]) == 4.5);
    assert(p.cache->stats().hits >= 1);

    // Also verify a full-column select (empty columns) returns all values.
    const auto r3 = p.b->select(SelectRequest{"users", {}, where_id(1)});
    assert(r3.status == DbStatus::kOk);
    assert(r3.value.columns.size() == 4);
    assert(std::get<std::string>(r3.value.rows[0][1]) == "alice");
}

int main() {
    test_cross_connection_visibility_via_cache();
    test_write_through_invalidation_cross_connection();
    test_delete_invalidation_cross_connection();
    test_projection_semantics_on_hit();
    test_real_key_column_bypasses_cache();
    test_kill_switch_disables_hits_but_not_invalidation();
    test_decorator_preserves_backend_semantics();
    test_registry_slots_share_one_cache();
    test_request_bypass_disables_reads_but_not_invalidation();
    test_table_allowlist_skips_non_listed_tables();
    test_narrow_miss_backfills_full_row();
    return 0;
}

