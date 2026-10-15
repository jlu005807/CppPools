// cache_* 指标族与 kill-switch 端点测试（cluster-cache spec §8）。
//
// 走真实 HttpServer + DataSourceRegistry（cache.enabled=true）：
// ① metrics 含 cache_* 指标族且随请求变化；
// ② stats 含 cache 聚合字段；
// ③ POST /api/v1/cache/disable|enable 真正改变命中行为（I0：失效仍执行）。

#include "cpppools/db/datasource_registry.h"
#include "cpppools/db/db_connection_pool.h"
#include "cpppools/pool/object_pool.h"
#include "cpppools/pool/thread_pool.h"
#include "cpppools/server/http_server.h"
#include "db/sqlite_database.h"
#include "test_temp_file.h"

#include <sqlite3.h>

#include <cassert>
#include <cstdio>
#include <memory>
#include <string>

using namespace cpppools;

namespace {

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

struct Fixture {
    std::string db_path;
    std::unique_ptr<db::DataSourceRegistry> registry;
    pool::ThreadPool thread_pool{[] {
        pool::ThreadPoolConfig c;
        c.name = "worker";
        c.worker_count = 1;
        return c;
    }()};
    pool::ObjectPool<server::RequestContext> request_pool{"request", 4, 8};
    std::unique_ptr<server::HttpServer> server;

    Fixture() {
        db::register_sqlite_backend();
        db_path = [] {
            const std::string real_path = cpppools::testing::make_test_temp_file(
                "cpppools_cachemetrics");
            sqlite3* raw = nullptr;
            assert(sqlite3_open(real_path.c_str(), &raw) == SQLITE_OK);
            assert(sqlite3_exec(raw,
                                "CREATE TABLE users(id INTEGER PRIMARY KEY,"
                                " name TEXT NOT NULL)",
                                nullptr, nullptr, nullptr) == SQLITE_OK);
            sqlite3_close(raw);
            return real_path;
        }();

        config::DataSourceConfig cfg;
        cfg.name = "main";
        cfg.type = "sqlite";
        cfg.is_default = true;
        cfg.pool_size = 2;
        cfg.sqlite_path = db_path;
        config::AppConfig::Cache cache_cfg;
        cache_cfg.enabled = true;
        cache_cfg.ttl_ms = 60000;
        auto result = db::DataSourceRegistry::build({cfg}, cache_cfg);
        assert(result.registry != nullptr);
        registry = std::move(result.registry);

        server = std::make_unique<server::HttpServer>(
            0, thread_pool, request_pool, *registry);
    }

    ~Fixture() {
        server.reset();
        registry.reset();
        std::remove(db_path.c_str());
        std::remove((db_path + "-wal").c_str());
        std::remove((db_path + "-shm").c_str());
    }

    server::ApiResponse post(const std::string& path, const std::string& body,
                             const std::string& extra_headers = "") {
        return server->route_request(
            "POST " + path + " HTTP/1.1\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n" +
                extra_headers + "\r\n" + body,
            true, body);
    }

    server::ApiResponse get(const std::string& path) {
        return server->route_request("GET " + path + " HTTP/1.1\r\n", true);
    }

    long long metric_value(const std::string& name) {
        const std::string body = get("/metrics").body;
        const std::string prefix = name + "{pool=\"main\"} ";
        const auto pos = body.find(prefix);
        assert(pos != std::string::npos && "指标缺失");
        return std::atoll(body.c_str() + pos + prefix.size());
    }

    long long flush_metric() {
        const std::string body = get("/metrics").body;
        const std::string prefix =
            "cpppools_cache_flush_total{pool=\"main\",reason=\"rejoin\"} ";
        const auto pos = body.find(prefix);
        assert(pos != std::string::npos && "flush 指标缺失");
        return std::atoll(body.c_str() + pos + prefix.size());
    }
};

}  // namespace

static void test_metrics_expose_cache_family() {
    Fixture f;
    const std::string body = f.get("/metrics").body;
    for (const char* metric : {"cpppools_cache_hits_total", "cpppools_cache_misses_total",
                               "cpppools_cache_entries", "cpppools_cache_bytes",
                               "cpppools_cache_evictions_total",
                               "cpppools_cache_invalidations_total",
                               "cpppools_cache_singleflight_merged_total",
                               "cpppools_cache_backfill_dropped_total",
                               "cpppools_cache_bypass_total"}) {
        assert(contains(body, metric));
    }
    assert(contains(body, "cpppools_cache_hits_total{pool=\"main\"}"));
}

static void test_metrics_track_hits_and_invalidations() {
    Fixture f;
    // 插入一行，读两次（第二次必命中）。
    assert(f.post("/api/v1/insert",
                  R"({"table":"users","values":{"id":1,"name":"alice"}})")
               .status_code == 201);
    assert(f.post("/api/v1/select",
                  R"({"table":"users","where":{"column":"id","value":1}})")
               .status_code == 200);
    assert(f.post("/api/v1/select",
                  R"({"table":"users","where":{"column":"id","value":1}})")
               .status_code == 200);
    assert(f.metric_value("cpppools_cache_hits_total") >= 1);
    assert(f.metric_value("cpppools_cache_entries") >= 1);

    // 写穿透失效计数。
    const long long inv_before = f.metric_value("cpppools_cache_invalidations_total");
    assert(f.post("/api/v1/update",
                  R"({"table":"users","set":{"name":"bob"},)"
                  R"("where":{"column":"id","value":1}})")
               .status_code == 200);
    assert(f.metric_value("cpppools_cache_invalidations_total") > inv_before);
}

static void test_stats_expose_cache_aggregates() {
    Fixture f;
    const std::string body = f.get("/api/v1/stats").body;
    assert(contains(body, "\"cache_enabled\":true"));
    assert(contains(body, "\"cache_entries\":"));
    assert(contains(body, "\"cache_hits_total\":"));
    assert(contains(body, "\"cache_misses_total\":"));
    // 集群身份（spec §8.1）：单节点退化口径。
    assert(contains(body, "\"cluster_epoch\":"));
    assert(contains(body, "\"node_id\":\"single\""));
}

static void test_kill_switch_endpoint_controls_hits() {
    Fixture f;
    assert(f.post("/api/v1/insert",
                  R"({"table":"users","values":{"id":1,"name":"alice"}})")
               .status_code == 201);
    // 读一次回填。
    assert(f.post("/api/v1/select",
                  R"({"table":"users","where":{"column":"id","value":1}})")
               .status_code == 200);

    // 关闭缓存。
    const auto off = f.post("/api/v1/cache/disable", "");
    assert(off.status_code == 200);
    assert(contains(off.body, "\"enabled\":false"));
    assert(contains(off.body, "\"affected_datasources\":1"));

    const long long hits_before = f.metric_value("cpppools_cache_hits_total");
    assert(f.post("/api/v1/select",
                  R"({"table":"users","where":{"column":"id","value":1}})")
               .status_code == 200 && "关闭后仍可读（直通内层）");
    assert(f.metric_value("cpppools_cache_hits_total") == hits_before &&
           "关闭后不得产生新命中");
    assert(contains(f.get("/api/v1/stats").body, "\"cache_enabled\":false"));

    // 失效仍执行（I0）：关闭期间更新，重开后读必为新值。
    assert(f.post("/api/v1/update",
                  R"({"table":"users","set":{"name":"carol"},)"
                  R"("where":{"column":"id","value":1}})")
               .status_code == 200);
    const auto on = f.post("/api/v1/cache/enable", "");
    assert(on.status_code == 200);
    assert(contains(on.body, "\"enabled\":true"));
    const auto r = f.post("/api/v1/select",
                          R"({"table":"users","columns":["name"],)"
                          R"("where":{"column":"id","value":1}})");
    assert(r.status_code == 200);
    assert(contains(r.body, "carol") && "重开缓存后不得读到旧值");
}

// Rejoin 头（spec §5.2 规则 B）：清空本地缓存加速归队收敛；10s 内至多一次
//（节流），flush 指标按 {pool,reason="rejoin"} 导出（§8.1）。
static void test_rejoin_header_flushes_cache_with_throttle() {
    Fixture f;
    assert(f.post("/api/v1/insert",
                  R"({"table":"users","values":{"id":1,"name":"alice"}})")
               .status_code == 201);
    assert(f.post("/api/v1/select",
                  R"({"table":"users","where":{"column":"id","value":1}})")
               .status_code == 200);
    assert(f.metric_value("cpppools_cache_entries") >= 1);

    // 带 Rejoin 头的请求：清空发生在请求处理前，因此本次 select 必然
    // miss（命中被清掉的证据），随后自行回填一条。
    const long long hits0 = f.metric_value("cpppools_cache_hits_total");
    const long long misses0 = f.metric_value("cpppools_cache_misses_total");
    const auto r1 = f.post("/api/v1/select",
                           R"({"table":"users","where":{"column":"id","value":1}})",
                           "X-CppPools-Rejoin: 1\r\n");
    assert(r1.status_code == 200);
    assert(f.flush_metric() == 1);
    assert(f.metric_value("cpppools_cache_hits_total") == hits0 &&
           "Rejoin 请求自身必须 miss（缓存已被清空）");
    assert(f.metric_value("cpppools_cache_misses_total") == misses0 + 1);

    // 10s 窗口内再来一次：节流（flush 不增长），且这次读命中了
    // 上一次回填的条目 —— 证明未发生第二次清空。
    f.post("/api/v1/select",
           R"({"table":"users","where":{"column":"id","value":1}})",
           "X-CppPools-Rejoin: 1\r\n");
    assert(f.flush_metric() == 1 && "10s 内至多清一次（节流）");
    assert(f.metric_value("cpppools_cache_hits_total") == hits0 + 1 &&
           "被节流时不得二次清空（上次回填的条目仍在）");

    // 清空后回填照常（规则 B 只清旧条目，不改变缓存能力）。
    assert(f.post("/api/v1/select",
                  R"({"table":"users","where":{"column":"id","value":1}})")
               .status_code == 200);
    assert(f.metric_value("cpppools_cache_entries") >= 1);
}

int main() {
    test_metrics_expose_cache_family();
    test_metrics_track_hits_and_invalidations();
    test_stats_expose_cache_aggregates();
    test_kill_switch_endpoint_controls_hits();
    test_rejoin_header_flushes_cache_with_throttle();
    return 0;
}
