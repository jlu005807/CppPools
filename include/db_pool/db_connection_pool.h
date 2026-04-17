#pragma once

#include "db_pool/mock_db_connection.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_set>
#include <vector>

struct DBPoolConfig {
    std::size_t pool_size{8};

    // false -> mock mode
    // true  -> mysql mode
    bool use_mysql{false};

    // MySQL settings (used only when use_mysql=true).
    std::string host{"127.0.0.1"};
    unsigned int port{3306};
    std::string user{"root"};
    std::string password{};
    std::string database{"cpppools"};
    std::string table{"user_scores"};
};

// DBConnectionPool: bounded reusable connection container.
//
// acquire(timeout):
//   waits up to timeout for a free connection.
//   returns nullptr on timeout.
//
// release(conn):
//   returns connection to queue and notifies a waiting thread.
class DBConnectionPool {
public:
    explicit DBConnectionPool(std::size_t pool_size);
    explicit DBConnectionPool(const DBPoolConfig& config);
    ~DBConnectionPool();

    DBConnectionPool(const DBConnectionPool&) = delete;
    DBConnectionPool& operator=(const DBConnectionPool&) = delete;

    // Returns nullptr when timeout is reached.
    MockDBConnection* acquire(std::chrono::milliseconds timeout);

    // Return a connection back to free queue.
    void release(MockDBConnection* conn);

    std::size_t free_count() const;
    std::size_t total_count() const { return total_count_; }
    bool using_mysql() const { return config_.use_mysql; }
    std::string mode_name() const { return config_.use_mysql ? "mysql" : "mock"; }
    const DBPoolConfig& config() const { return config_; }

private:
    // Protects free queue and count inspection.
    mutable std::mutex mutex_;
    // Coordinates waiting acquire() calls.
    std::condition_variable cv_;
    // Available reusable connections.
    std::queue<MockDBConnection*> free_conns_;
    // Borrowed connections currently checked out by callers.
    // This set lets release() reject duplicated/invalid returns safely.
    std::unordered_set<MockDBConnection*> in_use_conns_;
    // Owning list of all connections.
    std::vector<std::unique_ptr<MockDBConnection>> all_conns_;
    // Fixed max connections created at initialization.
    std::size_t total_count_{0};
    DBPoolConfig config_;
};

// DBConnGuard wraps acquire/release with RAII, so every successful acquire
// is safely returned even when exceptions happen in business logic.
class DBConnGuard {
public:
    DBConnGuard(DBConnectionPool& pool, std::chrono::milliseconds timeout);
    ~DBConnGuard();

    DBConnGuard(const DBConnGuard&) = delete;
    DBConnGuard& operator=(const DBConnGuard&) = delete;

    MockDBConnection* get() const { return conn_; }
    bool valid() const { return conn_ != nullptr; }

private:
    DBConnectionPool& pool_;
    MockDBConnection* conn_{nullptr};
};
