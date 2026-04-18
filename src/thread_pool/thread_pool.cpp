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
        }
        // 在锁外执行任务，减少临界区时长，提升并发吞吐。
        task();
    }
}

std::size_t ThreadPool::worker_count() const {
    return workers_.size();
}

std::size_t ThreadPool::pending_tasks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
}
