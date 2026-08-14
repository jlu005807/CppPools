#include "cpppools/db/db_connection_pool.h"
#include "cpppools/db/mock_score_store.h"
#include "cpppools/pool/object_pool.h"
#include "cpppools/server/http_server.h"
#include "cpppools/pool/thread_pool.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

using namespace cpppools::pool;
using namespace cpppools::db;
using namespace cpppools::server;


namespace {
std::string env_or_default(const char* key, const char* fallback) {
    const char* v = std::getenv(key);
    if (v == nullptr || *v == '\0') {
        return std::string(fallback);
    }
    return std::string(v);
}

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

bool env_db_mode_mysql() {
    std::string mode = env_or_default("CPPPOOLS_DB_MODE", "mock");
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return mode == "mysql";
}

DbPoolConfig build_db_config_from_env() {
    DbPoolConfig cfg;
    cfg.pool_size = env_to_uint("CPPPOOLS_DB_POOL_SIZE", 8);
    cfg.use_mysql = env_db_mode_mysql();
    cfg.host = env_or_default("CPPPOOLS_DB_HOST", "127.0.0.1");
    cfg.port = env_to_uint("CPPPOOLS_DB_PORT", 3306);
    cfg.user = env_or_default("CPPPOOLS_DB_USER", "root");
    cfg.password = env_or_default("CPPPOOLS_DB_PASSWORD", "");
    cfg.database = env_or_default("CPPPOOLS_DB_NAME", "cpppools");
    cfg.table = env_or_default("CPPPOOLS_DB_TABLE", "user_scores");
    return cfg;
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

int main(int argc, char** argv) {
    // 可选命令行参数：
    //   argv[1] = 端口（默认 8080）
    //   argv[2] = 线程池 worker 数（默认 6）
    //
    // 可选数据库环境变量：
    //   CPPPOOLS_DB_MODE=mock|mysql
    //   CPPPOOLS_DB_HOST / CPPPOOLS_DB_PORT
    //   CPPPOOLS_DB_USER / CPPPOOLS_DB_PASSWORD
    //   CPPPOOLS_DB_NAME / CPPPOOLS_DB_TABLE
    //   CPPPOOLS_DB_POOL_SIZE
    const int port = (argc > 1) ? std::atoi(argv[1]) : 8080;
    const int worker_count = (argc > 2) ? std::atoi(argv[2]) : 6;

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
        const DbPoolConfig db_cfg = build_db_config_from_env();
        const HttpServerConfig server_cfg = build_server_config_from_env();
        // mock 后端的共享存储。factory 化后池不再持有 store，由调用方提供。
        // 生命周期约束：store 必须比 db_pool 活得久 —— 由声明顺序保证
        // （store 先声明、后销毁；成员/局部变量逆序析构）。
        MockScoreStore mock_store;
        ConnectionFactory db_factory = [&db_cfg, &mock_store](std::size_t slot_index) {
            if (db_cfg.use_mysql) {
                return std::make_unique<DbConnection>(
                    static_cast<int>(slot_index) + 1, db_cfg.host, db_cfg.port,
                    db_cfg.user, db_cfg.password, db_cfg.database, db_cfg.table);
            }
            return std::make_unique<DbConnection>(
                static_cast<int>(slot_index) + 1, &mock_store);
        };
        DbConnectionPool db_pool("db", db_factory, db_cfg);

        if (db_pool.using_mysql()) {
            // 仅 MySQL 模式下需要额外回显连接目标（不打印密码）。
            std::cout << "mysql target: " << db_cfg.host << ":" << db_cfg.port
                      << "/" << db_cfg.database << "." << db_cfg.table << std::endl;
        }

        // 启动 HTTP 服务（阻塞运行）。
        // 其余启动信息由 HttpServer::start() 统一打印，避免重复输出。
        HttpServer server(
            static_cast<uint16_t>(std::max(port, 1)),
            thread_pool,
            request_pool,
            db_pool,
            server_cfg);
        server.start();
    } catch (const std::exception& ex) {
        std::cerr << "server failed: " << ex.what() << std::endl;
        return 1;
    }
    return 0;
}
