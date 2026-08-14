#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace cpppools::pool {

// 检测 T 是否提供无参 reset() 成员。
//
// 用途：决定对象出池时的重置策略。优先用 reset()，
// 因为它能保留 string / vector 已分配的 capacity；
// 而回退策略（*obj = T()）会构造临时对象再赋值，丢弃已有缓冲。
template <typename U, typename = void>
struct has_reset : std::false_type {};

template <typename U>
struct has_reset<U, std::void_t<decltype(std::declval<U&>().reset())>> : std::true_type {};

// ObjectPool 与 DbConnectionPool 共用的统计形状：两者都是"借 → 还"语义。
//
// 各字段语义：
// - capacity：上限（max_size）。与 size 是**两个不同的量**，不要混用。
// - size：已创建且存活的对象数（≤ capacity）。
// - in_use / available / peak_in_use：借出中 / 空闲 / 历史最大同时借出。
//
// 等式不变式**因池而异**，别套用错：
// - ObjectPool：in_use + available == size（对象要么借出、要么在空闲队列）。
// - DbConnectionPool：in_use + available == **capacity**，且 size ≤ capacity。
//   因为重建失败的空槽仍占一个槽位（计入 available）但没有存活连接（不计入 size）。
struct AcquireStats {
    std::string name;
    std::size_t capacity{0};
    std::size_t size{0};
    std::size_t in_use{0};
    std::size_t available{0};
    std::size_t peak_in_use{0};
    // available 的历史最小值（只在借出时下降，归还不上升）。
    std::size_t min_available{0};
    // acquire 调用总数与失败数（ObjectPool：达 max_size 返回空 Ptr；
    // DbConnectionPool：超时返回 nullptr）。失败率 = failed / total。
    // 口径固化：reset() 抛异常**不计入** failed（那属于任务异常，
    // 最终经 submit_detached 进 task_exception_total 可观测），failed 只数
    // “池拒绝/超时”这一种失败。
    std::uint64_t acquire_total{0};
    std::uint64_t acquire_failed_total{0};
};

// ObjectPool<T>：线程安全的对象复用池。
//
// 在三级池化协作链路中的位置：
//   HTTP 请求 -> ThreadPool 调度 -> [ObjectPool 复用 RequestContext] -> DbConnectionPool 查询
// 在 HttpServer::route_request 中用来复用 RequestContext，避免每次请求都做一次 new/delete。
//
// 用法示例：
//   ObjectPool<Foo> pool("request", 128, 256);   // (名字, 初始数, 上限)
//   auto foo = pool.acquire();                    // 可能为空 Ptr，调用方必须判空
//   if (!foo) { /* 池已达上限：快速失败 */ }
//   // foo 离开作用域后会自动归还到池中（RAII）。
//
// RAII 自动归还原理：
//   acquire() 返回的是 std::unique_ptr<T, PoolDeleter<T>>。
//   PoolDeleter 是一个无状态 struct（只存一个池指针），在 unique_ptr 析构时
//   被调用，把指针放回 free_list_，完成“借→还”的闭环。
//
// 为什么用无状态 deleter 而不是 std::function<void(T*)>：
//   后者捕获 this 的 lambda 通常命中小对象优化，但**标准不保证**，
//   理论上每次 acquire 可能堆分配 —— 与“池化就是为了避免分配”的初衷直接冲突。
//   无状态 deleter 则构造不分配、不抛，unique_ptr 尺寸也更小。
template <typename T>
class ObjectPool;

template <typename T>
struct PoolDeleter {
    ObjectPool<T>* pool{nullptr};
    void operator()(T* obj) const;
};

template <typename T>
class ObjectPool {
public:
    // RAII 句柄：释放 unique_ptr 时会自动把对象放回池中。
    using Ptr = std::unique_ptr<T, PoolDeleter<T>>;

    // 构造契约：init_size > 0、max_size > 0、max_size >= init_size。
    //
    // max_size 是硬上限而非“建议值”：达到后 acquire 立即返回空 Ptr（§5.1.1），
    // 不会阻塞等待。与三个池“有界 + 快速失败”的总体立场一致。
    ObjectPool(std::string name, std::size_t init_size, std::size_t max_size)
        : name_(std::move(name)), max_size_(max_size) {
        if (init_size == 0) {
            throw std::invalid_argument("ObjectPool init_size must be > 0");
        }
        if (max_size == 0) {
            throw std::invalid_argument("ObjectPool max_size must be > 0");
        }
        if (max_size < init_size) {
            throw std::invalid_argument("ObjectPool max_size must be >= init_size");
        }
        // 构造期预分配：若第 i 次 new 抛出，前 i-1 个对象必须自行清理 ——
        // 构造函数抛出时 ~ObjectPool() 不会被调用，free_list_（queue）析构
        // 只释放节点、不 delete 指针。与批 1 ThreadPool 构造期的处理对称。
        try {
            for (std::size_t i = 0; i < init_size; ++i) {
                free_list_.push(new T());
            }
        } catch (...) {
            while (!free_list_.empty()) {
                delete free_list_.front();
                free_list_.pop();
            }
            throw;
        }
        total_size_ = init_size;
        // 启动时全空闲：available 的历史最小从 init_size 起步，
        // 否则未借出过的池会误报 min_available == 0。
        min_available_ = init_size;
    }

    ~ObjectPool() {
        // Release 下 assert 是空操作，悬垂句柄会变成 UB 且无任何提示。
        // 因此无论构建类型都写一行 stderr 警告（不 abort：析构里不应终止进程）。
        std::size_t leaked = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            leaked = in_use_;
        }
        if (leaked > 0) {
            std::fprintf(stderr,
                         "[cpppools] ObjectPool(\"%s\") destroyed with %zu objects still in use\n",
                         name_.c_str(), leaked);
        }
        // 断言读 leaked（锁内快照）而不是无锁读 in_use_：
        // 该断言要抓的场景恰恰是“还有别的线程在借/还”，此时裸读就是数据竞争。
        assert(leaked == 0 && "ObjectPool destroyed while objects are still in use");

        std::queue<T*> doomed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            free_list_.swap(doomed);
        }
        // T 的析构是任意用户代码，在锁外执行（与批 1 stop_and_join 的
        // discarded 同理：锁内析构用户代码可能回调进池而自锁）。
        // 注：析构期本就预期无并发使用者，这里的锁主要防析构与迟到归还的重叠。
        while (!doomed.empty()) {
            delete doomed.front();
            doomed.pop();
        }
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    // 返回空 Ptr 表示已达 max_size 且无空闲对象（立即失败，不阻塞）。
    // **调用方必须判空**：max_size >= 并发需求的不变式由运行期配置维持，
    // 不是类型保证；不判空的后果是确定性的空指针解引用（见 spec §5.1.1）。
    Ptr acquire() {
        T* obj = nullptr;
        {
            // 锁粒度尽量小：临界区内只做池状态更新，不做对象重置。
            std::lock_guard<std::mutex> lock(mutex_);
            ++acquire_total_;
            if (!free_list_.empty()) {
                // 优先复用空闲对象，减少动态分配。
                obj = free_list_.front();
                free_list_.pop();
            } else if (total_size_ < max_size_) {
                // 未达上限才扩容。new 在这里抛（如 bad_alloc）时
                // in_use_ 尚未递增，无需回滚。
                obj = new T();
                ++total_size_;
            } else {
                // 已达上限且无空闲 → 立即失败，与线程池的“拒绝”语义对齐。
                ++acquire_failed_total_;
                return Ptr{};
            }
            ++in_use_;
            if (in_use_ > peak_in_use_) {
                peak_in_use_ = in_use_;   // 在临界区内更新，多调用方并发不丢峰值
            }
            // available 刚下降，可能是新的历史最小（归还只会使其上升，不上升沿）。
            const std::size_t avail_now = free_list_.size();
            if (avail_now < min_available_) {
                min_available_ = avail_now;
            }
        }

        // 出池后重置对象内容，避免脏数据污染后续逻辑。放在锁外执行，降低锁竞争。
        //
        // 异常安全窗口修复：reset 可能抛（如 T::reset 内部分配失败）。
        // 若不处理，in_use_ 会永久漂移、对象泄漏，析构告警还会记到无关
        // 调用者头上。必须销毁而非放回 free_list：抛出可能发生在 reset
        // 中途，对象处于部分赋值的不确定状态，不该交给下一个调用方。
        try {
            reset_object(obj);
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                --total_size_;
                --in_use_;
            }
            // T 的析构是任意用户代码，在锁外执行：若 T 的析构回调进本池
            // （如归还另一个 Ptr），锁内 delete 会自锁 —— 与批 1
            // stop_and_join 里 discarded 的教训完全同源。
            delete obj;
            throw;
        }
        // PoolDeleter 是无状态的（只存一个指针），构造不分配、不抛。
        return Ptr(obj, PoolDeleter<T>{this});
    }

    // 一次性取回全部运行指标。加一次锁取一致快照：
    // 本池有 in_use + available == size 这个等式不变式，各字段分开读会
    // 出现“借出 + 空闲 > 总数”这种物理上不可能的数。/api/v1/stats 是低频
    // 端点，一次锁换来一致性是合算的。
    //
    // 注意：当前 /api/v1/stats 等端点仍在连调多个旧 getter（每个都转调
    // 本函数），端点层面的“一次锁”接线在批 4 完成。
    AcquireStats stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        AcquireStats s;
        s.name = name_;
        s.capacity = max_size_;
        s.size = total_size_;
        s.in_use = in_use_;
        s.available = free_list_.size();
        s.peak_in_use = peak_in_use_;
        s.min_available = min_available_;
        s.acquire_total = acquire_total_;
        s.acquire_failed_total = acquire_failed_total_;
        return s;
    }

    const std::string& name() const { return name_; }
    std::size_t capacity() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return max_size_;
    }

    // 以下旧 getter 保留为薄封装：http_server 与既有测试有 20+ 个调用点。
    std::size_t in_use() const { return stats().in_use; }
    std::size_t available() const { return stats().available; }
    std::size_t total_size() const { return stats().size; }
    std::size_t peak_in_use() const { return stats().peak_in_use; }

private:
    template <typename U>
    friend struct PoolDeleter;

    static void reset_object(T* obj) {
        if constexpr (has_reset<T>::value) {
            obj->reset();
        } else {
            *obj = T();
        }
    }

    void release(T* obj) {
        std::lock_guard<std::mutex> lock(mutex_);
        free_list_.push(obj);
        assert(in_use_ > 0 && "ObjectPool in_use_ underflow");
        --in_use_;
    }

    mutable std::mutex mutex_;
    std::queue<T*> free_list_;
    std::string name_;
    std::size_t max_size_{0};
    std::size_t total_size_{0};
    std::size_t in_use_{0};
    std::size_t peak_in_use_{0};
    // 初始全空闲：历史最小从 init_size 起步。
    std::size_t min_available_{0};
    std::uint64_t acquire_total_{0};
    std::uint64_t acquire_failed_total_{0};
};

template <typename T>
void PoolDeleter<T>::operator()(T* obj) const {
    if (pool != nullptr && obj != nullptr) {
        pool->release(obj);
    }
}

}  // namespace cpppools::pool
