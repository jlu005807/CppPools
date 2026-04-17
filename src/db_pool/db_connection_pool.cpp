#include "db_pool/db_connection_pool.h"

#include <stdexcept>

namespace {
DBPoolConfig make_mock_config(std::size_t pool_size) {
    DBPoolConfig cfg;
    cfg.pool_size = pool_size;
    cfg.use_mysql = false;
    return cfg;
}
}  // namespace

DBConnectionPool::DBConnectionPool(std::size_t pool_size)
    : DBConnectionPool(make_mock_config(pool_size)) {}

DBConnectionPool::DBConnectionPool(const DBPoolConfig& config)
    : total_count_(config.pool_size),
      config_(config) {
    if (config_.pool_size == 0) {
        throw std::invalid_argument("DBConnectionPool size must be > 0");
    }

    all_conns_.reserve(config_.pool_size);
    for (std::size_t i = 0; i < config_.pool_size; ++i) {
        std::unique_ptr<MockDBConnection> conn;
        if (config_.use_mysql) {
            // MySQL mode: every pooled slot creates a real client connection.
            conn = std::make_unique<MockDBConnection>(
                static_cast<int>(i + 1),
                config_.host,
                config_.port,
                config_.user,
                config_.password,
                config_.database,
                config_.table);
        } else {
            // Mock mode: deterministic fake backend for local learning/demo.
            conn = std::make_unique<MockDBConnection>(static_cast<int>(i + 1));
        }

        free_conns_.push(conn.get());
        all_conns_.push_back(std::move(conn));
    }
}

DBConnectionPool::~DBConnectionPool() = default;

MockDBConnection* DBConnectionPool::acquire(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    // wait_for prevents unbounded blocking and makes overload visible.
    const bool ok = cv_.wait_for(lock, timeout, [this]() { return !free_conns_.empty(); });
    if (!ok) {
        return nullptr;
    }
    MockDBConnection* conn = free_conns_.front();
    free_conns_.pop();
    // Track active leases so duplicated release() calls can be rejected.
    in_use_conns_.insert(conn);
    return conn;
}

void DBConnectionPool::release(MockDBConnection* conn) {
    if (!conn) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = in_use_conns_.find(conn);
        if (it == in_use_conns_.end()) {
            // Reject duplicated/foreign releases to keep pool state sane.
            return;
        }
        in_use_conns_.erase(it);
        free_conns_.push(conn);
    }
    // Wake one waiting acquirer.
    cv_.notify_one();
}

std::size_t DBConnectionPool::free_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return free_conns_.size();
}

DBConnGuard::DBConnGuard(DBConnectionPool& pool, std::chrono::milliseconds timeout) : pool_(pool) {
    conn_ = pool_.acquire(timeout);
}

DBConnGuard::~DBConnGuard() {
    // Safe even when conn_ is nullptr.
    pool_.release(conn_);
}
