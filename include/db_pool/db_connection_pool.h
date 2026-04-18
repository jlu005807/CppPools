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

    // false -> mock 模式
    // true  -> mysql 模式
    bool use_mysql{false};

    // MySQL 参数（仅在 use_mysql=true 时生效）。
    std::string host{"127.0.0.1"};
    unsigned int port{3306};
    std::string user{"root"};
    std::string password{};
    std::string database{"cpppools"};
    std::string table{"user_scores"};
};

// DBConnectionPool：有界连接池。
//
// acquire(timeout)：
//   最多等待 timeout 时长来获取空闲连接；
//   超时返回 nullptr，避免业务线程无限阻塞。
//
// release(conn)：
//   将连接归还到空闲队列，并唤醒一个等待中的线程。
class DBConnectionPool {
public:
    explicit DBConnectionPool(std::size_t pool_size);
    explicit DBConnectionPool(const DBPoolConfig& config);
    ~DBConnectionPool();

    DBConnectionPool(const DBConnectionPool&) = delete;
    DBConnectionPool& operator=(const DBConnectionPool&) = delete;

    // 超时时返回 nullptr。
    MockDBConnection* acquire(std::chrono::milliseconds timeout);

    // 将连接归还到空闲队列。
    void release(MockDBConnection* conn);

    std::size_t free_count() const;
    std::size_t total_count() const { return total_count_; }
    bool using_mysql() const { return config_.use_mysql; }
    std::string mode_name() const { return config_.use_mysql ? "mysql" : "mock"; }
    const DBPoolConfig& config() const { return config_; }

private:
    // 保护连接队列和统计数据。
    mutable std::mutex mutex_;
    // 协调 acquire() 的等待与唤醒。
    std::condition_variable cv_;
    // 当前可借出的空闲连接。
    std::queue<MockDBConnection*> free_conns_;
    // 当前已借出连接集合。
    // 用于防御重复归还/非法归还，避免同一连接被重复入队。
    std::unordered_set<MockDBConnection*> in_use_conns_;
    // 对全部连接对象的所有权容器。
    std::vector<std::unique_ptr<MockDBConnection>> all_conns_;
    // 初始化时创建的固定连接总数（上限）。
    std::size_t total_count_{0};
    DBPoolConfig config_;
};

// DBConnGuard：基于 RAII 的连接托管器。
// 只要成功 acquire，即使业务中抛异常，析构时也会自动 release。
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
