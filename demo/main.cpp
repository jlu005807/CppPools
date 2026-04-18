#include "thread_pool/thread_pool.h"

#include <chrono>
#include <iostream>
#include <vector>

int main() {
    // 创建 4 个工作线程的线程池。
    // 这个 demo 只关注“任务提交 + future 取结果”这条最小闭环。
    ThreadPool pool(4);

    std::vector<std::future<int>> futures;
    futures.reserve(20);

    // 提交 20 个计算任务：每个任务返回 i*i。
    // sleep 10ms 用于模拟任务耗时，便于观察并发执行效果。
    for (int i = 1; i <= 20; ++i) {
        futures.emplace_back(pool.submit([i]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return i * i;
        }));
    }

    // 通过 future::get() 收集结果。
    // 若任务内部抛异常，也会在这里重新抛出，便于统一处理。
    long long sum = 0;
    for (auto& fut : futures) {
        sum += fut.get();
    }

    std::cout << "sum(1^2..20^2) = " << sum << '\n';
    std::cout << "expected = 2870" << '\n';
    return 0;
}
