#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace cpppools::pool {

struct ThreadPoolConfig {
    std::string name{"worker"};
    std::size_t worker_count{6};
    // 任务队列上限。必须 > 0 —— 本项目三个池都是有界的，
    // 不提供“无界”选项（传 0 会抛 invalid_argument）。
    // 无界队列在过载时内存没有上界，且客户端只能感受到延迟持续攀升，
    // 永远拿不到一个明确信号。
    std::size_t max_queue_size{1024};
    std::function<void(std::exception_ptr)> on_task_exception;
};

// 池已关闭后仍调用 submit —— 这是编程错误或停机竞态，故用异常。
// 用独立类型而非泛用 runtime_error，调用方才能精确捕获。
class PoolShutdownError : public std::runtime_error {
public:
    explicit PoolShutdownError(const std::string& what) : std::runtime_error(what) {}
};

// 线程池的运行指标。
//
// 不使用与其他两个池共用的 AcquireStats：线程池不是“借→还”池，
// 它的“待处理任务数”与“借出资源数”语义不同，
// 把 max_queue_size 映射成 capacity、pending 映射成 in_use 是错误建模。
// 命名约定与其他池一致（counter 带 _total、峰值带 peak_），形状按语义分开。
struct TaskStats {
    std::string name;
    std::size_t worker_count{0};
    std::size_t queue_capacity{0};
    std::size_t queue_pending{0};
    std::size_t peak_queue_pending{0};
    std::uint64_t submitted_total{0};
    std::uint64_t rejected_total{0};
    std::uint64_t completed_total{0};
    // 仅统计 submit_detached 提交的任务。
    // submit() 的异常仍被 packaged_task 吞进 future，池看不到 ——
    // 因此本值为 0 **不等于没有异常**。
    std::uint64_t task_exception_total{0};
    // 执行耗时：只有排队深度无法区分“排队久”与“执行慢”。
    std::uint64_t task_exec_us_total{0};
    std::uint64_t peak_task_exec_us{0};
};

// ThreadPool：固定大小的工作线程池，用于异步执行任务。
//
// 在三级池化协作链路中的位置：
//   HTTP 请求 -> [ThreadPool 调度] -> ObjectPool 复用上下文 -> DbConnectionPool 查询
// 也就是 HTTP 请求进入服务后第一时间被丢到这里，让 accept 主循环立即回到等待新连接。
//
// 设计目标：
// 1) 线程复用：避免高频创建/销毁线程带来的系统开销。
// 2) 两个提交入口：submit() 返回 future；submit_detached() 把异常交给回调。
// 3) **有界 + 拒绝**：队列达到 max_queue_size 就拒绝，把背压立即传导给调用方，
//    而不是无限堆积到内存耗尽。
// 4) 两种关闭语义：shutdown() 排空，shutdown_now() 丢弃待处理。
//
// 失败模式的划分（与项目其他层一致：不用异常做控制流）：
// - 队列满   = 预期的运行期背压 → **返回值**（nullopt / false）
// - 池已关闭 = 编程错误或停机竞态 → **抛 PoolShutdownError**
// 调用方靠这两者的区别决定行为（HTTP 层：前者回 503，后者直接关连接）。
//
// 并发模型（生产者 / 消费者）：
// - 生产者：调用 submit() 的线程（如 HttpServer::start 的 accept 循环），
//   持锁把任务塞进 tasks_，通过 cv_.notify_one() 唤醒一个 worker。
// - 消费者：worker_loop() 内部的工作线程，cv_.wait 在 tasks_ 空闲时挂起，
//   被唤醒后取出任务、在锁外执行，避免长任务阻塞队列。
//
// 学习提示：
//   condition_variable 必须搭配 unique_lock 使用。wait() 的 predicate 形式
//   能自动处理"虚假唤醒"，是 C++ 并发的标准写法。
class ThreadPool {
public:
    explicit ThreadPool(std::size_t thread_count);
    explicit ThreadPool(ThreadPoolConfig config);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template <class F, class... Args>
    // submit(...)：向队列提交任务。
    // 返回 nullopt 表示队列已满被拒绝（运行期背压，不是错误）。
    // 池已关闭时抛 PoolShutdownError。
    auto submit(F&& f, Args&&... args)
        -> std::optional<std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>>;

    template <class F, class... Args>
    // submit_detached(...)：不需要 future 的提交入口。
    // 任务异常在 worker 内被捕获，交给 on_task_exception 并计数。
    // 返回 false 表示队列已满被拒绝；池已关闭时抛 PoolShutdownError。
    bool submit_detached(F&& f, Args&&... args);

    // 停止接收新任务，排空队列后 join。
    void shutdown();

    // 停止接收新任务，**丢弃尚未开始**的任务，然后 join。
    //
    // 与 shutdown() 的共同限制（诚实说明）：两者都会等**正在执行**的任务跑完。
    // 标准 C++ 无法安全强杀线程（detach 后线程仍引用已析构的 this，是 UB），
    // 所以“任务挂死导致进程无法退出”在池层面**无解**，
    // 只能靠任务自身可中断。本类不提供 shutdown(timeout) 这种假承诺。
    void shutdown_now();

    // 当前线程池持有的工作线程数量。
    std::size_t worker_count() const;

    // 当前任务队列中的待执行任务数量（近似快照）。
    // 实现要点：用 std::atomic 取代加锁读取 tasks_.size()。
    // 这样 /api/v1/stats 这种"读多写少"的查询可以完全无锁，
    // 避免和正在 submit/工作线程取任务的临界区争夺 mutex_。
    std::size_t pending_tasks() const;

    // 待处理任务数的历史峰值。
    //
    // 由线程池自己在 enqueue() 里统计，而不是让指标端点去采样：
    // 后者只能看到被查询那一瞬的值，真正的堆积峰值很容易错过。
    std::size_t peak_pending_tasks() const;

    // 一次性取回全部运行指标。**完全无锁**，均读 atomic。
    //
    // 诚实说明：这不是一个完全原子的快照 —— 入队类计数在 mutex_ 内更新，
    // 而 completed/exception/exec_us 由 worker 在锁外更新（否则每任务多一次加锁，
    // 直接拉低热路径吞吐）。跟 ObjectPool 不同，线程池没有
    // “in_use + available == size” 这类等式不变式，因此跳字段的瞬时不一致
    // 不会产生“物理上不可能的数”。
    //
    // queue_pending 读的是 pending_count_ 而不是 tasks_.size()：
    // 两者在临界区内恒等（enqueue 与 worker_loop 同步维护），
    // 因此读 atomic 完全等价，而不必让指标采集去与热路径抢锁。
    TaskStats stats() const;

private:
    // 每个工作线程的核心循环逻辑。worker_index 用于线程命名。
    void worker_loop(std::size_t worker_index);

    // 把已类型擦除的任务入队。返回 false = 队列已满被拒绝；
    // 池已关闭时抛 PoolShutdownError。
    bool enqueue(std::packaged_task<void()> task);

    // 报告 detached 任务的异常。noexcept：回调自己抛出不得杀死 worker。
    void report_task_exception(std::exception_ptr eptr) noexcept;

    // shutdown() 与 shutdown_now() 的共同实现，差异只在是否清队。
    void stop_and_join(bool discard_pending);

    std::vector<std::thread> workers_;
    // 队列元素用 packaged_task 而不是 std::function：
    // std::function 要求目标**可拷贝**，而捕获了 move-only 参数的 lambda
    // 只可移动，根本存不进去。packaged_task 只要求可移动。
    std::queue<std::packaged_task<void()>> tasks_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_{false};
    ThreadPoolConfig config_;

    // 与 tasks_ 同步维护的待处理任务计数。
    // 入队时 +1、出队时 -1，更新都发生在 mutex_ 的临界区内，
    // 因此 atomic 这里只是为了让“读”侧无锁，不是用来代替互斥。
    std::atomic<std::size_t> pending_count_{0};
    // 待处理任务数的历史峰值，同样在临界区内更新。
    std::atomic<std::size_t> peak_pending_count_{0};

    // 下面两组计数器的更新位置不同，不要弄混：
    // ① 入队/拒绝发生在 enqueue() 的临界区内；
    // ② 完成/异常/耗时由 worker 在**锁外**更新 —— 放进锁里会给每个任务
    //    多一次加锁，直接拉低热路径吞吐。
    std::atomic<std::uint64_t> submitted_total_{0};
    std::atomic<std::uint64_t> rejected_total_{0};
    std::atomic<std::uint64_t> completed_total_{0};
    std::atomic<std::uint64_t> task_exception_total_{0};
    std::atomic<std::uint64_t> task_exec_us_total_{0};
    std::atomic<std::uint64_t> peak_task_exec_us_{0};
};

// detail 内的一切**不是公开契约**，可能随时变动。
namespace detail {

// 把可调用对象与参数捆绑成一个无参可调用体。
//
// 不用 std::bind：它会 decay 参数且调用时**按左值传递**，
// 因此 submit(f, std::unique_ptr<X>{}) 这类 move-only 参数无法工作。
//
// 返回类型必须是 decltype(auto) 而不是 auto：
//   auto 会**剥掉引用**，而 submit() 声明的是 invoke_result_t（保留引用）。
//   两边不一致的后果已实测：返回 const T& 的可调用对象会编译通过但
//   future 里存的是指向 lambda 内部临时对象的引用 —— get() 拿到悬垂引用，
//   无告警、无 ASan 报告，只是值静默变错；返回 T& 则直接编译失败。
//
// 两个使用约束：
// 1) **单发**：std::apply(std::move(fn), std::move(tup)) 会把 tup 移空，
//    返回的可调用体只能调用一次（packaged_task 已保证这一点）。
// 2) **形参为 T& 时需显式 std::ref**：std::move(tup) 传右值，绑不上左值引用。
//    这是与 std::bind 相比的行为回退。逃生口（已实测可用）：
//        int x = 1;
//        pool.submit([](int& r) { r += 41; return r; }, std::ref(x));  // x 真的被改
//    make_tuple 会把 reference_wrapper<T> 解包成 T&。
template <class F, class... Args>
auto make_bound(F&& f, Args&&... args) {
    return [fn = std::forward<F>(f),
            tup = std::make_tuple(std::forward<Args>(args)...)]() mutable -> decltype(auto) {
        return std::apply(std::move(fn), std::move(tup));
    };
}

}  // namespace detail

template <class F, class... Args>
auto ThreadPool::submit(F&& f, Args&&... args)
    -> std::optional<std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>> {
    using ReturnType = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;

    // 内层 packaged_task 提供 future：任务异常被存进 future，由调用方 get() 时重抛。
    std::packaged_task<ReturnType()> inner(
        detail::make_bound(std::forward<F>(f), std::forward<Args>(args)...));
    std::future<ReturnType> fut = inner.get_future();

    // 外层 packaged_task<void()> 仅作为**只需可移动的类型删除容器**，
    // 其 future 被丢弃。之前用 std::function 作容器，而 std::function 要求目标
    // **可拷贝**，捕获了 move-only 对象的 lambda 存不进去。
    //
    // 开销：两层各一份 shared state，共 2 次堆分配 —— 与改造前的
    // make_shared<packaged_task>（控制块 + shared state）持平，**不是回退**。
    std::packaged_task<void()> outer([inner = std::move(inner)]() mutable { inner(); });

    if (!enqueue(std::move(outer))) {
        return std::nullopt;
    }
    return std::optional<std::future<ReturnType>>(std::move(fut));
}

template <class F, class... Args>
bool ThreadPool::submit_detached(F&& f, Args&&... args) {
    auto bound = detail::make_bound(std::forward<F>(f), std::forward<Args>(args)...);

    std::packaged_task<void()> task([this, bound = std::move(bound)]() mutable {
        // try/catch 必须在这里，**不能放在 worker_loop 里**：
        // 外层 packaged_task::operator() 会把异常存进（被丢弃的）future 而不重抛，
        // 所以 worker_loop 里的 catch 永远不会触发。
        try {
            bound();
        } catch (...) {
            report_task_exception(std::current_exception());
        }
    });

    return enqueue(std::move(task));
}

}  // namespace cpppools::pool
