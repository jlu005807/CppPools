#include "cpppools/db/db_connection_pool.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace cpppools::db {

DbConnectionPool::DbConnectionPool(std::string name,
                                   ConnectionFactory factory,
                                   std::size_t pool_size,
                                   std::chrono::milliseconds idle_probe_threshold)
    : name_(std::move(name)),
      factory_(std::move(factory)),
      idle_probe_threshold_(idle_probe_threshold) {
    if (pool_size == 0) {
        throw std::invalid_argument("DbConnectionPool size must be > 0");
    }
    if (!factory_) {
        throw std::invalid_argument("DbConnectionPool factory must not be null");
    }

    // fail-fast：任一槽位创建失败 → 整个池拒绝启动。
    // 一个启动了但 0 条可用连接、对所有请求回 500 的池，
    // 比一个拒绝启动并打印清晰错误的池更糟。配置错误应在部署时暴露。
    //
    // 构造函数抛出时本函数后续代码不会执行，已创建的槽位由 slots_
    // （vector<unique_ptr>）随对象内存一起回收 —— 无泄漏。
    slots_.reserve(pool_size);
    for (std::size_t i = 0; i < pool_size; ++i) {
        auto slot = std::make_unique<PooledConn>();
        slot->slot_index = i;
        slot->conn = factory_(i);  // 返回 nullptr 或抛异常都算失败
        if (!slot->conn) {
            throw std::runtime_error("DbConnectionPool: failed to create connection for slot " +
                                     std::to_string(i));
        }
        slot->last_released_at = std::chrono::steady_clock::now();
        conn_to_slot_[slot->conn.get()] = slot.get();
        free_slots_.push(slot.get());
        slots_.push_back(std::move(slot));
    }
    free_count_atomic_.store(pool_size, std::memory_order_relaxed);
    min_free_ = pool_size;
}

DbConnectionPool::DbConnectionPool(std::string name,
                                   ConnectionFactory factory,
                                   const DbPoolConfig& config,
                                   std::chrono::milliseconds idle_probe_threshold)
    : DbConnectionPool(std::move(name), std::move(factory), config.pool_size, idle_probe_threshold) {
    config_ = config;  // 保留配置快照供 mode_name()/config() 使用
}

DbConnectionPool::~DbConnectionPool() {
    // 与 ObjectPool 的析构约束一致：所有借出的连接必须先归还，
    // 否则 DbConnGuard 持有的指针在池析构后会变野。
    // Release 下 assert 是空操作，因此无论构建类型都写一行 stderr 警告。
    std::size_t leaked = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        leaked = in_use_slots_.size();
    }
    if (leaked > 0) {
        std::fprintf(stderr,
                     "[cpppools] DbConnectionPool(\"%s\") destroyed with %zu connections still in use\n",
                     name_.c_str(), leaked);
    }
    assert(leaked == 0 && "DbConnectionPool destroyed while connections are still in use");
    // slots_（vector<unique_ptr<PooledConn>>）自动清理，连接随槽位析构。
}

Database* DbConnectionPool::acquire(std::chrono::milliseconds timeout) {
    // **绝对 deadline**：timeout 是含探活与重建的总上界。
    // 若每次重试都重新 wait_for(timeout)，最坏总耗时是 N × timeout + N × 探活耗时，
    // 而 HTTP 层依赖这个上界（db_acquire_timeout 默认 100ms 内回 503）。
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    // 本次 acquire 已尝试过重建且失败的槽位：
    // 空槽放回空闲队列后**不得**在同一次 acquire 里被再次借出重试，
    // 否则会在原地打转直到 deadline（每次重建都 ++rebuild_failed_total）。
    // spec 的语义是「保留槽位，**下次** acquire 再试」（自愈）。
    // 用 vector 而非 unordered_set：槽位数 <= pool_size（默认 8），
    // 线性查找比每请求一次哈希集合的堆分配便宜。
    std::vector<PooledConn*> tried_this_call;

    std::unique_lock<std::mutex> lock(mutex_);
    ++acquire_total_;  // 锁内更新：与其他池状态同一把锁，无需 atomic
    while (true) {
        // 谓词只看「有没有空闲槽位」，**不做轮转**：轮转会改变队列顺序，
        // 使随后的锁内扫描取到与谓词所见不同的槽位（曾导致一次 acquire 误销毁多个连接）。
        // 若空闲槽全是本次已试过的空槽，扫描会找不到、走下面的无谓词挂起，
        // 由 deadline 兜底 —— 不会活锁（循环有界于 deadline），也不烧 CPU。
        const bool got_slot = cv_.wait_until(lock, deadline, [this]() {
            return !free_slots_.empty();
        });
        if (!got_slot) {
            ++acquire_failed_total_;
            return nullptr;  // 到 deadline 仍无（未尝试过的）空闲槽位
        }
        // wait_until 在超时点会最后一次评估谓词，为真则返回真 ——
        // 若不检查，会在 deadline 之后仍发起一次完整的探活/重建。
        // 真实上界因此是 deadline + 一次已开始的探活/重建时长（见头文件注释）。
        if (std::chrono::steady_clock::now() >= deadline) {
            ++acquire_failed_total_;
            return nullptr;
        }

        // 锁内扫描：轮转跳过本次已试过的槽位，取出第一个未尝试的。
        // 扫描会改变队列顺序，但这是锁内唯一持有者的操作，无并发可见性问题。
        PooledConn* slot = nullptr;
        const std::size_t n = free_slots_.size();
        for (std::size_t i = 0; i < n; ++i) {
            PooledConn* s = free_slots_.front();
            free_slots_.pop();
            if (std::find(tried_this_call.begin(), tried_this_call.end(), s) ==
                tried_this_call.end()) {
                slot = s;
                break;
            }
            free_slots_.push(s);
        }
        if (slot == nullptr) {
            // 空闲槽全是本次已试过的空槽。**不得 continue 空转**：
            // 谓词「队列非空」会立即为真（空槽就在队列里），形成
            // 锁拿放+轮转的忙等待直到 deadline（实测 200ms 烧 98% CPU，
            // 而触发条件恰是 DB 持续故障+factory 快速失败——最坏时刻）。
            // 改为无谓词挂起：等别人的 release（产生本次未试过的新空闲槽，
            // release 里有 notify_one）或 deadline；虚假唤醒只是再扫一圈。
            if (std::chrono::steady_clock::now() >= deadline) {
                ++acquire_failed_total_;
                return nullptr;
            }
            cv_.wait_until(lock, deadline);
            continue;
        }

        in_use_slots_.insert(slot);
        const std::size_t remaining = free_slots_.size();
        free_count_atomic_.store(remaining, std::memory_order_relaxed);
        // 在临界区内更新历史最小值：已持锁，无需 CAS。
        if (remaining < min_free_) {
            min_free_ = remaining;
        }

        // 仅空闲超阈值才探活（HikariCP 风格）：热连接零开销。
        // 空槽（conn == nullptr）必须走重建路径。
        const auto now = std::chrono::steady_clock::now();
        const bool needs_probe =
            slot->conn == nullptr ||
            (now - slot->last_released_at) >= idle_probe_threshold_;
        lock.unlock();

        bool ok = true;
        if (needs_probe) {
            // 锁外执行：探活可能是网络往返，锁内做会独占整个池。
            ok = probe_and_rebuild(slot);
        }

        lock.lock();
        if (ok) {
            return slot->conn.get();
        }
        // 重建失败：空槽放回空闲队列（槽位保留 → 下次 acquire 再试 → 自愈），
        // 从借出集合移除；并记入 tried_this_call，防止本次 acquire 原地重试。
        tried_this_call.push_back(slot);
        in_use_slots_.erase(slot);
        slot->last_released_at = std::chrono::steady_clock::now();
        free_slots_.push(slot);
        free_count_atomic_.fetch_add(1, std::memory_order_relaxed);
        // 唤醒其他等待者：它们可能借这个空槽尝试重建（若 DB 已恢复则直接成功）。
        // 不 notify 的话，自愈机会会被推迟一整个 acquire 周期且链式丢失。
        cv_.notify_one();
        if (std::chrono::steady_clock::now() >= deadline) {
            ++acquire_failed_total_;
            return nullptr;
        }
        // 继续循环：可能还有其他空闲槽位。
    }
}

bool DbConnectionPool::probe_and_rebuild(PooledConn* slot) {
    // 调用约定：调用方已释放 mutex_，且 slot 已登记在 in_use_slots_
    // （对其他线程不可见），因此这里无需持锁。
    if (slot->conn && slot->conn->healthy()) {
        return true;  // 探活通过，直接复用
    }

    // 探活失败（或空槽）：销毁旧连接并用 factory 重建。
    //
    // 对 slot->conn 的读写必须持锁：stats() 会在锁内遍历它。
    // 锁外写 unique_ptr 与锁内读是未同步的并发读写（形式上 UB）。
    // 昂贵操作（delete / factory / healthy）仍留在锁外。
    Database* old = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        old = slot->conn.release();
        conn_to_slot_.erase(old);
    }
    delete old;  // 锁外：析构是任意用户代码（批 1/批 2 教训同源）

    std::unique_ptr<Database> fresh;
    try {
        fresh = factory_(slot->slot_index);
    } catch (...) {
        fresh = nullptr;  // factory 抛出等同创建失败：保留空槽等待自愈
    }
    if (!fresh) {
        rebuild_failed_total_.fetch_add(1, std::memory_order_relaxed);
        // slot->conn 保持 nullptr：空槽是「保留槽位」的唯一载体。
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        slot->conn = std::move(fresh);
        conn_to_slot_[slot->conn.get()] = slot;
    }
    rebuild_total_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void DbConnectionPool::release(Database* conn) {
    if (!conn) {
        // 合法路径：DbConnGuard 没拿到连接时析构会调 release(nullptr)。
        // **不得**计入 invalid_release_total，否则池耗尽场景会刷假告警。
        return;
    }

    PooledConn* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = conn_to_slot_.find(conn);
        if (it == conn_to_slot_.end() || in_use_slots_.find(it->second) == in_use_slots_.end()) {
            // 重复归还 / 野指针归还：拒绝入队、保持池状态一致。
            // 不抛异常（归还通常在析构路径），但要可观测 ——
            // 静默丢弃会让这类 bug 完全无法排查。
            invalid_release_total_.fetch_add(1, std::memory_order_relaxed);
            std::fprintf(stderr,
                         "[cpppools] DbConnectionPool(\"%s\"): invalid release of conn=%p "
                         "(duplicate or foreign pointer)\n",
                         name_.c_str(), static_cast<const void*>(conn));
            return;
        }
        slot = it->second;
        in_use_slots_.erase(it->second);
        slot->last_released_at = std::chrono::steady_clock::now();
        free_slots_.push(slot);
        // 刚归还的槽位对任何等待中的 acquire 都是「未尝试」的。
        free_count_atomic_.fetch_add(1, std::memory_order_relaxed);
    }
    // 唤醒一个等待 acquire() 的线程。
    cv_.notify_one();
}

DbPoolStats DbConnectionPool::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    DbPoolStats s;
    s.base.name = name_;
    s.base.capacity = slots_.size();
    // size = 存活连接数。重建失败的空槽不算存活 ——
    // 这个差值正好暴露「池当前的退化程度」。
    std::size_t alive = 0;
    for (const auto& slot : slots_) {
        if (slot->conn) {
            ++alive;
        }
    }
    s.base.size = alive;
    s.base.in_use = in_use_slots_.size();
    s.base.available = free_slots_.size();
    // 峰值借出数：用 capacity - 历史最小空闲 推得（两者都在锁内维护）。
    s.base.peak_in_use = slots_.size() - min_free_;
    s.base.min_available = min_free_;
    s.invalid_release_total = invalid_release_total_.load(std::memory_order_relaxed);
    s.rebuild_total = rebuild_total_.load(std::memory_order_relaxed);
    s.rebuild_failed_total = rebuild_failed_total_.load(std::memory_order_relaxed);
    s.base.acquire_total = acquire_total_;
    s.base.acquire_failed_total = acquire_failed_total_;
    return s;
}

std::size_t DbConnectionPool::free_count() const {
    // 无锁读取：/api/v1/stats 高频查询场景下避免和 acquire/release 抢 mutex_。
    return free_count_atomic_.load(std::memory_order_relaxed);
}

std::size_t DbConnectionPool::total_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return slots_.size();
}

std::uint64_t DbConnectionPool::invalid_release_total() const {
    return invalid_release_total_.load(std::memory_order_relaxed);
}

std::uint64_t DbConnectionPool::rebuild_total() const {
    return rebuild_total_.load(std::memory_order_relaxed);
}

std::uint64_t DbConnectionPool::rebuild_failed_total() const {
    return rebuild_failed_total_.load(std::memory_order_relaxed);
}

DbConnGuard::DbConnGuard(DbConnectionPool& pool, std::chrono::milliseconds timeout)
    : pool_(&pool) {
    conn_ = pool_->acquire(timeout);
}

DbConnGuard::~DbConnGuard() {
    // 即使 conn_ 为空也安全（release 内部会直接返回）。
    if (pool_ != nullptr) {
        pool_->release(conn_);
    }
}

DbConnGuard::DbConnGuard(DbConnGuard&& other) noexcept
    : pool_(other.pool_), conn_(other.conn_) {
    // 源置空：防止双重归还。
    other.pool_ = nullptr;
    other.conn_ = nullptr;
}

DbConnGuard& DbConnGuard::operator=(DbConnGuard&& other) noexcept {
    if (this != &other) {
        if (pool_ != nullptr) {
            pool_->release(conn_);  // 先归还自己持有的
        }
        pool_ = other.pool_;
        conn_ = other.conn_;
        other.pool_ = nullptr;
        other.conn_ = nullptr;
    }
    return *this;
}

}  // namespace cpppools::db
