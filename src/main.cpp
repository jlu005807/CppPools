#include "cpppools/config/config.h"
#include "cpppools/db/datasource_registry.h"
#include "db/sqlite_database.h"
#ifdef CPPPOOLS_USE_MYSQL
#include "cpppools/db/mysql_database.h"
#endif
#include "cpppools/pool/object_pool.h"
#include "cpppools/server/http_server.h"
#include "server/local_control.h"
#include "cpppools/pool/thread_pool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace cpppools::pool;
using namespace cpppools::db;
using namespace cpppools::server;
namespace config = cpppools::config;


namespace {

// 优雅停机的信号桥：信号处理函数只能碰原子量（async-signal-safe 约束），
// 所以这里只存原子量，handler 里转调 request_stop()（它也只碰原子标志）。
std::atomic<cpppools::server::HttpServer*> g_stop_server{nullptr};
std::atomic<int> g_stop_signal_count{0};

void handle_stop_signal(int sig) {
    // 第二次信号 = 用户等不及排空：强制退出（_exit 是 async-signal-safe）。
    if (g_stop_signal_count.fetch_add(1, std::memory_order_relaxed) > 0) {
        _exit(sig == SIGINT ? 130 : 143);
    }
    cpppools::server::HttpServer* server = g_stop_server.load(std::memory_order_relaxed);
    if (server != nullptr) {
        server->request_stop();
    }
}

void handle_managed_stop_signal(int) {
    cpppools::server::request_managed_stop();
}

// RAII：信号注册的生效范围与 server 的生命周期严格对齐 ——
// 无论 start() 正常返回还是抛异常，析构都会解指针并恢复默认处置，
// 不会有 handler 悬在已销毁对象上。
struct StopBridge {
    StopBridge() {
        std::signal(SIGTERM, handle_stop_signal);
        std::signal(SIGINT, handle_stop_signal);
    }
    ~StopBridge() {
        g_stop_server.store(nullptr, std::memory_order_relaxed);
        g_stop_signal_count.store(0, std::memory_order_relaxed);
        std::signal(SIGTERM, SIG_DFL);
        std::signal(SIGINT, SIG_DFL);
    }
};

struct ManagedStopBridge {
    ManagedStopBridge() {
        std::signal(SIGTERM, handle_managed_stop_signal);
        std::signal(SIGINT, handle_managed_stop_signal);
    }
    ~ManagedStopBridge() {
        std::signal(SIGTERM, SIG_DFL);
        std::signal(SIGINT, SIG_DFL);
    }
};

unsigned int env_to_uint(const char* key, unsigned int fallback) {
    const char* v = std::getenv(key);
    if (v == nullptr || *v == '\0') {
        return fallback;
    }
    const long parsed = std::strtol(v, nullptr, 10);
    if (parsed <= 0) {
        return fallback;
    }
    return static_cast<unsigned int>(parsed);
}

HttpServerConfig build_server_config_from_env() {
    // 可选调优环境变量：
    //   CPPPOOLS_DB_ACQUIRE_TIMEOUT_MS  连接池获取超时（毫秒）
    //   CPPPOOLS_LATENCY_WINDOW_SIZE    延迟统计滑动窗口大小
    HttpServerConfig cfg;
    cfg.db_acquire_timeout = std::chrono::milliseconds(
        static_cast<int>(env_to_uint("CPPPOOLS_DB_ACQUIRE_TIMEOUT_MS", 100)));
    cfg.latency_window_size = static_cast<std::size_t>(
        env_to_uint("CPPPOOLS_LATENCY_WINDOW_SIZE", 1024));
    return cfg;
}

std::string control_directory_argument(int argc, char** argv) {
#ifdef _WIN32
    // Narrow CRT argv uses the system code page and loses paths outside it.
    (void)argc;
    (void)argv;
    int wide_argc = 0;
    const std::unique_ptr<wchar_t*, decltype(&LocalFree)> wide_argv(
        CommandLineToArgvW(GetCommandLineW(), &wide_argc), &LocalFree);
    if (!wide_argv) {
        throw std::runtime_error("could not read Unicode command line");
    }
    for (int i = 1; i < wide_argc; ++i) {
        if (std::wstring(wide_argv.get()[i]) != L"--control-dir") {
            continue;
        }
        if (i + 1 >= wide_argc || wide_argv.get()[i + 1][0] == L'\0') {
            throw std::invalid_argument("--control-dir requires a path argument");
        }
        const std::wstring value(wide_argv.get()[i + 1]);
        const int length = static_cast<int>(value.size());
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            value.data(), length, nullptr, 0, nullptr, nullptr);
        if (size == 0) {
            throw std::invalid_argument("--control-dir is not valid Unicode");
        }
        std::string result(static_cast<std::size_t>(size), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length,
                               result.data(), size, nullptr, nullptr) != size) {
            throw std::runtime_error("could not encode control directory as UTF-8");
        }
        return result;
    }
#else
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--control-dir") {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                throw std::invalid_argument("--control-dir requires a path argument");
            }
            return argv[i + 1];
        }
    }
#endif
    return {};
}

bool accept_managed_stop(LocalControl* control) {
    if (!managed_stop_requested() && control == nullptr) {
        return false;
    }
    std::string error;
    const bool requested = managed_stop_requested() || control->stop_requested(&error);
    if (!requested) {
        if (!error.empty() && control->claim_stop_request_warning()) {
            std::cerr << "[warn] ignored control stop request: " << error << '\n';
        }
        return false;
    }
    if (control != nullptr && !control->publish_stopping(&error)) {
        std::cerr << "[warn] could not publish stopping marker: " << error << '\n';
    }
    return true;
}

// 请求上下文对象池的**下限**容量。
//
// 取 128：每个 RequestContext 持一个 4KB 接收缓冲，启动期预分配约 512KB。
// 实际容量（也是上限 max_size）按 max(128, worker_count * 2) 计算：
// 必须 >= 并发需求（每个 worker 同时最多持 1 个），否则 acquire 会失败。
// 该不变式由这里的配置计算维持，**不是类型保证**——所以调用方仍必须判空。
constexpr std::size_t kRequestPoolSizeFloor = 128;

// 线程池任务队列上限默认值（可用 CPPPOOLS_MAX_QUEUE_SIZE 覆盖）。
// 达到后 submit 拒绝，accept 循环直接回 503。
//
// 选 1024：足以吸收突发，又不致于在持续过载时堆积过多内存。
// 宁可快速失败也不要无界堆积 —— 后者下客户端只能感受到延迟持续攀升，
// 永远拿不到一个明确的信号。
//
// 做成可配而不是写死常量，除了运维需要调优，也是为了让背压路径
// 能被真实验证 —— 默认 1024 下本地几乎压不出 503。
constexpr std::size_t kMaxQueueSize = 1024;
}  // namespace

int run_server(int argc, char** argv, const std::string& control_dir_arg,
               LocalControl* control) {
    // 配置来源：JSON 配置文件，路径按优先级解析：
    //   --config <path>  >  环境变量 CPPPOOLS_CONFIG  >  config/cpppools.json
    // 配置错误直接 fail-fast（不回落默认）——“连哪个库”是必须显式声明的事实。
    // 期 3 起：数据源建连完全由配置驱动（环境变量建连已删除）。
    if (argc >= 2 && std::string(argv[1]) == "--config" && argc < 3) {
        std::cerr << "--config requires a path argument" << std::endl;
        return 1;
    }
    const std::string config_path = config::resolve_config_path(argc, argv);
    const config::ConfigLoadResult cfg_result = config::load_config(config_path);
    if (!cfg_result.ok) {
        std::cerr << "config error: " << cfg_result.error << std::endl;
        std::cerr << "see config/cpppools.example.json for the expected format" << std::endl;
        return 1;
    }
    const config::AppConfig& app_cfg = cfg_result.config;
    const int port = app_cfg.server.port;
    const int worker_count = static_cast<int>(app_cfg.server.worker_threads);
    if (accept_managed_stop(control)) {
        return 0;
    }

    try {
        // 初始化“三大池子 + HTTP 服务”。
        // 池子对象由 main 统一管理生命周期，服务内部只持引用。
        // 声明顺序即销毁逆序：server 最先销毁，所以池子必定比它活得久。
        // sanity 上限：把「配置错误」与「资源耗尽」在错误信息里区分开。
        // 无上限时 argv[2]=100000 会先去创建十万个线程、再预分配 ~800MB
        // 对象池缓冲，最终以 bad_alloc 收场——不如在入口处就拒绝。
        constexpr int kMaxWorkers = 256;
        if (worker_count > kMaxWorkers) {
            std::cerr << "worker_count " << worker_count << " exceeds sanity limit "
                      << kMaxWorkers << "; refusing to start" << std::endl;
            return 1;
        }
        // 配置回显：配置文件路径 + 数据源清单（密码绝不回显）。
        std::cout << "config file   : " << config_path << std::endl;
        for (const auto& ds : app_cfg.datasources) {
            std::cout << "datasource    : " << ds.name << " (" << ds.type << ")"
                      << (ds.is_default ? " [default]" : "") << std::endl;
        }
        // 后端注册（显式调用，避免全局静态初始化顺序依赖）。
        // MySQL 后端仅在使用 CPPPOOLS_USE_MYSQL 编译时注册。
        register_sqlite_backend();
#ifdef CPPPOOLS_USE_MYSQL
        register_mysql_backend();
#endif

        // 多数据源注册表（期 4）：逐源探连 + 自省 → 每源一个连接池 →
        // table→datasource 索引。任一源失败整体 fail-fast；
        // 歧义表（多源同名）在构建期打 stderr 警告（spec §4.1）。
        // :memory: 字面路径强制单槽也在 registry 内处理（spec §10.1）。
        RegistryBuildResult registry_result =
            DataSourceRegistry::build(app_cfg.datasources, app_cfg.cache);
        if (registry_result.registry == nullptr) {
            std::cerr << "config error: " << registry_result.error << std::endl;
            return 1;
        }
        std::unique_ptr<DataSourceRegistry> registry = std::move(registry_result.registry);
        if (accept_managed_stop(control)) {
            return 0;
        }


        ThreadPoolConfig tp_cfg;
        tp_cfg.name = "worker";
        tp_cfg.worker_count = static_cast<std::size_t>(std::max(worker_count, 1));
        tp_cfg.max_queue_size = env_to_uint("CPPPOOLS_MAX_QUEUE_SIZE", kMaxQueueSize);
        // 必须显式接上这个回调，否则 submit_detached 的异常可见性只是个
        // 能力而从未被启用 —— 任务里的异常仍会悄无声息地消失。
        tp_cfg.on_task_exception = [](std::exception_ptr eptr) {
            try {
                std::rethrow_exception(eptr);
            } catch (const std::exception& ex) {
                std::cerr << "[error] task exception: " << ex.what() << std::endl;
            } catch (...) {
                std::cerr << "[error] task exception: unknown" << std::endl;
            }
        };
        ThreadPool thread_pool(tp_cfg);
        // 对象池容量按 worker 数计算（见 kRequestPoolSizeFloor 注释）：
        // max_size 必须 >= 并发需求，否则请求会被 acquire 拒绝。
        const std::size_t request_pool_size =
            std::max(kRequestPoolSizeFloor, tp_cfg.worker_count * 2);
        // init_size == max_size：启动即预分配全部，运行期内存峰值在启动期就定型。
        // 在这个配置下 acquire 的扩容分支不可达，失败路径仅当并发超过配置才出现。
        ObjectPool<RequestContext> request_pool("request", request_pool_size, request_pool_size);
        if (accept_managed_stop(control)) {
            return 0;
        }
        // db_acquire_timeout 过渡期仍走环境变量；
        // latency_window_size 配置文件优先（避免“解析了但无效”的裂口）。
        HttpServerConfig server_cfg = build_server_config_from_env();
        server_cfg.latency_window_size = app_cfg.server.latency_window_size;
        // 集群视图（/api/v1/cluster 的响应素材；cluster 段缺省 = 单节点模式）。
        if (app_cfg.has_cluster) {
            server_cfg.cluster_view.node_id = app_cfg.cluster.node_id;
            server_cfg.cluster_view.epoch = app_cfg.cluster.epoch;
            for (const auto& n : app_cfg.cluster.nodes) {
                server_cfg.cluster_view.nodes.push_back(
                    {n.node_id, n.host, n.port});
            }
        }
        server_cfg.cluster_view.cache_ttl_ms = app_cfg.cache.ttl_ms;
        server_cfg.cluster_view.blacklist_ttl_ms = app_cfg.cache.blacklist_ttl_ms;
        server_cfg.control_dir = control_dir_arg;

        // 启动 HTTP 服务（阻塞运行）。
        // 其余启动信息由 HttpServer::start() 统一打印，避免重复输出。
        // HTTP 层挂整个注册表：CRUD 请求按表名经 registry.route 分发，
        // 指标/自省端点遍历全部数据源池。
        HttpServer server(
            static_cast<uint16_t>(std::max(port, 1)),
            thread_pool,
            request_pool,
            *registry,
            server_cfg);
        if (accept_managed_stop(control)) {
            return 0;
        }
        // 优雅停机：SIGTERM/SIGINT → 停 accept → 等在途请求排空 → 正常返回
        // （此后按声明逆序销毁：server → 三大池，生命周期安全）。
        // 排空等待中再收一次信号 = 强制退出（见 handle_stop_signal）。
        std::unique_ptr<StopBridge> stop_bridge;
        if (control == nullptr) {
            g_stop_server.store(&server, std::memory_order_relaxed);
            stop_bridge = std::make_unique<StopBridge>();
        }
        server.start();
    } catch (const std::exception& ex) {
        std::cerr << "server failed: " << ex.what() << std::endl;
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    std::string control_dir;
    std::unique_ptr<LocalControl> control;
    std::unique_ptr<ManagedStopBridge> managed_bridge;
    try {
        control_dir = control_directory_argument(argc, argv);
        if (!control_dir.empty()) {
            std::string error;
            control = LocalControl::open(control_dir, &error);
            if (!control) {
                std::cerr << "control error: " << error << std::endl;
                return 1;
            }
            managed_bridge = std::make_unique<ManagedStopBridge>();
            if (!control->publish_starting(&error)) {
                std::cerr << "control starting marker failed: " << error << std::endl;
                return 1;
            }
        }

        const int exit_code = run_server(argc, argv, control_dir, control.get());
        if (exit_code != 0) {
            return exit_code;
        }
        if (control) {
            std::string error;
            if (!control->publish_stopped(exit_code, &error)) {
                std::cerr << "control stopped marker failed: " << error << std::endl;
                return 1;
            }
        }
        return exit_code;
    } catch (const std::exception& ex) {
        std::cerr << "server failed: " << ex.what() << std::endl;
        return 1;
    }
}
