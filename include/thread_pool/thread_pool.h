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

// ThreadPool: a fixed-size worker pool for async task execution.
//
// Design goals:
// 1) Reuse worker threads to avoid frequent thread creation/destruction.
// 2) Provide a generic submit(...) API with future-based result retrieval.
// 3) Shutdown gracefully: finish queued tasks, then join all workers.
class ThreadPool {
public:
    explicit ThreadPool(std::size_t thread_count);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template <class F, class... Args>
    // submit(...) pushes a task into queue and returns future for result.
    // Throws std::runtime_error when pool is already shutting down.
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>;

    // Stop accepting new tasks and wait for all workers to exit.
    void shutdown();

    // Number of worker threads currently owned by the pool.
    std::size_t worker_count() const;

    // Approximate count of tasks currently waiting in queue.
    std::size_t pending_tasks() const;

private:
    // Core routine run by every worker thread.
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

    // packaged_task bridges a callable and a future:
    // worker executes packaged_task, caller waits on future.
    auto task = std::make_shared<std::packaged_task<ReturnType()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...));

    std::future<ReturnType> fut = task->get_future();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) {
            throw std::runtime_error("submit on stopped ThreadPool");
        }
        // Store as type-erased void() task, keeping interface simple.
        tasks_.emplace([task]() { (*task)(); });
    }
    // Wake one sleeping worker to consume the new task.
    cv_.notify_one();
    return fut;
}
