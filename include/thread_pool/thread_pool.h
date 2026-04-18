#pragma once

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
// 设计目标：
// 1) 线程复用：避免高频创建/销毁线程带来的系统开销。
// 2) 通用提交接口：支持任意可调用对象，并通过 future 获取结果。
// 3) 优雅关闭：停止接收新任务，但保证队列中已有任务执行完成后再退出。
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
    std::size_t pending_tasks() const;

private:
    // 每个工作线程的核心循环逻辑。
    void worker_loop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_{false};
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
    }
    // 唤醒一个休眠线程来消费新任务。
    cv_.notify_one();
    return fut;
}
