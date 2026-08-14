#include "cpppools/server/http_server.h"

#include "server/http_util.h"
#include "server/route_table.h"
#include "server/score_api.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SocketType = SOCKET;
static constexpr SocketType kInvalidSocket = INVALID_SOCKET;
using SsizeType = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketType = int;
static constexpr SocketType kInvalidSocket = -1;
using SsizeType = ssize_t;
#endif

// 版本号由 CMake 通过 target_compile_definitions 注入（见 CMakeLists.txt）。
// 直接用 clang++/g++ 手工编译时回退到 dev 标记。
#ifndef CPPPOOLS_VERSION
#define CPPPOOLS_VERSION "0.0.0-dev"
#endif

namespace cpppools::server {

using db::DbConnectionPool;
using pool::ObjectPool;
using pool::ThreadPool;

namespace {

void close_socket(SocketType s) {
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

// SocketGuard：把 socket 的关闭绑定到栈对象上。
//
// 为什么需要：手工在函数结尾调用 close_socket 时，一旦中途抛异常就会跳过关闭，
// 导致 fd 永久泄漏；而异常又会被 packaged_task 吞进一个被丢弃的 future，故障完全静默。
// 本项目通篇以 RAII 为主题，HTTP 层也必须如此。
class SocketGuard {
public:
    explicit SocketGuard(SocketType s) : sock_(s) {
#ifdef SO_NOSIGPIPE
        // macOS / BSD：按 socket 粒度禁止 SIGPIPE。
        // 不做这一步的后果很重：向已被对端关闭的连接 send 会收到 SIGPIPE，
        // 默认处置是**终止整个进程**——一个客户端超时就能搞挂服务。
        // 选用 per-socket 选项而非全局 signal()，避免库代码静默修改进程信号处置。
        const int on = 1;
        setsockopt(sock_, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    }

    ~SocketGuard() {
        if (sock_ != kInvalidSocket) {
            close_socket(sock_);
        }
    }

    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;

    SocketType get() const { return sock_; }

private:
    SocketType sock_;
};

// send 的平台差异：Linux 用 MSG_NOSIGNAL 抑制 SIGPIPE，
// macOS/BSD 已在 SocketGuard 里用 SO_NOSIGPIPE 处理，Windows 无此问题。
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

// ScopeExit：作用域退出时执行收尾动作，异常路径同样生效。
template <typename F>
class ScopeExit {
public:
    explicit ScopeExit(F&& f) : fn_(std::move(f)) {}

    // 必须 noexcept + 内部吃异常：若本对象是在异常展开期间被析构，
    // 而 fn_() 又抛出新异常，会直接 std::terminate，外层 try/catch 根本拦不住
    // ——那就正好抵消了引入 ScopeExit 想要的健壮性。
    ~ScopeExit() noexcept {
        try {
            fn_();
        } catch (...) {
            // 收尾动作不得改变异常传播路径。
        }
    }

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    F fn_;
};

// decay_t：避免传左值时 F 推导成引用类型，从而让 fn_ 变成悬垂引用成员。
template <typename F>
ScopeExit<std::decay_t<F>> make_scope_exit(F&& f) {
    return ScopeExit<std::decay_t<F>>(std::forward<F>(f));
}

// 原子更新峰值：仅当 candidate 更大时写入。
void update_peak(std::atomic<std::uint64_t>& peak, std::uint64_t candidate) {
    std::uint64_t current = peak.load(std::memory_order_relaxed);
    while (candidate > current &&
           !peak.compare_exchange_weak(current, candidate, std::memory_order_relaxed)) {
    }
}

ApiResponse make_text_response(std::string body) {
    ApiResponse resp;
    resp.content_type = "text/plain; charset=utf-8";
    resp.body = std::move(body);
    return resp;
}

// 路由是否属于业务 API（用于计数与延迟采样）。
// /health、/metrics、/api/v1/stats、/api/v1/get-config 都是基础设施/自省端点，
// 不计入 API 指标：否则采集器/监控轮询它们就会把延迟分位
// 压向“stats() 自身的成本”，污染业务口径。
bool is_api_route(RouteId id) {
    switch (id) {
        case RouteId::kHealth:
        case RouteId::kMetrics:
        case RouteId::kStats:
        case RouteId::kGetConfig:
            return false;
        case RouteId::kCreateScore:
        case RouteId::kGetScore:
        case RouteId::kUpdateScore:
        case RouteId::kDeleteScore:
            return true;
    }
    return false;
}

// 把缓冲区完整写到 socket。send 可能短写，循环直到发完或出错。
//
// 三个守卫缺一不可：
// - written < 0 且 errno == EINTR → 重试（信号打断不算错误）；
// - written < 0 其余 → 对端已断（如 EPIPE），返回 false，调用方关 fd；
// - written == 0 → 罕见但必须防：不防就是死循环，而且恰好发生在过载这种最糟时刻。
//
// 用 kSendFlags（Linux 上是 MSG_NOSIGNAL）避免 SIGPIPE 直接终止进程。
// 响应均为百字节级，新连接的发送缓冲必然容得下，因此在阻塞 socket 上
// 也不会真的卡住调用方。**若未来响应体变大，这个前提需要重新评估。**
bool send_all(SocketType fd, const char* data, std::size_t total) noexcept {
    std::size_t sent = 0;
    while (sent < total) {
        const SsizeType written = send(fd, data + sent, total - sent, kSendFlags);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

// 在 accept 循环里直接写一个最小 503，用于线程池队列已满的背压场景。
//
// 为何不走正常的 make_error_response + 池化缓冲：此时还没拿到 worker，
// 也没从 ObjectPool 借到 RequestContext，整个处理链都未开始。
// 过载时的响应必须代价最低，否则只会把过载放大。
//
// 写失败一律忽略：客户端可能已经走了，而此处本就是降级路径。
// 用 kSendFlags（Linux 上是 MSG_NOSIGNAL）+ EINTR 重试，与正常路径一致，
// 避免 SIGPIPE 直接终止进程。
void send_overload_response(SocketType fd) noexcept {
    // 使用 JSON 错误体，而不是纯文本：
    // docs/api/backend_api.md §1.2 声明“所有错误（4xx/5xx）统一为
    // {"message", "status_code"}”。用纯文本会让按文档实现的客户端 JSON 解析失败，
    // 把一个清楚的“服务过载”降级成模糊的客户端故障 —— 与回 503 的初衷相悖。
    static constexpr char kBody[] = R"({"message":"server overloaded","status_code":503})";
    static constexpr char kHead[] =
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 49\r\n"
        "Connection: close\r\n"
        "\r\n";
    // 这个 static_assert 目前真的能拦住错误：发送的 body 就是 kBody 本身。
    // （初版把完整响应写成一个字面量、又单独定义了一份 kBody 只用于 assert，
    //   结果改文案时 assert 照样通过，Content-Length 却已与真实 body 不符 ——
    //   那是一个给人虚假安全感的守卫，比没有守卫更危险。）
    static_assert(sizeof(kBody) - 1 == 49, "改 kBody 后必须同步 kHead 的 Content-Length");

    // 写失败一律忽略：客户端可能已经走了，而此处本就是降级路径。
    if (send_all(fd, kHead, sizeof(kHead) - 1)) {
        (void)send_all(fd, kBody, sizeof(kBody) - 1);
    }
}

}  // namespace

HttpServer::HttpServer(uint16_t port,
                       ThreadPool& thread_pool,
                       ObjectPool<RequestContext>& request_pool,
                       DbConnectionPool& db_pool,
                       HttpServerConfig config)
    : port_(port),
      thread_pool_(thread_pool),
      request_pool_(request_pool),
      db_pool_(db_pool),
      config_(std::move(config)) {
    if (config_.db_acquire_timeout <= std::chrono::milliseconds(0)) {
        config_.db_acquire_timeout = std::chrono::milliseconds(1);
    }
    if (config_.latency_window_size == 0) {
        config_.latency_window_size = 1;
    }
}

void HttpServer::start() {
#ifdef _WIN32
    // Windows 下必须先初始化 WinSock，才能调用 socket/bind/listen。
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        throw std::runtime_error("WSAStartup failed");
    }
#endif

    // 创建 TCP socket：AF_INET（IPv4）+ SOCK_STREAM（流式）。
    const SocketType server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == kInvalidSocket) {
        throw std::runtime_error("socket creation failed");
    }
    // 监听 socket 也交给 RAII：异常路径下不泄漏。
    SocketGuard server_guard(server_fd);

    // SO_REUSEADDR：允许端口在 TIME_WAIT 状态下被重复绑定，便于反复 Ctrl+C 重启。
    const int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   // 0.0.0.0：监听所有本机网卡。
    addr.sin_port = htons(port_);               // 主机字节序 -> 网络字节序。

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        throw std::runtime_error("bind failed");
    }

    // listen backlog=64：内核为半连接队列+全连接队列预留的容量上限。
    if (listen(server_fd, 64) < 0) {
        throw std::runtime_error("listen failed");
    }

    // 用 endl 而非 '\n'：start() 之后进入永不返回的 accept 循环，
    // 若不 flush，stdout 被重定向（systemd / docker / nohup）时启动横幅会永久留在缓冲区里。
    std::cout << "cpppools listening on http://127.0.0.1:" << port_ << std::endl;
    std::cout << "  db backend    : " << db_pool_.mode_name() << std::endl;
    std::cout << "  worker threads: " << thread_pool_.worker_count() << std::endl;
    std::cout << "  db pool size  : " << db_pool_.total_count() << std::endl;

    int request_id = 0;
    // 经典阻塞式 accept 循环：
    // 每次 accept 成功后，把客户端处理逻辑投递到线程池，确保主循环持续响应新连接。
    // 这就是三级池化的入口：accept 主循环只做调度，真正的 I/O + 计算都在 worker 线程上。
    while (true) {
        sockaddr_in client_addr{};
#ifdef _WIN32
        int len = sizeof(client_addr);
#else
        socklen_t len = sizeof(client_addr);
#endif
        const SocketType client_fd =
            accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &len);
        if (client_fd == kInvalidSocket) {
            if (errno == EINTR) {
                continue;  // 被信号打断，重试
            }
            // 其余错误（如 EMFILE fd 耗尽）：短暂让出 CPU，避免 100% 忙循环。
            std::cerr << "[warn] accept failed: errno=" << errno << '\n';
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        ++request_id;

        try {
            // 用 submit_detached 而不是 submit：本处不需要 future，
            // 而 submit 的 future 一旦被丢弃，handle_client 里抛的异常就永久静默
            // （packaged_task 会把异常存进无人查看的 future 而不重抛）。
            // submit_detached 会把异常交给 ThreadPoolConfig::on_task_exception。
            if (!thread_pool_.submit_detached([this, client_fd, request_id]() {
                    this->handle_client(static_cast<std::intptr_t>(client_fd), request_id);
                })) {
                // 队列已满 = 过载。真实背压：直接回 503 让客户端立即知道，
                // 而不是默默丢弃连接让客户端挂到超时。
                // 注意此处尚未读请求，HTTP 允许服务端在收到完整请求前就响应。
                rejected_requests_.fetch_add(1, std::memory_order_relaxed);
                send_overload_response(client_fd);
                close_socket(client_fd);
            }
        } catch (const pool::PoolShutdownError& ex) {
            // 池已关闭 = 服务正在停机。不必再写响应，直接关这条连接，
            // 并继续 accept 循环（而不是让整个循环随之退出）。
            //
            // 必须与上面的“队列满”分开处理：两者都是“没提交成功”，
            // 但一个该告知客户端过载，一个不该——这也是 submit_detached
            // 在池已关闭时抛异常而不是返回 false 的原因。
            std::cerr << "[error] submit failed: " << ex.what() << '\n';
            close_socket(client_fd);
        } catch (const std::exception& ex) {
            // 提交本身失败：捕获物移动抛出、或队列节点分配 bad_alloc。
            //
            // 必须在这里接住：过载时内存压力最大，恰是 bad_alloc 最可能出现的时刻。
            // 若让它穿出 accept 循环，main 的 catch 会打印并 return 1 ——
            // 一次瞬时分配失败就把整个服务干掉，与本批“过载时不要垮”的目标直接冲突。
            //
            // 不写响应体：此时连分配都可能失败，再去造响应只会雪上加霜。
            std::cerr << "[error] submit threw: " << ex.what() << '\n';
            rejected_requests_.fetch_add(1, std::memory_order_relaxed);
            close_socket(client_fd);
        }
    }
}

void HttpServer::handle_client(std::intptr_t client_fd, int request_id) {
    // socket 交给 RAII 托管：无论正常返回还是抛异常，析构都会关闭它。
    SocketGuard guard(static_cast<SocketType>(client_fd));

    // 三级池化的第二级：从对象池借出接收缓冲与响应字符串，作用域结束自动归还。
    auto ctx = request_pool_.acquire();
    if (!ctx) {
        // 对象池已达 max_size 且无空闲。必须判空：max_size >= 并发需求
        // 的不变式由 main.cpp 的运行期配置维持，不是类型保证 ——
        // 不判空的后果是下一行直接空指针解引用崩服务（见 spec §5.1.1）。
        //
        // 指标口径：与连接池超时一致——此请求已进入处理链并产出了响应，
        // 因此 total_requests_ 与 error_requests_ 都计。（对比：accept 循环
        // 的队列满拒绝根本没进处理链，只计 rejected_requests_。）
        total_requests_.fetch_add(1, std::memory_order_relaxed);
        error_requests_.fetch_add(1, std::memory_order_relaxed);
        // 过载时每次打日志会刷屏（e2e 实测 40 连接打出 37 行），只提示一次。
        static std::atomic_flag pool_exhausted_warned = ATOMIC_FLAG_INIT;
        if (!pool_exhausted_warned.test_and_set(std::memory_order_relaxed)) {
            std::cerr << "[warn] request pool exhausted (this warning prints once)\n";
        }
        const ApiResponse resp =
            make_error_response(503, "Service Unavailable", "request pool exhausted");
        // 局部缓冲即可：响应仅约 100 字节。此时拿不到池化的 ctx->response
        // （ctx 正是借不出来的那个东西）。
        std::string response_buf;
        append_http_response(resp, response_buf);
        (void)send_all(guard.get(), response_buf.data(), response_buf.size());
        return;
    }
    ctx->request_id = request_id;

    total_requests_.fetch_add(1, std::memory_order_relaxed);
    const auto active = active_requests_.fetch_add(1, std::memory_order_relaxed) + 1;
    update_peak(peak_active_requests_, active);
    const auto release_active = make_scope_exit([this]() {
        // 无论从哪条路径退出（含抛异常）都必须递减，否则该指标只增不减、永久失真。
        active_requests_.fetch_sub(1, std::memory_order_relaxed);
    });
    (void)release_active;

    // 仅处理"一次读取 + 一次响应 + 主动关闭连接"模型，不支持 keep-alive 与 chunked。
    // 由于所有参数都走 query string，请求体不需要读取。
    SsizeType n = 0;
    do {
        n = recv(guard.get(), ctx->buffer.data(), kRequestBufferBytes, 0);
    } while (n < 0 && errno == EINTR);  // 被信号打断应重试，不算错误

    if (n <= 0) {
        return;
    }
    const auto received = static_cast<std::size_t>(n);
    // 读满缓冲意味着请求可能还有未读部分，请求行未必完整。
    // 把这个事实传给路由层，由它区分 400（格式错）与 414（URL 过长）。
    const bool request_complete = received < kRequestBufferBytes;
    const std::string_view request(ctx->buffer.data(), received);

    ApiResponse resp;
    try {
        resp = route_request(request, request_complete);
    } catch (const std::exception& ex) {
        // 兜底：任何未预期异常都转成 500，而不是让连接静默悬挂。
        error_requests_.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "[error] request_id=" << request_id << " exception: " << ex.what() << '\n';
        resp = make_error_response(500, "Internal Server Error", ex.what());
    } catch (...) {
        error_requests_.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "[error] request_id=" << request_id << " unknown exception\n";
        resp = make_error_response(500, "Internal Server Error", "unknown internal error");
    }

    // 写入池化的 response 缓冲（reset() 已 clear 但保留 capacity）。
    append_http_response(resp, ctx->response);

    // 发送失败不影响计数（请求已被完整处理），guard 会关闭 socket。
    (void)send_all(guard.get(), ctx->response.data(), ctx->response.size());
}

ApiResponse HttpServer::route_request(std::string_view request, bool request_complete) {
    // 零拷贝解析请求行；所有 string_view 都借自 request，仅在本函数调用期内有效。
    const auto line = parse_request_line(request);
    if (!line.has_value()) {
        error_requests_.fetch_add(1, std::memory_order_relaxed);
        if (!request_complete) {
            // 读满缓冲且解析失败：基本可断定是请求行超长。
            // 这里必须显式报错，而不能宽容处理——否则截断后的参数值
            // （如 score=98765 被截成 score=98）会被当成合法输入写入数据库。
            return make_error_response(414, "URI Too Long", "request line is too long");
        }
        return make_error_response(400, "Bad Request", "malformed request line");
    }

    const TargetParts target = split_target(line->target);
    const RouteMatch match = find_route(target.path, line->method);

    if (match.status == MatchStatus::kNoSuchPath) {
        error_requests_.fetch_add(1, std::memory_order_relaxed);
        return make_error_response(404, "Not Found", "not found");
    }
    if (match.status == MatchStatus::kMethodMismatch) {
        // 路径存在但方法不匹配 -> 405（而不是把不存在的路径也回 405）。
        error_requests_.fetch_add(1, std::memory_order_relaxed);
        ApiResponse resp =
            make_error_response(405, "Method Not Allowed", "method not allowed for this path");
        // RFC 7231 6.5.5 要求 405 必须带 Allow 头，路由表里现成有这个信息。
        if (match.spec != nullptr) {
            resp.allow = (match.spec->method == HttpMethod::kPost) ? "POST" : "GET";
        }
        return resp;
    }

    const RouteId id = match.spec->id;
    const bool api_route = is_api_route(id);
    if (api_route) {
        api_requests_.fetch_add(1, std::memory_order_relaxed);
    }

    // 必需参数缺失统一回 400，与 Alluxio 的 mRequiredQueryParams 校验同款。
    if (const auto missing = first_missing_param(*match.spec, target.query)) {
        error_requests_.fetch_add(1, std::memory_order_relaxed);
        std::ostringstream msg;
        msg << "missing required parameter: " << *missing;
        return make_error_response(400, "Bad Request", msg.str());
    }

    const auto begin = std::chrono::steady_clock::now();
    const ScoreApiContext api_ctx{db_pool_, target.query, config_.db_acquire_timeout};

    ApiResponse resp;
    // switch 覆盖全部 RouteId：新增路由若忘记补分支，-Werror=switch 会报编译错误。
    switch (id) {
        case RouteId::kHealth:      resp = handle_health();                  break;
        case RouteId::kMetrics:     resp = handle_metrics();                 break;
        case RouteId::kStats:       resp = handle_stats();                   break;
        case RouteId::kGetConfig:   resp = handle_get_config();              break;
        case RouteId::kCreateScore: resp = handle_create_score(api_ctx);     break;
        case RouteId::kGetScore:    resp = handle_get_score(api_ctx);        break;
        case RouteId::kUpdateScore: resp = handle_update_score(api_ctx);     break;
        case RouteId::kDeleteScore: resp = handle_delete_score(api_ctx);     break;
    }

    if (api_route) {
        const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - begin)
                                    .count();
        record_api_latency_ms(static_cast<double>(elapsed_us) / 1000.0);
    }

    // 统一收口错误计数：handler 只管产出状态码，计数由这里统一处理。
    if (resp.status_code >= 400) {
        error_requests_.fetch_add(1, std::memory_order_relaxed);
    }
    if (resp.status_code == 503) {
        // 口径不变式：route_request 内 503 的**唯一**来源是借 DB 连接超时，
        // 所以按状态码计与按成因计当前等价。若未来新增其他 503 来源
        // （对象池路径挪入、DB 改造后的新端点），必须把该计数改成
        // 由 handler 显式上报成因，否则指标会静默撒谎。
        db_timeout_requests_.fetch_add(1, std::memory_order_relaxed);
    }
    return resp;
}

ApiResponse HttpServer::handle_health() const {
    // 存活探针刻意返回纯文本而非 JSON：契约越简单越不容易随业务演进而破坏。
    return make_text_response("cpppools worker is active");
}

ApiResponse HttpServer::handle_metrics() const {
    const auto uptime = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started_at_)
                            .count();
    const LatencyStats lstats = collect_latency_stats();

    // 每个池只调一次 stats()：一次锁取一致快照，而不是连调多个 getter
    // （每个 getter 都转调 stats()，会变成 N 次完整快照）。
    const pool::TaskStats tps = thread_pool_.stats();
    const pool::AcquireStats ops = request_pool_.stats();
    const db::DbPoolStats dps = db_pool_.stats();

    // Prometheus exposition 文本格式：counter 带 _total 后缀，单位写入指标名。
    // 池指标统一带 pool 标签：DB 中间件改造后会有 N 个连接池，
    // 无标签的指标名到那时无法区分数据源。指标名直接换新、不双写旧名
    // （仓内确认无消费者，见 spec §7 决策 15）。
    std::ostringstream out;
    out << "# HELP cpppools_uptime_ms Process uptime in milliseconds.\n"
        << "# TYPE cpppools_uptime_ms gauge\n"
        << "cpppools_uptime_ms " << uptime << "\n"
        << "# HELP cpppools_requests_total Total HTTP requests received.\n"
        << "# TYPE cpppools_requests_total counter\n"
        << "cpppools_requests_total " << total_requests_.load(std::memory_order_relaxed) << "\n"
        << "# HELP cpppools_api_requests_total Total business API requests served.\n"
        << "# TYPE cpppools_api_requests_total counter\n"
        << "cpppools_api_requests_total " << api_requests_.load(std::memory_order_relaxed) << "\n"
        << "# HELP cpppools_error_requests_total Total responses with status >= 400.\n"
        << "# TYPE cpppools_error_requests_total counter\n"
        << "cpppools_error_requests_total " << error_requests_.load(std::memory_order_relaxed) << "\n"
        << "# HELP cpppools_rejected_requests_total Connections rejected in accept loop: "
           "thread pool queue full, or submission itself failed (e.g. bad_alloc).\n"
        << "# TYPE cpppools_rejected_requests_total counter\n"
        << "cpppools_rejected_requests_total "
        << rejected_requests_.load(std::memory_order_relaxed) << "\n"
        << "# HELP cpppools_db_acquire_timeout_total Requests that got 503 because "
           "borrowing a DB connection timed out (request level; the pool-level "
           "counter is cpppools_pool_acquire_failed_total).\n"
        << "# TYPE cpppools_db_acquire_timeout_total counter\n"
        << "cpppools_db_acquire_timeout_total " << db_timeout_requests_.load(std::memory_order_relaxed) << "\n"
        << "# HELP cpppools_active_requests Requests currently in flight.\n"
        << "# TYPE cpppools_active_requests gauge\n"
        << "cpppools_active_requests " << active_requests_.load(std::memory_order_relaxed) << "\n";

    // ===== 线程池（TaskStats）=====
    // 命名约定与其他池一致（counter 带 _total、峰值带 peak_），但形状不同：
    // 线程池不是“借→还”池，不复用 AcquireStats（见 spec §7）。
    const std::string tp_label = "{pool=\"" + tps.name + "\"}";
    out << "# HELP cpppools_thread_pool_workers Configured worker thread count.\n"
        << "# TYPE cpppools_thread_pool_workers gauge\n"
        << "cpppools_thread_pool_workers" << tp_label << " " << tps.worker_count << "\n"
        << "# HELP cpppools_thread_pool_queue_capacity Task queue capacity.\n"
        << "# TYPE cpppools_thread_pool_queue_capacity gauge\n"
        << "cpppools_thread_pool_queue_capacity" << tp_label << " " << tps.queue_capacity << "\n"
        << "# HELP cpppools_thread_pool_queue_pending Tasks currently queued.\n"
        << "# TYPE cpppools_thread_pool_queue_pending gauge\n"
        << "cpppools_thread_pool_queue_pending" << tp_label << " " << tps.queue_pending << "\n"
        << "# HELP cpppools_thread_pool_peak_queue_pending Historical peak of queued tasks.\n"
        << "# TYPE cpppools_thread_pool_peak_queue_pending gauge\n"
        << "cpppools_thread_pool_peak_queue_pending" << tp_label << " " << tps.peak_queue_pending << "\n"
        << "# HELP cpppools_thread_pool_submitted_total Tasks accepted into the queue.\n"
        << "# TYPE cpppools_thread_pool_submitted_total counter\n"
        << "cpppools_thread_pool_submitted_total" << tp_label << " " << tps.submitted_total << "\n"
        << "# HELP cpppools_thread_pool_rejected_total Tasks rejected because the queue was full.\n"
        << "# TYPE cpppools_thread_pool_rejected_total counter\n"
        << "cpppools_thread_pool_rejected_total" << tp_label << " " << tps.rejected_total << "\n"
        << "# HELP cpppools_thread_pool_completed_total Tasks finished execution.\n"
        << "# TYPE cpppools_thread_pool_completed_total counter\n"
        << "cpppools_thread_pool_completed_total" << tp_label << " " << tps.completed_total << "\n"
        << "# HELP cpppools_thread_pool_task_exception_total Task exceptions reported via "
           "on_task_exception. WARNING: only covers submit_detached tasks; exceptions of "
           "submit() tasks are carried by their futures and NOT counted here. A value of "
           "0 does NOT mean no exceptions occurred.\n"
        << "# TYPE cpppools_thread_pool_task_exception_total counter\n"
        << "cpppools_thread_pool_task_exception_total" << tp_label << " " << tps.task_exception_total << "\n"
        << "# HELP cpppools_thread_pool_task_exec_us_total Cumulative task execution time (us).\n"
        << "# TYPE cpppools_thread_pool_task_exec_us_total counter\n"
        << "cpppools_thread_pool_task_exec_us_total" << tp_label << " " << tps.task_exec_us_total << "\n"
        << "# HELP cpppools_thread_pool_peak_task_exec_us Longest single task execution (us).\n"
        << "# TYPE cpppools_thread_pool_peak_task_exec_us gauge\n"
        << "cpppools_thread_pool_peak_task_exec_us" << tp_label << " " << tps.peak_task_exec_us << "\n";

    // ===== 借→还池（ObjectPool / DbConnectionPool）：同一指标族 + pool 标签 =====
    // 空槽语义：available 含重建失败的空槽；size 只数存活连接/对象，
    // 因此 DbPool 的等式是 in_use + available == capacity、size <= capacity。
    const auto emit_acquire_family = [&out](const pool::AcquireStats& s) {
        const std::string label = "{pool=\"" + s.name + "\"}";
        out << "cpppools_pool_capacity" << label << " " << s.capacity << "\n"
            << "cpppools_pool_size" << label << " " << s.size << "\n"
            << "cpppools_pool_in_use" << label << " " << s.in_use << "\n"
            << "cpppools_pool_available" << label << " " << s.available << "\n"
            << "cpppools_pool_peak_in_use" << label << " " << s.peak_in_use << "\n"
            << "cpppools_pool_min_available" << label << " " << s.min_available << "\n"
            << "cpppools_pool_acquire_total" << label << " " << s.acquire_total << "\n"
            << "cpppools_pool_acquire_failed_total" << label << " " << s.acquire_failed_total << "\n";
    };
    out << "# HELP cpppools_pool_capacity Pool capacity (hard upper bound).\n"
        << "# TYPE cpppools_pool_capacity gauge\n"
        << "# HELP cpppools_pool_size Allocated objects / alive connections "
           "(<= capacity; empty rebuild slots excluded for DB pool).\n"
        << "# TYPE cpppools_pool_size gauge\n"
        << "# HELP cpppools_pool_in_use Currently borrowed.\n"
        << "# TYPE cpppools_pool_in_use gauge\n"
        << "# HELP cpppools_pool_available Idle (includes empty rebuild slots for DB pool).\n"
        << "# TYPE cpppools_pool_available gauge\n"
        << "# HELP cpppools_pool_peak_in_use Historical peak of concurrent borrows.\n"
        << "# TYPE cpppools_pool_peak_in_use gauge\n"
        << "# HELP cpppools_pool_min_available Historical minimum of idle count.\n"
        << "# TYPE cpppools_pool_min_available gauge\n"
        << "# HELP cpppools_pool_acquire_total Acquire calls.\n"
        << "# TYPE cpppools_pool_acquire_total counter\n"
        << "# HELP cpppools_pool_acquire_failed_total Failed acquires: object pool hit "
           "max_size; DB pool timed out. Failure rate = failed / total.\n"
        << "# TYPE cpppools_pool_acquire_failed_total counter\n";
    emit_acquire_family(ops);
    emit_acquire_family(dps.base);

    // ===== DB 池特有计数 =====
    const std::string db_label = "{pool=\"" + dps.base.name + "\"}";
    out << "# HELP cpppools_db_pool_invalid_release_total Duplicate or foreign pointer releases.\n"
        << "# TYPE cpppools_db_pool_invalid_release_total counter\n"
        << "cpppools_db_pool_invalid_release_total" << db_label << " " << dps.invalid_release_total << "\n"
        << "# HELP cpppools_db_pool_rebuild_total Connections rebuilt after failed health probe.\n"
        << "# TYPE cpppools_db_pool_rebuild_total counter\n"
        << "cpppools_db_pool_rebuild_total" << db_label << " " << dps.rebuild_total << "\n"
        << "# HELP cpppools_db_pool_rebuild_failed_total Rebuild attempts that failed "
           "(slot kept empty for self-healing).\n"
        << "# TYPE cpppools_db_pool_rebuild_failed_total counter\n"
        << "cpppools_db_pool_rebuild_failed_total" << db_label << " " << dps.rebuild_failed_total << "\n";

    out << "# HELP cpppools_api_latency_ms Latency quantiles of business API requests.\n"
        << "# TYPE cpppools_api_latency_ms summary\n"
        << std::fixed << std::setprecision(3)
        << "cpppools_api_latency_ms{quantile=\"0.5\"} " << lstats.p50_ms << "\n"
        << "cpppools_api_latency_ms{quantile=\"0.95\"} " << lstats.p95_ms << "\n"
        << "cpppools_api_latency_ms{quantile=\"0.99\"} " << lstats.p99_ms << "\n"
        << "cpppools_api_latency_ms_count " << lstats.samples << "\n";

    return make_text_response(out.str());
}

ApiResponse HttpServer::handle_stats() const {
    const auto uptime = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started_at_)
                            .count();
    const LatencyStats lstats = collect_latency_stats();

    // 每池一次快照（同 handle_metrics）。
    const pool::TaskStats tps = thread_pool_.stats();
    const pool::AcquireStats ops = request_pool_.stats();
    const db::DbPoolStats dps = db_pool_.stats();

    std::ostringstream body;
    body << "{"
         << "\"uptime_ms\":" << uptime << ","
         << "\"total_requests\":" << total_requests_.load(std::memory_order_relaxed) << ","
         << "\"api_requests\":" << api_requests_.load(std::memory_order_relaxed) << ","
         << "\"error_requests\":" << error_requests_.load(std::memory_order_relaxed) << ","
         << "\"rejected_requests\":" << rejected_requests_.load(std::memory_order_relaxed) << ","
         << "\"active_requests\":" << active_requests_.load(std::memory_order_relaxed) << ","
         << "\"peak_active_requests\":" << peak_active_requests_.load(std::memory_order_relaxed) << ","
         << "\"db_timeout_requests\":" << db_timeout_requests_.load(std::memory_order_relaxed) << ","
         << "\"db_mode\":\"" << db_pool_.mode_name() << "\","
         // 线程池：旧字段名保留（thread_pool_workers/pending/peak_thread_pool_pending），
         // 新增队列容量与四个计数器。
         << "\"thread_pool_workers\":" << tps.worker_count << ","
         << "\"thread_pool_queue_capacity\":" << tps.queue_capacity << ","
         << "\"thread_pool_pending\":" << tps.queue_pending << ","
         << "\"peak_thread_pool_pending\":" << tps.peak_queue_pending << ","
         << "\"thread_pool_submitted_total\":" << tps.submitted_total << ","
         << "\"thread_pool_rejected_total\":" << tps.rejected_total << ","
         << "\"thread_pool_completed_total\":" << tps.completed_total << ","
         << "\"thread_pool_task_exception_total\":" << tps.task_exception_total << ","
         << "\"thread_pool_task_exec_us_total\":" << tps.task_exec_us_total << ","
         << "\"thread_pool_peak_task_exec_us\":" << tps.peak_task_exec_us << ","
         // 连接池：旧字段保留（db_pool_free 含空槽）；
         // db_pool_alive 是存活连接数（重建失败的空槽不计），暴露退化程度。
         << "\"db_pool_free\":" << dps.base.available << ","
         << "\"db_pool_total\":" << dps.base.capacity << ","
         << "\"db_pool_alive\":" << dps.base.size << ","
         << "\"min_db_pool_free\":" << dps.base.min_available << ","
         << "\"db_pool_acquire_total\":" << dps.base.acquire_total << ","
         << "\"db_pool_acquire_failed_total\":" << dps.base.acquire_failed_total << ","
         << "\"db_pool_invalid_release_total\":" << dps.invalid_release_total << ","
         << "\"db_pool_rebuild_total\":" << dps.rebuild_total << ","
         << "\"db_pool_rebuild_failed_total\":" << dps.rebuild_failed_total << ","
         // 对象池：旧字段保留；新增容量与 acquire 计数。
         << "\"memory_pool_in_use\":" << ops.in_use << ","
         << "\"memory_pool_available\":" << ops.available << ","
         << "\"memory_pool_total\":" << ops.size << ","
         << "\"memory_pool_capacity\":" << ops.capacity << ","
         << "\"peak_memory_pool_in_use\":" << ops.peak_in_use << ","
         << "\"memory_pool_acquire_total\":" << ops.acquire_total << ","
         << "\"memory_pool_acquire_failed_total\":" << ops.acquire_failed_total << ","
         << "\"db_acquire_timeout_ms\":" << config_.db_acquire_timeout.count() << ","
         << "\"latency_window_size\":" << config_.latency_window_size << ","
         << "\"latency_samples\":" << lstats.samples << ","
         << std::fixed << std::setprecision(3)
         << "\"avg_latency_ms\":" << lstats.avg_ms << ","
         << "\"p50_latency_ms\":" << lstats.p50_ms << ","
         << "\"p95_latency_ms\":" << lstats.p95_ms << ","
         << "\"p99_latency_ms\":" << lstats.p99_ms
         << "}";

    ApiResponse resp;
    resp.body = body.str();
    return resp;
}

ApiResponse HttpServer::handle_get_config() const {
    // 启动期配置快照：让客户端 / SDK 无需读服务端配置文件即可自省运行参数。
    const pool::TaskStats tps = thread_pool_.stats();
    const pool::AcquireStats ops = request_pool_.stats();
    std::ostringstream body;
    body << "{"
         << "\"version\":\"" << CPPPOOLS_VERSION << "\","
         << "\"db_mode\":\"" << db_pool_.mode_name() << "\","
         << "\"db_pool_total\":" << db_pool_.total_count() << ","
         << "\"db_acquire_timeout_ms\":" << config_.db_acquire_timeout.count() << ","
         << "\"thread_pool_workers\":" << tps.worker_count << ","
         << "\"thread_pool_queue_capacity\":" << tps.queue_capacity << ","
         << "\"memory_pool_total\":" << ops.size << ","
         << "\"memory_pool_capacity\":" << ops.capacity << ","
         << "\"latency_window_size\":" << config_.latency_window_size << ","
         << "\"max_request_bytes\":" << kRequestBufferBytes << ","
         << "\"endpoint_count\":" << (route_table_end() - route_table_begin())
         << "}";

    ApiResponse resp;
    resp.body = body.str();
    return resp;
}

void HttpServer::record_api_latency_ms(double latency_ms) {
    // 过滤 nan / inf / 负值：否则 avg 与分位数会输出非法 JSON 字面量。
    if (!std::isfinite(latency_ms) || latency_ms < 0.0) {
        return;
    }
    std::lock_guard<std::mutex> lock(latency_mutex_);
    api_latencies_ms_.push_back(latency_ms);
    while (api_latencies_ms_.size() > config_.latency_window_size) {
        api_latencies_ms_.pop_front();
    }
}

HttpServer::LatencyStats HttpServer::collect_latency_stats() const {
    std::vector<double> snapshot;
    {
        std::lock_guard<std::mutex> lock(latency_mutex_);
        snapshot.assign(api_latencies_ms_.begin(), api_latencies_ms_.end());
    }

    LatencyStats stats;
    if (snapshot.empty()) {
        return stats;
    }

    double sum_ms = 0.0;
    for (const double v : snapshot) {
        sum_ms += v;
    }

    std::sort(snapshot.begin(), snapshot.end());
    stats.samples = snapshot.size();
    stats.avg_ms = sum_ms / static_cast<double>(stats.samples);
    stats.p50_ms = percentile_nearest_rank(snapshot, 0.50);
    stats.p95_ms = percentile_nearest_rank(snapshot, 0.95);
    stats.p99_ms = percentile_nearest_rank(snapshot, 0.99);
    return stats;
}

}  // namespace cpppools::server
