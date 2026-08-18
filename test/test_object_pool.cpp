#include "cpppools/pool/object_pool.h"

#include <atomic>
#include <cassert>
#include <thread>
#include <vector>
#include <string>
#include <type_traits>

using namespace cpppools::pool;

struct Dummy {
    int x{0};
    std::string name;
};

// 带 reset() 的类型：用于验证 ObjectPool 优先调用 reset() 而不是重建赋值。
//
// 为何重要：重建赋值（*obj = T()）会构造临时对象再移动赋值，
// 原对象内 string/vector 已分配的 capacity 会被丢弃 —— 那池化就流于形式了。
struct Resettable {
    int value{0};
    std::string buffer;
    // 静态计数：记录 reset() 被调用的次数，供断言使用。
    static int reset_calls;

    void reset() {
        value = 0;
        buffer.clear();  // clear 不释放 capacity
        ++reset_calls;
    }
};
int Resettable::reset_calls = 0;

// reset() 会抛异常的类型：用于验证 acquire 的异常安全窗口修复。
//
// reset_calls 静态计数是关键：没有它，本测试会在**无修复的实现下假绿**——
// 旧代码里异常在 Ptr 构造前穿出，计数“碰巧”全对，而对象其实泄漏了。
// 只有能数出“泄漏对象是否还躺在 free_list 里”，才分得清销毁与泄漏。
struct ThrowingReset {
    int value{0};
    static int reset_calls;
    // 只让**第一次** reset 抛：之后池里任何对象都能正常重置。
    // 不能“永远抛”——否则修复版销毁旧对象后扩容出的新对象，
    // 在第二次 acquire 的 reset 里又会抛，测试在 try 外面根本走不到断言。
    static bool next_reset_throws;
    // 析构计数：没有它，“回滚了计数但忘了 delete”的实现会让测试假绿——
    // 计数全对、第二次 acquire 也走扩容，唯独对象静默泄漏。
    static int dtor_calls;
    ~ThrowingReset() { ++dtor_calls; }
    void reset() {
        ++reset_calls;
        if (next_reset_throws) {
            next_reset_throws = false;
            throw std::runtime_error("reset boom");
        }
    }
};
int ThrowingReset::reset_calls = 0;
bool ThrowingReset::next_reset_throws = false;
int ThrowingReset::dtor_calls = 0;

// ---------------------------------------------------------------------------
// 构造契约
// ---------------------------------------------------------------------------

static void test_constructor_rejects_invalid_arguments() {
    bool threw = false;
    try {
        ObjectPool<Dummy> bad("p", 0, 4);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw && "init_size == 0 必须抛 invalid_argument");

    threw = false;
    try {
        ObjectPool<Dummy> bad2("p", 4, 0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw && "max_size == 0 必须抛 invalid_argument");

    threw = false;
    try {
        ObjectPool<Dummy> bad3("p", 8, 4);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw && "max_size < init_size 必须抛 invalid_argument");
}

// ---------------------------------------------------------------------------
// 有界 + 立即失败（spec §5.1）
// ---------------------------------------------------------------------------

static void test_acquire_returns_empty_at_max_size() {
    // 立即失败、不阻塞：ObjectPool 的失败路径在 max_size >= worker_count 时
    // 本不可达，但**调用方契约是必须判空** —— 这个不变式由运行期配置维持，
    // 不是类型保证（见 spec §5.1.1）。因此池必须真的返回空 Ptr。
    ObjectPool<Dummy> pool("cap", 1, 2);

    auto a = pool.acquire();
    auto b = pool.acquire();
    assert(a && b);
    assert(pool.total_size() == 2 && pool.in_use() == 2);

    auto c = pool.acquire();
    assert(!c && "达到 max_size 且无空闲时必须返回空 Ptr");
    assert(pool.total_size() == 2 && "不得再扩容");

    // acquire 计数：3 次调用、1 次失败。
    const AcquireStats st = pool.stats();
    assert(st.acquire_total == 3);
    assert(st.acquire_failed_total == 1);
}

// ---------------------------------------------------------------------------
// 异常安全窗口修复（spec §5.3）
// ---------------------------------------------------------------------------

static void test_reset_exception_destroys_object_and_rolls_back() {
    ThrowingReset::reset_calls = 0;
    ThrowingReset::dtor_calls = 0;
    ThrowingReset::next_reset_throws = true;  // 只让第一次 reset 抛
    ObjectPool<ThrowingReset> pool("throw", 1, 1);  // max_size=1，见末尾的区分逻辑
    assert(pool.total_size() == 1);

    bool threw = false;
    try {
        (void)pool.acquire();  // 借出初始对象，reset 抛出
    } catch (const std::runtime_error& ex) {
        threw = true;
        assert(std::string(ex.what()) == "reset boom");
    }
    assert(threw && "reset() 的异常必须传播给调用方");
    assert(ThrowingReset::reset_calls == 1);

    // 修复点 1：in_use_ 必须回滚，否则析构告警会记到无关调用者头上。
    assert(pool.in_use() == 0 && "抛异常后 in_use 必须回滚");
    // 修复点 2：半重置的对象必须被**销毁**而不是放回 free_list。
    // 放回会把“部分赋值的不确定状态”交给下一个调用方（见 spec §5.3）。
    assert(pool.total_size() == 0 && "抛异常的对象必须销毁并从池总数中扣除");
    assert(pool.available() == 0 && "不得放回 free_list");
    // 对象真的被 delete 了（而非“只回滚计数、忘了销毁”的静默泄漏）。
    // 初始 1 个 + 未扩容，此处必为 1。
    assert(ThrowingReset::dtor_calls == 1 && "抛异常的对象必须被析构");

    // 此时三种错误实现会走向三种不同结局（next_reset_throws 已为 false）：
    //   a) 对象被放回 free_list → 借到它，reset 成功，reset_calls == 3；
    //   b) 对象泄漏（旧 bug：既没销毁也没放回）→ total_size 仍为 1，
    //      max_size=1 堵死扩容 → 第二次 acquire 返回**空 Ptr**；
    //   c) 正确销毁 → total_size 为 0 → 扩容出全新对象，reset 成功，
    //      reset_calls == 2，且新对象是干净状态。
    auto obj = pool.acquire();
    assert(obj && "若为空 Ptr，说明旧对象只是泄漏了（未销毁、未归还）");
    assert(ThrowingReset::reset_calls == 2 &&
           "若为 3，说明抛异常的对象被放回了 free_list 又被借出");
    assert(obj->value == 0 && "扩容出的必须是全新构造的对象");
    assert(pool.total_size() == 1);

    // Strengthened: reset exception must not inflate acquire_failed_total.
    // It is a task exception, not a pool rejection.
    const AcquireStats stats = pool.stats();
    assert(stats.acquire_total == 2 && "two acquire calls were made");
    assert(stats.acquire_failed_total == 0 && "reset exception must not count as pool rejection");
    assert(stats.peak_in_use == 1 && "peak should reflect the successful second acquire");
}

// ---------------------------------------------------------------------------
// 统计（spec §5.4：一次锁取一致快照；旧 getter 保留为薄封装）
// ---------------------------------------------------------------------------

static void test_stats_reports_consistent_snapshot() {
    ObjectPool<Dummy> pool("snap", 2, 4);

    {
        const AcquireStats s = pool.stats();
        assert(s.name == "snap");
        assert(s.capacity == 4);
        assert(s.size == 2 && "初始即已创建 init_size 个对象");
        assert(s.in_use == 0);
        assert(s.available == 2);
        assert(s.peak_in_use == 0);
    }

    {
        auto a = pool.acquire();
        auto b = pool.acquire();
        auto c = pool.acquire();  // 扩容一个
        const AcquireStats s = pool.stats();
        assert(s.size == 3);
        assert(s.in_use == 3);
        assert(s.available == 0);
        assert(s.peak_in_use == 3);
        // 等式不变式：in_use + available == size。
        // 这正是 stats() 取一次锁（而非各字段独立读）要保护的东西。
        assert(s.in_use + s.available == s.size);
    }

    const AcquireStats after = pool.stats();
    assert(after.in_use == 0);
    assert(after.available == 3);
    assert(after.peak_in_use == 3 && "历史峰值保留");
}

// ---------------------------------------------------------------------------
// 以下为自 test_pools.cpp 迁移的既有用例（仅构造签名更新）。
// ---------------------------------------------------------------------------

static void test_object_pool_basic() {
    ObjectPool<Dummy> pool("basic", 2, 8);
    auto a = pool.acquire();
    auto b = pool.acquire();
    assert(pool.in_use() == 2);
    assert(pool.total_size() >= 2);

    a->x = 7;
    b->name = "ok";
}

static void test_object_pool_raii_return() {
    ObjectPool<Dummy> pool("raii", 1, 4);
    {
        auto obj = pool.acquire();
        obj->x = 42;
        assert(pool.in_use() == 1);
        assert(pool.available() == 0);
    }
    // obj goes out of scope and should automatically return to pool.
    assert(pool.in_use() == 0);
    assert(pool.available() == 1);
}

static void test_object_pool_prefers_reset_over_reassignment() {
    Resettable::reset_calls = 0;
    ObjectPool<Resettable> pool("reset", 1, 4);

    std::size_t capacity_after_first = 0;
    {
        auto obj = pool.acquire();
        // 填入足够长的内容，确保发生了堆分配（越过 SSO 阈值）。
        obj->buffer.assign(512, 'x');
        obj->value = 42;
        capacity_after_first = obj->buffer.capacity();
        assert(capacity_after_first >= 512);
    }

    {
        auto obj = pool.acquire();
        // 字段已被重置。
        assert(obj->value == 0);
        assert(obj->buffer.empty());
        // 关键断言：capacity 必须被保留，否则池化没有意义。
        assert(obj->buffer.capacity() >= capacity_after_first);
    }

    // reset() 确实被调用过（而不是走重建赋值分支）。
    assert(Resettable::reset_calls >= 2);
}

static void test_object_pool_without_reset_still_works() {
    // Dummy 没有 reset()，应回退到重建赋值，行为与以前一致。
    ObjectPool<Dummy> pool("noreset", 1, 4);
    {
        auto obj = pool.acquire();
        obj->x = 7;
        obj->name = "dirty";
    }
    auto obj = pool.acquire();
    assert(obj->x == 0);
    assert(obj->name.empty());
}

static void test_object_pool_tracks_peak_in_use() {
    ObjectPool<Dummy> pool("peak", 4, 8);
    assert(pool.peak_in_use() == 0);
    {
        auto a = pool.acquire();
        auto b = pool.acquire();
        auto c = pool.acquire();
        assert(pool.in_use() == 3);
        assert(pool.peak_in_use() == 3);
    }
    // 归还后当前值回落，但历史峰值保留。
    assert(pool.in_use() == 0);
    assert(pool.peak_in_use() == 3);
}

// 并发借还下的不变式守护（整体评审 I-4：三池此前零并发测试）。
// 8 线程各自 acquire/release 多轮；任意时刻 stats() 快照必须满足
// in_use + available == size 与 in_use <= capacity。
static void test_concurrent_acquire_release_keeps_invariants() {
    ObjectPool<Dummy> pool("conc", 4, 8);
    constexpr int kThreads = 8;
    constexpr int kRounds = 500;

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([&pool] {
            for (int r = 0; r < kRounds; ++r) {
                auto obj = pool.acquire();
                if (obj) {
                    obj->x = r;  // 触碰一下，确保不是空转
                }
                // obj 出作用域自动归还
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }

    const AcquireStats s = pool.stats();
    assert(s.in_use == 0);
    assert(s.in_use + s.available == s.size);
    assert(s.size <= s.capacity);
    assert(s.acquire_total == static_cast<std::uint64_t>(kThreads) * kRounds);
    assert(s.peak_in_use >= 1);
}

int main() {
    test_constructor_rejects_invalid_arguments();
    test_acquire_returns_empty_at_max_size();
    test_reset_exception_destroys_object_and_rolls_back();
    test_stats_reports_consistent_snapshot();
    test_object_pool_basic();
    test_object_pool_raii_return();
    test_object_pool_prefers_reset_over_reassignment();
    test_object_pool_without_reset_still_works();
    test_object_pool_tracks_peak_in_use();
    test_concurrent_acquire_release_keeps_invariants();
    return 0;
}
