#include "cpppools/pool/thread_pool.h"

#include <atomic>
#include <chrono>
#include <cstdio>

#if defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif

namespace cpppools::pool {
namespace {

// 给当前线程命名。失败一律忽略 —— 命名只为了排障可读性，
// 不得因为命名失败而影响启动。
//
// 平台差异：macOS 的 pthread_setname_np 只接一个参数、只能命名当前线程；
// Linux 版本取 pthread_t，且名字上限 16 字节（含结尾零），超长会直接失败，
// 因此先截断。
void set_current_thread_name(const std::string& name) noexcept {
#if defined(__APPLE__)
    (void)pthread_setname_np(name.substr(0, 63).c_str());
#elif defined(__linux__)
    (void)pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
#else
    (void)name;
#endif
}

// 便利构造用的默认配置。
//
// 按字段名赋值而不用聚合初始化：
// 1) name / max_queue_size 的默认值只在头文件里有一份真相；
// 2) worker_count 与 max_queue_size 同为 size_t，聚合初始化下一旦有人调整
//    ThreadPoolConfig 的字段顺序，就会**静默地**把队列上限当成 worker 数。
ThreadPoolConfig make_default_config(std::size_t thread_count) {
    ThreadPoolConfig cfg;
    cfg.worker_count = thread_count;
    return cfg;
}

}  // namespace

ThreadPool::ThreadPool(std::size_t thread_count) : ThreadPool(make_default_config(thread_count)) {}

ThreadPool::ThreadPool(ThreadPoolConfig config) : config_(std::move(config)) {
    if (config_.worker_count == 0) {
        throw std::invalid_argument("worker_count must be > 0");
    }
    // 本项目三个池都是有界的，不提供“0 = 无界”这种魔法值。
    // 无界队列与本批的全部目标（过载时快速失败而非无限堆积）直接矛盾。
    if (config_.max_queue_size == 0) {
        throw std::invalid_argument("max_queue_size must be > 0");
    }

    // 启动时预先创建好工作线程，后续 submit() 只负责入队任务即可。
    workers_.reserve(config_.worker_count);
    try {
        for (std::size_t i = 0; i < config_.worker_count; ++i) {
            workers_.emplace_back([this, i]() { worker_loop(i); });
        }
    } catch (...) {
        // 构造抛出 => ~ThreadPool 不会被调用 => workers_ 析构时对
        // 已创建的 joinable thread 调析构 => std::terminate。
        // 必须在这里自己收尾，否则“线程资源耗尽”这个可恢复错误会变成进程直接死。
        stop_and_join(/*discard_pending=*/true);
        throw;
    }
}

ThreadPool::~ThreadPool() {
    // 析构走 shutdown_now() 而非 shutdown()：析构不应无限期阻塞。
    // 队列里积压上千任务时 shutdown() 会把进程退出拖很久；异常展开路径上更糟。
    // 需要优雅排空的调用方请显式调 shutdown()。
    shutdown_now();
}

bool ThreadPool::enqueue(std::packaged_task<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) {
            throw PoolShutdownError("submit on stopped ThreadPool");
        }
        // 有界队列：达到上限就拒绝，而不是无限堆积。
        // 无限堆积下客户端只能感受到延迟持续攀升，永远拿不到快速失败，
        // 而内存会一直长到耗尽。拒绝才能把背压真正传导出去。
        // max_queue_size 已在构造时保证 > 0。
        if (tasks_.size() >= config_.max_queue_size) {
            rejected_total_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        tasks_.push(std::move(task));
        submitted_total_.fetch_add(1, std::memory_order_relaxed);
        // 队列长度的递增必须发生在临界区内：
        // 否则 worker_loop 端有可能在我们 ++ 之前先取出该任务并 --，导致计数短暂为负。
        const std::size_t pending = pending_count_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (pending > peak_pending_count_.load(std::memory_order_relaxed)) {
            peak_pending_count_.store(pending, std::memory_order_relaxed);
        }
    }
    // 唤醒一个休眠线程来消费新任务。
    cv_.notify_one();
    return true;
}

void ThreadPool::report_task_exception(std::exception_ptr eptr) noexcept {
    task_exception_total_.fetch_add(1, std::memory_order_relaxed);
    if (!config_.on_task_exception) {
        return;
    }
    // 回调在 worker 线程、在 catch(...) 内被调用。若它自己抛出且无人接，
    // worker 线程会终止 —— 池静默少一个 worker，而 worker_count() 仍返回原值
    // （指标撒谎），最坏情况所有 worker 死光、队列永久积压。
    try {
        config_.on_task_exception(eptr);
    } catch (...) {
        // 故意吞掉：保住 worker 存活比把回调的 bug 冒泡重要。
        // 但不能吞得毫无痕迹 —— 一个永远抛异常的回调会让所有异常报告
        // 永久失效。只提示一次：过载时每次都打会刷屏，反而掩盖其他日志。
        static std::atomic_flag warned = ATOMIC_FLAG_INIT;
        if (!warned.test_and_set(std::memory_order_relaxed)) {
            std::fprintf(stderr,
                         "[cpppools] ThreadPool(\"%s\"): on_task_exception 回调自身抛出异常，"
                         "已吞掉以保住 worker 存活（本提示只打一次）\n",
                         config_.name.c_str());
        }
    }
}

void ThreadPool::shutdown() {
    stop_and_join(/*discard_pending=*/false);
}

void ThreadPool::shutdown_now() {
    stop_and_join(/*discard_pending=*/true);
}

void ThreadPool::stop_and_join(bool discard_pending) {
    std::queue<std::packaged_task<void()>> discarded;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) {
            return;
        }
        // stop_ 阻止后续 submit()。队列里已存在的任务是否保留由 discard_pending 决定。
        stop_ = true;

        if (discard_pending) {
            // 清队必须在临界区内完成，且同步清零计数，
            // 否则 pending_count_ 与 tasks_.size() 会永久失同步。
            //
            // 被丢弃的 packaged_task 析构时会破坏其 promise，
            // 因此 submit() 的等待方会在 .get() 时拿到
            // std::future_error(broken_promise) 而不是阻塞。这是行为变更。
            tasks_.swap(discarded);
            pending_count_.store(0, std::memory_order_relaxed);
        }
    }

    // 销毁被丢弃的任务。位置受**两个**约束夹击，两者都不能违：
    //
    // 1) 不能在锁内：packaged_task 析构会释放捕获物，而捕获物的析构是
    //    **任意用户代码**，可能回调进本池（stats() / submit()）。
    //    mutex_ 非递归，锁内析构会自锁（已实测：捕获物析构里只调一个
    //    无害的 stats() 就挂住）。
    // 2) 不能拖到 join() 之后：调用方可能在等一个由捕获物析构发出的信号
    //    （测试就这么确定性地等“清队已发生”）。若在 join 后才销毁，
    //    而 join 又在等 worker 结束、worker 等调用方放行 —— 三方死锁。
    //    （初次修复把它放到函数作用域末尾，正是踩了这个坑。）
    discarded = {};

    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::worker_loop(std::size_t worker_index) {
    set_current_thread_name(config_.name + "-" + std::to_string(worker_index));

    while (true) {
        std::packaged_task<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return stop_ || !tasks_.empty(); });
            // 仅在以下两个条件同时满足时退出线程：
            // 1) 已请求 shutdown；
            // 2) 队列任务已全部消费完。
            if (stop_ && tasks_.empty()) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
            // 出队也在临界区内同步更新计数，保证 pending_count_ 始终等于 tasks_.size()。
            pending_count_.fetch_sub(1, std::memory_order_relaxed);
        }

        // 在锁外执行任务，减少临界区时长，提升并发吞吐。
        //
        // 注意：这里**不能**靠 try/catch 捕获任务异常 —— packaged_task::operator()
        // 会把异常存进自己的 future 而不重抛。detached 任务的异常捕获
        // 放在 submit_detached 安装的 wrapper lambda 里（见 thread_pool.h）。
        const auto started = std::chrono::steady_clock::now();
        task();
        const auto elapsed_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started)
                .count());

        // 计数器在锁外更新：放进锁里会给每个任务多一次加锁。
        completed_total_.fetch_add(1, std::memory_order_relaxed);
        task_exec_us_total_.fetch_add(elapsed_us, std::memory_order_relaxed);
        std::uint64_t peak = peak_task_exec_us_.load(std::memory_order_relaxed);
        while (elapsed_us > peak &&
               !peak_task_exec_us_.compare_exchange_weak(peak, elapsed_us,
                                                         std::memory_order_relaxed)) {
            // compare_exchange_weak 失败时会把最新值写回 peak，循环重试。
            // 用 CAS 而不是“读→比→写”：多 worker 并发时后者会丢峰值。
        }
    }
}

TaskStats ThreadPool::stats() const {
    TaskStats s;
    s.name = config_.name;
    s.worker_count = workers_.size();
    s.queue_capacity = config_.max_queue_size;
    // 完全无锁：pending_count_ 与 tasks_.size() 在临界区内恒等，
    // 读 atomic 完全等价，且不必让指标采集去与 submit/worker 的热路径抢 mutex_。
    s.queue_pending = pending_count_.load(std::memory_order_relaxed);
    s.peak_queue_pending = peak_pending_count_.load(std::memory_order_relaxed);
    s.submitted_total = submitted_total_.load(std::memory_order_relaxed);
    s.rejected_total = rejected_total_.load(std::memory_order_relaxed);
    s.completed_total = completed_total_.load(std::memory_order_relaxed);
    s.task_exception_total = task_exception_total_.load(std::memory_order_relaxed);
    s.task_exec_us_total = task_exec_us_total_.load(std::memory_order_relaxed);
    s.peak_task_exec_us = peak_task_exec_us_.load(std::memory_order_relaxed);
    return s;
}

std::size_t ThreadPool::worker_count() const {
    return workers_.size();
}

std::size_t ThreadPool::pending_tasks() const {
    // 无锁读取：指标接口可能被采集器高频轮询，避免和 submit/worker 抢 mutex_。
    return pending_count_.load(std::memory_order_relaxed);
}

std::size_t ThreadPool::peak_pending_tasks() const {
    return peak_pending_count_.load(std::memory_order_relaxed);
}

}  // namespace cpppools::pool
