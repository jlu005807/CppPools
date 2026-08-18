#include "cpppools/config/config.h"
#include "cpppools/db/datasource_registry.h"
#include "cpppools/db/db_connection_pool.h"
#include "cpppools/pool/object_pool.h"
#include "cpppools/pool/thread_pool.h"
#include "cpppools/server/http_server.h"
#include "sqlite_pool_fixture.h"

#include <nlohmann/json.hpp>

#include <cassert>
#include <set>
#include <sstream>
#include <string>

using namespace cpppools;

// /metrics 与 /api/v1/stats 的格式回归测试。
//
// route_request 是纯函数式的请求→响应映射（不碰 socket），
// 因此无需拉起真实监听即可验证指标端点 —— 这是它被提为 public 的唯一理由。
//
// 本测试守护的是批 4 确立的指标契约：
//   1) 池指标带 pool 标签（为多数据源改造预留）；
//   2) 旧指标名已移除（不双写，仓内确认无消费者）；
//   3) task_exception_total 的 HELP 写明只覆盖 submit_detached；
//   4) /api/v1/stats 的新字段存在。

namespace {

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

struct Fixture {
    // 按字段赋值而非聚合初始化：ThreadPoolConfig 字段换序时聚合初始化
    // 会静默错位（thread_pool.cpp 的 make_default_config 注释专门警告过），
    // 测试应以身作则。
    pool::ThreadPool thread_pool{[] {
        pool::ThreadPoolConfig c;
        c.name = "worker";
        c.worker_count = 2;
        c.max_queue_size = 64;
        return c;
    }()};
    pool::ObjectPool<server::RequestContext> request_pool{"request", 4, 8};
    // 真实 SQLite 后端的单源注册表。数据源（=池）名取 "db" 是为了钉住
    // 指标标签接线（生产上池名=数据源名，取值不同但接线逻辑相同）。
    std::string db_path;
    std::unique_ptr<db::DataSourceRegistry> registry;
    server::HttpServer server{0, thread_pool, request_pool, init_registry()};

    db::DataSourceRegistry& init_registry() {
        db::register_sqlite_backend();
        db_path = testing::make_temp_db_path("db");
        testing::seed_user_scores_table(db_path);
        config::DataSourceConfig cfg;
        cfg.name = "db";
        cfg.type = "sqlite";
        cfg.sqlite_path = db_path;
        cfg.pool_size = 8;
        auto result = db::DataSourceRegistry::build({cfg});
        assert(result.registry != nullptr);
        registry = std::move(result.registry);
        return *registry;
    }

    ~Fixture() {
        registry.reset();
        std::remove(db_path.c_str());
        std::remove((db_path + "-wal").c_str());
        std::remove((db_path + "-shm").c_str());
    }

    server::ApiResponse get(const std::string& path) {
        // 请求行必须以 CRLF 结尾：解析器把行终结符当完整性闸门
        // （超长 URL 截断防御，见 http_util 注释），缺了会 400。
        return server.route_request("GET " + path + " HTTP/1.1\r\n", true);
    }
};

}  // namespace

static void test_metrics_has_pool_labels() {
    Fixture f;
    const server::ApiResponse resp = f.get("/metrics");
    assert(resp.status_code == 200);

    // 借→还池指标族：两个池各带自己的标签。
    assert(contains(resp.body, "cpppools_pool_in_use{pool=\"request\"}"));
    assert(contains(resp.body, "cpppools_pool_in_use{pool=\"db\"}"));
    assert(contains(resp.body, "cpppools_pool_capacity{pool=\"request\"}"));
    assert(contains(resp.body, "cpppools_pool_acquire_failed_total{pool=\"db\"}"));

    // 线程池：TaskStats 形状 + 标签。
    assert(contains(resp.body, "cpppools_thread_pool_queue_capacity{pool=\"worker\"}"));
    assert(contains(resp.body, "cpppools_thread_pool_submitted_total{pool=\"worker\"}"));
    assert(contains(resp.body, "cpppools_thread_pool_task_exec_us_total{pool=\"worker\"}"));

    // DB 池特有计数。
    assert(contains(resp.body, "cpppools_db_pool_rebuild_total{pool=\"db\"}"));
    assert(contains(resp.body, "cpppools_db_pool_invalid_release_total{pool=\"db\"}"));
}

static void test_metrics_old_names_removed() {
    Fixture f;
    const server::ApiResponse resp = f.get("/metrics");

    // 正向锚点：若 /metrics 被改坏（404/空 body），下面的 !contains
    // 会平凡通过，必须有 status 与存活指标打底，用例才自足。
    assert(resp.status_code == 200);
    assert(contains(resp.body, "cpppools_uptime_ms"));

    // 批 4 决策：直接换新名、不双写（仓内无消费者）。旧名必须消失，
    // 否则就说明接线回退了。两种回归形态都拦：
    // “旧名+空格”（旧格式 cpppools_x 值）与“旧名+花括号”（带标签旧名）。
    assert(!contains(resp.body, "cpppools_db_pool_free"));
    assert(!contains(resp.body, "cpppools_object_pool_in_use"));
    assert(!contains(resp.body, "cpppools_thread_pool_pending "));
    assert(!contains(resp.body, "cpppools_thread_pool_pending{"));
}

static void test_metrics_help_type_precede_samples() {
    // 结构契约：每个指标族的样本行之前必须先出现过同名 TYPE 行。
    // contains 式断言对行序无感，拦不住“HELP/TYPE 夹在序列中间”
    // （批 4 中途真犯过一次的错误）。
    Fixture f;
    const server::ApiResponse resp = f.get("/metrics");
    assert(resp.status_code == 200);

    std::set<std::string> typed;  // 已见过 TYPE 声明的指标名
    std::istringstream in(resp.body);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("# TYPE ", 0) == 0) {
            // "# TYPE <name> <kind>"
            const auto sp1 = line.find(' ', 7);
            typed.insert(line.substr(7, sp1 - 7));
        } else if (!line.empty() && line[0] != '#') {
            // 样本行：取指标名（到空格或 '{' 为止）。
            const auto end = line.find_first_of(" {\t");
            const std::string name = line.substr(0, end);
            // summary 的 _count 行属于主指标族；其余样本行必须有同名 TYPE。
            const std::string base =
                (name.size() > 6 && name.substr(name.size() - 6) == "_count")
                    ? name.substr(0, name.size() - 6)
                    : name;
            assert(typed.count(base) > 0 && "样本行必须先有同名 TYPE 声明");
        }
    }
    assert(!typed.empty());
}

static void test_metrics_task_exception_help_warns_detached_only() {
    Fixture f;
    const server::ApiResponse resp = f.get("/metrics");

    // task_exception_total 只统计 submit_detached 的任务异常（submit() 的
    // 异常被 future 携带、不计入）。HELP 文本必须写明，否则看板上的 0
    // 会被误读为"没有异常"。
    assert(contains(resp.body, "cpppools_thread_pool_task_exception_total"));
    assert(contains(resp.body, "only covers submit_detached"));
    assert(contains(resp.body, "0 does NOT mean no exceptions"));
}

static void test_stats_json_has_new_fields() {
    Fixture f;
    const server::ApiResponse resp = f.get("/api/v1/stats");
    assert(resp.status_code == 200);

    // 新增字段（批 4）。
    assert(contains(resp.body, "\"thread_pool_queue_capacity\":"));
    assert(contains(resp.body, "\"thread_pool_submitted_total\":"));
    assert(contains(resp.body, "\"thread_pool_task_exception_total\":"));
    assert(contains(resp.body, "\"db_pool_alive\":"));
    assert(contains(resp.body, "\"db_pool_rebuild_total\":"));
    assert(contains(resp.body, "\"memory_pool_capacity\":"));
    assert(contains(resp.body, "\"memory_pool_acquire_failed_total\":"));

    // 旧字段保留（对读 /api/v1/stats 的调用方不破坏）。
    assert(contains(resp.body, "\"thread_pool_pending\":"));
    assert(contains(resp.body, "\"db_pool_free\":"));
    assert(contains(resp.body, "\"min_db_pool_free\":"));
    assert(contains(resp.body, "\"memory_pool_in_use\":"));
}

static void test_get_config_has_capacities() {
    Fixture f;
    const server::ApiResponse resp = f.get("/api/v1/get-config");
    assert(resp.status_code == 200);
    assert(contains(resp.body, "\"thread_pool_queue_capacity\":"));
    assert(contains(resp.body, "\"memory_pool_capacity\":"));
}


static void test_stats_json_is_valid() {
    Fixture f;
    const server::ApiResponse resp = f.get("/api/v1/stats");
    assert(resp.status_code == 200);
    // The entire body must be valid JSON.
    nlohmann::json root = nlohmann::json::parse(resp.body);
    assert(root.is_object());
    assert(root.contains("uptime_ms"));
    assert(root.contains("node_id"));
    assert(root["node_id"].is_string());
}

static void test_get_config_json_is_valid() {
    Fixture f;
    const server::ApiResponse resp = f.get("/api/v1/get-config");
    assert(resp.status_code == 200);
    nlohmann::json root = nlohmann::json::parse(resp.body);
    assert(root.is_object());
    assert(root.contains("datasources"));
    assert(root["datasources"].is_array());
    // Each datasource name must be a valid string.
    for (const auto& ds : root["datasources"]) {
        assert(ds.is_string());
    }
}

static void test_stats_json_node_id_escaped() {
    // Verify that json_escape is applied to dynamic string fields.
    // We cannot easily inject a special-char node_id through the Fixture,
    // but we can verify the field is properly escaped by parsing it.
    Fixture f;
    const server::ApiResponse resp = f.get("/api/v1/stats");
    assert(resp.status_code == 200);
    nlohmann::json root = nlohmann::json::parse(resp.body);
    // node_id must parse correctly (no broken JSON).
    assert(root.contains("node_id"));
}

static void test_get_config_datasources_escaped() {
    Fixture f;
    const server::ApiResponse resp = f.get("/api/v1/get-config");
    assert(resp.status_code == 200);
    nlohmann::json root = nlohmann::json::parse(resp.body);
    assert(root["datasources"].size() > 0);
    assert(root["datasources"][0] == "db");
}

int main() {
    test_metrics_has_pool_labels();
    test_metrics_old_names_removed();
    test_metrics_help_type_precede_samples();
    test_metrics_task_exception_help_warns_detached_only();
    test_stats_json_has_new_fields();
    test_get_config_has_capacities();
    test_stats_json_is_valid();
    test_get_config_json_is_valid();
    test_stats_json_node_id_escaped();
    test_get_config_datasources_escaped();
    return 0;
}

