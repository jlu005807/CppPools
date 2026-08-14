// OwnershipRouter 归属决策测试（cluster-cache spec §3.6 决策矩阵）。
//
// 本文件两层：
// ① 路由器单测（本部分）：决策矩阵五行逐行 + 单节点退化 + 计数器；
// ② HTTP 级集成（期2-T3 追加）：真实 HttpServer 上的 307/epoch/Fallback。
//
// 归属真值不依赖被测实现：测试内用 §3.1 冻结口径独立建环求 owner，
// 再断言 decide 的行为 —— 实现漂移与测试共谋的可能性被切断。

#include "server/hash_ring.h"
#include "server/ownership_router.h"

#include "db/sqlite_database.h"

#include "cpppools/db/datasource_registry.h"
#include "cpppools/server/http_server.h"
#include "cpppools/pool/object_pool.h"
#include "cpppools/pool/thread_pool.h"

#include <sqlite3.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

using cpppools::db::Value;
using cpppools::server::ClusterView;
using cpppools::server::HashRing;
using cpppools::server::OwnershipDecision;
using cpppools::server::OwnershipRouter;
using cpppools::server::hash_ring::query_key;

namespace {

ClusterView three_node_view() {
    ClusterView cv;
    cv.node_id = "node-a";
    cv.epoch = 5;
    cv.nodes = {
        {"node-a", "127.0.0.1", 9001},
        {"node-b", "127.0.0.1", 9002},
        {"node-c", "127.0.0.1", 9003},
    };
    cv.vnodes_per_node = 100;
    return cv;
}

// 独立建环（§3.1 口径）：测试自己算 owner，不借道被测代码。
HashRing independent_ring(const ClusterView& cv) {
    HashRing ring;
    for (const auto& n : cv.nodes) {
        ring.add_node(n.node_id, cv.vnodes_per_node);
    }
    return ring;
}

// 找一个归属 self 的键与一个归属他者的键（确定性搜索，不硬编码哈希）。
struct KeyPair {
    std::string owned_id;      // 归属 self 的键值（id 列）
    std::string foreign_id;    // 归属他者的键值
    std::string foreign_owner;
};

KeyPair find_keys(const ClusterView& cv) {
    const HashRing ring = independent_ring(cv);
    KeyPair out;
    for (std::int64_t i = 1; i < 100000; ++i) {
        const std::string id = std::to_string(i);
        const std::string key = query_key("main", "users", "id", id, 0);
        const std::string owner = ring.owner(key);
        if (out.owned_id.empty() && owner == cv.node_id) {
            out.owned_id = id;
        }
        if (out.foreign_id.empty() && owner != cv.node_id) {
            out.foreign_id = id;
            out.foreign_owner = owner;
        }
        if (!out.owned_id.empty() && !out.foreign_id.empty()) {
            return out;
        }
    }
    assert(false && "10 万个键都找不到双归属，环实现有问题");
    return out;
}

const ClusterView::Node& node_of(const ClusterView& cv, const std::string& id) {
    for (const auto& n : cv.nodes) {
        if (n.node_id == id) {
            return n;
        }
    }
    assert(false && "owner 不在 nodes 表");
    return cv.nodes[0];
}

}  // namespace

// 五行决策矩阵（spec §3.6，逐行断言）。
static void test_decision_matrix_five_rows() {
    const ClusterView cv = three_node_view();
    const KeyPair keys = find_keys(cv);
    OwnershipRouter router(cv);

    // R1 owner + 无头 → 本地处理，缓存允许。
    OwnershipDecision d1 = router.decide("main", "users", "id",
                                         Value{std::stoll(keys.owned_id)},
                                         /*has_fallback=*/false, /*epoch=*/"",
                                         "main");
    assert(d1.action == OwnershipDecision::Action::kLocal);
    assert(d1.cache_allowed);
    assert(!d1.epoch_mismatch);

    // R2 owner + Fallback 头 → 仍本地、仍允许缓存（提示只能更保守，
    // 而 owner 上的 fallback 头不构成更保守的理由）。
    OwnershipDecision d2 = router.decide("main", "users", "id",
                                         Value{std::stoll(keys.owned_id)},
                                         /*has_fallback=*/true, "", "main");
    assert(d2.action == OwnershipDecision::Action::kLocal);
    assert(d2.cache_allowed);

    // R3 owner + epoch 不符 → 本地处理但 bypass 缓存，且须回报 epoch。
    OwnershipDecision d3 = router.decide("main", "users", "id",
                                         Value{std::stoll(keys.owned_id)},
                                         false, "4", "main");
    assert(d3.action == OwnershipDecision::Action::kLocal);
    assert(!d3.cache_allowed);
    assert(d3.epoch_mismatch);

    // R3b owner + epoch 相符 → 与无头等价（允许缓存）。
    OwnershipDecision d3b = router.decide("main", "users", "id",
                                          Value{std::stoll(keys.owned_id)},
                                          false, "5", "main");
    assert(d3b.cache_allowed && !d3b.epoch_mismatch);

    // R4 非 owner + 无 Fallback → 307 重定向到 owner（Location = owner 基址）。
    OwnershipDecision d4 = router.decide("main", "users", "id",
                                         Value{std::stoll(keys.foreign_id)},
                                         false, "", "main");
    assert(d4.action == OwnershipDecision::Action::kRedirect);
    assert(!d4.cache_allowed);
    const ClusterView::Node& owner = node_of(cv, keys.foreign_owner);
    const std::string expected_loc =
        "http://" + owner.host + ":" + std::to_string(owner.port);
    assert(d4.location == expected_loc);

    // R5 非 owner + Fallback 头 → 就地处理、bypass 缓存（失效不受影响，I0）。
    OwnershipDecision d5 = router.decide("main", "users", "id",
                                         Value{std::stoll(keys.foreign_id)},
                                         true, "", "main");
    assert(d5.action == OwnershipDecision::Action::kLocal);
    assert(!d5.cache_allowed);
    assert(!d5.epoch_mismatch);
}

// 单节点模式（nodes 空）：恒本地、恒允许缓存 —— 期1 行为逐字节不变的前提。
static void test_single_node_always_local_and_cacheable() {
    ClusterView cv;  // node_id="single", epoch=0, nodes 空
    OwnershipRouter router(cv);
    OwnershipDecision d = router.decide("main", "users", "id", Value{int64_t{42}},
                                        false, "", "main");
    assert(d.action == OwnershipDecision::Action::kLocal);
    assert(d.cache_allowed);
    assert(!d.epoch_mismatch);
    // 单节点下即使带错误 epoch 头也不 bypass（无集群拓扑可言，
    // epoch 门控只服务多节点滚动重启场景；且单节点缓存是唯一副本，
    // bypass 只会白白回源）。
    OwnershipDecision d2 = router.decide("main", "users", "id", Value{int64_t{42}},
                                         false, "99", "main");
    assert(d2.cache_allowed && !d2.epoch_mismatch);
    assert(router.stats().redirects == 0);
}

// 计数器：redirects 与 bypass reason 随决策增长（§8.1 指标取数入口）。
static void test_counters_track_decisions() {
    const ClusterView cv = three_node_view();
    const KeyPair keys = find_keys(cv);
    OwnershipRouter router(cv);
    const auto s0 = router.stats();
    assert(s0.redirects == 0);
    assert(s0.bypass_not_owner.empty() && s0.bypass_epoch.empty());

    router.decide("main", "users", "id", Value{std::stoll(keys.foreign_id)}, false, "", "main");
    router.decide("main", "users", "id", Value{std::stoll(keys.foreign_id)}, true, "", "main");
    router.decide("main", "users", "id", Value{std::stoll(keys.owned_id)}, false, "4", "main");

    const auto s1 = router.stats();
    assert(s1.redirects == 1);
    assert(s1.bypass_fallback.at("main") == 1);   // R5：客户端申报绕行（故障信号）
    assert(s1.bypass_not_owner.empty());          // 配置错位退化未发生
    assert(s1.bypass_epoch.at("main") == 1);      // R3：epoch 门控
}

// 路由键哈希口径与黄金向量同源：V3 键在 decide 内编码必须逐字节等于
// query_key("main","users","id","42",0) —— 用独立环反查 owner 对拍。
static void test_hash_encoding_matches_golden_spec() {
    ClusterView cv = three_node_view();
    OwnershipRouter router(cv);
    const HashRing ring = independent_ring(cv);
    // 任意键：decide 的重定向目标必须与独立环的 owner 一致（编码同源的
    // 可观测推论 —— 编码漂移会让两者指向不同节点）。
    for (std::int64_t id = 1; id <= 50; ++id) {
        const std::string expected_owner = ring.owner(
            query_key("main", "users", "id", std::to_string(id), 0));
        OwnershipDecision d = router.decide("main", "users", "id", Value{id},
                                            false, "", "main");
        if (expected_owner == cv.node_id) {
            assert(d.action == OwnershipDecision::Action::kLocal);
        } else {
            assert(d.action == OwnershipDecision::Action::kRedirect);
            const ClusterView::Node& owner = node_of(cv, expected_owner);
            assert(d.location ==
                   "http://" + owner.host + ":" + std::to_string(owner.port));
        }
    }
}

// ===================================================================
// HTTP 级：真实 HttpServer + Registry（cache.enabled=true）上钉决策矩阵。
// ===================================================================

namespace {

// 两节点视图：self=node-a（本进程），node-b 在另一端口（本测试不真起）。
ClusterView http_cluster_view() {
    ClusterView cv;
    cv.node_id = "node-a";
    cv.epoch = 7;
    cv.nodes = {
        {"node-a", "127.0.0.1", 18101},
        {"node-b", "127.0.0.1", 18102},
    };
    cv.vnodes_per_node = 100;
    return cv;
}

struct HttpFixture {
    std::string db_path;
    std::unique_ptr<cpppools::db::DataSourceRegistry> registry;
    cpppools::pool::ThreadPool thread_pool{[] {
        cpppools::pool::ThreadPoolConfig c;
        c.name = "worker";
        c.worker_count = 1;
        return c;
    }()};
    cpppools::pool::ObjectPool<cpppools::server::RequestContext> request_pool{
        "request", 4, 8};
    std::unique_ptr<cpppools::server::HttpServer> server;
    ClusterView cv;

    HttpFixture() {
        cpppools::db::register_sqlite_backend();
        db_path = [] {
            std::string path = "/tmp/cpppools_ownership_XXXXXX";
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
                                " name TEXT NOT NULL)",
                                nullptr, nullptr, nullptr) == SQLITE_OK);
            sqlite3_close(raw);
            return real_path;
        }();

        cpppools::config::DataSourceConfig cfg;
        cfg.name = "main";
        cfg.type = "sqlite";
        cfg.is_default = true;
        cfg.pool_size = 2;
        cfg.sqlite_path = db_path;
        cpppools::config::AppConfig::Cache cache_cfg;
        cache_cfg.enabled = true;
        cache_cfg.ttl_ms = 60000;
        auto result = cpppools::db::DataSourceRegistry::build({cfg}, cache_cfg);
        assert(result.registry != nullptr);
        registry = std::move(result.registry);

        cv = http_cluster_view();
        cpppools::server::HttpServerConfig scfg;
        scfg.cluster_view = cv;
        server = std::make_unique<cpppools::server::HttpServer>(
            0, thread_pool, request_pool, *registry, std::move(scfg));
    }

    ~HttpFixture() {
        server.reset();
        registry.reset();
        std::remove(db_path.c_str());
        std::remove((db_path + "-wal").c_str());
        std::remove((db_path + "-shm").c_str());
    }

    cpppools::server::ApiResponse post(const std::string& path,
                                       const std::string& body,
                                       const std::string& extra_headers = "") {
        return server->route_request(
            "POST " + path + " HTTP/1.1\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n" +
                extra_headers + "\r\n" + body,
            true, body);
    }

    std::string header_of(const cpppools::server::ApiResponse& resp,
                          const std::string& name) {
        for (const auto& [k, v] : resp.extra_headers) {
            if (k == name) {
                return v;
            }
        }
        return std::string{};
    }

    long long metric_value(const std::string& name) {
        const std::string body =
            server->route_request("GET /metrics HTTP/1.1\r\n", true).body;
        // 锚定行首：避免误中 HELP/TYPE 注释行中的同名前缀。
        const std::string prefix = "\n" + name + " ";
        const auto pos = body.find(prefix);
        if (pos == std::string::npos) {
            return -1;
        }
        return std::atoll(body.c_str() + pos + prefix.size());
    }

    long long cache_metric(const std::string& name) {
        const std::string body =
            server->route_request("GET /metrics HTTP/1.1\r\n", true).body;
        const std::string prefix = name + "{pool=\"main\"} ";
        const auto pos = body.find(prefix);
        assert(pos != std::string::npos && "缓存指标缺失");
        return std::atoll(body.c_str() + pos + prefix.size());
    }
};

// 归属真值：独立建环（D1 口径：未显式指定 datasource → 空串）。
std::string http_owner_of(const ClusterView& cv, std::int64_t id) {
    HashRing ring;
    for (const auto& n : cv.nodes) {
        ring.add_node(n.node_id, cv.vnodes_per_node);
    }
    return ring.owner(query_key("", "users", "id", std::to_string(id), 0));
}

std::int64_t find_http_key(const ClusterView& cv, bool want_self) {
    for (std::int64_t id = 1; id < 100000; ++id) {
        const bool is_self = http_owner_of(cv, id) == cv.node_id;
        if (is_self == want_self) {
            return id;
        }
    }
    assert(false);
    return -1;
}

std::string select_body(std::int64_t id) {
    return "{\"table\":\"users\",\"where\":{\"column\":\"id\",\"value\":" +
           std::to_string(id) + "}}";
}

std::string insert_body(std::int64_t id, const std::string& name) {
    return "{\"table\":\"users\",\"values\":{\"id\":" + std::to_string(id) +
           ",\"name\":\"" + name + "\"}}";
}

}  // namespace

// R4 HTTP：非 owner 无 Fallback → 307 + Location 指向 owner 的同名路径。
static void test_http_foreign_key_gets_307() {
    HttpFixture f;
    const std::int64_t id = find_http_key(f.cv, /*want_self=*/false);
    assert(f.post("/api/v1/insert", insert_body(id, "x")).status_code == 307);
    const auto resp = f.post("/api/v1/select", select_body(id));
    assert(resp.status_code == 307);
    assert(f.header_of(resp, "Location") == "http://127.0.0.1:18102/api/v1/select");
    assert(f.metric_value("cpppools_redirects_total") >= 2);
    // 未执行 → 无缓存痕迹。
    assert(f.cache_metric("cpppools_cache_entries") == 0);
}

// R5 HTTP：非 owner 带 Fallback → 就地处理、不缓存（规则 A）。
static void test_http_fallback_processes_locally_without_cache() {
    HttpFixture f;
    const std::int64_t id = find_http_key(f.cv, false);
    const std::string fb = "X-CppPools-Fallback: 1\r\n";

    assert(f.post("/api/v1/insert", insert_body(id, "eve"), fb).status_code == 201);
    const auto r1 = f.post("/api/v1/select", select_body(id), fb);
    assert(r1.status_code == 200);
    assert(r1.body.find("eve") != std::string::npos);
    const auto r2 = f.post("/api/v1/select", select_body(id), fb);
    assert(r2.status_code == 200);
    // 两次读都不得产生命中/条目（bypass）。
    assert(f.cache_metric("cpppools_cache_hits_total") == 0);
    assert(f.cache_metric("cpppools_cache_entries") == 0);
}

// R3 HTTP：owner + epoch 不符 → 正常处理但 bypass 缓存 + 回显 X-CppPools-Epoch。
static void test_http_epoch_mismatch_bypasses_and_echoes() {
    HttpFixture f;
    const std::int64_t id = find_http_key(f.cv, /*want_self=*/true);
    assert(f.post("/api/v1/insert", insert_body(id, "amy")).status_code == 201);

    const auto r = f.post("/api/v1/select", select_body(id),
                          "X-CppPools-Epoch: 999\r\n");
    assert(r.status_code == 200);
    assert(r.body.find("amy") != std::string::npos);
    assert(f.header_of(r, "X-CppPools-Epoch") == "7" && "必须回显 self epoch");
    // bypass：两次 epoch 不符的读不产生命中/条目。
    f.post("/api/v1/select", select_body(id), "X-CppPools-Epoch: 999\r\n");
    assert(f.cache_metric("cpppools_cache_hits_total") == 0);
    assert(f.cache_metric("cpppools_cache_entries") == 0);
    // reason=epoch_mismatch 计数器：两次不符读 → 2（§8.1 指标族）。
    const std::string metrics =
        f.server->route_request("GET /metrics HTTP/1.1\r\n", true).body;
    assert(metrics.find(
               "cpppools_cache_bypass_total{pool=\"main\",reason=\"epoch_mismatch\"} 2")
           != std::string::npos);
}

// R1/R2 HTTP：owner 正常路径（无头/正确 epoch）缓存照旧命中。
static void test_http_owner_path_caches_normally() {
    HttpFixture f;
    const std::int64_t id = find_http_key(f.cv, true);
    assert(f.post("/api/v1/insert", insert_body(id, "kim")).status_code == 201);
    assert(f.post("/api/v1/select", select_body(id)).status_code == 200);
    const auto r2 = f.post("/api/v1/select", select_body(id),
                           "X-CppPools-Epoch: 7\r\n");
    assert(r2.status_code == 200);
    assert(r2.body.find("kim") != std::string::npos);
    assert(f.cache_metric("cpppools_cache_hits_total") >= 1);
    // epoch 相符不回报头。
    assert(f.header_of(r2, "X-CppPools-Epoch").empty());
}

// B5 专项：非 owner 带 Fallback 的写仍执行失效（I0）—— invalidations 增长。
static void test_http_b5_fallback_write_still_invalidates() {
    HttpFixture f;
    const std::int64_t id = find_http_key(f.cv, false);
    const std::string fb = "X-CppPools-Fallback: 1\r\n";
    assert(f.post("/api/v1/insert", insert_body(id, "bob"), fb).status_code == 201);
    const long long inv0 = f.cache_metric("cpppools_cache_invalidations_total");
    const auto upd = f.post("/api/v1/update",
                            "{\"table\":\"users\",\"set\":{\"name\":\"bob2\"},"
                            "\"where\":{\"column\":\"id\",\"value\":" +
                                std::to_string(id) + "}}",
                            fb);
    assert(upd.status_code == 200);
    assert(f.cache_metric("cpppools_cache_invalidations_total") > inv0 &&
           "Fallback 就地写的失效必须执行（I0）");
}

// 非法 where（非唯一列）不门控：本地校验管道给 400，而不是 307 踢给 owner。
static void test_http_invalid_where_stays_local_400() {
    HttpFixture f;
    const auto r = f.post("/api/v1/select",
                          "{\"table\":\"users\",\"where\":{\"column\":\"name\","
                          "\"value\":\"x\"}}");
    assert(r.status_code == 400);
    // NULL 键值同理：必然 400 的请求不得被 307 踢给 owner（评审修正）。
    const auto r2 = f.post("/api/v1/select",
                           "{\"table\":\"users\",\"where\":{\"column\":\"id\","
                           "\"value\":null}}");
    assert(r2.status_code == 400);
    assert(f.metric_value("cpppools_redirects_total") == 0);
}

int main() {
    test_decision_matrix_five_rows();
    test_single_node_always_local_and_cacheable();
    test_counters_track_decisions();
    test_hash_encoding_matches_golden_spec();
    test_http_foreign_key_gets_307();
    test_http_fallback_processes_locally_without_cache();
    test_http_epoch_mismatch_bypasses_and_echoes();
    test_http_owner_path_caches_normally();
    test_http_b5_fallback_write_still_invalidates();
    test_http_invalid_where_stays_local_400();
    return 0;
}
