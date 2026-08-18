#include "db/statement_cache.h"

#include <cassert>
#include <string>
#include <stdexcept>

using cpppools::db::StatementCache;

// statement 用 int 模拟（值即“编译代价”）：make 返回递增的“句柄号”，
// 用于断言“命中返回的是同一个 statement”。

static void test_get_or_create_caches_and_hits() {
    StatementCache<int> cache(4);
    int next_handle = 1;
    auto make = [&next_handle] { return next_handle++; };

    int& a1 = cache.get_or_create("SELECT 1", make);
    assert(a1 == 1);
    assert(cache.misses() == 1);
    assert(cache.hits() == 0);

    // 同一 key：命中同一个 statement，make 不再被调用。
    int& a2 = cache.get_or_create("SELECT 1", make);
    assert(a2 == 1);
    assert(&a1 == &a2);
    assert(cache.hits() == 1);
    assert(cache.misses() == 1);
    assert(next_handle == 2 && "命中不得重新创建");

    // 不同 key：新建。
    int& b = cache.get_or_create("SELECT 2", make);
    assert(b == 2);
    assert(cache.misses() == 2);
    assert(cache.size() == 2);
}

static void test_capacity_triggers_eviction() {
    StatementCache<int> cache(2);
    int next_handle = 1;
    auto make = [&next_handle] { return next_handle++; };

    cache.get_or_create("k1", make);
    cache.get_or_create("k2", make);
    assert(cache.size() == 2);

    // 第三个 key 触发整体清空后重建：缓存里只剩新 entry。
    cache.get_or_create("k3", make);
    assert(cache.size() == 1);
    assert(cache.misses() == 3);

    // 被淘汰的 key 再取会重新创建（miss）。
    cache.get_or_create("k1", make);
    assert(cache.misses() == 4);
}

static void test_hit_does_not_recreate() {
    // 容量 2：k1, k2 入缓存；再摸一次 k1（命中）。
    // 整体清空策略下 LRU 顺序外部不可观测，这里守护的是
    // 命中不重新创建（hits/misses 计数正确）。
    StatementCache<int> cache(2);
    int next_handle = 1;
    auto make = [&next_handle] { return next_handle++; };

    cache.get_or_create("k1", make);
    cache.get_or_create("k2", make);
    cache.get_or_create("k1", make);  // 命中
    assert(cache.hits() == 1);
    assert(cache.misses() == 2);
}


static void test_factory_throw_does_not_leave_lru_or_capacity_corrupt() {
    StatementCache<int> cache(2);
    bool fail = true;
    try {
        cache.get_or_create("bad", [&]() -> int {
            if (fail) throw std::runtime_error("prepare failed");
            return 7;
        });
        assert(false && "factory must throw");
    } catch (const std::runtime_error&) {
    }
    assert(cache.size() == 0);
    fail = false;
    assert(cache.get_or_create("good1", [] { return 9; }) == 9);
    assert(cache.get_or_create("good2", [] { return 10; }) == 10);
    assert(cache.size() == 2);

    try {
        cache.get_or_create("bad-full", []() -> int {
            throw std::runtime_error("prepare failed while full");
        });
        assert(false && "factory must throw");
    } catch (const std::runtime_error&) {
    }
    assert(cache.size() == 2);
    assert(cache.get_or_create("good1", [] { return -1; }) == 9);
    assert(cache.get_or_create("good2", [] { return -1; }) == 10);
}


struct DtorCounter {
    int value;
    static int alive;
    DtorCounter(int v) : value(v) { ++alive; }
    DtorCounter(const DtorCounter& o) : value(o.value) { ++alive; }
    DtorCounter& operator=(const DtorCounter&) = delete;
    ~DtorCounter() { --alive; }
};
int DtorCounter::alive = 0;

static void test_clear_destroys_all_entries() {
    DtorCounter::alive = 0;
    {
        StatementCache<DtorCounter> cache(4);
        cache.get_or_create("a", [] { return DtorCounter(1); });
        cache.get_or_create("b", [] { return DtorCounter(2); });
        cache.get_or_create("c", [] { return DtorCounter(3); });
        assert(cache.size() == 3);
        assert(DtorCounter::alive == 3);
        cache.clear();
        assert(cache.size() == 0);
        assert(DtorCounter::alive == 0);
    }
    assert(DtorCounter::alive == 0);
}

int main() {
    test_get_or_create_caches_and_hits();
    test_capacity_triggers_eviction();
    test_hit_does_not_recreate();
    test_factory_throw_does_not_leave_lru_or_capacity_corrupt();
    test_clear_destroys_all_entries();
    return 0;
}
