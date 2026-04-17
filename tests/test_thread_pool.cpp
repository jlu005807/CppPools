#include "thread_pool/thread_pool.h"

#include <cassert>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

static void test_basic_submit() {
    ThreadPool pool(2);
    auto f1 = pool.submit([] { return 1 + 2; });
    auto f2 = pool.submit([](int x) { return x * 3; }, 7);

    assert(f1.get() == 3);
    assert(f2.get() == 21);
}

static void test_many_tasks() {
    ThreadPool pool(4);
    assert(pool.worker_count() == 4);

    std::vector<std::future<int>> futures;
    futures.reserve(200);

    for (int i = 0; i < 200; ++i) {
        futures.emplace_back(pool.submit([i] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return i;
        }));
    }

    long long total = 0;
    for (auto& fut : futures) {
        total += fut.get();
    }
    assert(total == (199LL * 200LL) / 2LL);

    // After all futures are ready, queue should drain to empty.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(pool.pending_tasks() == 0);
}

static void test_submit_after_shutdown_should_throw() {
    ThreadPool pool(2);
    pool.shutdown();

    bool thrown = false;
    try {
        (void)pool.submit([] { return 42; });
    } catch (const std::runtime_error&) {
        thrown = true;
    }
    assert(thrown);
}

int main() {
    test_basic_submit();
    test_many_tasks();
    test_submit_after_shutdown_should_throw();
    return 0;
}
