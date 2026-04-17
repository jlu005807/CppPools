#include "db_pool/db_connection_pool.h"
#include "memory_pool/object_pool.h"
#include "server/http_server.h"
#include "thread_pool/thread_pool.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>

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

HttpServerConfig build_server_config_from_env() {
    // Optional tuning env vars:
    //   CPPPOOLS_DB_ACQUIRE_TIMEOUT_MS
    //   CPPPOOLS_LATENCY_WINDOW_SIZE
    HttpServerConfig cfg;
    cfg.db_acquire_timeout = std::chrono::milliseconds(
        static_cast<int>(env_to_uint("CPPPOOLS_DB_ACQUIRE_TIMEOUT_MS", 100)));
    cfg.latency_window_size = static_cast<std::size_t>(
        env_to_uint("CPPPOOLS_LATENCY_WINDOW_SIZE", 1024));
    return cfg;
}
}  // namespace

int main(int argc, char** argv) {
    // Optional CLI args:
    //   argv[1] = port (default 8080)
    //   argv[2] = worker count (default 6)
    //
    // Optional DB env vars:
    //   CPPPOOLS_DB_MODE=mock|mysql
    //   CPPPOOLS_DB_HOST / CPPPOOLS_DB_PORT
    //   CPPPOOLS_DB_USER / CPPPOOLS_DB_PASSWORD
    //   CPPPOOLS_DB_NAME / CPPPOOLS_DB_TABLE
    //   CPPPOOLS_DB_POOL_SIZE
    const int port = (argc > 1) ? std::atoi(argv[1]) : 8080;
    const int worker_count = (argc > 2) ? std::atoi(argv[2]) : 6;

    try {
        ThreadPool thread_pool(static_cast<std::size_t>(std::max(worker_count, 1)));
        ObjectPool<RequestContext> request_pool(128);
        const DBPoolConfig db_cfg = build_db_config_from_env();
        const HttpServerConfig server_cfg = build_server_config_from_env();
        DBConnectionPool db_pool(db_cfg);

        std::cout << "db mode: " << db_pool.mode_name() << '\n';
        if (db_pool.using_mysql()) {
            std::cout << "mysql host=" << db_cfg.host
                      << " port=" << db_cfg.port
                      << " db=" << db_cfg.database
                      << " table=" << db_cfg.table << '\n';
        }
        std::cout << "db acquire timeout(ms): " << server_cfg.db_acquire_timeout.count() << '\n';
        std::cout << "latency window size: " << server_cfg.latency_window_size << '\n';

        HttpServer server(
            static_cast<uint16_t>(std::max(port, 1)),
            thread_pool,
            request_pool,
            db_pool,
            "web",
            server_cfg);
        server.start();
    } catch (const std::exception& ex) {
        std::cerr << "server failed: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
