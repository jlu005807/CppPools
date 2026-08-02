#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// ThreadPool：固定大小的工作线程池，用于异步执行任务。
//
// 在三级池化协作链路中的位置：
//   HTTP 请求 -> [ThreadPool 调度] -> ObjectPool 复用上下文 -> DBConnectionPool 查询
// 也就是 HTTP 请求进入服务后第一时间被丢到这里，让 accept 主循环立即回到等待新连接。
//
// 设计目标：
// 1) 线程复用：避免高频创建/销毁线程带来的系统开销。
// 2) 通用提交接口：支持任意可调用对象，并通过 future 获取结果。
// 3) 优雅关闭：停止接收新任务，但保证队列中已有任务执行完成后再退出。
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
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template <class F, class... Args>
    // submit(...)：向队列提交任务，并返回 future 用于等待结果。
    // 当线程池已经进入关闭流程时，抛出 std::runtime_error。
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>;

    // 停止接收新任务，并等待所有工作线程退出。
    void shutdown();

    // 当前线程池持有的工作线程数量。
    std::size_t worker_count() const;

    // 当前任务队列中的待执行任务数量（近似快照）。
    // 实现要点：用 std::atomic 取代加锁读取 tasks_.size()。
    // 这样 /api/stats 这种"读多写少"的查询可以完全无锁，
    // 避免和正在 submit/工作线程取任务的临界区争夺 mutex_。
    std::size_t pending_tasks() const;

private:
    // 每个工作线程的核心循环逻辑。
    void worker_loop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_{false};

    // 与 tasks_ 同步维护的待处理任务计数。
    // 入队时 +1、出队时 -1，更新都发生在 mutex_ 的临界区内，
    // 因此 atomic 这里只是为了让"读"侧无锁，不是用来代替互斥。
    std::atomic<std::size_t> pending_count_{0};
};

template <class F, class... Args>
auto ThreadPool::submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
    using ReturnType = std::invoke_result_t<F, Args...>;

    // packaged_task 用于把“可调用对象”和 future 绑定起来：
    // 工作线程执行 packaged_task，调用方通过 future 获取结果/异常。
    auto task = std::make_shared<std::packaged_task<ReturnType()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...));

    std::future<ReturnType> fut = task->get_future();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) {
            throw std::runtime_error("submit on stopped ThreadPool");
        }
        // 统一存储为 void() 任务，简化队列类型与调度逻辑。
        tasks_.emplace([task]() { (*task)(); });
        // 队列长度的递增必须发生在临界区内：
        // 否则 worker_loop 端有可能在我们 ++ 之前先取出该任务并 --，导致计数短暂为负。
        pending_count_.fetch_add(1, std::memory_order_relaxed);
    }
    // 唤醒一个休眠线程来消费新任务。
    cv_.notify_one();
    return fut;
}
