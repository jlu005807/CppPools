#include "cpppools/db/db_connection_pool.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#endif

using namespace cpppools::db;
using namespace std::chrono_literals;

// 可控 healthy() 的假 Database：池的全部健康检查逻辑都靠它验证。
//
// 为何必须让 healthy() 可注入：真实后端（SQLite 的 SELECT 1、MySQL 的 ping）
// 都无法制造"探活失败"场景，只有虚函数注入能控制这条路径。
class FakeDatabase : public Database {
public:
    FakeDatabase() : Database(Schema{}, "fake") {}

    bool healthy() override {
        ++probe_calls;
        return healthy_flag.load();
    }

    std::string backend_type() const override { return "fake"; }

    static std::atomic<bool> healthy_flag;
    static std::atomic<int> probe_calls;

protected:
    DbResult<ResultSet> do_select(const SelectRequest&) override {
        return {DbStatus::kOk, ResultSet{}, ""};
    }
    DbResult<InsertResult> do_insert(const InsertRequest&) override {
        return {DbStatus::kOk, InsertResult{}, ""};
    }
    DbResult<RowCount> do_update(const UpdateRequest&) override {
        return {DbStatus::kOk, 0, ""};
    }
    DbResult<RowCount> do_remove(const DeleteRequest&) override {
        return {DbStatus::kOk, 0, ""};
    }
};
std::atomic<bool> FakeDatabase::healthy_flag{true};
std::atomic<int> FakeDatabase::probe_calls{0};

// 重建时的人为延迟：让每次重建尝试消耗可测量的时间，
// deadline 测试靠它区分「绝对 deadline」与「每次重试重新计满」。
static std::atomic<int> g_rebuild_delay_ms{0};

static void reset_fake_state() {
    FakeDatabase::healthy_flag.store(true);
    FakeDatabase::probe_calls.store(0);
    g_rebuild_delay_ms.store(0);
}

static ConnectionFactory make_fake_factory(std::atomic<int>* created = nullptr) {
    return [created](std::size_t) -> std::unique_ptr<Database> {
        if (created != nullptr) {
            created->fetch_add(1);
        }
        return std::make_unique<FakeDatabase>();
    };
}

// ---------------------------------------------------------------------------
// 构造契约与 fail-fast
// ---------------------------------------------------------------------------

static void test_constructor_rejects_invalid_arguments() {
    bool threw = false;
    try {
        DbConnectionPool bad("p", make_fake_factory(), 0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw && "pool_size == 0 必须抛 invalid_argument");

    threw = false;
    try {
        DbConnectionPool bad2("p", nullptr, 2);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw && "空 factory 必须抛 invalid_argument");
}

static void test_construction_fails_fast_when_factory_fails() {
    // 任一槽位创建失败 → 整个池拒绝启动（fail-fast，已是现状，补回归测试）。
    // 理由：一个启动了但 0 条可用连接、对所有请求回 500 的池，
    // 比一个拒绝启动并报错的池更糟。
    auto failing = [](std::size_t) -> std::unique_ptr<Database> { return nullptr; };
    bool threw = false;
    try {
        DbConnectionPool pool("p", failing, 2);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw && "factory 返回 nullptr 时构造必须失败");

    // factory 抛异常同样必须穿出构造函数。
    auto throwing = [](std::size_t) -> std::unique_ptr<Database> {
        throw std::runtime_error("factory boom");
    };
    threw = false;
    try {
        DbConnectionPool pool("p", throwing, 2);
    } catch (const std::runtime_error& ex) {
        threw = std::string(ex.what()) == "factory boom";
    }
    assert(threw && "factory 抛出的异常必须穿出构造函数");
}

// ---------------------------------------------------------------------------
// 借还基本语义
// ---------------------------------------------------------------------------

static void test_basic_acquire_release() {
    DbConnectionPool pool("basic", make_fake_factory(), 2);
    auto* a = pool.acquire(10ms);
    auto* b = pool.acquire(10ms);
    assert(a != nullptr && b != nullptr);
    assert(a != b);
    assert(pool.free_count() == 0);

    pool.release(a);
    pool.release(b);
    assert(pool.free_count() == 2);
}

static void test_acquire_times_out_when_exhausted() {
    DbConnectionPool pool("timeout", make_fake_factory(), 1);
    auto* held = pool.acquire(10ms);
    assert(held != nullptr);

    // 池已耗尽：第二次 acquire 必须超时返回 nullptr，不得无限阻塞。
    auto* timed_out = pool.acquire(30ms);
    assert(timed_out == nullptr);

    // acquire 计数：2 次调用、1 次超时失败。
    const DbPoolStats st = pool.stats();
    assert(st.base.acquire_total == 2);
    assert(st.base.acquire_failed_total == 1);

    pool.release(held);
    assert(pool.free_count() == 1);
}

static void test_guard_auto_release_and_move_semantics() {
    DbConnectionPool pool("guard", make_fake_factory(), 1);
    {
        DbConnGuard guard(pool, 10ms);
        assert(guard.valid());
        assert(pool.free_count() == 0);
    }
    assert(pool.free_count() == 1 && "guard 析构必须归还连接");

    // 移动构造：支持从函数返回 / 放入容器。
    // 移动后源 guard 不得双重归还。
    {
        DbConnGuard src(pool, 10ms);
        assert(src.valid());
        DbConnGuard dst(std::move(src));
        assert(dst.valid());
        assert(src.get() == nullptr && "被移走的 guard 必须置空");
    }
    assert(pool.free_count() == 1 && "移动后只能归还一次");

    // 移动赋值：先归还自己持有的，再接管对方的。
    {
        DbConnGuard g1(pool, 10ms);
        DbConnGuard g2(pool, 10ms);  // 池只有 1 槽，g2 拿不到
        assert(g1.valid() && !g2.valid());
        g2 = std::move(g1);
        assert(g2.valid() && !g1.valid());
        assert(pool.free_count() == 0 && "赋值不得造成额外归还");
    }
    assert(pool.free_count() == 1);
}

// ---------------------------------------------------------------------------
// 非法归还可观测
// ---------------------------------------------------------------------------

static void test_invalid_release_is_counted_and_null_is_not() {
    DbConnectionPool pool("invalid", make_fake_factory(), 1);
    auto* conn = pool.acquire(10ms);
    assert(conn != nullptr);

    pool.release(conn);
    assert(pool.invalid_release_total() == 0 && "合法归还不得计数");

    pool.release(conn);  // 重复归还
    assert(pool.invalid_release_total() == 1 && "重复归还必须计数");

    int external = 0;
    pool.release(reinterpret_cast<Database*>(&external));  // 野指针归还
    assert(pool.invalid_release_total() == 2 && "野指针归还必须计数");

    pool.release(nullptr);  // guard 拿不到连接时的合法路径
    assert(pool.invalid_release_total() == 2 && "空指针归还是合法路径，不得计数");

    assert(pool.free_count() == 1 && "非法归还不得污染池状态");
}

// ---------------------------------------------------------------------------
// 健康检查：仅空闲超阈值时探活（HikariCP 风格）
// ---------------------------------------------------------------------------

static void test_hot_connection_skips_probe() {
    // threshold=1h：刚归还的连接是"热"的，acquire 不得探活（零开销）。
    reset_fake_state();
    DbConnectionPool pool("hot", make_fake_factory(), 1, 1h);
    auto* c1 = pool.acquire(10ms);
    pool.release(c1);

    auto* c2 = pool.acquire(10ms);
    assert(c2 == c1 && "热连接应直接复用");
    assert(FakeDatabase::probe_calls.load() == 0 && "热连接不得触发探活");
    pool.release(c2);
}

static void test_cold_connection_is_probed_and_reused_when_healthy() {
    // threshold=0：每次 acquire 都探活。健康则直接复用，不重建。
    reset_fake_state();
    std::atomic<int> created{0};
    DbConnectionPool pool("cold", make_fake_factory(&created), 1, 0ms);
    auto* c1 = pool.acquire(10ms);
    // threshold=0 意味着连第一次借出都探活（刚构造的连接也不例外）。
    assert(FakeDatabase::probe_calls.load() == 1);
    pool.release(c1);

    FakeDatabase::probe_calls.store(0);
    auto* c2 = pool.acquire(10ms);
    assert(FakeDatabase::probe_calls.load() == 1 && "冷连接必须探活");
    // 用 factory 计数而非指针相等判断"未重建"：
    // allocator 可能把刚归还的地址再发出来，指针相等不是可靠信号。
    assert(created.load() == 1 && "探活通过后不得重建");
    pool.release(c2);
}

static void test_unhealthy_connection_is_rebuilt() {
    // 探活失败 → 销毁旧连接 → factory 重建 → 返回新连接。
    reset_fake_state();
    std::atomic<int> created{0};
    DbConnectionPool pool("rebuild", make_fake_factory(&created), 1, 0ms);
    assert(created.load() == 1);

    auto* c1 = pool.acquire(10ms);  // threshold=0：首借也探活（且通过）
    pool.release(c1);

    FakeDatabase::probe_calls.store(0);  // 重置基线，只数下面这次
    FakeDatabase::healthy_flag.store(false);
    auto* c2 = pool.acquire(10ms);
    assert(FakeDatabase::probe_calls.load() == 1);
    assert(c2 != nullptr && "重建必须成功返回新连接");
    // 重建的证据是 factory 计数 +1，而不是 c2 != c1：
    // delete 之后 allocator 完全可能把同一地址再分配出来。
    assert(created.load() == 2 && "重建必须调用 factory");
    assert(pool.rebuild_total() == 1);
    pool.release(c2);
}

static void test_rebuild_failure_preserves_slot_and_self_heals() {
    // 重建失败 → 保留槽位（conn=nullptr 的空槽回到空闲队列），
    // 下次 acquire 再试 → factory 恢复后自愈。
    //
    // 这是批 3 最绕的一条：若槽位不保留，free 队列永久少一个，
    // 全部槽位失败后 acquire 永远超时，再也不会触发重建尝试。
    reset_fake_state();
    std::atomic<int> created{0};
    std::atomic<bool> factory_ok{true};
    auto flaky = [&](std::size_t) -> std::unique_ptr<Database> {
        if (!factory_ok.load()) {
            return nullptr;
        }
        created.fetch_add(1);
        return std::make_unique<FakeDatabase>();
    };

    DbConnectionPool pool("heal", flaky, 1, 0ms);
    assert(created.load() == 1);

    auto* c1 = pool.acquire(10ms);  // threshold=0：首借也探活（且通过）
    pool.release(c1);

    // 第一次重建：探活失败 + factory 失败 → 空槽保留 → 超时返回 nullptr。
    FakeDatabase::probe_calls.store(0);  // 重置基线
    FakeDatabase::healthy_flag.store(false);
    factory_ok.store(false);
    auto* none = pool.acquire(50ms);
    assert(none == nullptr && "重建失败时必须超时返回 nullptr");
    assert(pool.rebuild_failed_total() == 1);

    // factory 恢复。下一次 acquire 应借到空槽并重建成功 —— 自愈。
    factory_ok.store(true);
    auto* c2 = pool.acquire(50ms);
    assert(c2 != nullptr && "槽位保留后，factory 恢复时必须自愈");
    assert(created.load() == 2);
    assert(pool.rebuild_total() == 1 && "成功的重建计 rebuild_total");
    pool.release(c2);
    assert(pool.free_count() == 1);
}

static void test_timeout_is_total_bound_including_probe_and_rebuild() {
    // acquire(timeout) 的 timeout 是**含探活与重建**的总上界（绝对 deadline）。
    //
    // 正确实现（绝对 deadline）：t=0 重建尝试耗 60ms 失败，空槽已 tried，
    //   之后无事可做，等到 deadline 返回 —— 总耗时 ≈ timeout（200ms）。
    // bug 实现（每次重试重新 wait_for(timeout)）：尝试 60ms 后重新等满 200ms
    //   → 总 ~260ms。断言 < 240ms 即可区分两者。
    reset_fake_state();
    std::atomic<bool> factory_ok{true};
    auto flaky = [&](std::size_t) -> std::unique_ptr<Database> {
        const int delay = g_rebuild_delay_ms.load();
        if (delay > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
        if (!factory_ok.load()) {
            return nullptr;
        }
        return std::make_unique<FakeDatabase>();
    };
    DbConnectionPool pool("deadline", flaky, 1, 0ms);

    auto* a = pool.acquire(10ms);
    pool.release(a);
    assert(pool.free_count() == 1);

    // 让探活必失败、重建必延迟 60ms 后失败。
    FakeDatabase::healthy_flag.store(false);
    factory_ok.store(false);
    g_rebuild_delay_ms.store(60);

    const auto start = std::chrono::steady_clock::now();
    auto* none = pool.acquire(200ms);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();

    assert(none == nullptr);
    assert(elapsed >= 190 && "必须真的等到 deadline，不得提前放弃");
    assert(elapsed < 240 && "timeout 必须是绝对 deadline，不得每次重试重新计满");
}

// ---------------------------------------------------------------------------
// 统计与旧 getter 兼容
// ---------------------------------------------------------------------------

static void test_stats_reports_consistent_snapshot() {
    DbConnectionPool pool("stats", make_fake_factory(), 3);
    {
        const DbPoolStats s = pool.stats();
        assert(s.base.name == "stats");
        assert(s.base.capacity == 3);
        assert(s.base.size == 3);
        assert(s.base.in_use == 0);
        assert(s.base.available == 3);
        assert(s.invalid_release_total == 0);
    }
    auto* a = pool.acquire(10ms);
    auto* b = pool.acquire(10ms);
    {
        const DbPoolStats s = pool.stats();
        assert(s.base.in_use == 2);
        assert(s.base.available == 1);
        assert(s.base.peak_in_use == 2);
        assert(s.base.in_use + s.base.available == s.base.size);
    }
    pool.release(a);
    pool.release(b);
    assert(pool.stats().base.min_available == 1 && "历史最小空闲数保留");
}

// 有空槽（重建失败保留）时，stats 的语义：
// size 只数存活连接（暴露退化程度），available 含空槽，
// 因此等式是 in_use + available == capacity，而 size <= capacity。
static void test_stats_reflects_degraded_pool() {
    reset_fake_state();
    std::atomic<bool> factory_ok{true};
    auto flaky = [&](std::size_t) -> std::unique_ptr<Database> {
        if (!factory_ok.load()) {
            return nullptr;
        }
        return std::make_unique<FakeDatabase>();
    };
    // 1 个槽位：重建失败一次 → 1 个空槽。不用 2 槽，因为 spec 语义是
    // 同一次 acquire 会尝试**所有**未试过的空闲槽（会继续取下一条），
    // 2 槽场景两个都会被销毁，不是本测试要验证的退化形态。
    DbConnectionPool pool("degraded", flaky, 1, 0ms);

    auto* a = pool.acquire(10ms);
    pool.release(a);

    FakeDatabase::healthy_flag.store(false);
    factory_ok.store(false);
    auto* none = pool.acquire(20ms);  // 探活失败 + 重建失败 → 空槽
    assert(none == nullptr);

    const DbPoolStats s = pool.stats();
    assert(s.base.capacity == 1);
    assert(s.base.size == 0 && "空槽不计入存活连接数（暴露退化程度）");
    assert(s.base.in_use == 0);
    assert(s.base.available == 1 && "空槽仍在空闲队列，等待自愈");
    assert(s.base.in_use + s.base.available == s.base.capacity);
    assert(s.rebuild_failed_total == 1);
}

// I-1 回归：空槽全部试过后不得忙等待（整体深度评审发现、实测确认的盲区）。
// 旧实现里谓词「队列非空」立即为真（空槽就在队列里），扫描却找不到
// 未尝试的槽 → continue → 谓词又立即为真……锁拿放空转直到 deadline，
// 实测 200ms acquire 烧掉 98% CPU（rusage 探针）。触发条件恰是
// 「DB 持续故障 + factory 快速失败 + 并发请求」——生产上最坏的时刻。
static void test_no_busy_wait_when_all_slots_failed() {
#if defined(__APPLE__) || defined(__linux__)
    reset_fake_state();
    std::atomic<bool> factory_ok{true};
    auto flaky = [&](std::size_t) -> std::unique_ptr<Database> {
        if (!factory_ok.load()) {
            return nullptr;
        }
        return std::make_unique<FakeDatabase>();
    };
    DbConnectionPool pool("nospin", flaky, 1, 0ms);

    auto* warm = pool.acquire(10ms);
    pool.release(warm);

    FakeDatabase::healthy_flag.store(false);
    factory_ok.store(false);

    struct rusage r0;
    getrusage(RUSAGE_SELF, &r0);
    const auto t0 = std::chrono::steady_clock::now();
    auto* none = pool.acquire(150ms);
    const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    struct rusage r1;
    getrusage(RUSAGE_SELF, &r1);
    const long long user_ms =
        static_cast<long long>(r1.ru_utime.tv_sec - r0.ru_utime.tv_sec) * 1000 +
        static_cast<long long>(r1.ru_utime.tv_usec - r0.ru_utime.tv_usec) / 1000;

    assert(none == nullptr);
    assert(wall >= 140 && "必须真的等到 deadline");
    // 修复后该路径是条件变量挂起：user CPU 应接近 0。
    // 阈值取 wall/3 留足调度余量，而忙等待是 ~100% —— 区分度足够。
    assert(user_ms < wall / 3 && "空槽全部试过后必须挂起等待，不得忙等待烧 CPU");
#endif
}

// 并发借还 + 探活交织（整体评审 I-4）。threshold=0 让每次借出都走
// probe_and_rebuild 的锁外路径，与 release/stats 的锁内路径充分交织；
// 结束后池状态必须精确复原。
static void test_concurrent_acquire_release_with_probes() {
    reset_fake_state();
    DbConnectionPool pool("conc", make_fake_factory(), 4, 0ms);
    constexpr int kThreads = 4;
    constexpr int kRounds = 300;

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([&pool] {
            for (int r = 0; r < kRounds; ++r) {
                Database* c = pool.acquire(500ms);
                assert(c != nullptr);
                pool.release(c);
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }

    const DbPoolStats s = pool.stats();
    assert(s.base.in_use == 0);
    assert(s.base.available == s.base.capacity);
    assert(s.base.size == s.base.capacity);  // 健康连接从不重建
    assert(s.base.acquire_total == static_cast<std::uint64_t>(kThreads) * kRounds);
    assert(s.base.acquire_failed_total == 0);
    assert(FakeDatabase::probe_calls.load() == kThreads * kRounds &&
           "threshold=0 下每次借出都必须探活一次");
}

// acquire 与 stats() 并发：stats 的锁内遍历不得与借还互斥出死锁，
// 也不得读到撕裂状态（等式不变式在每个快照上都成立）。
static void test_stats_concurrent_with_acquire() {
    reset_fake_state();
    DbConnectionPool pool("statconc", make_fake_factory(), 4, 0ms);
    std::atomic<bool> stop{false};

    std::thread reader([&pool, &stop] {
        while (!stop.load()) {
            const DbPoolStats s = pool.stats();
            assert(s.base.in_use + s.base.available == s.base.capacity);
            assert(s.base.size <= s.base.capacity);
        }
    });

    for (int r = 0; r < 500; ++r) {
        Database* c = pool.acquire(500ms);
        assert(c != nullptr);
        pool.release(c);
    }

    stop.store(true);
    reader.join();
}

// When one slot fails rebuild (empty slot preserved), acquire must advance
// to the next free slot rather than blocking or spinning on the failed one.
// This freezes the proven "try all untried slots" behavior.
static void test_acquire_advances_past_a_failed_slot() {
    reset_fake_state();
    std::atomic<int> created{0};
    std::atomic<int> factory_calls{0};
    auto flaky = [&](std::size_t) -> std::unique_ptr<Database> {
        int n = factory_calls.fetch_add(1);
        if (n == 2) {
            return nullptr;
        }
        created.fetch_add(1);
        return std::make_unique<FakeDatabase>();
    };
    DbConnectionPool pool("advance", flaky, 2, 0ms);
    assert(created.load() == 2);

    auto* a = pool.acquire(10ms);
    auto* b = pool.acquire(10ms);
    pool.release(a);
    pool.release(b);

    FakeDatabase::healthy_flag.store(false);
    auto* c = pool.acquire(500ms);
    assert(c != nullptr && "acquire must advance past the failed slot");
    assert(created.load() == 3 && "only one rebuild should succeed");
    assert(pool.rebuild_failed_total() == 1);
    assert(pool.rebuild_total() == 1);
    pool.release(c);
}

int main() {
    test_constructor_rejects_invalid_arguments();
    test_construction_fails_fast_when_factory_fails();
    test_basic_acquire_release();
    test_acquire_times_out_when_exhausted();
    test_guard_auto_release_and_move_semantics();
    test_invalid_release_is_counted_and_null_is_not();
    test_hot_connection_skips_probe();
    test_cold_connection_is_probed_and_reused_when_healthy();
    test_unhealthy_connection_is_rebuilt();
    test_rebuild_failure_preserves_slot_and_self_heals();
    test_timeout_is_total_bound_including_probe_and_rebuild();
    test_no_busy_wait_when_all_slots_failed();
    test_concurrent_acquire_release_with_probes();
    test_stats_concurrent_with_acquire();
    test_stats_reports_consistent_snapshot();
    test_stats_reflects_degraded_pool();
    test_acquire_advances_past_a_failed_slot();
    return 0;
}
