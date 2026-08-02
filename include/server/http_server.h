#pragma once

#include "db_pool/db_connection_pool.h"
#include "memory_pool/object_pool.h"
#include "thread_pool/thread_pool.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <string>

// RequestContext 由 ObjectPool<RequestContext> 统一复用，
// 避免在高频请求路径上频繁进行堆分配。
struct RequestContext {
    // 由 accept 主循环分配的递增请求编号。
    int request_id{0};
    // 从 /api/process?user=<id> 查询参数中解析得到的 user_id。
    int user_id{0};
    // 原始请求路径，便于日志记录和后续路由扩展。
    std::string path;
};

// HttpServer 运行期可调参数。
// 这些配置项保持“显式且可观察”，方便学习时看到参数调优对服务行为的影响。
struct HttpServerConfig {
    // 从 DBConnectionPool 借连接时的最大等待时长。
    std::chrono::milliseconds db_acquire_timeout{100};
    // /api/stats 计算分位延迟时使用的滑动窗口样本数量。
    std::size_t latency_window_size{1024};
};

// HttpServer：教学用途的最小 HTTP 服务端实现。
//
// "自研 Socket 级轻量 HTTP/1.1 服务" 的含义：
// - 直接基于 BSD socket / Winsock 写 accept 主循环，不依赖任何第三方 HTTP 库；
// - 仅实现 GET 方法 + "一次读取 / 一次响应 / 主动关闭"模型，不做 keep-alive、
//   chunked、TLS，目的是把"HTTP 协议层"清晰暴露给学习者；
// - 路由能力以 if/else 形式直观体现，便于初学者跟踪请求 -> 响应的全过程。
//
// 与三大池子的协作分工：
// 1) 接收 TCP 连接（accept 主循环跑在调用者线程，通常是 main）；
// 2) 把"处理这个 client_fd"的任务投递到 ThreadPool（避免阻塞 accept）；
// 3) 工作线程从 ObjectPool 借出 RequestContext，处理完成后 RAII 自动归还；
// 4) 业务路径里用 DBConnGuard 从 DBConnectionPool 借出连接，作用域结束自动归还。
//
// 可观测性：
//   /api/stats 会暴露三大池子的"瞬时 + 历史峰值"以及 /api/process 的延迟分位
//   （avg / p50 / p95 / p99），用于运行期观察池化效果。
class HttpServer {
public:
    // web_root 指向静态资源目录（需包含 index.html 与 app.js）。
    HttpServer(uint16_t port,
               ThreadPool& thread_pool,
               ObjectPool<RequestContext>& request_pool,
               DBConnectionPool& db_pool,
               std::string web_root,
               HttpServerConfig config = {});

    // 阻塞启动：进入 accept 循环并持续向线程池分发客户端处理任务。
    void start();

private:
    struct LatencyStats {
        double avg_ms{0.0};
        double p50_ms{0.0};
        double p95_ms{0.0};
        double p99_ms{0.0};
        std::size_t samples{0};
    };

    // 处理单个客户端连接。
    void handle_client(std::intptr_t client_fd, int request_id);

    // 组装原始 HTTP/1.1 响应文本。
    std::string build_http_response(const std::string& body,
                                    const std::string& content_type,
                                    int status_code = 200,
                                    const std::string& status_text = "OK") const;

    // 解析请求行并分发到对应路由处理逻辑。
    std::string route_request(const std::string& request, int request_id);

    // 读取静态文件内容（失败返回空字符串）。
    static std::string read_file_or_empty(const std::string& path);

    // 构造最小化 JSON 错误响应体。
    std::string json_error(const std::string& message) const;

    // 记录 /api/process 延迟样本，用于后续分位数统计。
    void record_process_latency_ms(double latency_ms);
    LatencyStats collect_latency_stats() const;

    uint16_t port_;
    // 共享线程池：执行 socket 处理任务。
    ThreadPool& thread_pool_;
    // 请求上下文对象池。
    ObjectPool<RequestContext>& request_pool_;
    // 数据库连接池。
    DBConnectionPool& db_pool_;
    // 静态资源根目录。
    std::string web_root_;
    // 运行期调优参数（超时、统计窗口大小等）。
    HttpServerConfig config_;

    // 运行期监控计数器（用于 dashboard 可观测性输出）。
    // 这里使用 memory_order_relaxed 足够，因为我们只需要近似实时遥测值。
    std::atomic<std::uint64_t> total_requests_{0};
    std::atomic<std::uint64_t> api_requests_{0};
    std::atomic<std::uint64_t> static_requests_{0};
    std::atomic<std::uint64_t> error_requests_{0};
    std::atomic<std::uint64_t> process_requests_{0};
    std::atomic<std::uint64_t> active_process_requests_{0};
    std::atomic<std::uint64_t> db_timeout_requests_{0};
    std::atomic<std::uint64_t> peak_pending_tasks_{0};
    std::atomic<std::uint64_t> peak_memory_in_use_{0};
    std::atomic<std::uint64_t> peak_active_process_{0};
    std::atomic<std::uint64_t> min_db_free_{std::numeric_limits<std::uint64_t>::max()};
    std::chrono::steady_clock::time_point started_at_{std::chrono::steady_clock::now()};

    mutable std::mutex latency_mutex_;
    std::deque<double> process_latencies_ms_;
};
