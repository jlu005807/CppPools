#include "thread_pool/thread_pool.h"

ThreadPool::ThreadPool(std::size_t thread_count) {
    if (thread_count == 0) {
        throw std::invalid_argument("thread_count must be > 0");
    }

    // 启动时预先创建好工作线程，后续 submit() 只负责入队任务即可。
    workers_.reserve(thread_count);
    for (std::size_t i = 0; i < thread_count; ++i) {
        workers_.emplace_back([this]() { worker_loop(); });
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) {
            return;
        }
        // stop_ 只阻止后续 submit()，不会丢弃队列里已存在的任务。
        stop_ = true;
    }

    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::worker_loop() {
    while (true) {
        std::function<void()> task;
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
        task();
    }
}

std::size_t ThreadPool::worker_count() const {
    return workers_.size();
}

std::size_t ThreadPool::pending_tasks() const {
    // 无锁读取：stats 接口可能被前端高频轮询，避免和 submit/worker 抢 mutex_。
    return pending_count_.load(std::memory_order_relaxed);
}
