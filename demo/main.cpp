#include "thread_pool/thread_pool.h"

#include <chrono>
#include <iostream>
#include <vector>

int main() {
    ThreadPool pool(4);

    std::vector<std::future<int>> futures;
    futures.reserve(20);

    for (int i = 1; i <= 20; ++i) {
        futures.emplace_back(pool.submit([i]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return i * i;
        }));
    }

    long long sum = 0;
    for (auto& fut : futures) {
        sum += fut.get();
    }

    std::cout << "sum(1^2..20^2) = " << sum << '\n';
    std::cout << "expected = 2870" << '\n';
    return 0;
}

