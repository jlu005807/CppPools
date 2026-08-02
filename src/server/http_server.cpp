#include "server/http_server.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SocketType = SOCKET;
static constexpr SocketType kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketType = int;
static constexpr SocketType kInvalidSocket = -1;
#endif

namespace {
void close_socket(SocketType s) {
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

// 规范化错误消息字符串，避免 JSON 中出现首尾空白或换行。
std::string trim(const std::string& s) {
    std::size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])) != 0) {
        ++start;
    }
    std::size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) {
        --end;
    }
    return s.substr(start, end - start);
}

struct ParsedTarget {
    std::string route_path;
    std::string query;
};

ParsedTarget parse_target(const std::string& target) {
    const auto qmark = target.find('?');
    if (qmark == std::string::npos) {
        return {target, {}};
    }
    return {target.substr(0, qmark), target.substr(qmark + 1)};
}

// 从 query string 中提取指定 key 的值，不做 URL 解码。
std::string get_query_value(const std::string& query, const std::string& key) {
    std::size_t begin = 0;
    while (begin <= query.size()) {
        const auto amp = query.find('&', begin);
        const std::string pair = (amp == std::string::npos)
            ? query.substr(begin)
            : query.substr(begin, amp - begin);

        const auto eq = pair.find('=');
        const std::string k = (eq == std::string::npos) ? pair : pair.substr(0, eq);
        if (k == key) {
            return (eq == std::string::npos) ? std::string{} : pair.substr(eq + 1);
        }

        if (amp == std::string::npos) {
            break;
        }
        begin = amp + 1;
    }
    return {};
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string static_content_type(const std::string& asset_path) {
    if (ends_with(asset_path, ".html")) {
        return "text/html; charset=utf-8";
    }
    if (ends_with(asset_path, ".js")) {
        return "application/javascript; charset=utf-8";
    }
    if (ends_with(asset_path, ".css")) {
        return "text/css; charset=utf-8";
    }
    return "text/plain; charset=utf-8";
}

bool is_allowed_static_route(const std::string& route_path) {
    static const std::array<const char*, 2> kAllowed = {
        "/index.html",
        "/app.js"
    };
    for (const char* v : kAllowed) {
        if (route_path == v) {
            return true;
        }
    }
    return false;
}

// 解析“非负 int”参数，失败时返回 false。
bool parse_non_negative_int(const std::string& s, int* value) {
    if (value == nullptr || s.empty()) {
        return false;
    }

    errno = 0;
    char* end = nullptr;
    const long parsed = std::strtol(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0') {
        return false;
    }
    if (parsed < 0 || parsed > INT_MAX) {
        return false;
    }
    *value = static_cast<int>(parsed);
    return true;
}

// 解析“非负 unsigned int”参数，失败时返回 false。
bool parse_non_negative_uint(const std::string& s, unsigned int* value) {
    if (value == nullptr || s.empty()) {
        return false;
    }

    errno = 0;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0') {
        return false;
    }
    if (parsed > static_cast<unsigned long>(UINT_MAX)) {
        return false;
    }
    *value = static_cast<unsigned int>(parsed);
    return true;
}

// 原子更新峰值：仅当 candidate 更大时写入。
void update_peak(std::atomic<std::uint64_t>& peak, std::uint64_t candidate) {
    std::uint64_t current = peak.load(std::memory_order_relaxed);
    while (candidate > current &&
           !peak.compare_exchange_weak(current, candidate, std::memory_order_relaxed)) {
    }
}

// 原子更新最小值：仅当 candidate 更小时写入。
void update_min(std::atomic<std::uint64_t>& min_value, std::uint64_t candidate) {
    std::uint64_t current = min_value.load(std::memory_order_relaxed);
    while (candidate < current &&
           !min_value.compare_exchange_weak(current, candidate, std::memory_order_relaxed)) {
    }
}

// 最近邻秩法分位数（Nearest Rank）。
double percentile_nearest_rank(std::vector<double>& sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    const std::size_t n = sorted.size();
    const std::size_t rank = static_cast<std::size_t>(std::ceil(p * static_cast<double>(n)));
    const std::size_t safe_rank = std::max<std::size_t>(rank, 1);
    const std::size_t index = std::min<std::size_t>(safe_rank - 1, n - 1);
    return sorted[index];
}
}  // namespace

HttpServer::HttpServer(uint16_t port,
                       ThreadPool& thread_pool,
                       ObjectPool<RequestContext>& request_pool,
                       DBConnectionPool& db_pool,
                       std::string web_root,
                       HttpServerConfig config)
    : port_(port),
      thread_pool_(thread_pool),
      request_pool_(request_pool),
      db_pool_(db_pool),
      web_root_(std::move(web_root)),
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
    // 这是 Winsock 与 BSD socket 的最大差异之一：句柄类型也不同（SOCKET vs int）。
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        throw std::runtime_error("WSAStartup failed");
    }
#endif

    // 创建 TCP socket：AF_INET（IPv4）+ SOCK_STREAM（流式）。
    SocketType server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == kInvalidSocket) {
        throw std::runtime_error("socket creation failed");
    }

    // SO_REUSEADDR：允许端口在 TIME_WAIT 状态下被重复绑定，便于反复 Ctrl+C 重启。
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   // 0.0.0.0：监听所有本机网卡。
    addr.sin_port = htons(port_);               // 主机字节序 -> 网络字节序。

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close_socket(server_fd);
        throw std::runtime_error("bind failed");
    }

    // listen backlog=64：内核为半连接队列+全连接队列预留的容量上限。
    if (listen(server_fd, 64) < 0) {
        close_socket(server_fd);
        throw std::runtime_error("listen failed");
    }

    std::cout << "HTTP server listening on http://127.0.0.1:" << port_ << '\n';
    std::cout << "thread_pool workers active, db free/total = "
              << db_pool_.free_count() << "/" << db_pool_.total_count() << '\n';

    int request_id = 0;
    // 经典阻塞式 accept 循环：
    // 每次 accept 成功后，把客户端处理逻辑投递到线程池，确保主循环持续响应新连接。
    // 这就是"三级池化"的入口：accept 主循环只做调度，真正的 I/O+计算都在 worker 线程上。
    while (true) {
        sockaddr_in client_addr{};
#ifdef _WIN32
        int len = sizeof(client_addr);
#else
        socklen_t len = sizeof(client_addr);
#endif
        SocketType client_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &len);
        if (client_fd == kInvalidSocket) {
            continue;
        }
        ++request_id;

        try {
            thread_pool_.submit([this, client_fd, request_id]() {
                this->handle_client(static_cast<std::intptr_t>(client_fd), request_id);
            });
        } catch (...) {
            close_socket(client_fd);
            throw;
        }
    }
}

void HttpServer::handle_client(std::intptr_t client_fd, int request_id) {
    SocketType sock = static_cast<SocketType>(client_fd);
    char buffer[4096];
    // 教学版本仅处理“一次读取 + 一次响应 + 主动关闭连接”模型，
    // 暂不支持 keep-alive 与 chunked 解析。
    const int n = recv(sock, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) {
        close_socket(sock);
        return;
    }
    buffer[n] = '\0';
    std::string request(buffer);

    total_requests_.fetch_add(1, std::memory_order_relaxed);
    const std::string response = route_request(request, request_id);
    send(sock, response.c_str(), static_cast<int>(response.size()), 0);
    close_socket(sock);
}

std::string HttpServer::build_http_response(const std::string& body,
                                            const std::string& content_type,
                                            int status_code,
                                            const std::string& status_text) const {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status_code << " " << status_text << "\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    return oss.str();
}

std::string HttpServer::route_request(const std::string& request, int request_id) {
    // 解析请求首行：METHOD PATH HTTP/VERSION
    std::istringstream req_stream(request);
    std::string method;
    std::string target;
    std::string version;
    req_stream >> method >> target >> version;
    const ParsedTarget parsed = parse_target(target);
    const std::string route_path = parsed.route_path.empty() ? "/" : parsed.route_path;

    if (method != "GET") {
        error_requests_.fetch_add(1, std::memory_order_relaxed);
        return build_http_response(json_error("only GET is supported"), "application/json", 405, "Method Not Allowed");
    }

    if (route_path == "/api/health") {
        api_requests_.fetch_add(1, std::memory_order_relaxed);
        const auto uptime =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at_).count();
        std::ostringstream body;
        body << "{"
             << "\"status\":\"ok\","
             << "\"uptime_ms\":" << uptime << ","
             << "\"timestamp_hint\":\"steady_clock\""
             << "}";
        return build_http_response(body.str(), "application/json; charset=utf-8");
    }

    const std::string static_route = (route_path == "/") ? "/index.html" : route_path;

    if (is_allowed_static_route(static_route)) {
        static_requests_.fetch_add(1, std::memory_order_relaxed);
        const std::string filename = static_route.substr(1);  // 去掉前导 '/'
        const std::string content = read_file_or_empty(web_root_ + "/" + filename);
        if (content.empty()) {
            error_requests_.fetch_add(1, std::memory_order_relaxed);
            return build_http_response(filename + " not found", "text/plain", 500, "Internal Server Error");
        }
        return build_http_response(content, static_content_type(filename));
    }

    if (route_path == "/api/stats") {
        api_requests_.fetch_add(1, std::memory_order_relaxed);
        // uptime 从进程启动开始计时，便于 dashboard 展示运行时长。
        const auto uptime =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at_).count();
        const LatencyStats lstats = collect_latency_stats();

        const std::size_t pending_now = thread_pool_.pending_tasks();
        update_peak(peak_pending_tasks_, pending_now);
        const std::uint64_t raw_min_db_free = min_db_free_.load(std::memory_order_relaxed);
        const std::uint64_t safe_min_db_free =
            raw_min_db_free == std::numeric_limits<std::uint64_t>::max()
                ? static_cast<std::uint64_t>(db_pool_.free_count())
                : raw_min_db_free;

        std::ostringstream body;
        body << "{"
             << "\"uptime_ms\":" << uptime << ","
             << "\"total_requests\":" << total_requests_.load(std::memory_order_relaxed) << ","
             << "\"api_requests\":" << api_requests_.load(std::memory_order_relaxed) << ","
             << "\"static_requests\":" << static_requests_.load(std::memory_order_relaxed) << ","
             << "\"error_requests\":" << error_requests_.load(std::memory_order_relaxed) << ","
             << "\"process_requests\":" << process_requests_.load(std::memory_order_relaxed) << ","
             << "\"active_process_requests\":" << active_process_requests_.load(std::memory_order_relaxed) << ","
             << "\"db_timeout_requests\":" << db_timeout_requests_.load(std::memory_order_relaxed) << ","
             << "\"db_mode\":\"" << db_pool_.mode_name() << "\","
             << "\"thread_pool_workers\":" << thread_pool_.worker_count() << ","
             << "\"thread_pool_pending\":" << pending_now << ","
             << "\"peak_thread_pool_pending\":" << peak_pending_tasks_.load(std::memory_order_relaxed) << ","
             << "\"db_pool_free\":" << db_pool_.free_count() << ","
             << "\"db_pool_total\":" << db_pool_.total_count() << ","
             << "\"min_db_pool_free\":" << safe_min_db_free << ","
             << "\"memory_pool_in_use\":" << request_pool_.in_use() << ","
             << "\"memory_pool_available\":" << request_pool_.available() << ","
             << "\"memory_pool_total\":" << request_pool_.total_size() << ","
             << "\"peak_memory_pool_in_use\":" << peak_memory_in_use_.load(std::memory_order_relaxed) << ","
             << "\"peak_active_process_requests\":" << peak_active_process_.load(std::memory_order_relaxed) << ","
             << "\"db_acquire_timeout_ms\":" << config_.db_acquire_timeout.count() << ","
             << "\"latency_window_size\":" << config_.latency_window_size << ","
             << "\"latency_samples\":" << lstats.samples << ","
             << std::fixed << std::setprecision(3)
             << "\"avg_latency_ms\":" << lstats.avg_ms << ","
             << "\"p50_latency_ms\":" << lstats.p50_ms << ","
             << "\"p95_latency_ms\":" << lstats.p95_ms << ","
             << "\"p99_latency_ms\":" << lstats.p99_ms
             << "}";
        return build_http_response(body.str(), "application/json; charset=utf-8");
    }

    if (route_path == "/api/process") {
        api_requests_.fetch_add(1, std::memory_order_relaxed);
        process_requests_.fetch_add(1, std::memory_order_relaxed);
        const auto active = active_process_requests_.fetch_add(1, std::memory_order_relaxed) + 1;
        update_peak(peak_active_process_, active);
        const auto process_begin = std::chrono::steady_clock::now();
        // 统一收口：记录本次处理耗时并减少 active_process_requests 计数。
        const auto finalize_latency = [this, process_begin]() {
            const auto process_end = std::chrono::steady_clock::now();
            const double process_ms =
                std::chrono::duration_cast<std::chrono::microseconds>(process_end - process_begin).count() / 1000.0;
            record_process_latency_ms(process_ms);
            active_process_requests_.fetch_sub(1, std::memory_order_relaxed);
        };

        const std::string raw_user = get_query_value(parsed.query, "user");
        int user_id = 0;
        if (!parse_non_negative_int(raw_user, &user_id)) {
            error_requests_.fetch_add(1, std::memory_order_relaxed);
            finalize_latency();
            return build_http_response(
                json_error("invalid user parameter, expected non-negative integer"),
                "application/json",
                400,
                "Bad Request");
        }

        const std::string raw_hold_ms = get_query_value(parsed.query, "hold_ms");
        unsigned int hold_ms = 0;
        if (!raw_hold_ms.empty()) {
            if (!parse_non_negative_uint(raw_hold_ms, &hold_ms)) {
                error_requests_.fetch_add(1, std::memory_order_relaxed);
                finalize_latency();
                return build_http_response(
                    json_error("invalid hold_ms parameter, expected non-negative integer"),
                    "application/json",
                    400,
                    "Bad Request");
            }
        }
        hold_ms = std::min<unsigned int>(hold_ms, 5000U);

        const std::string raw_work_ms = get_query_value(parsed.query, "work_ms");
        unsigned int work_ms = 0;
        if (!raw_work_ms.empty()) {
            if (!parse_non_negative_uint(raw_work_ms, &work_ms)) {
                error_requests_.fetch_add(1, std::memory_order_relaxed);
                finalize_latency();
                return build_http_response(
                    json_error("invalid work_ms parameter, expected non-negative integer"),
                    "application/json",
                    400,
                    "Bad Request");
            }
        }
        work_ms = std::min<unsigned int>(work_ms, 5000U);

        // 从对象池借出 RequestContext；req_ctx 析构时会自动归还对象池（RAII）。
        auto req_ctx = request_pool_.acquire();
        req_ctx->request_id = request_id;
        req_ctx->user_id = user_id;
        req_ctx->path = route_path;
        const std::uint64_t memory_in_use_now = request_pool_.in_use();
        update_peak(peak_memory_in_use_, memory_in_use_now);
        if (work_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(work_ms));
        }

        // 从连接池借出 DB 连接（含超时）；通过 DBConnGuard 在作用域结束时自动归还。
        DBConnGuard guard(db_pool_, config_.db_acquire_timeout);
        if (!guard.valid()) {
            error_requests_.fetch_add(1, std::memory_order_relaxed);
            db_timeout_requests_.fetch_add(1, std::memory_order_relaxed);
            finalize_latency();
            return build_http_response(json_error("db connection timeout"), "application/json", 503, "Service Unavailable");
        }
        update_min(min_db_free_, static_cast<std::uint64_t>(db_pool_.free_count()));
        if (hold_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
        }

        const std::string db_result = guard.get()->query_user_score(req_ctx->user_id);
        // 回包里附带当前池子快照，便于前端在一次请求内直接观测状态变化。
        const std::size_t pending_now = thread_pool_.pending_tasks();
        update_peak(peak_pending_tasks_, pending_now);
        std::ostringstream body;
        body << "{"
             << "\"request_id\":" << req_ctx->request_id << ","
             << "\"thread_id\":\"" << std::this_thread::get_id() << "\","
             << "\"work_ms\":" << work_ms << ","
             << "\"hold_ms\":" << hold_ms << ","
             << "\"db_free\":" << db_pool_.free_count() << ","
             << "\"memory_pool_in_use\":" << request_pool_.in_use() << ","
             << "\"thread_pool_pending\":" << thread_pool_.pending_tasks() << ","
             << "\"active_process_requests\":" << active_process_requests_.load(std::memory_order_relaxed) << ","
             << "\"payload\":" << db_result
             << "}";
        finalize_latency();
        return build_http_response(body.str(), "application/json; charset=utf-8");
    }

    error_requests_.fetch_add(1, std::memory_order_relaxed);
    return build_http_response(json_error("not found"), "application/json", 404, "Not Found");
}

std::string HttpServer::read_file_or_empty(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        return {};
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

std::string HttpServer::json_error(const std::string& message) const {
    std::ostringstream oss;
    oss << "{\"error\":\"" << trim(message) << "\"}";
    return oss.str();
}

void HttpServer::record_process_latency_ms(double latency_ms) {
    if (!std::isfinite(latency_ms) || latency_ms < 0.0) {
        return;
    }
    std::lock_guard<std::mutex> lock(latency_mutex_);
    process_latencies_ms_.push_back(latency_ms);
    while (process_latencies_ms_.size() > config_.latency_window_size) {
        process_latencies_ms_.pop_front();
    }
}

HttpServer::LatencyStats HttpServer::collect_latency_stats() const {
    std::vector<double> snapshot;
    {
        std::lock_guard<std::mutex> lock(latency_mutex_);
        snapshot.assign(process_latencies_ms_.begin(), process_latencies_ms_.end());
    }

    LatencyStats stats;
    if (snapshot.empty()) {
        return stats;
    }

    double sum_ms = 0.0;
    for (double v : snapshot) {
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
