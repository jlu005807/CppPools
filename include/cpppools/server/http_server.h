#pragma once

#include "cpppools/db/datasource_registry.h"
#include "cpppools/db/db_connection_pool.h"
#include "cpppools/pool/object_pool.h"
#include "cpppools/pool/thread_pool.h"
#include "cpppools/server/api_response.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>

namespace cpppools::server {

class OwnershipRouter;  // 归属路由器（期2）：实现细节在 src/server/。

// 单次请求的接收缓冲上限。请求行超过此值会得到 414。
inline constexpr std::size_t kRequestBufferBytes = 4096;

// 请求体上限（Content-Length 模型）。超过返回 413。
// JSON 行级 CRUD 的 body 是 KB 级，64KB 留足余量。
inline constexpr std::size_t kMaxBodyBytes = 65536;

// RequestContext 由 ObjectPool<RequestContext> 复用，是三级池化中的第二级。
//
// 池的价值所在：每次请求都需要一个 4KB 接收缓冲与一个响应字符串。
// 不池化就意味着每请求两次堆分配 + 释放；池化后这些缓冲在启动期一次性建好，
// 请求路径上只做借还。
//
// reset() 而不是重建：ObjectPool 检测到 reset() 存在时优先调用它。
// 这里用 clear() 而不是重新构造，就是为了保住已分配的 capacity，
// 否则池化只剩下“对象个数”的形式，真正昂贵的堆缓冲每次还是重新申请。
struct RequestContext {
    // 由 accept 主循环分配的递增请求编号（uint64 防长运行回绕为负）。
    std::uint64_t request_id{0};
    // 请求接收缓冲（多留 1 字节供结尾零）。
    std::vector<char> buffer;
    // 待发送的完整 HTTP 响应文本。
    std::string response;
    // 请求体（Content-Length 读入；无 body 的请求保持为空）。
    std::string body;

    RequestContext() : buffer(kRequestBufferBytes + 1, '\0') {}

    void reset() {
        request_id = 0;
        // buffer 不改尺寸：内容会被下一次 recv 覆盖，无需清零。
        // response / body 用 clear() 保留 capacity。
        response.clear();
        body.clear();
    }
};

// HttpServer 运行期可调参数。
// 这些配置项保持“显式且可观察”，方便学习时看到参数调优对服务行为的影响。
// 集群视图：/api/v1/cluster 的响应素材（cluster-cache spec §6.2）。
// 由 main 从配置文件构造；nodes 为空 = 单节点模式（端点自动合成单节点列表，
// SDK 无需特判）。哈希参数与 TTL 随端点下发，保证两端同参（不变量 I1）。
struct ClusterView {
    struct Node {
        std::string node_id;
        std::string host;
        int port{8080};
    };
    std::string node_id{"single"};
    long long epoch{0};
    std::vector<Node> nodes;
    long long cache_ttl_ms{60000};
    long long blacklist_ttl_ms{60000};
    int vnodes_per_node{100};
};

struct HttpServerConfig {
    // 从 DbConnectionPool 借连接时的最大等待时长。
    std::chrono::milliseconds db_acquire_timeout{100};
    // /api/v1/stats 计算分位延迟时使用的滑动窗口样本数量。
    std::size_t latency_window_size{1024};
    // 集群视图（缺省 = 单节点模式）。
    ClusterView cluster_view;
};

// HttpServer：单节点三级池化 API 服务的 HTTP 接入层。
//
// "自研 Socket 级轻量 HTTP/1.1 服务" 的含义：
// - 直接基于 BSD socket / Winsock 写 accept 主循环，不依赖任何第三方 HTTP 库；
// - 仅实现 GET / POST + "一次读取 / 一次响应 / 主动关闭"模型，不做 keep-alive、
//   chunked、TLS，目的是把"HTTP 协议层"清晰暴露给学习者；
// - 参数走 query string（旧端点）或 Content-Length 请求体（通用 CRUD 端点，
//   见 handle_client 的读体段：超限 413、读体超时保护、路由前预校验请求行）。
//
// 本服务是纯 API 服务：不托管任何静态资源，客户端由独立的 SDK 承担。
// API 面共 9 个端点：4 个通用行级 CRUD + schema 自省 + 4 个健康/状态查询。
//
// 路由：由 route_table.h 的 constexpr 表集中声明，
//   route_request 按 RouteId 做 switch 分发。新增端点若忘记补 switch 分支，
//   会被 -Werror=switch 拦成编译错误，而不是静默退化成 404。
//
// 与三大池子的协作分工：
// 1) 接收 TCP 连接（accept 主循环跑在调用者线程，通常是 main）；
// 2) 把"处理这个 client_fd"的任务投递到 ThreadPool（避免阻塞 accept）；
// 3) 工作线程从 ObjectPool 借出 RequestContext（接收缓冲 + 响应字符串），
//    处理完成后 RAII 自动归还；
// 4) CRUD 路径里按表名经 DataSourceRegistry 路由到对应数据源，
//    再用 DbConnGuard 从该源的连接池借出连接，作用域结束自动归还。
//
// 可观测性：
//   /api/v1/stats 暴露三大池子的"瞬时 + 历史峰值"以及 API 请求的延迟分位
//   （avg / p50 / p95 / p99）；/metrics 以 Prometheus 文本格式暴露同一批数据。
class HttpServer {
public:
    HttpServer(uint16_t port,
               pool::ThreadPool& thread_pool,
               pool::ObjectPool<RequestContext>& request_pool,
               db::DataSourceRegistry& registry,
               HttpServerConfig config = {});
    // 在 .cpp 定义：router_ 是不完整类型的前向声明 + unique_ptr。
    ~HttpServer();

    // 阻塞启动：进入 accept 循环并持续向线程池分发客户端处理任务。
    // 收到 request_stop() 后优雅退出：停止 accept → 等待在途请求排空
    // （上限 5s）→ 返回。
    void start();

    // 请求优雅停机（信号安全：只置原子标志）。可从信号处理函数调用。
    void request_stop() { stopping_.store(true, std::memory_order_relaxed); }

    // 实际监听端口：port 0（临时端口）时由 bind 后回填，供测试连接。
    // start() 之前为 0。
    uint16_t bound_port() const { return bound_port_.load(std::memory_order_relaxed); }

    // 解析请求行、匹配路由并分发到对应 handler。
    // request_complete 为 false 表示请求行可能被接收缓冲截断；
    // body 是 Content-Length 模型读全的请求体（通用 CRUD 端点的 JSON 入参）。
    //
    // 公开的理由：它不碰 socket，可脱离监听直接测试
    // （test_metrics 等用它验证指标/状态端点）。
    // 注意它**不是无副作用**的：会更新计数器、采样延迟，经 CRUD 路由
    // 还会真实借连接读写后端 —— 测试里当它是"一次真实请求"对待。
    // 生产路径仍由 handle_client 调用。
    ApiResponse route_request(std::string_view request, bool request_complete,
                              std::string_view body = {});

    // 处理单个客户端连接（recv → 路由 → send → 关 fd，全程 RAII）。
    //
    // 公开的理由与 route_request 同源：它只依赖 fd + 三个池引用，
    // 测试（test_http_server）用 socketpair 直接驱动它验证
    // 对象池耗尽→503、DB 超时→503、计数器口径这些接线，无需真实监听。
    void handle_client(std::intptr_t client_fd, std::uint64_t request_id);

private:
    struct LatencyStats {
        double avg_ms{0.0};
        double p50_ms{0.0};
        double p95_ms{0.0};
        double p99_ms{0.0};
        std::size_t samples{0};
    };

    // ===== 各路由的 handler =====
    ApiResponse handle_health() const;
    ApiResponse handle_metrics() const;
    ApiResponse handle_stats() const;
    ApiResponse handle_get_config() const;
    ApiResponse handle_cluster() const;
    // 缓存 kill-switch（spec §8.3）：对全部数据源的共享缓存生效。
    ApiResponse handle_cache_control(bool enable);

    // 归队清缓存（spec §5.2 规则 B）：带 X-CppPools-Rejoin 头的请求触发，
    // 10s 节流，best-effort（非正确性依赖，无回报）。
    void handle_rejoin();

    // 记录 API 请求延迟样本，用于后续分位数统计。
    void record_api_latency_ms(double latency_ms);
    LatencyStats collect_latency_stats() const;

    uint16_t port_;
    // 共享线程池：执行 socket 处理任务。
    pool::ThreadPool& thread_pool_;
    // 请求上下文对象池。
    pool::ObjectPool<RequestContext>& request_pool_;
    // 数据库连接池。
    db::DataSourceRegistry& registry_;
    // 运行期调优参数（超时、统计窗口大小等）。
    HttpServerConfig config_;
    // 归属路由器（spec §3.6）：从 config_.cluster_view 建环，启动后只读。
    // 单节点模式下 decide 恒本地，行为与无集群时逐字节一致。
    std::unique_ptr<OwnershipRouter> router_;

    // 优雅停机标志：request_stop() 置位后 accept 循环退出（SIGTERM/SIGINT
    // 经 main 注册的信号处理函数调用 request_stop，只碰这个原子量，信号安全）。
    std::atomic<bool> stopping_{false};

    // Rejoin 节流（cluster-cache spec §5.2 规则 B）：上次执行归队清缓存的
    // steady_clock 毫秒时刻；10s 窗口内至多一次（CAS 抢占）。
    std::atomic<long long> last_rejoin_ms_{-(1LL << 40)};

    // bind 后的实际监听端口（port 0 时内核分配临时端口，回填供测试连接）。
    std::atomic<std::uint16_t> bound_port_{0};

    // 运行期监控计数器（用于可观测性输出）。
    // 这里使用 memory_order_relaxed 足够，因为我们只需要近似实时遥测值。
    //
    // 注意：池的峰值/最小值指标（peak_in_use / min_available）由各个池自己维护，
    // 不在这里重复跟踪——否则新增请求路径时很容易漏更新。
    std::atomic<std::uint64_t> total_requests_{0};
    std::atomic<std::uint64_t> api_requests_{0};
    std::atomic<std::uint64_t> error_requests_{0};
    // 因线程池队列已满而在 accept 循环里直接回 503 的连接数。
    //
    // 与 error_requests_ 分开：后者是“已进入处理链但出错”，
    // 本计数器是“根本没能进入处理链”——两者的运维含义完全不同：
    // 前者指向代码/后端问题，后者指向容量不足。
    std::atomic<std::uint64_t> rejected_requests_{0};
    std::atomic<std::uint64_t> active_requests_{0};
    std::atomic<std::uint64_t> db_timeout_requests_{0};
    std::atomic<std::uint64_t> peak_active_requests_{0};
    std::chrono::steady_clock::time_point started_at_{std::chrono::steady_clock::now()};

    mutable std::mutex latency_mutex_;
    std::deque<double> api_latencies_ms_;

    // Active socket tracking for graceful shutdown (Task 2).
    mutable std::mutex active_sockets_mutex_;
    std::unordered_set<std::intptr_t> active_sockets_;

    void register_active_socket(std::intptr_t fd);
    void unregister_active_socket(std::intptr_t fd);
    void interrupt_active_sockets() noexcept;
};

}  // namespace cpppools::server
