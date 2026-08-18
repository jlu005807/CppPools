#include "cpppools/config/config.h"
#include "cpppools/db/datasource_registry.h"
#include "cpppools/db/db_connection_pool.h"
#include "cpppools/pool/object_pool.h"
#include "cpppools/pool/thread_pool.h"
#include "cpppools/server/http_server.h"
#include "sqlite_pool_fixture.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
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
    // 真实 SQLite 后端的单源注册表（每个 Fixture 一份独立临时库）。
    std::string db_path;
    std::unique_ptr<db::DataSourceRegistry> registry;
    server::HttpServer server;

    // 可配对象池容量：耗尽测试用 (1,1)，正常路径用宽松容量。
    explicit Fixture(std::size_t pool_init, std::size_t pool_max)
        : request_pool("request", pool_init, pool_max),
          server(0, thread_pool, request_pool, init_registry()) {}

    db::DataSourceRegistry& init_registry() {
        db::register_sqlite_backend();
        db_path = testing::make_temp_db_path("http");
        testing::seed_user_scores_table(db_path);
        config::DataSourceConfig cfg;
        cfg.name = "main";
        cfg.type = "sqlite";
        cfg.sqlite_path = db_path;
        cfg.pool_size = 1;
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

    db::DbConnectionPool& db_pool() { return *registry->default_pool(); }

    // 走一次完整的 handle_client：peer 端预写请求，返回收到的响应文本。
    // request_id 透传给 handle_client（X-Request-Id 回显测试可注入不同值）。
    std::string round_trip(const std::string& request, std::uint64_t request_id = 1) {
        int fds[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        const std::size_t to_write = request.size();
        assert(send(fds[1], request.data(), to_write, 0) ==
               static_cast<ssize_t>(to_write));

        server.handle_client(static_cast<std::intptr_t>(fds[0]), request_id);
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
    auto* held = f.db_pool().acquire(std::chrono::milliseconds(10));
    assert(held != nullptr);

    const std::string body = R"({"table":"user_scores","where":{"column":"user_id","value":5}})";
    const std::string req = "POST /api/v1/select HTTP/1.1\r\n"
                            "Content-Length: " + std::to_string(body.size()) + "\r\n"
                            "\r\n" + body;
    const std::string resp = f.round_trip(req);
    assert(resp.find("503") != std::string::npos);

    const std::string s = f.stats_body();
    assert(Fixture::stats_field(s, "db_timeout_requests") == 1);
    // CRUD 属业务 API：计 api_requests 与 total。
    assert(Fixture::stats_field(s, "api_requests") == 1);
    assert(Fixture::stats_field(s, "total_requests") == 1);

    f.db_pool().release(held);
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

static void test_request_with_body_reaches_routing() {
    // 带 Content-Length 的请求：body 不应污染头区解析 —— 请求必须正常走到路由
    // （/health 只接受 GET → 405），而不是因为 body 字节被判 400 格式错。
    Fixture f(2, 4);
    const std::string body = R"({"hello":"world"})";
    const std::string req = "POST /health HTTP/1.1\r\n"
                            "Content-Length: " + std::to_string(body.size()) + "\r\n"
                            "\r\n" + body;
    const std::string resp = f.round_trip(req);
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

static void test_get_without_body_unchanged() {
    // 无 Content-Length 的普通请求不受读体逻辑影响（回归守护）。
    Fixture f(2, 4);
    const std::string resp = f.round_trip("GET /health HTTP/1.1\r\n");
    assert(resp.find("200") != std::string::npos);
}

static void test_response_waits_for_body() {
    // 行为守护：body 未发完时，响应不得先到达（handle_client 必须在路由前读体）。
    Fixture f(2, 4);
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    const std::string header = "POST /health HTTP/1.1\r\nContent-Length: 5\r\n\r\n";
    assert(send(fds[1], header.data(), header.size(), 0) ==
           static_cast<ssize_t>(header.size()));

    std::thread server_thread([&f, fd = fds[0]] {
        f.server.handle_client(static_cast<std::intptr_t>(fd), 1);
    });

    // 等 100ms：body 还没发，响应不应到达（recv 超时返回 <= 0）。
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    struct timeval short_tv{0, 100 * 1000};
    setsockopt(fds[1], SOL_SOCKET, SO_RCVTIMEO, &short_tv, sizeof(short_tv));
    char peek[64];
    const ssize_t early = recv(fds[1], peek, sizeof(peek), 0);
    assert(early <= 0 && "body 未发送完毕时响应不得先到达");

    // 发 body → 响应应到达。
    struct timeval long_tv{2, 0};
    setsockopt(fds[1], SOL_SOCKET, SO_RCVTIMEO, &long_tv, sizeof(long_tv));
    assert(send(fds[1], "abcde", 5, 0) == 5);
    std::string resp;
    char buf[256];
    ssize_t nn;
    while ((nn = recv(fds[1], buf, sizeof(buf), 0)) > 0) {
        resp.append(buf, static_cast<std::size_t>(nn));
    }
    server_thread.join();
    ::close(fds[1]);
    assert(resp.find("405") != std::string::npos);
}

static void test_large_body_across_multiple_segments() {
    // body 分 3 段各 2KB 发送（CL=6KB > 首次 recv 的 4KB 缓冲）：
    // 读体循环必须真正循环多轮并拼装出完整 body。
    // 用 /health 的 405 证明请求走完了完整链路（头区未被 body 污染）。
    Fixture f(2, 4);
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    const std::size_t kSeg = 2048;
    const std::size_t kTotal = kSeg * 3;
    const std::string header = "POST /health HTTP/1.1\r\n"
                               "Content-Length: " + std::to_string(kTotal) + "\r\n"
                               "\r\n";
    assert(send(fds[1], header.data(), header.size(), 0) ==
           static_cast<ssize_t>(header.size()));

    std::thread server_thread([&f, fd = fds[0]] {
        f.server.handle_client(static_cast<std::intptr_t>(fd), 1);
    });

    const std::string segment(kSeg, 'x');
    for (int i = 0; i < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        assert(send(fds[1], segment.data(), segment.size(), 0) ==
               static_cast<ssize_t>(kSeg));
    }

    struct timeval tv{2, 0};
    setsockopt(fds[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::string resp;
    char buf[256];
    ssize_t nn;
    while ((nn = recv(fds[1], buf, sizeof(buf), 0)) > 0) {
        resp.append(buf, static_cast<std::size_t>(nn));
    }
    server_thread.join();
    ::close(fds[1]);
    assert(resp.find("405") != std::string::npos && "多段 body 必须被完整读完后走路由");
}

static void test_body_over_limit_counts_error() {
    // I4 回归：413 属“进了处理链的失败”，必须计 error_requests（口径与对象池 503 一致）。
    Fixture f(2, 4);
    const std::string req = "POST /health HTTP/1.1\r\n"
                            "Content-Length: 999999\r\n"
                            "\r\n";
    (void)f.round_trip(req);
    const std::string s = f.stats_body();
    assert(Fixture::stats_field(s, "total_requests") == 1);
    assert(Fixture::stats_field(s, "error_requests") == 1);
}

static void test_malformed_request_line_does_not_wait_for_body() {
    // I3 回归：声明了 body 的畸形请求行必须立刻回 400，而不是等 body（钉死 worker）。
    Fixture f(2, 4);
    const std::string req = "GARBAGE-NOT-A-REQUEST\r\n"
                            "Content-Length: 60000\r\n"
                            "\r\n";
    const std::string resp = f.round_trip(req);  // 若等 body，socketpair 对端永不发 → 挂死
    assert(resp.find("400") != std::string::npos);
}

static void test_request_id_header_echoed() {
    // X-Request-Id 回显：每个响应都带，且随 accept 序号递增 ——
    // 客户端用它与服务端日志对账（贯穿排障的前提）。
    Fixture f(2, 4);
    const std::string r1 = f.round_trip("GET /health HTTP/1.1\r\n", 1);
    assert(r1.find("X-Request-Id: 1\r\n") != std::string::npos);
    // 不同请求携带各自的 id（accept 循环里是递增序号，此处注入模拟）。
    const std::string r2 = f.round_trip("GET /health HTTP/1.1\r\n", 42);
    assert(r2.find("X-Request-Id: 42\r\n") != std::string::npos);
    // 错误路径同样携带（排障最需要 id 的恰是失败请求）。
    const std::string r3 = f.round_trip("BADREQUESTLINE", 7);
    assert(r3.find("X-Request-Id: 7\r\n") != std::string::npos);
}

static void test_graceful_stop_returns_from_start() {
    // request_stop() 后 start() 必须在有限时间内返回（accept 循环退出）：
    // 200ms accept 超时唤醒 + 停机标志检查；无在途请求时立即排空返回。
    Fixture f(2, 4);
    std::atomic<bool> stopped{false};
    std::thread server_thread([&f, &stopped] {
        f.server.start();
        stopped.store(true);
    });
    // 等监听就绪（start 内 bind/listen 同步完成，留少量余量）。
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    f.server.request_stop();
    // 上限 3s，远超 200ms 唤醒周期；用标志轮询而非阻塞 join，
    // 若 accept 循环未响应停机标志会断言失败而不是测试挂死。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!stopped.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(stopped.load() && "request_stop 后 start() 必须返回");
    server_thread.join();  // 线程已结束，join 立即返回
}

// ===== Phase 4 Task 3: Fragmented header tests =====

static void send_fragment(int fd, const char* data) {
    const std::size_t len = std::strlen(data);
    assert(send(fd, data, len, 0) == static_cast<ssize_t>(len));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
}

static void test_fragmented_header_reassembled() {
    // Task 3: send request line and headers in multiple TCP segments.
    // The server must reassemble them into a complete request before routing.
    Fixture f(2, 4);
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    std::thread server_thread([&f, fd = fds[0]] {
        f.server.handle_client(static_cast<std::intptr_t>(fd), 1);
    });

    // Send header in fragments - no \r\n\r\n until the last fragment.
    send_fragment(fds[1], "POST /health HT");
    send_fragment(fds[1], "TP/1.1\r\nContent-Len");
    send_fragment(fds[1], "gth: 5\r\n");
    // No terminator yet - server should not respond.
    {
        struct timeval tv{0, 200 * 1000};
        setsockopt(fds[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char peek[64];
        ssize_t early = recv(fds[1], peek, sizeof(peek), 0);
        assert(early <= 0 && "no response before terminator");
    }
    // Send terminator + body.
    send_fragment(fds[1], "\r\nabcde");

    struct timeval tv{3, 0};
    setsockopt(fds[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::string resp;
    char buf[256];
    ssize_t n;
    while ((n = recv(fds[1], buf, sizeof(buf), 0)) > 0) {
        resp.append(buf, static_cast<std::size_t>(n));
    }
    server_thread.join();
    ::close(fds[1]);
    assert(resp.find("405") != std::string::npos && "fragmented header must be reassembled and routed");
}

static void test_incomplete_header_timeout_does_not_hang() {
    // Task 3: client sends partial header (no \r\n\r\n) and disappears.
    // Server must not hang 30s; it should return a response within a few seconds
    // (after the 1s subsequent-read timeout kicks in).
    Fixture f(2, 4);
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    auto start_time = std::chrono::steady_clock::now();
    std::thread server_thread([&f, fd = fds[0]] {
        f.server.handle_client(static_cast<std::intptr_t>(fd), 1);
    });

    // Send a valid request line but no terminator.
    {
        const std::string req_line = "GET /health HTTP/1.1\r\n";
        assert(send(fds[1], req_line.data(), req_line.size(), 0) ==
               static_cast<ssize_t>(req_line.size()));
    }
    // Do NOT send \r\n - let the server's 1s subsequent-read timeout fire.

    struct timeval tv{5, 0};
    setsockopt(fds[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char buf[256];
    ssize_t n = recv(fds[1], buf, sizeof(buf), 0);
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start_time).count();
    server_thread.join();
    ::close(fds[1]);

    // Server should respond (likely 400 or 414 for incomplete request).
    assert(n > 0 && "server must respond to incomplete header, not hang silently");
    assert(elapsed < 5 && "incomplete header must not wait 30s");
}

// ===== Phase 4 Task 2: Shutdown safety with queued/stalled requests =====

static void test_queued_request_drained_on_shutdown() {
    // Task 2: worker is held on an incomplete body; a second connection is queued;
    // request_stop(); release the first request; start() must drain the queued one.
    Fixture f(2, 4);
    std::atomic<bool> stopped{false};

    // Use a single worker so the first request blocks the pool.
    std::thread server_thread([&f, &stopped] {
        f.server.start();
        stopped.store(true);
    });

    // Wait for bind.
    uint16_t port = 0;
    for (int i = 0; i < 200 && port == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        port = f.server.bound_port();
    }
    assert(port != 0);

    // First connection: send header without body -> worker blocks reading body.
    const int fd1 = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd1 >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(connect(fd1, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    const std::string hdr1 = "POST /health HTTP/1.1\r\nContent-Length: 5\r\n\r\n";
    assert(send(fd1, hdr1.data(), hdr1.size(), 0) == static_cast<ssize_t>(hdr1.size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Second connection: queued behind the first.
    const int fd2 = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd2 >= 0);
    assert(connect(fd2, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    const std::string hdr2 = "GET /health HTTP/1.1\r\n";
    assert(send(fd2, hdr2.data(), hdr2.size(), 0) == static_cast<ssize_t>(hdr2.size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Stop: start() should not return while the worker is still busy.
    f.server.request_stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    assert(!stopped.load() && "start() must not return while in-flight requests remain");

    // Release the first request by sending the body.
    assert(send(fd1, "abcde", 5, 0) == 5);

    // start() should now drain and return.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!stopped.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(stopped.load() && "start() must return after draining in-flight + queued work");

    // Read responses from both connections.
    struct timeval tv{2, 0};
    setsockopt(fd1, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd2, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char buf[256];
    // fd1 got 405 (POST /health).
    ssize_t n1 = recv(fd1, buf, sizeof(buf), 0);
    assert(n1 > 0);
    std::string resp1(buf, static_cast<std::size_t>(n1));
    assert(resp1.find("405") != std::string::npos);
    // fd2 got 200 (GET /health).
    ssize_t n2 = recv(fd2, buf, sizeof(buf), 0);
    assert(n2 > 0);
    std::string resp2(buf, static_cast<std::size_t>(n2));
    assert(resp2.find("200") != std::string::npos);

    ::close(fd1);
    ::close(fd2);
    server_thread.join();
}

static void test_stalled_connection_forced_shutdown() {
    // Task 2: peer never completes the request. After the 5s drain window,
    // start() must force-interrupt the socket and return.
    Fixture f(2, 4);
    std::atomic<bool> stopped{false};

    std::thread server_thread([&f, &stopped] {
        f.server.start();
        stopped.store(true);
    });

    uint16_t port = 0;
    for (int i = 0; i < 200 && port == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        port = f.server.bound_port();
    }
    assert(port != 0);

    // Connect and send header without body; never send the body.
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    const std::string hdr = "POST /health HTTP/1.1\r\nContent-Length: 5\r\n\r\n";
    assert(send(fd, hdr.data(), hdr.size(), 0) == static_cast<ssize_t>(hdr.size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Request stop. The stalled connection should cause a forced shutdown.
    f.server.request_stop();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!stopped.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    assert(stopped.load() && "forced shutdown must return within 15s for stalled connection");

    ::close(fd);
    server_thread.join();
}

// ===== Phase 4 Task 4: Send deadline with non-reading peer =====

static void test_send_deadline_with_non_reading_peer() {
    // Task 4: request a response larger than the peer's receive buffer,
    // and leave the peer unread. The handler must exit after the send deadline
    // instead of blocking forever.
    Fixture f(2, 4);
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    // Shrink the peer's receive buffer so the send blocks quickly.
    int rcvbuf = 1024;
    setsockopt(fds[1], SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    // Also set the send buffer small on the server side.
    int sndbuf = 1024;
    setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    // Request /api/v1/stats which returns a large JSON body.
    auto start_time = std::chrono::steady_clock::now();
    std::thread server_thread([&f, fd = fds[0]] {
        f.server.handle_client(static_cast<std::intptr_t>(fd), 1);
    });

    // Send the request.
    const std::string req = "GET /api/v1/stats HTTP/1.1\r\n";
    assert(send(fds[1], req.data(), req.size(), 0) == static_cast<ssize_t>(req.size()));

    // Do NOT read the response. The server's send_all should time out after 10s.
    // We wait for the server thread to finish, with a 15s upper bound.
    server_thread.join();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start_time).count();

    // Send deadline is 10s; allow some slack for CI.
    assert(elapsed < 15 && "send deadline must prevent indefinite blocking");
    assert(elapsed >= 1 && "should not return instantly with unread peer");

    ::close(fds[1]);
}

static void test_graceful_stop_drains_inflight_request() {
    // 有在途请求时停机：start() 必须等在途跑完（排空）才返回，
    // 而不是立即返回把它丢弃 —— 排空分支的真实连接回归
    // （走真实 accept 循环：port 0 临时端口 + bound_port() 连接）。
    Fixture f(2, 4);
    std::atomic<bool> stopped{false};
    std::thread server_thread([&f, &stopped] {
        f.server.start();
        stopped.store(true);
    });
    // 等 bind 回填端口。
    uint16_t port = 0;
    for (int i = 0; i < 200 && port == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        port = f.server.bound_port();
    }
    assert(port != 0 && "bind 后必须回填端口");

    // 真实连接：发带头部的请求但暂不发 body → worker 挂在读体（在途）。
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    const std::string header = "POST /health HTTP/1.1\r\nContent-Length: 5\r\n\r\n";
    assert(send(fd, header.data(), header.size(), 0) ==
           static_cast<ssize_t>(header.size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // 停机请求：有在途请求 → start() 不得立即返回。
    f.server.request_stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    assert(!stopped.load() && "有在途请求时 start() 不得在排空前返回");

    // 补发 body → 在途完成 → 排空 → start() 返回，且请求拿到正常响应。
    assert(send(fd, "abcde", 5, 0) == 5);
    const struct timeval tv{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::string response;
    char buf[256];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
        response.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    assert(response.find("405") != std::string::npos && "在途请求必须被完整处理");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!stopped.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(stopped.load() && "在途完成后 start() 必须返回");
    server_thread.join();
}

int main() {
    test_normal_request_through_handle_client();
    test_object_pool_exhausted_returns_503();
    test_db_timeout_through_handle_client_returns_503();
    test_stats_endpoint_not_counted_as_api();
    test_request_with_body_reaches_routing();
    test_body_over_limit_returns_413();
    test_get_without_body_unchanged();
    test_response_waits_for_body();
    test_large_body_across_multiple_segments();
    test_body_over_limit_counts_error();
    test_malformed_request_line_does_not_wait_for_body();
    test_request_id_header_echoed();
    test_graceful_stop_returns_from_start();
    test_graceful_stop_drains_inflight_request();
    test_fragmented_header_reassembled();
    test_incomplete_header_timeout_does_not_hang();
    test_queued_request_drained_on_shutdown();
    test_stalled_connection_forced_shutdown();
    test_send_deadline_with_non_reading_peer();
    return 0;
}
