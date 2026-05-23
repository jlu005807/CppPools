#include "db_pool/db_connection_pool.h"

#include <cassert>
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
            // MySQL 模式：池中每个槽位都建立一条真实数据库连接。
            conn = std::make_unique<MockDBConnection>(
                static_cast<int>(i + 1),
                config_.host,
                config_.port,
                config_.user,
                config_.password,
                config_.database,
                config_.table);
        } else {
            // Mock 模式：使用确定性模拟后端，便于本地学习与演示。
            conn = std::make_unique<MockDBConnection>(static_cast<int>(i + 1));
        }

        free_conns_.push(conn.get());
        all_conns_.push_back(std::move(conn));
    }
    // 构造完成后所有连接都处于空闲状态，初始化 atomic 镜像。
    free_count_atomic_.store(config_.pool_size, std::memory_order_relaxed);
}

DBConnectionPool::~DBConnectionPool() {
    // 与 ObjectPool 的析构约束保持一致：所有借出的连接必须先归还，
    // 否则 DBConnGuard 持有的指针在 pool 析构后会变野。
    // 仅在 Debug 模式下生效，Release 版本零开销。
    assert(in_use_conns_.empty() && "DBConnectionPool destroyed while connections are still in use");
}

MockDBConnection* DBConnectionPool::acquire(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    // wait_for 可避免无限阻塞，让“资源耗尽”以超时形式显式暴露给上层。
    const bool ok = cv_.wait_for(lock, timeout, [this]() { return !free_conns_.empty(); });
    if (!ok) {
        return nullptr;
    }
    MockDBConnection* conn = free_conns_.front();
    free_conns_.pop();
    // 记录借出连接，后续 release() 可据此校验合法性。
    in_use_conns_.insert(conn);
    // 同步更新 atomic 镜像，使 free_count() 的无锁读取始终与队列长度一致。
    free_count_atomic_.fetch_sub(1, std::memory_order_relaxed);
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
            // 拒绝重复归还或外部非法连接，保持池状态一致。
            return;
        }
        in_use_conns_.erase(it);
        free_conns_.push(conn);
        free_count_atomic_.fetch_add(1, std::memory_order_relaxed);
    }
    // 唤醒一个等待 acquire() 的线程。
    cv_.notify_one();
}

std::size_t DBConnectionPool::free_count() const {
    // 无锁读取：/api/stats 高频查询场景下避免和 acquire/release 抢 mutex_。
    return free_count_atomic_.load(std::memory_order_relaxed);
}

DBConnGuard::DBConnGuard(DBConnectionPool& pool, std::chrono::milliseconds timeout) : pool_(pool) {
    conn_ = pool_.acquire(timeout);
}

DBConnGuard::~DBConnGuard() {
    // 即使 conn_ 为空也安全（release 内部会直接返回）。
    pool_.release(conn_);
}
