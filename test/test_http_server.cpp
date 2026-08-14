#include "cpppools/db/db_connection_pool.h"
#include "cpppools/pool/object_pool.h"
#include "cpppools/pool/thread_pool.h"
#include "cpppools/server/http_server.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

using namespace cpppools;

// handle_client 层的端到端测试（整体深度评审 I-3 补上的真空地带）。
//
// route_request 只覆盖"请求文本→响应文本"；handle_client 里的
// 对象池判空→503、计数器口径、DB 超时→503 这些接线此前零测试，
// 且生产配置下对象池 503 路径不可达 —— 既不可达又无测试的分支
// 是回归腐坏的最佳温床。
//
// 方法：socketpair 造一对 fd，一端交给 handle_client（它会 recv+send
// 后关闭），另一端预写请求、事后读响应。无需真实监听端口。
// macOS/Linux 均可用（AF_UNIX + SOCK_STREAM）。

namespace {

struct Fixture {
    pool::ThreadPool thread_pool{[] {
        pool::ThreadPoolConfig c;
        c.name = "worker";
        c.worker_count = 1;
        return c;
    }()};
    pool::ObjectPool<server::RequestContext> request_pool;
    db::DbConnectionPool db_pool{1};
    server::HttpServer server;

    // 可配对象池容量：耗尽测试用 (1,1)，正常路径用宽松容量。
    explicit Fixture(std::size_t pool_init, std::size_t pool_max)
        : request_pool("request", pool_init, pool_max),
          server(0, thread_pool, request_pool, db_pool) {}

    // 走一次完整的 handle_client：peer 端预写请求，返回收到的响应文本。
    std::string round_trip(const std::string& request) {
        int fds[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        const std::size_t to_write = request.size();
        assert(send(fds[1], request.data(), to_write, 0) ==
               static_cast<ssize_t>(to_write));

        server.handle_client(static_cast<std::intptr_t>(fds[0]), 1);
        // fds[0] 已被 handle_client 内的 SocketGuard 关闭。

        std::string response;
        char buf[4096];
        ssize_t n;
        while ((n = recv(fds[1], buf, sizeof(buf), 0)) > 0) {
            response.append(buf, static_cast<std::size_t>(n));
        }
        ::close(fds[1]);
        return response;
    }

    // 从 /api/v1/stats 里取一个整数字段的值（轻量提取，不引 JSON 库：
    // 测试依赖的是字段存在与数值，字段顺序由 handle_stats 保证）。
    static long long stats_field(const std::string& stats_body, const std::string& key) {
        const std::string needle = "\"" + key + "\":";
        const auto pos = stats_body.find(needle);
        assert(pos != std::string::npos && "stats 缺少该字段");
        return std::atoll(stats_body.c_str() + pos + needle.size());
    }

    std::string stats_body() {
        const server::ApiResponse resp =
            server.route_request("GET /api/v1/stats HTTP/1.1\r\n", true);
        assert(resp.status_code == 200);
        return resp.body;
    }
};

}  // namespace

static void test_normal_request_through_handle_client() {
    Fixture f(2, 4);
    const std::string resp = f.round_trip("GET /health HTTP/1.1\r\n");
    assert(resp.find("200") != std::string::npos);
    assert(resp.find("cpppools worker is active") != std::string::npos);

    // 口径：/health 不是业务 API，不计 api_requests，但计 total_requests。
    const std::string s = f.stats_body();
    assert(Fixture::stats_field(s, "total_requests") == 1);
    assert(Fixture::stats_field(s, "api_requests") == 0);
}

static void test_object_pool_exhausted_returns_503() {
    // 对象池 (1,1)：先借空，handle_client 必走判空→503 分支。
    Fixture f(1, 1);
    auto held = f.request_pool.acquire();
    assert(held);

    const std::string resp = f.round_trip("GET /health HTTP/1.1\r\n");
    assert(resp.find("503") != std::string::npos);
    assert(resp.find("request pool exhausted") != std::string::npos);

    // 口径自洽（评审关注点）：进了处理链才失败的算 error，且计入 total；
    // 它**不是** accept 循环的队列满拒绝，不得计入 rejected。
    const std::string s = f.stats_body();
    assert(Fixture::stats_field(s, "total_requests") == 1);
    assert(Fixture::stats_field(s, "error_requests") == 1);
    assert(Fixture::stats_field(s, "rejected_requests") == 0);
    assert(Fixture::stats_field(s, "memory_pool_acquire_failed_total") == 1);
}

static void test_db_timeout_through_handle_client_returns_503() {
    // 借空连接池（1 槽）后发 CRUD：handler 的 DbConnGuard 拿不到连接 → 503。
    Fixture f(2, 4);
    auto* held = f.db_pool.acquire(std::chrono::milliseconds(10));
    assert(held != nullptr);

    const std::string resp = f.round_trip("GET /api/v1/get-score?user=5 HTTP/1.1\r\n");
    assert(resp.find("503") != std::string::npos);

    const std::string s = f.stats_body();
    assert(Fixture::stats_field(s, "db_timeout_requests") == 1);
    // CRUD 属业务 API：计 api_requests 与 total。
    assert(Fixture::stats_field(s, "api_requests") == 1);
    assert(Fixture::stats_field(s, "total_requests") == 1);

    f.db_pool.release(held);
}

static void test_stats_endpoint_not_counted_as_api() {
    // S-5：stats/get-config 是自省端点，轮询它们不得污染 api_requests 与延迟分位。
    Fixture f(2, 4);
    (void)f.round_trip("GET /api/v1/stats HTTP/1.1\r\n");

    // 注：stats_body() 走 route_request 直连（不经过 handle_client，不计数），
    // 因此 total_requests 只算 round_trip 那一次。
    const std::string s = f.stats_body();
    assert(Fixture::stats_field(s, "api_requests") == 0);
    assert(Fixture::stats_field(s, "total_requests") == 1);
}

int main() {
    test_normal_request_through_handle_client();
    test_object_pool_exhausted_returns_503();
    test_db_timeout_through_handle_client_returns_503();
    test_stats_endpoint_not_counted_as_api();
    return 0;
}
