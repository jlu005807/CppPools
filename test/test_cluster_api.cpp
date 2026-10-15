// GET /api/v1/cluster 端点契约测试（cluster-cache spec §6.2/§6.3）。
//
// 端点是 SDK 建环的唯一信息源：拓扑、哈希参数、TTL（I1 下发）、
// 数据源配置摘要必须全部自包含。

#include "cpppools/db/datasource_registry.h"
#include "cpppools/pool/object_pool.h"
#include "server/hash_ring.h"
#include "cpppools/pool/thread_pool.h"
#include "cpppools/server/http_server.h"
#include "sqlite_pool_fixture.h"
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

std::string make_temp_db(const char* tag) {
    const std::string real_path = cpppools::testing::make_test_temp_file(
        std::string("cpppools_clusterapi_") + tag);
    sqlite3* raw = nullptr;
    assert(sqlite3_open(real_path.c_str(), &raw) == SQLITE_OK);
    assert(sqlite3_exec(raw, "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT)",
                        nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
    return real_path;
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

    Fixture(bool cluster_mode, uint16_t port, server::ClusterView cluster) {
        (void)cluster_mode;
        server::HttpServerConfig cfg;
        cfg.cluster_view = std::move(cluster);
        server = std::make_unique<server::HttpServer>(
            port, thread_pool, request_pool, build_registry(), std::move(cfg));
    }

    // server 用 unique_ptr：registry 必须先建好才能构造，用指针延迟构造。
    std::unique_ptr<server::HttpServer> server;

    server::ApiResponse get(const std::string& path) {
        return server->route_request("GET " + path + " HTTP/1.1\r\n", true);
    }

    db::DataSourceRegistry& build_registry() {
        db::register_sqlite_backend();  // 幂等；后端工厂注册表必须先登记
        db_path = make_temp_db("main");
        config::DataSourceConfig cfg;
        cfg.name = "main";
        cfg.type = "sqlite";
        cfg.is_default = true;
        cfg.pool_size = 1;
        cfg.sqlite_path = db_path;
        auto result = db::DataSourceRegistry::build({cfg});
        assert(result.registry != nullptr);
        registry = std::move(result.registry);
        return *registry;
    }

    ~Fixture() {
        server.reset();   // 先于 registry 销毁（server 持 registry 引用）
        registry.reset();
        std::remove(db_path.c_str());
        std::remove((db_path + "-wal").c_str());
        std::remove((db_path + "-shm").c_str());
    }
};

server::ClusterView make_cluster_view() {
    server::ClusterView v;
    v.node_id = "node-a";
    v.epoch = 7;
    v.vnodes_per_node = 100;
    v.cache_ttl_ms = 30000;
    v.blacklist_ttl_ms = 60000;
    v.nodes.push_back({"node-a", "10.0.0.1", 8081});
    v.nodes.push_back({"node-b", "10.0.0.2", 8082});
    return v;
}

}  // namespace

// 集群模式：拓扑 / 哈希参数 / TTL / 摘要全部自包含。
static void test_cluster_endpoint_full_contract() {
    Fixture f(true, 8999, make_cluster_view());
    const server::ApiResponse resp = f.get("/api/v1/cluster");
    assert(resp.status_code == 200);

    // 拓扑。
    assert(contains(resp.body, "\"self\":\"node-a\""));
    assert(contains(resp.body, "\"epoch\":7"));
    assert(contains(resp.body, "\"node_id\":\"node-a\""));
    assert(contains(resp.body, "\"node_id\":\"node-b\""));
    assert(contains(resp.body, "\"host\":\"10.0.0.1\""));
    assert(contains(resp.body, "\"host\":\"10.0.0.2\""));
    assert(contains(resp.body, "\"port\":8082"));

    // 哈希参数（建环所需的每一项，spec §6.2）。
    assert(contains(resp.body, "\"algorithm\":\"murmur3_32\""));
    assert(contains(resp.body, "\"seed\":0"));
    assert(contains(resp.body, "\"unsigned\":true"));
    assert(contains(resp.body, "\"vnodes_per_node\":100"));
    assert(contains(resp.body, "\"attempt_base\":0"));

    // TTL 下发（不变量 I1 的跨进程一致由此保证，spec §5.2）。
    assert(contains(resp.body, "\"cache_ttl_ms\":30000"));
    assert(contains(resp.body, "\"blacklist_ttl_ms\":60000"));

    // 数据源配置摘要（spec §6.3）：name + type + digest，不含凭据。
    assert(contains(resp.body, "\"datasource_digest\""));
    assert(contains(resp.body, "\"name\":\"main\""));
    assert(contains(resp.body, "\"type\":\"sqlite\""));
    assert(contains(resp.body, "\"digest\":\""));
    assert(!contains(resp.body, "password"));

    // digest 确定性（spec §6.3 的契约本身）：
    // ① 同配置两次请求逐字节一致；
    // ② 与 murmur3_32(LP(type) ‖ LP(sqlite_path)) 的进程内计算一致。
    const server::ApiResponse resp2 = f.get("/api/v1/cluster");
    assert(resp2.body == resp.body);
    const std::string expect_input =
        server::hash_ring::lp("sqlite") + server::hash_ring::lp(f.db_path);
    char hex[9];
    std::snprintf(hex, sizeof(hex), "%08x", server::hash_ring::murmur3_32(expect_input));
    assert(contains(resp.body, std::string("\"digest\":\"") + hex + "\""));
}

// 单节点模式（cluster 段缺省）：返回单节点列表，SDK 逻辑统一。
static void test_cluster_endpoint_single_node_mode() {
    Fixture f(false, 8999, server::ClusterView{});  // 默认 = 单节点
    const server::ApiResponse resp = f.get("/api/v1/cluster");
    assert(resp.status_code == 200);
    assert(contains(resp.body, "\"self\":\"single\""));
    assert(contains(resp.body, "\"node_id\":\"single\""));
    assert(contains(resp.body, "\"host\":\"127.0.0.1\""));
    assert(contains(resp.body, "\"port\":8999"));
    assert(contains(resp.body, "\"epoch\":0"));
}

// 响应是合法 JSON 且 Content-Type 正确。
static void test_cluster_endpoint_shape() {
    Fixture f(true, 8999, make_cluster_view());
    const server::ApiResponse resp = f.get("/api/v1/cluster");
    assert(resp.content_type.find("application/json") != std::string::npos);
    assert(resp.body.front() == '{' && resp.body.back() == '}');
    // 是 GET 端点：POST 应 405（路由表口径）。
    const server::ApiResponse post =
        f.server->route_request("POST /api/v1/cluster HTTP/1.1\r\n\r\n", true);
    assert(post.status_code == 405);
}

int main() {
    test_cluster_endpoint_full_contract();
    test_cluster_endpoint_single_node_mode();
    test_cluster_endpoint_shape();
    return 0;
}
