#pragma once

#include "cpppools/db/database.h"
#include "cpppools/pool/object_pool.h"  // AcquireStats（借→还池的共用统计形状）

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cpppools::db {

struct DbPoolConfig {
    std::size_t pool_size{8};

    // 后端类型标识（"sqlite" / "mysql" / ...）：仅用于指标标签与
    // /api/v1/get-config 回显，建连逻辑在 ConnectionFactory 里。
    std::string backend{"sqlite"};
};

// 创建一条连接（一个 Database 实例：连接 + 自省快照 + statement cache）。
// 参数是槽位编号（用于日志）。返回 nullptr 或抛异常都表示创建失败。
//
// 为何抽成 factory（而不是池内写死建连）：健康检查失败后需要**重建**，
// 重建就必须知道「如何创建一条连接」。factory 化后池代码与后端类型
// 完全解耦 —— 期 3 把元素从 DbConnection 切到 Database，池零改动。
//
// 约束：必须线程安全（acquire 的重建路径在 worker 线程调用）。
using ConnectionFactory = std::function<std::unique_ptr<Database>(std::size_t slot_index)>;

// 连接池的统计：共用的借还形状 + 连接池特有计数。
struct DbPoolStats {
    pool::AcquireStats base;
    std::uint64_t invalid_release_total{0};  // 重复归还 / 野指针归还
    std::uint64_t rebuild_total{0};          // 探活失败后重建成功
    std::uint64_t rebuild_failed_total{0};   // 重建失败（空槽保留，等待自愈）
};

// DbConnectionPool：有界连接池，带健康检查与失效重建。
// 元素是 Database（每槽位一个实例：连接 + schema 快照 + statement cache）。
//
// 在三级池化协作链路中的位置：
//   HTTP 请求 -> ThreadPool 调度 -> ObjectPool 复用上下文 -> [连接池 查询]
// 是链路里唯一接触外部资源（SQLite / MySQL）的池。
//
// 结构：N 个**槽位**（PooledConn），地址在整个池生命周期内稳定。
// 空闲/借出两个队列存的都是 PooledConn*，因此销毁/重建内层连接
// 不影响身份，也不会出现「指针复用被误判为合法归还」的问题。
//
// acquire(timeout)：
//   最多等待 timeout 时长来获取空闲连接；**timeout 是绝对 deadline**
//   （不是每次重试重新计满），超时返回 nullptr。
//   注意真实上界：deadline 之后**不再开始新的等待**，但一次已开始的探活/重建
//   会跑到底 —— 故最坏耗时 = timeout + 一次探活/重建时长（healthy() 无超时参数，
//   MySQL 下 mysql_ping/重连无法中断，见 spec §14）。
//   借到的连接若空闲超过 idle_probe_threshold 会先探活；
//   探活失败则销毁并用 factory 重建；重建失败保留空槽、下次再试（自愈）。
//   同一次 acquire 会尝试**所有**未试过的空闲槽位（各重建一次），
//   但同一槽位不会原地重试（tried_this_call 防护）。
//
// release(conn)：
//   归还到空闲队列并唤醒等待者。重复归还 / 野指针归还被拒绝、
//   计入 invalid_release_total 并打 stderr（不抛异常：归还通常在析构路径）。
class DbConnectionPool {
public:
    // 主构造。idle_probe_threshold：连接空闲超过该时长才在 acquire 时探活
    // （HikariCP 风格：热连接零开销，冷连接才付一次往返）。
    // 设为 0 表示每次都探活（测试用）。
    DbConnectionPool(std::string name,
                     ConnectionFactory factory,
                     std::size_t pool_size,
                     std::chrono::milliseconds idle_probe_threshold = std::chrono::milliseconds{30000});

    // 携带配置元数据的变体：factory 化后建连逻辑在 factory 里，
    // 但 mode_name()/config() 仍需要配置快照（指标与
    // /api/v1/get-config 在用）。pool_size 取自 config.pool_size。
    DbConnectionPool(std::string name,
                     ConnectionFactory factory,
                     const DbPoolConfig& config,
                     std::chrono::milliseconds idle_probe_threshold = std::chrono::milliseconds{30000});

    ~DbConnectionPool();

    DbConnectionPool(const DbConnectionPool&) = delete;
    DbConnectionPool& operator=(const DbConnectionPool&) = delete;

    // 超时时返回 nullptr。
    Database* acquire(std::chrono::milliseconds timeout);

    // 将连接归还到空闲队列。
    void release(Database* conn);

    // 一次性取回全部统计（一次锁，一致快照）。
    DbPoolStats stats() const;

    // 以下旧 getter 保留为薄封装（转调 stats()）：
    // http_server 与既有测试有大量调用点。
    std::size_t free_count() const;
    std::size_t total_count() const;
    std::uint64_t invalid_release_total() const;
    std::uint64_t rebuild_total() const;
    std::uint64_t rebuild_failed_total() const;

    bool using_mysql() const { return config_.backend == "mysql"; }
    std::string mode_name() const { return config_.backend; }
    const DbPoolConfig& config() const { return config_; }
    const std::string& name() const { return name_; }

private:
    // 槽位：一个可能为空的连接 + 池的簿记。
    // last_released_at 放在槽位里（而不是 Database 里）：
    // 这是池的簿记，不是连接的属性；且由 mutex_ 保护，无需 atomic。
    struct PooledConn {
        std::unique_ptr<Database> conn;  // nullptr = 空槽（重建失败后保留）
        std::chrono::steady_clock::time_point last_released_at{};
        std::size_t slot_index{0};
    };

    // 在锁外执行探活/重建（见 cpp 注释：锁内探活会独占整个池）。
    // 成功返回 true（slot->conn 已就绪）；失败返回 false（slot->conn 为空）。
    bool probe_and_rebuild(PooledConn* slot);

    mutable std::mutex mutex_;
    std::condition_variable cv_;

    // 槽位的所有权容器；地址稳定，是池内的身份标识。
    std::vector<std::unique_ptr<PooledConn>> slots_;
    // 当前可借出的空闲槽位。
    std::queue<PooledConn*> free_slots_;
    // 当前已借出的槽位。用于防御重复归还/野指针归还。
    std::unordered_set<PooledConn*> in_use_slots_;
    // conn 指针 → 槽位的反查表，release(Database*) 用它定位槽位。
    std::unordered_map<Database*, PooledConn*> conn_to_slot_;

    std::string name_;
    ConnectionFactory factory_;
    std::chrono::milliseconds idle_probe_threshold_;
    DbPoolConfig config_;  // 便利构造路径保留配置快照（mode_name()/config() 用）

    // 与 free_slots_ 同步维护的空闲计数（让 free_count() 无锁读）。
    std::atomic<std::size_t> free_count_atomic_{0};
    // 空闲计数的历史最小值。读写都在 mutex_ 内（acquire 更新、stats 读取），
    // 无需 atomic —— 用普通成员，避免被误读为"可无锁读"。
    std::size_t min_free_{0};

    std::atomic<std::uint64_t> invalid_release_total_{0};
    std::atomic<std::uint64_t> rebuild_total_{0};
    std::atomic<std::uint64_t> rebuild_failed_total_{0};

    // acquire 调用总数与超时失败数。在 mutex_ 内更新（计数更新点都在锁内；
    // 探活/重建本身在锁外执行），与 free_slots_ 等同一把锁，无需 atomic。
    std::uint64_t acquire_total_{0};
    std::uint64_t acquire_failed_total_{0};

};

// DbConnGuard：基于 RAII 的连接托管器。
// 只要成功 acquire，即使业务中抛异常，析构时也会自动 release。
//
// 可移动（不可拷贝）：支持从函数返回 / 放入容器。
// pool_ 用指针而非引用：引用无法重新绑定，移动赋值根本写不出来。
class DbConnGuard {
public:
    DbConnGuard(DbConnectionPool& pool, std::chrono::milliseconds timeout);
    ~DbConnGuard();

    DbConnGuard(const DbConnGuard&) = delete;
    DbConnGuard& operator=(const DbConnGuard&) = delete;
    DbConnGuard(DbConnGuard&& other) noexcept;
    DbConnGuard& operator=(DbConnGuard&& other) noexcept;

    Database* get() const { return conn_; }
    bool valid() const { return conn_ != nullptr; }

private:
    DbConnectionPool* pool_{nullptr};
    Database* conn_{nullptr};
};

}  // namespace cpppools::db
