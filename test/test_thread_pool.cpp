#include "cpppools/pool/thread_pool.h"

#include <cassert>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif

using namespace cpppools::pool;

// submit 现在返回 optional：nullopt 表示队列满被拒绝。
// 测试里确定不会被拒绝的提交用这个辅助函数取出 future，
// 免得每处都写 std::move(*opt) 而淹没测试意图。
template <typename Opt>
static auto must_submit(Opt&& opt) {
    assert(opt.has_value() && "submit 意外被拒绝");
    return std::move(*opt);
}

static void test_basic_submit() {
    ThreadPool pool(2);
    auto f1 = must_submit(pool.submit([] { return 1 + 2; }));
    auto f2 = must_submit(pool.submit([](int x) { return x * 3; }, 7));

    assert(f1.get() == 3);
    assert(f2.get() == 21);
}

static void test_many_tasks() {
    ThreadPool pool(4);
    assert(pool.worker_count() == 4);

    std::vector<std::future<int>> futures;
    futures.reserve(200);

    for (int i = 0; i < 200; ++i) {
        futures.emplace_back(must_submit(pool.submit([i] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return i;
        })));
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

static void test_tracks_peak_pending_tasks() {
    // 峰值由线程池自己在 submit() 里统计，而不是让调用方（如 stats 端点）
    // 去采样——后者只能看到“被查询那一瞬”的值，真正的堆积峰值很容易错过。
    ThreadPool pool(1);
    assert(pool.peak_pending_tasks() == 0);

    // 先堵住唯一的 worker，后续任务就会在队列里堆积。
    std::promise<void> gate;
    auto gate_future = gate.get_future();
    auto blocker = must_submit(pool.submit([&gate_future] { gate_future.wait(); }));

    std::vector<std::future<void>> queued;
    queued.reserve(5);
    for (int i = 0; i < 5; ++i) {
        queued.emplace_back(must_submit(pool.submit([] {})));
    }

    // 此时队列里应有接近 5 个待处理任务。
    assert(pool.peak_pending_tasks() >= 5);

    gate.set_value();
    blocker.get();
    for (auto& f : queued) {
        f.get();
    }

    // 队列清空后当前值回落，历史峰值保留。
    assert(pool.pending_tasks() == 0);
    assert(pool.peak_pending_tasks() >= 5);
}

// ---------------------------------------------------------------------------
// 以下为本批新增行为的测试。
// ---------------------------------------------------------------------------

static void test_submit_rejects_when_queue_is_full() {
    // 有界队列：达到 max_queue_size 后 submit 返回 nullopt 而不是无限堆积。
    //
    // 「队列满」是预期的运行期背压，不是错误，所以用返回值表达而非异常
    // ——这与项目在 DB 层已确立的「状态按值返回」一致。
    ThreadPoolConfig cfg;
    cfg.worker_count = 1;
    cfg.max_queue_size = 3;
    ThreadPool pool(cfg);

    // 堵住唯一的 worker，让后续任务只能待在队列里。
    std::promise<void> gate;
    auto gate_future = gate.get_future();
    std::promise<void> worker_started;
    auto worker_started_future = worker_started.get_future();
    auto blocker = must_submit(pool.submit([&] {
        worker_started.set_value();
        gate_future.wait();
    }));

    // 必须等 worker 真的取走 blocker。否则 blocker 仍占着 1 个队列名额，
    // 下面第 3 个任务就会被提前拒绝 —— 这是竞态而非保证，
    // 不做握手的话测试会零星失败（已实际碰到过）。
    worker_started_future.wait();

    // 填满队列。
    std::vector<std::future<void>> accepted;
    for (int i = 0; i < 3; ++i) {
        auto opt = pool.submit([] {});
        assert(opt.has_value() && "队列未满时不应拒绝");
        accepted.emplace_back(std::move(*opt));
    }

    // 第 4 个必须被拒绝。
    auto rejected = pool.submit([] { return 1; });
    assert(!rejected.has_value() && "队列已满时必须返回 nullopt");

    gate.set_value();
    blocker.get();
    for (auto& f : accepted) {
        f.get();
    }
}

static void test_submit_after_shutdown_throws_pool_shutdown_error() {
    // 池已关闭 = 使用了一个已死的池，属编程错误/停机竞态 → 用异常。
    // 用独立异常类型而非泛用 runtime_error，调用方才能精确捕获。
    ThreadPool pool(2);
    pool.shutdown();

    bool caught_shutdown = false;
    try {
        (void)pool.submit([] { return 42; });
    } catch (const PoolShutdownError&) {
        caught_shutdown = true;
    } catch (const std::runtime_error&) {
        // 旧行为：抛泛用 runtime_error。留着这个分支是为了让断言而不是
        // 未捕获异常来报告失败。
    }
    assert(caught_shutdown && "submit 在池关闭后必须抛 PoolShutdownError");
}

static void test_submit_detached_after_shutdown_also_throws() {
    // 两个入口在「池已关闭」上必须行为一致：都抛。
    //
    // 不能让 submit_detached 用返回 false 表示「已关闭」——
    // HttpServer::start 靠区分这两种失败决定行为：
    //   队列满   → 写 503 后关闭这条连接
    //   池已关闭 → 服务正在停机，直接关闭
    // 若都返回 false，HTTP 层无从区分。
    ThreadPool pool(2);
    pool.shutdown();

    bool caught_shutdown = false;
    try {
        (void)pool.submit_detached([] {});
    } catch (const PoolShutdownError&) {
        caught_shutdown = true;
    } catch (const std::runtime_error&) {
    }
    assert(caught_shutdown && "submit_detached 在池关闭后必须抛 PoolShutdownError");
}

static void test_submit_detached_reports_task_exception() {
    // packaged_task::operator() 会把异常**存进 future 而不重抛**，
    // 所以只要调用方丢弃 future（HttpServer::start 正是如此），
    // 异常就永久静默。submit_detached 存在的全部理由就是把这条路径补上。
    std::atomic<int> reported{0};
    std::string message;
    std::mutex message_mutex;

    ThreadPoolConfig cfg;
    cfg.worker_count = 1;
    cfg.on_task_exception = [&](std::exception_ptr eptr) {
        try {
            std::rethrow_exception(eptr);
        } catch (const std::exception& ex) {
            std::lock_guard<std::mutex> lock(message_mutex);
            message = ex.what();
        }
        reported.fetch_add(1);
    };

    {
        ThreadPool pool(cfg);
        const bool ok = pool.submit_detached([] { throw std::runtime_error("boom"); });
        assert(ok);
        pool.shutdown();  // 排空队列并 join，保证任务已执行完
    }

    assert(reported.load() == 1 && "任务异常必须交给 on_task_exception");
    std::lock_guard<std::mutex> lock(message_mutex);
    assert(message == "boom");
}

static void test_exception_callback_throwing_does_not_kill_worker() {
    // 回调在 worker 线程、在 catch(...) 内被调用。若它自己抛出且无人接，
    // worker 线程会终止 —— 池静默少一个 worker，而 worker_count() 仍返回原值
    // （指标撒谎），最坏情况所有 worker 死光、队列永久积压。
    //
    // 必须同时断言 callback_invoked：否则当回调根本没被调用时
    // （比如 packaged_task 把异常吞了），它就不会抛，worker 天然存活，
    // 测试会在**功能不存在时就通过** —— 那就是一个假绿测试。
    std::atomic<int> callback_invoked{0};
    std::atomic<int> executed_after{0};

    ThreadPoolConfig cfg;
    cfg.worker_count = 1;  // 只一个 worker：它死了后续任务就永远不会跑
    cfg.on_task_exception = [&callback_invoked](std::exception_ptr) {
        callback_invoked.fetch_add(1);
        throw std::runtime_error("callback boom");
    };

    {
        ThreadPool pool(cfg);
        const bool ok1 = pool.submit_detached([] { throw std::runtime_error("task boom"); });
        const bool ok2 = pool.submit_detached([&] { executed_after.fetch_add(1); });
        assert(ok1 && ok2);
        pool.shutdown();
    }

    assert(callback_invoked.load() == 1 && "回调必须真的被调用过，否则本测试形同虚设");
    assert(executed_after.load() == 1 && "回调抛异常后 worker 必须存活并继续执行后续任务");
}

static void test_accepts_move_only_arguments() {
    // std::bind 会 decay 参数且调用时按左值传递，因此 move-only 参数无法工作。
    // 注意：这条测试的 RED 是**编译错误**而非断言失败 ——
    // 类型系统层面的能力只能以这种形式被验证，没有运行期版本。
    ThreadPool pool(2);

    auto fut = must_submit(pool.submit([](std::unique_ptr<int> p) { return *p + 1; },
                                       std::make_unique<int>(41)));
    assert(fut.get() == 42);

    std::atomic<int> seen{0};
    const bool ok = pool.submit_detached([&seen](std::unique_ptr<int> p) { seen.store(*p); },
                                        std::make_unique<int>(7));
    assert(ok);
    pool.shutdown();
    assert(seen.load() == 7);
}

static void test_shutdown_drains_pending_tasks() {
    // shutdown()：停止接收新任务，但队列里已有的全部执行完。
    std::atomic<int> executed{0};
    std::promise<void> gate;
    auto gate_future = gate.get_future();
    std::promise<void> worker_started;
    auto worker_started_future = worker_started.get_future();

    ThreadPoolConfig cfg;
    cfg.worker_count = 1;
    ThreadPool pool(cfg);

    // 先堵住唯一 worker，后续 5 个任务只能待在队列里。
    auto blocker = must_submit(pool.submit([&] {
        worker_started.set_value();
        gate_future.wait();
    }));
    worker_started_future.wait();

    for (int i = 0; i < 5; ++i) {
        const bool ok = pool.submit_detached([&executed] { executed.fetch_add(1); });
        assert(ok);
    }

    gate.set_value();
    pool.shutdown();
    blocker.get();

    assert(executed.load() == 5 && "shutdown 必须排空队列里已有的任务");
}

// 等待“丢弃待处理任务”真的发生过。
//
// 为何需要这个辅助：直接写成
//     gate.set_value(); pool.shutdown_now();
// 是**竞态绿** —— gate 放行后 worker 与主线程争抢 mutex_，
// worker 若先拿到锁就会把待处理任务全部执行掉。
// 实测：在两句之间插 1 微秒延迟，executed 就从 0 变成 2；10 微秒变成 5。
// 也就是说它在空闲开发机上能过，只是因为主线程路径恰好短一点。
//
// 确定性信号：pending_count_ **只在丢弃分支里被清零**，
// 而 worker 此刻被 blocker 堵住、不可能自己减到 0，
// 因此观测到 0 就证明清队已在锁内完成 —— 此后放行 gate 才是安全的。
static void wait_until_queue_discarded(const ThreadPool& pool) {
    while (pool.pending_tasks() != 0) {
        std::this_thread::yield();
    }
}

static void test_shutdown_now_discards_pending_tasks() {
    // shutdown_now()：丢弃**尚未开始**的任务，只等正在执行的跑完。
    //
    // 诚实说明：这**不能**解决“任务挂死导致进程无法退出”——
    // 标准 C++ 无法安全强杀线程（detach 后线程仍引用已析构的 this，是 UB）。
    // 它只把风险面从“整个队列”缩到“正在执行的任务”。
    std::atomic<int> executed{0};
    std::promise<void> gate;
    auto gate_future = gate.get_future();
    std::promise<void> worker_started;
    auto worker_started_future = worker_started.get_future();

    ThreadPoolConfig cfg;
    cfg.worker_count = 1;
    ThreadPool pool(cfg);

    auto blocker = must_submit(pool.submit([&] {
        worker_started.set_value();
        gate_future.wait();
    }));
    // 等 worker 真的拿到了 blocker，否则下面的任务可能先被执行，测试会零星失败。
    worker_started_future.wait();

    for (int i = 0; i < 5; ++i) {
        const bool ok = pool.submit_detached([&executed] { executed.fetch_add(1); });
        assert(ok);
    }
    assert(pool.pending_tasks() == 5);

    // 把停机放到辅助线程：它会在锁内清队，然后阻塞在 join() 上等 blocker 结束。
    std::thread stopper([&pool] { pool.shutdown_now(); });
    wait_until_queue_discarded(pool);  // 确定性地等到“清队已发生”
    gate.set_value();                  // 此后才放行 worker
    stopper.join();
    blocker.get();

    assert(executed.load() == 0 && "shutdown_now 必须丢弃尚未开始的任务");
}

// 析构时发信号的小对象，用作测试任务的捕获物。
//
// 用它而不是去观测 pool.pending_tasks()：后者在“测析构行为”时需要读一个
// 正在析构的对象的成员，生命周期上站不住。而“任务被丢弃 → 其捕获物析构”
// 是一个完全在池外部可观测的事件。
//
// 顺带还验证了一个关键修复：被丢弃任务的捕获物必须在**锁外**析构。
// 若在临界区内析构，而捕获物的析构又回调进池，就会自锁。
class DiscardSignal {
public:
    explicit DiscardSignal(std::promise<void>* p) : promise_(p) {}
    DiscardSignal(DiscardSignal&& other) noexcept : promise_(other.promise_) {
        other.promise_ = nullptr;  // 只有最后一份拷贝负责发信号
    }
    DiscardSignal(const DiscardSignal&) = delete;
    DiscardSignal& operator=(const DiscardSignal&) = delete;
    DiscardSignal& operator=(DiscardSignal&&) = delete;
    ~DiscardSignal() {
        if (promise_ != nullptr) {
            promise_->set_value();
        }
    }

private:
    std::promise<void>* promise_;
};

static void test_destructor_discards_pending_tasks() {
    // 析构调 shutdown_now() 而非 shutdown()：析构不应无限期阻塞。
    // 队里积了上千任务时 shutdown() 会把进程退出拖很久；异常展开路径上更糟。
    std::atomic<int> executed{0};
    std::promise<void> gate;
    auto gate_future = gate.get_future();
    std::promise<void> worker_started;
    auto worker_started_future = worker_started.get_future();
    std::promise<void> discarded;
    auto discarded_future = discarded.get_future();

    // 堆分配 + 从辅助线程 delete：这样才真正走**析构**路径。
    // 主线程随后只等自己的 promise，**不再触碰 pool**，因此不存在
    // “读一个正在析构的对象”的生命周期问题。
    ThreadPoolConfig cfg;
    cfg.worker_count = 1;
    auto* pool = new ThreadPool(cfg);

    const bool ok_blocker = pool->submit_detached([&] {
        worker_started.set_value();
        gate_future.wait();
    });
    assert(ok_blocker);
    worker_started_future.wait();

    for (int i = 0; i < 5; ++i) {
        const bool ok = pool->submit_detached([&executed] { executed.fetch_add(1); });
        assert(ok);
    }
    // 最后一个任务携带信号器：它被丢弃时捕获物析构 → 发信号。
    const bool ok_signal = pool->submit_detached(
        [&executed, sig = DiscardSignal(&discarded)] { executed.fetch_add(1); });
    assert(ok_signal);

    // 析构是「清队 + join」，不能靠抢锁取胜（直接写 gate.set_value() 再等析构，
    // 实测插 1 微秒延迟就会失败）。派线程触发析构，
    // 主线程等到“任务已被丢弃”的信号后才放行 gate。
    std::thread stopper([pool] { delete pool; });
    discarded_future.wait();
    gate.set_value();
    stopper.join();

    assert(executed.load() == 0 && "析构必须走 shutdown_now 语义（丢弃待处理任务）");
}

static void test_discarded_task_future_gets_broken_promise() {
    // 这是析构改走 shutdown_now 带来的**行为变更**，必须有测试守护：
    // 丢弃一个未执行的 packaged_task 会破坏其 promise，
    // 等待方 .get() 从“拿到结果”变为抛 std::future_error(broken_promise)。
    std::promise<void> gate;
    auto gate_future = gate.get_future();
    std::promise<void> worker_started;
    auto worker_started_future = worker_started.get_future();

    std::promise<void> discarded;
    auto discarded_future = discarded.get_future();

    std::future<int> orphaned;
    {
        ThreadPoolConfig cfg;
        cfg.worker_count = 1;
        ThreadPool pool(cfg);

        const bool ok_blocker = pool.submit_detached([&] {
            worker_started.set_value();
            gate_future.wait();
        });
        assert(ok_blocker);
        worker_started_future.wait();

        orphaned = must_submit(pool.submit([] { return 7; }));
        // 携带信号器的第二个任务，用于确定性地等到“清队已发生”。
        const bool ok_signal =
            pool.submit_detached([sig = DiscardSignal(&discarded)] {});
        assert(ok_signal);

        std::thread stopper([&pool] { pool.shutdown_now(); });
        discarded_future.wait();
        gate.set_value();
        stopper.join();
    }

    bool broken = false;
    try {
        (void)orphaned.get();
    } catch (const std::future_error& err) {
        broken = (err.code() == std::future_errc::broken_promise);
    }
    assert(broken && "被丢弃任务的 future 必须抛 broken_promise");
}

static void test_stats_counts_submitted_rejected_and_completed() {
    ThreadPoolConfig cfg;
    cfg.name = "statpool";
    cfg.worker_count = 1;
    cfg.max_queue_size = 2;
    ThreadPool pool(cfg);

    {
        const TaskStats s = pool.stats();
        assert(s.name == "statpool");
        assert(s.worker_count == 1);
        assert(s.queue_capacity == 2);
        assert(s.submitted_total == 0);
        assert(s.rejected_total == 0);
        assert(s.completed_total == 0);
    }

    std::promise<void> gate;
    auto gate_future = gate.get_future();
    std::promise<void> worker_started;
    auto worker_started_future = worker_started.get_future();
    auto blocker = must_submit(pool.submit([&] {
        worker_started.set_value();
        gate_future.wait();
    }));
    worker_started_future.wait();

    // 填满队列（容量 2），再多提两个必被拒。
    std::vector<std::future<void>> accepted;
    for (int i = 0; i < 2; ++i) {
        accepted.emplace_back(must_submit(pool.submit([] {})));
    }
    const bool submit_rejected = !pool.submit([] {}).has_value();
    const bool detached_rejected = !pool.submit_detached([] {});
    assert(submit_rejected && detached_rejected);

    {
        const TaskStats s = pool.stats();
        assert(s.submitted_total == 3 && "blocker + 2 个入队成功");
        assert(s.rejected_total == 2 && "拒绝计数必须覆盖两个入口");
        assert(s.queue_pending == 2);
        assert(s.peak_queue_pending >= 2);
    }

    gate.set_value();
    pool.shutdown();
    blocker.get();

    const TaskStats s = pool.stats();
    assert(s.completed_total == 3 && "排空后完成数应等于入队数");
    assert(s.queue_pending == 0);
}

static void test_submit_exception_goes_to_future_not_to_counter() {
    // task_exception_total 会系统性低报：submit() 的异常仍被 packaged_task
    // 吞进 future，池看不到。看板上该值为 0 **不等于没有异常** ——
    // 这条测试就是把这个限制固定下来，以免日后有人误以为它覆盖全部任务。
    ThreadPoolConfig cfg;
    cfg.worker_count = 1;
    ThreadPool pool(cfg);

    auto fut = must_submit(pool.submit([]() -> int { throw std::runtime_error("via submit"); }));
    const bool ok = pool.submit_detached([] { throw std::runtime_error("via detached"); });
    assert(ok);
    pool.shutdown();

    bool submit_threw = false;
    try {
        (void)fut.get();
    } catch (const std::runtime_error&) {
        submit_threw = true;
    }
    assert(submit_threw && "submit 的异常应由 future 携带");

    const TaskStats s = pool.stats();
    assert(s.task_exception_total == 1 && "仅统计 detached 任务的异常");
}

static void test_stats_records_task_execution_time() {
    // 只有排队深度无法区分“排队久”与“执行慢”，所以要统计执行耗时。
    ThreadPoolConfig cfg;
    cfg.worker_count = 1;
    ThreadPool pool(cfg);

    assert(pool.stats().task_exec_us_total == 0);

    must_submit(pool.submit([] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }))
        .get();
    pool.shutdown();

    const TaskStats s = pool.stats();
    // 宽松下界：只要能证明“真的在计时”就够了，不对调度延迟做强假设。
    assert(s.task_exec_us_total >= 10000 && "执行耗时累计必须反映真实耗时");
    assert(s.peak_task_exec_us >= 10000);
}

static void test_worker_threads_are_named() {
    // 命名后在 lldb / Instruments / ps -M 里才不是一堆匿名线程。
    // name 也是后续指标标签的数据源（多数据源后会有 N 个连接池）。
    ThreadPoolConfig cfg;
    cfg.name = "namedpool";
    cfg.worker_count = 1;
    ThreadPool pool(cfg);

    auto observed = must_submit(pool.submit([] {
        char buf[64] = {};
#if defined(__APPLE__) || defined(__linux__)
        pthread_getname_np(pthread_self(), buf, sizeof(buf));
#endif
        return std::string(buf);
    })).get();
    pool.shutdown();

#if defined(__APPLE__) || defined(__linux__)
    assert(observed == "namedpool-0" && "worker 线程应命名为 <name>-<index>");
#else
    (void)observed;  // 其他平台不断言名字
#endif
}

// 多生产者并发提交 + 并发读 stats（整体评审 I-4；同时守护批 4 的
// “stats 无锁读不与热路径抢锁”声明）。
static void test_concurrent_submit_detached_and_stats() {
    ThreadPoolConfig cfg;
    cfg.name = "conc";
    cfg.worker_count = 4;
    cfg.max_queue_size = 4096;
    std::atomic<int> executed{0};
    cfg.on_task_exception = [](std::exception_ptr) {};
    ThreadPool pool(cfg);

    constexpr int kProducers = 4;
    constexpr int kPerProducer = 2000;
    std::atomic<bool> stop{false};

    std::thread reader([&pool, &stop] {
        while (!stop.load()) {
            const TaskStats s = pool.stats();
            assert(s.queue_pending <= s.queue_capacity);
        }
    });

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int i = 0; i < kProducers; ++i) {
        producers.emplace_back([&pool, &executed] {
            for (int k = 0; k < kPerProducer; ++k) {
                // 队列足够大（4096 > 峰值积压），不应有拒绝；
                // 若偶发拒绝也只是计数，不算失败。
                (void)pool.submit_detached([&executed] { executed.fetch_add(1); });
            }
        });
    }
    for (auto& p : producers) {
        p.join();
    }
    stop.store(true);
    reader.join();
    pool.shutdown();  // 排空后再断言完成数

    const TaskStats s = pool.stats();
    assert(static_cast<std::uint64_t>(executed.load()) == s.completed_total);
    assert(s.submitted_total == s.completed_total + s.rejected_total);
    assert(s.completed_total >= static_cast<std::uint64_t>(kProducers) * kPerProducer - s.rejected_total);
}

int main() {
    test_basic_submit();
    test_many_tasks();
    test_tracks_peak_pending_tasks();
    test_submit_rejects_when_queue_is_full();
    test_submit_after_shutdown_throws_pool_shutdown_error();
    test_submit_detached_after_shutdown_also_throws();
    test_submit_detached_reports_task_exception();
    test_exception_callback_throwing_does_not_kill_worker();
    test_accepts_move_only_arguments();
    test_shutdown_drains_pending_tasks();
    test_shutdown_now_discards_pending_tasks();
    test_destructor_discards_pending_tasks();
    test_discarded_task_future_gets_broken_promise();
    test_stats_counts_submitted_rejected_and_completed();
    test_submit_exception_goes_to_future_not_to_counter();
    test_stats_records_task_execution_time();
    test_worker_threads_are_named();
    test_concurrent_submit_detached_and_stats();
    return 0;
}
