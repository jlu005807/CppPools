#include "db_pool/db_connection_pool.h"
#include "memory_pool/object_pool.h"
#include "thread_pool/thread_pool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

struct DemoContext {
    int request_id{0};
    int user_id{0};
    std::string trace;
};

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

DBPoolConfig build_db_config_from_env() {
    DBPoolConfig cfg;
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
}  // namespace

int main(int argc, char** argv) {
    // Optional args:
    //   argv[1] = task_count (default 1000)
    //   argv[2] = worker_count (default 6)
    const int task_count = (argc > 1) ? std::atoi(argv[1]) : 1000;
    const int worker_count = (argc > 2) ? std::atoi(argv[2]) : 6;

    const int safe_task_count = std::max(task_count, 1);
    const int safe_worker_count = std::max(worker_count, 1);

    ThreadPool thread_pool(static_cast<std::size_t>(safe_worker_count));
    ObjectPool<DemoContext> object_pool(128);
    const DBPoolConfig db_cfg = build_db_config_from_env();
    DBConnectionPool db_pool(db_cfg);

    std::atomic<int> success_count{0};
    std::atomic<int> fail_count{0};

    std::mutex latency_mutex;
    std::vector<double> latencies_ms;
    latencies_ms.reserve(static_cast<std::size_t>(safe_task_count));

    std::vector<std::future<void>> futures;
    futures.reserve(static_cast<std::size_t>(safe_task_count));

    const auto begin_all = std::chrono::steady_clock::now();
    for (int i = 0; i < safe_task_count; ++i) {
        futures.emplace_back(thread_pool.submit([i, &object_pool, &db_pool, &success_count, &fail_count, &latencies_ms, &latency_mutex]() {
            auto ctx = object_pool.acquire();
            ctx->request_id = i + 1;
            ctx->user_id = (i % 200) + 1;
            ctx->trace = "multi-pool-demo";

            const auto begin_one = std::chrono::steady_clock::now();
            DBConnGuard guard(db_pool, std::chrono::milliseconds(100));
            if (!guard.valid()) {
                ++fail_count;
                return;
            }

            (void)guard.get()->query_user_score(ctx->user_id);
            const auto end_one = std::chrono::steady_clock::now();

            const double ms = std::chrono::duration_cast<std::chrono::microseconds>(end_one - begin_one).count() / 1000.0;
            {
                std::lock_guard<std::mutex> lock(latency_mutex);
                latencies_ms.push_back(ms);
            }
            ++success_count;
        }));
    }

    for (auto& fut : futures) {
        fut.get();
    }
    const auto end_all = std::chrono::steady_clock::now();

    const double total_ms =
        std::chrono::duration_cast<std::chrono::microseconds>(end_all - begin_all).count() / 1000.0;
    const double qps = (total_ms > 0.0) ? (success_count.load() * 1000.0 / total_ms) : 0.0;

    double avg_latency = 0.0;
    if (!latencies_ms.empty()) {
        double sum = 0.0;
        for (double x : latencies_ms) {
            sum += x;
        }
        avg_latency = sum / static_cast<double>(latencies_ms.size());
    }

    std::cout << "=== Multi-Pools Demo Result ===\n";
    std::cout << "tasks_total          : " << safe_task_count << '\n';
    std::cout << "tasks_success        : " << success_count.load() << '\n';
    std::cout << "tasks_failed         : " << fail_count.load() << '\n';
    std::cout << "db_mode              : " << db_pool.mode_name() << '\n';
    if (db_pool.using_mysql()) {
        std::cout << "mysql_target         : " << db_cfg.host << ":" << db_cfg.port
                  << "/" << db_cfg.database << "." << db_cfg.table << '\n';
    }
    std::cout << "thread_pool_workers  : " << thread_pool.worker_count() << '\n';
    std::cout << "thread_pool_pending  : " << thread_pool.pending_tasks() << '\n';
    std::cout << "object_pool_total    : " << object_pool.total_size() << '\n';
    std::cout << "object_pool_in_use   : " << object_pool.in_use() << '\n';
    std::cout << "object_pool_available: " << object_pool.available() << '\n';
    std::cout << "db_pool_total        : " << db_pool.total_count() << '\n';
    std::cout << "db_pool_free         : " << db_pool.free_count() << '\n';
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "elapsed_ms           : " << total_ms << '\n';
    std::cout << "avg_latency_ms       : " << avg_latency << '\n';
    std::cout << "qps                  : " << qps << '\n';

    return 0;
}

