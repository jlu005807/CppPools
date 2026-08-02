#pragma once

#include "db_pool/mock_db_connection.h"

#include <atomic>
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
// 在三级池化协作链路中的位置：
//   HTTP 请求 -> ThreadPool 调度 -> ObjectPool 复用上下文 -> [DBConnectionPool 查询]
// 是链路里唯一接触外部资源（mock 后端 / 真实 MySQL）的池，因此实现关注两点：
//   1) 超时机制：避免业务线程在资源耗尽时无限等待；
//   2) 归还校验：unordered_set 拦截重复归还/野指针归还，保护池状态。
//
// acquire(timeout)：
//   最多等待 timeout 时长来获取空闲连接；
//   超时返回 nullptr，避免业务线程无限阻塞。
//   底层使用 std::condition_variable::wait_for + predicate，可同时防虚假唤醒。
//
// release(conn)：
//   将连接归还到空闲队列，并唤醒一个等待中的线程（cv_.notify_one()）。
//
// 学习提示：
//   一般生产环境的连接池还会做"连接健康探活/失效重连/最大空闲时间"等扩展，
//   本项目为教学目标保持最小核心，重点突出"有界 + 超时 + RAII 归还"三件事。
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

    // 与 free_conns_ 同步维护的空闲计数。
    // 让 free_count() 这种 stats getter 可以无锁查询，不和 acquire/release 抢 mutex_。
    // 更新点都在临界区内，atomic 这里只是为"读"侧提供无锁视图。
    std::atomic<std::size_t> free_count_atomic_{0};
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
