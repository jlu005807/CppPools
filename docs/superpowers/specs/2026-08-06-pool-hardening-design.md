# 三大池子加固设计（Pool Hardening）

- 日期：2026-08-06
- 状态：待评审
- 定位：这是 DB 中间件改造（`2026-08-06-db-middleware-design.md`）的**前置工作**。
  三个池是项目的运行时主干，必须先把它们修正完善，才推进到中间件改造。

## 1. 背景

三个池的当前实现（`include/cpppools/pool/thread_pool.h` 118 行、
`include/cpppools/pool/object_pool.h` 162 行、
`include/cpppools/db/db_connection_pool.h` 131 行）存在若干真实缺陷、
缺少生产级特性，且三者之间语义与接口不一致。

本设计一次性处理三类问题：**真缺陷修复 + 生产级特性补齐 + 三池语义统一**。

## 2. 范围

### 做

- ThreadPool：有界队列与拒绝、异常可见性、`shutdown_now`、`std::bind` 替换、线程命名、执行耗时统计
- ObjectPool：异常安全窗口修复、`max_size` 上限、无状态 deleter、atomic 统计、Release 下的失效信号
- DbConnectionPool：连接健康检查与重建、非法归还可观测、`ConnectionFactory` 抽取、`DbConnGuard` 移动语义
- 三池统一：命名（`name()`）、统计结构、耗尽语义的明确化与文档化

### 不做（明确排除）

| 不做的事 | 理由 |
|---|---|
| work-stealing / 多队列分片 | 单队列争用在当前规模不是瓶颈。**属过早优化**，应先有可测量的证据再动 |
| 动态调整 worker 数量 | 同上，且引入线程增删的生命周期复杂度 |
| statement cache | 依赖 prepared statement，而后者依赖 DB 中间件改造的 `Database` 抽象。**必须等改造之后** |
| `shutdown` 带超时 | 标准 C++ **无法安全强杀线程**（detach 后线程仍引用已析构的 `this`，是 UB）。加 timeout 是假承诺，详见 §4.4 |

## 3. 耗尽策略：实际是两种，各有理由

三池统一为「有界」，但「等不等」按资源性质分两种。**不把它包装成完全统一。**

| 池 | 上限 | 耗尽行为 | 返回值 | 理由 |
|---|---|---|---|---|
| ThreadPool | `max_queue_size` | **立即拒绝** | `std::nullopt` | 背压必须立刻传导给客户端。阻塞 accept 循环会让过载更糟 |
| ObjectPool | `max_size` | **立即拒绝** | 空 `Ptr` | `max_size ≥ worker_count` 时该路径**不可达**（每 worker 同时最多持 1 个对象），不为不可达路径写等待机制 |
| DbConnectionPool | `pool_size` | **阻塞至超时** | `nullptr` | 外部资源，连接在毫秒级被归还，短暂等待能显著提高成功率 |

**共性**：三池都有界、都能快速失败、都不会无限堆积。差异仅在「等不等」。
README 需按此表原样说明，不得写成「三池策略完全一致」。

## 4. 批 1：ThreadPool

### 4.1 配置

```cpp
namespace cpppools::pool {

struct ThreadPoolConfig {
    // 用于指标标签与 worker 线程名前缀（线程名形如 "<name>-0"）。
    std::string name{"worker"};
    std::size_t worker_count{6};
    // 任务队列上限。达到上限后 submit 拒绝而非无限堆积。
    std::size_t max_queue_size{1024};
    // 可选：submit_detached 提交的任务抛出异常时的回调。
    // 为空时仅计入 task_exception_total。
    std::function<void(std::exception_ptr)> on_task_exception;
};

// 池已关闭后仍调用 submit —— 这是编程错误（使用了已死的池），故用异常。
class PoolShutdownError : public std::runtime_error {
public:
    explicit PoolShutdownError(const std::string& what) : std::runtime_error(what) {}
};

}  // namespace cpppools::pool
```

### 4.1.1 构造函数

```cpp
explicit ThreadPool(ThreadPoolConfig config);

// 便利构造：内部转为 ThreadPoolConfig{.worker_count = n}。
// 保留的理由：现有 5 个调用点中 4 个在测试里
// （test_thread_pool.cpp:14,23,48,63），保留它可避免为了改构造形式
// 而改一批与本次改动无关的测试。
explicit ThreadPool(std::size_t thread_count);
```

### 4.2 两个提交入口

```cpp
// 返回 nullopt 表示队列已满被拒绝（运行期背压，不是错误）。
// 池已关闭时抛 PoolShutdownError。
// 注意：本函数仍可因堆分配失败、捕获物移动抛出而传播异常。
template <class F, class... Args>
auto submit(F&& f, Args&&... args)
    -> std::optional<std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>>;

// 任务异常在 worker 内被捕获，交给 on_task_exception 并计入 task_exception_total。
// 返回 false = 队列已满被拒绝；池已关闭时同样抛 PoolShutdownError。
template <class F, class... Args>
bool submit_detached(F&& f, Args&&... args);
```

**为何必须双入口**：`std::packaged_task::operator()` 会把任务异常**存进 future 而不重抛**，
因此 worker 根本看不到异常。只要调用方丢弃 future（`HttpServer::start` 正是如此），
异常就永久静默。给 `submit()` 加回调救不了这一点 ——
必须另开一条不经 packaged_task 封装语义的路径。

**两个入口都在池已关闭时抛异常**（而不是 `submit_detached` 返回 false）：
`HttpServer::start` 靠区分这两种失败来决定行为 ——
队列满 → 写 503 后关闭；池已关闭（服务正在停机）→ 直接关闭。
若两者都返回 false，HTTP 层无法区分。

**`on_task_exception` 的约束**：回调在 worker 线程、在 `catch (...)` 内被调用，
必须线程安全、不得阻塞。池内部**必须**把调用包在 `try { cb(eptr); } catch (...) {}` 里 ——
否则回调自己抛出会终止 worker 线程，池**静默少一个 worker** 而 `worker_count()`
仍返回原值（指标撒谢），最坏情况所有 worker 死光、队列永久积压。

**worker 内 try/catch 覆盖不到两处**（已知限制，不假装能兼顾）：
1. 任务对象**析构**时抛出（可能发生在 catch 块外）；
2. `submit_detached` 自身在**调用方线程**上的失败（捕获物移动抛出、队列节点分配 `bad_alloc`）。

**为何 `submit` 用返回值而非异常表达「队列满」**：
- 池已关闭 = **编程错误或停机竞态** → 异常
- 队列满 = **预期的运行期背压** → 返回值

这与项目在 DB 层已确立的「状态按值返回、不用异常做控制流」一致。
调用方必须显式处理 `nullopt`，漏处理会被编译器提醒。

补一句诚实的定性：`HttpServer::start` 在停机期仍在跑 accept 循环，
所以「池已关闭」对它而言更像**竞态**而非编程错误。异常仍是合适载体（非热路径、一次性），
但 `http_server.cpp` 里那个 try/catch **不是多余代码**，不得删除。

### 4.3 任务队列元素类型（先定这一条，否则 §4.2 无法编译）

**现状的阻塞点**：`thread_pool.h:74` 是 `std::queue<std::function<void()>> tasks_`，
而 `std::function` 要求目标**可拷贝构造**。一旦捕获物含 move-only 类型
（正是 §4.4 要修的场景），lambda 只可移动、不可拷贝，**存不进去**。

现有 `submit()` 侥幸能编译，只因为它把 lambda 包进了
`std::make_shared<std::packaged_task<R()>>`（`thread_pool.h:94`），
入队的是捕获 `shared_ptr` 的**可拷贝** lambda（`:104`）。
而 `submit_detached` 不走 packaged_task 封装，就没有这层隐形的 shared_ptr → **直接编译失败**。

**决定：队列元素改为 `std::packaged_task<void()>`**。

```cpp
std::queue<std::packaged_task<void()>> tasks_;   // 只要求可移动
```

理由：`std::packaged_task` 只要求可移动，move-only 捕获物直接能存，
不需额外的 `make_shared` 堆分配。

注意：此处的 `packaged_task<void()>` 仅用作**只需可移动的类型删除容器**，
其 future 被丢弃。`submit()` 的 future 仍由内层自己的 `packaged_task<R()>` 提供 ——
两层职责不同，不要弄混。

### 4.4 `std::bind` 替换

现状（`thread_pool.h:95`）用 `std::bind`，它会 decay 参数且调用时按左值传递，
因此 `submit(f, std::unique_ptr<X>{})` 这类 move-only 参数无法工作。

改为 lambda + tuple 转发：

```cpp
auto bound = [fn = std::forward<F>(f),
              tup = std::make_tuple(std::forward<Args>(args)...)]() mutable
             -> ReturnType {
    return std::apply(std::move(fn), std::move(tup));
};
```

**两个必须写清的细节**：

1. 返回类型必须写 `std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>`。
   因为 `make_tuple` 已经 decay 了参数，实际调用签名是 `decay_t<Args>&&`，
   不是原始的 `Args`。
2. **这是一条行为回退**：`std::bind` 支持、新方案不支持的场景是
   「形参为 `int&` 的可调用对象」—— `std::move(tup)` 传右值，绑不上左值引用。
   逃生口：调用方显式用 `std::ref(x)`，`make_tuple` 会把 `reference_wrapper<T>`
   解包成 `T&`。此项需进 §10 破坏性变更清单。

### 4.5 关闭语义

```cpp
void shutdown();       // 停止接收新任务，排空队列后 join
void shutdown_now();   // 停止接收新任务，丢弃未开始的任务，join
~ThreadPool();         // 调用 shutdown_now()
```

**析构用 `shutdown_now()` 而非 `shutdown()`**：析构不应无限期阻塞。
若队列里积压上千任务，`shutdown()` 会把进程退出拖很久；异常展开路径上更糟。
需要优雅排空的调用方显式调 `shutdown()`。

**两个必须记录的行为后果**（与现行为不同，得写进 §10 与 §14）：

1. **`submit()` 的等待方会拿到 `std::future_error(broken_promise)`**。
   丢弃一个未执行的 `packaged_task` 会破坏其 promise，`.get()` 抗异常而非阻塞。
   现行为（排空）下所有 future 都会正常兑现，改后不再成立。
2. **被丢弃的任务持有的裸资源会泄漏**。`http_server.cpp:251-253` 的任务
   按值捕获了 `client_fd`（裸 int），fd 的关闭发生在 `handle_client` 内部的
   `SocketGuard`。任务被丢弃 → `handle_client` 从未执行 → **该 fd 永不关闭**。
   当前只发生在进程退出时（由 OS 兜底），**不构成线上正确性问题**，
   但这是 `shutdown_now` 的固有代价。因此确立一条约束：
   **经 `submit_detached` 提交的任务不得按值捕获需要显式释放的裸资源**；
   若必须捕获，请用 RAII 类型捕获（如把 `SocketGuard` 上移到 lambda 捕获列表），
   以便任务被丢弃时自动释放。

**诚实说明**：两者都会等**正在执行**的任务跑完。标准 C++ 无法安全强杀线程，
所以「任务挂死导致进程无法退出」在池层面**无解**，只能靠任务自身可中断。
不提供 `shutdown(timeout)` 这种假承诺。

### 4.6 统计

```cpp
struct TaskStats {
    std::string name;
    std::size_t worker_count;
    std::size_t queue_capacity;
    std::size_t queue_pending;
    std::size_t peak_queue_pending;
    std::uint64_t submitted_total;
    std::uint64_t rejected_total;
    std::uint64_t completed_total;
    std::uint64_t task_exception_total;
    // 执行耗时：用于区分"排队久"与"执行慢"。现状只有排队深度，无法区分二者。
    std::uint64_t task_exec_us_total;
    std::uint64_t peak_task_exec_us;
};

TaskStats stats() const;
```

`ThreadPool` 不使用 `AcquireStats`（见 §7）。

**`task_exception_total` 会系统性低报**：`submit()` 路径的异常仍被 packaged_task
吞进 future，因此该 counter **只统计 `submit_detached` 提交的任务**。
指标名与 Prometheus HELP 文本必须写明这一点，
否则看板上的「异常数 0」会被误读为「没有异常」。

**不加排队等待耗时指标**（`task_queue_wait_us_total`）：它需要入队时打时间戳，
每任务多一次 `steady_clock::now()`。当前没有人拿这个数做决策，
`task_exec_us_total` + `completed_total` 已能算平均执行耗时，足够区分两类慢。
待有实际需求再加。

### 4.7 线程命名

worker 线程命名为 `<name>-<index>`，便于 lldb / Instruments / `ps -M` 定位。
平台差异：

- macOS：`pthread_setname_np(const char*)`（只能给当前线程命名，故在 worker 入口调用）
- Linux：`pthread_setname_np(pthread_t, const char*)`，名字上限 16 字节（含结尾零）
- Windows：`SetThreadDescription`

名字超长时截断，不报错。

## 5. 批 2：ObjectPool

### 5.1 接口

```cpp
template <typename T>
class ObjectPool {
public:
    using Ptr = std::unique_ptr<T, PoolDeleter<T>>;

    ObjectPool(std::string name, std::size_t init_size, std::size_t max_size);

    // 返回空 Ptr 表示已达 max_size 且无空闲对象（立即失败，不阻塞）。
    Ptr acquire();

    AcquireStats stats() const;
    const std::string& name() const;
};
```

`init_size == 0` 或 `max_size < init_size` → 抛 `std::invalid_argument`。

**移除变参 `acquire(Args&&...)`**（现状 `object_pool.h:74-75`）。理由：

1. 现有调用点**全部无参**（`http_server.cpp:268`；`test_pools.cpp` 共 10 处），不会破坏编译；
2. 带参时无法走 `reset()` 保留 capacity 的快路径（`object_pool.h:141` 的注释自己已说明），
   与池化初衷相悖。

连带简化：`reset_object` 里 `sizeof...(Args) == 0` 的分支（`object_pool.h:131-143`）整段删除。
同时要更新 `object_pool.h:34-35` 的用法注释（当前写的是 `pool.acquire(...)`）。

### 5.1.1 调用方必须判空（新引入的契约）

`acquire()` 现在可能返回空 `Ptr`，而 `http_server.cpp:268-269` 是：

```cpp
auto ctx = request_pool_.acquire();
ctx->request_id = request_id;      // 无任何判空
```

**必须同步修改为**：

```cpp
auto ctx = request_pool_.acquire();
if (!ctx) {
    // 与 DB 池耗尽一致：503 + 计入 error_requests_
    send_minimal_503(guard.get());
    return;
}
```

**为何这不能靠「该路径不可达」免掉**：初稿用「`max_size ≥ worker_count` 所以不可达」
论证过可以不判空。经核实，**该前提当前不成立**：
`main.cpp:76` 的 `kRequestPoolSize` 是常量 `128`，与 `worker_count` 无关；
`worker_count` 来自 `argv[2]`（`main.cpp:91`），`README.md:225` 还建议用户传 `32`，
用户完全可以传 `200`。

因此 §9 的默认值 `max(128, worker_count * 2)` **必须真的落到 `main.cpp:76,98`**（按
`worker_count` 计算，而非写死 128），并且即便如此，调用方仍**必须无条件判空** ——
因为这个不变式由**运行期配置**维持，不是类型保证。
不判空的后果是确定性的空指针解引用崩服务，比回 503 严重一个数量级。

### 5.2 无状态 deleter 替换 `std::function`

现状（`object_pool.h:47`）`Ptr = std::unique_ptr<T, std::function<void(T*)>>`，
每次 `acquire` 构造一个捕获 `this` 的 `std::function`。
捕获单个指针通常命中小对象优化，但**标准不保证**，理论上每次 acquire 可能堆分配。
这与「池化就是为了避免分配」的初衷直接冲突。

```cpp
template <typename T>
class ObjectPool;   // 前置声明

template <typename T>
struct PoolDeleter {
    ObjectPool<T>* pool{nullptr};
    void operator()(T* p) const {
        if (pool != nullptr && p != nullptr) {
            pool->release(p);
        }
    }
};
```

收益：消除 acquire 路径上的潜在堆分配；`unique_ptr` 尺寸从
「指针 + std::function」缩小为「指针 + 指针」。

### 5.3 修复 acquire 的异常安全窗口

现状（`object_pool.h:89-98`）：`++in_use_` 在锁内完成，但随后的
`reset_object()` 与 `Ptr` 构造在**锁外**且都可能抛异常
（`*obj = T(...)` 的 string/vector 分配、`std::function` 的堆分配）。
一旦抛出：`obj` 泄漏、`in_use_` 永久漂移，析构断言会把这笔账记到无关调用者头上。

修复：

```cpp
Ptr acquire() {
    T* obj = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!free_list_.empty()) {
            obj = free_list_.front();
            free_list_.pop();
        } else if (total_size_ < max_size_) {
            obj = new T();          // 可能抛：此时尚未 ++in_use_，无需回滚
            ++total_size_;
        } else {
            ++acquire_failed_total_;
            return Ptr{};           // 已达上限且无空闲 → 立即失败
        }
        ++in_use_;
        if (in_use_ > peak_in_use_) {
            peak_in_use_ = in_use_;
        }
    }

    // 锁外重置。任何异常都必须把对象与计数回滚，否则计数永久漂移。
    try {
        reset_object(obj);
    } catch (...) {
        // 销毁而不是放回 free_list：见下方说明。
        {
            std::lock_guard<std::mutex> lock(mutex_);
            delete obj;
            --total_size_;
            --in_use_;
        }
        throw;
    }
    return Ptr{obj, PoolDeleter<T>{this}};   // 无状态 deleter，构造不分配、不抛
}
```

`Ptr` 构造改用无状态 deleter 后本身不再可能抛，因此只需保护 `reset_object`。

**为何 catch 里销毁对象而不是 `release(obj)` 放回池**：
抛出可能发生在 `reset_object` **中途**（例如 `*obj = T(...)` 的赋值抛出），
此时对象处于**部分赋值的不确定状态**。若放回 free_list，下一个调用方会拿到它；
虽然下次 `acquire` 会再 `reset_object` 一次（通常能修复），但若 `T` 的赋值
不满足「失败后仍处于有效状态」，污染就传给了无关调用方。
销毁同时也与 §6.3 对坏连接的处理（销毁 + 重建）保持一致。

代价：`total_size_` 递减，下次 acquire 会重新 `new`。这是可接受的 ——
`reset()` 抛异常本身就是异常路径，不在热路径上。

### 5.4 统计读取：一次锁取一致快照

现状四个 getter（`object_pool.h:101-123`）各加一次锁，
`/api/v1/stats` 一次调用要抢 4 次锁。

**决定：`stats()` 加一次锁，一次性读出全部字段**，而不是改成四个独立 atomic。

理由：若各字段分别读 atomic，会出现 `in_use + available > size` 这类
**物理上不可能的不自洽快照**（某线程刚 `++in_use_` 尚未 pop free_list 的窗口内被读到），
让看指标的人困惑。一次锁 vs 四次锁已经拿到了主要收益，
而 `/api/v1/stats` 是低频端点、不在热路径上，一致性优先。

保留旧 getter（`in_use()` / `available()` / `total_size()` / `peak_in_use()`）作为薄封装，
内部转调 `stats()`。这样 `http_server.cpp` 的 20+ 个调用点与现有测试断言不用动。

### 5.5 Release 下的失效信号（两个池都要加）

现状析构断言（`object_pool.h:64`）在 `NDEBUG` 下是空操作，
悬垂句柄变成 UB 且无任何提示。

**`DbConnectionPool` 有完全相同的问题**：`db_connection_pool.cpp:54-59` 的
`assert(in_use_conns_.empty())` 同样在 `NDEBUG` 下失效（它的注释 `:57` 自己写着
「仅在 Debug 模式下生效，Release 版本零开销」）。
§2 声称「三池统一」，这一项**必须两个池都做**，否则就是只做了一半。

实现：抽一个共用小工具放在 `pool_stats.h`：

```cpp
// 在析构里报告未归还资源。不抛异常（析构函数），不 abort（Release 下应可继续退出）。
void warn_leaked(const char* pool_kind, const std::string& name, std::size_t count) noexcept;
```

`assert` 保留（Debug 下 abort，便于定位），同时无论构建类型都向 `stderr` 写一行警告：

```
[cpppools] ObjectPool("request") destroyed with 3 objects still in use
```

## 6. 批 3：DbConnectionPool

### 6.1 ConnectionFactory 抽取

```cpp
using ConnectionFactory = std::function<std::unique_ptr<DbConnection>()>;

DbConnectionPool(std::string name,
                 ConnectionFactory factory,
                 std::size_t pool_size,
                 std::chrono::seconds idle_probe_threshold = std::chrono::seconds{30});
```

**为何现在就抽**：健康检查失败后需要**重建**连接，重建就必须知道「如何创建一条连接」。
若直接依赖 `DbPoolConfig` 写死创建逻辑，DB 中间件改造时这段代码要重写。
抽成 factory 后，**创建逻辑无需在池内重写**。

这不是投机式设计 —— DB 改造是**已确定的下一阶段**，属已知需求。

**不要把收益夸大成「池代码不动」**（初稿如此写过，不成立）：
`ConnectionFactory` 把 `DbConnection` 焊进了类型里。改造时元素类型要换成 `Database`
（见中间件设计 §11 第 3 期），连带 `db_connection_pool.h:86`（`queue<DbConnection*>`）、
`:89`（`unordered_set<DbConnection*>`）、`:99`（`vector<unique_ptr<DbConnection>>`）、
`acquire()` 返回类型、`DbConnGuard::get()`（`:122`）全部要改。
真正的收益是「**创建逻辑与配置解耦**」，不是池代码零改动。
（若要真做到零改动，唯一办法是把池模版化为 `ConnectionPool<T>` ——
那属于 §2 明确不做的范围。）

### 6.1.1 `DbConnection::healthy()` 改为虚函数

现状：`db_connection.h:39` 是 `bool healthy();`（**非虚**），
`db_connection.h:30` 的 `~DbConnection();` 也**非虚**。

后果：即使测试写一个 `class FakeConn : public DbConnection` 并「重写」`healthy()`，
池通过 `DbConnection*` 调用的仍是基类版本（mock 模式下 `db_connection.cpp:74-77`
恒返回 true）——**§6.3 的重建路径永远测不到**，而那是批 3 最绕的代码。

**决定：`healthy()` 改为 `virtual`，并给 `DbConnection` 加虚析构。**

代价：`DbConnection` 从聚合类型变为多态类型（多一个 vptr）。
收益：除了解锁测试，这与中间件设计的 `Database` NVI 基类
（`virtual bool healthy() = 0`）**方向天然一致**，改造时不会白做。
有了虚析构后，`all_conns_` 里的 `unique_ptr<DbConnection>` 删除也变得安全。

需进 §10 破坏性变更清单。

### 6.1.2 factory 的约束（必须写清，否则易写出悬垂）

factory 在重建时由**池内部、worker 线程**调用，因此：

1. **必须线程安全且可重入**。
2. **连接 `id` 的生成**：现状是构造循环里的 `i + 1`（`db_connection_pool.cpp:33`）。
   重建时池传入**槽位编号**作为 id，保证日志与指标里的 id 稳定：
   `ConnectionFactory = std::function<std::unique_ptr<DbConnection>(std::size_t slot_index)>`。
3. **`MockScoreStore` 的归属发生转移**。现状它是池的成员，且
   `db_connection_pool.h:95-97` 有一段注释专门解释「声明顺序故意放在 `all_conns_` 之前，
   保证 store 比连接活得更久」。factory 化后 store 必须挑到 factory 的闭包里
   （即 `main.cpp` 或测试作用域），**这个生命周期约束从「类内声明顺序保证」
   退化成「调用方自觉」**。必须在文档与 `main.cpp` 注释里显式记录：
   **store 必须比池活得久**，否则很容易写出「lambda 捕获局部 store 的引用、
   store 先于池析构」的悬垂。

### 6.2 启动失败：保持现有 fail-fast

**这是现状，不是新增能力**（初稿写成新工作项，虚增了工作量）：
`db_connection_pool.cpp:20-52` 的构造函数循环内直接 `make_unique<DbConnection>(...)`，
而 MySQL 模式下 `db_connection.cpp:44 / 58 / 61` 三处都 `throw std::runtime_error`，
异常直接穿出池构造函数；`main.cpp:118-121` 捕获后退出。

**本批的实际工作只有两项**：

1. 补上对应的**回归测试**（这个确实该加 —— 现在一条都没有）；
2. 明确 factory 抛出时池处于安全状态：已构造的连接由 `all_conns_` 的 `unique_ptr`
   正常析构，无泄漏。

**理由（保持不变）**：一个启动了但 0 条可用连接、对所有请求回 500 的中间件，
比一个拒绝启动并打印清楚错误的中间件更糟。配置错误应在部署时暴露。

不提供 `min_ready` 部分容忍，也不做完全 lazy 建连。

### 6.3 健康检查：仅空闲超阈值时探活

#### 6.3.1 先引入槽位类型（否则「保留槽位」无处可放）

初稿说「重建失败保留槽位」，但现有三个容器里**没有任何东西能表示「空槽」**：
连接一旦从 `free_conns_` 弹出（`db_connection_pool.cpp:69`），
就只存在于调用者手里和 `in_use_conns_`；销毁后指针就消失了。
后果：`free_conns_` 永久少一个，全部槽位失败后 `free_conns_` 恒空 →
`acquire` 永远超时 → **再也不会触发任何重建尝试**，直接推翻了「自愈」的设计目标。

**决定：引入显式槽位类型，并用 `PooledConn*` 作为池内唯一身份标识。**

```cpp
struct PooledConn {
    std::unique_ptr<DbConnection> conn;   // 为 nullptr 表示空槽，待重建
    std::chrono::steady_clock::time_point last_released_at{};
    std::size_t slot_index{0};
};

std::vector<std::unique_ptr<PooledConn>> slots_;   // 取代 all_conns_，地址稳定
std::queue<PooledConn*>                  free_conns_;
std::unordered_set<PooledConn*>          in_use_conns_;
```

重建失败时把 `conn` 置空后**仍将 `PooledConn*` 推回 `free_conns_`** ——
槽位仍可被取出→再次尝试重建，自愈路径成立。

这个结构顺带解决三个问题：

1. **两容器一致性**：`free_conns_` 与 `in_use_conns_` 都存 `PooledConn*`，
   指针在整个池生命周期内**稳定**，销毁/重建内层 `conn` 不影响身份。
   不再需要「在 vector 里线性查找并替换 unique_ptr + 同步 erase/insert 两个集合」。
2. **`last_released_at` 的存放与同步**：放在槽位里，在 `release()` 与 `acquire()`
   的临界区内读写，**天然由 `mutex_` 保护，无需 atomic**。
   若放进 `DbConnection`，既污染连接类型（正是 §6.6 批评过的那类分层问题），
   又会在中间件改造删除 `DbConnection` 时一起没。
3. **指针复用 ABA**：`delete` 旧连接后 `new` 出的新连接可能落在同一地址，
   若旧指针被延迟归还会被误判为合法，把 §6.4 想提高的可观测性反向削弱。
   用稳定的 `PooledConn*` 做身份可规避。
   （此项为**推测**：无法从现有代码证明一定会发生，但新结构免费规避了它。）

#### 6.3.2 acquire 流程

```
deadline = steady_clock::now() + timeout        // 绝对 deadline，不是每次重算
loop:
  持锁：cv_.wait_until(lock, deadline, [] { return !free_conns_.empty() || stop_; })
         超时且仍无空闲 → return nullptr
         取出 slot = free_conns_.front(); pop; in_use_conns_.insert(slot)
         读取 needs_probe = (slot->conn == nullptr) ||
                             (now - slot->last_released_at >= idle_probe_threshold)
  释锁
  若 !needs_probe → return slot->conn.get()        // 热连接，零开销
  锁外执行：
     若 slot->conn 存在且 healthy() 成功 → return slot->conn.get()
     否则 → slot->conn.reset();  slot->conn = factory(slot->slot_index)
              成功 → return slot->conn.get()
              失败 → ++rebuild_failed_total_；持锁把 slot 推回 free_conns_
                      并从 in_use_conns_ 移除；若 now >= deadline → return nullptr
                      否则 continue loop
```

**探活与重建必须在锁外**。理由：`healthy()` 在 MySQL 下是 `mysql_ping`
（`db_connection.cpp:79`），一次网络往返，死连接上可能耗到 TCP 超时级别。
若在 `mutex_` 内调用，**整个池被独占阻塞**，所有其他线程的 acquire/release 全部排队，
比不做探活坏得多。锁外安全的依据：该 slot 已登记在 `in_use_conns_`，
对其他线程不可见。

**超时是含探活与重建的总上界**。初稿写成循环但没说 deadline，
若每次重新 `wait_for(timeout)`，最坏总耗时是 `N × timeout + N × ping_timeout`。
而 HTTP 层依赖这个上界：`http_server.h:54` 默认 `db_acquire_timeout{100}` ms、
`score_api.cpp:123-126` 靠它在 100ms 内回 503。上界一破，
`test_score_api.cpp:117-129` 的语义和线上 P99 一起坏掉。
探活本身也要能被 deadline 截断（否则 `mysql_ping` 的默认超时可能远超 100ms）。

**为何不每次 acquire 都探活**：MySQL 上 `mysql_ping` 是一次网络往返。
对单记录 CRUD（本身只有 1–2 次往返）而言，开销占比过高。
「仅空闲超阈值才探活」是 HikariCP 的做法：热连接零开销，冷连接才付一次往返。

**为何重建失败保留槽位而非永久标死**：数据库重启、网络抖动都是瞬时故障，
保留槽位可让池在故障恢复后自愈。若全部槽位都不可用，`acquire` 超时返回 `nullptr`
→ 上层回 503，行为与「池耗尽」一致。

### 6.4 非法归还可观测

现状（**`db_connection_pool.cpp:88-92`**）对重复归还/野指针归还直接 `return`：

```cpp
88:  const auto it = in_use_conns_.find(conn);
89:  if (it == in_use_conns_.end()) {
90:      // 拒绝重复归还或外部非法连接，保持池状态一致。
91:      return;
92:  }
```

无计数无日志，bug 完全不可观测。改为：计入 `invalid_release_total`，
并向 `stderr` 写一行警告（含池名与指针值）。
仍不抛异常 —— 归还路径通常在析构函数中，抛异常会引发更严重的问题。

**关键：`conn == nullptr`（`db_connection_pool.cpp:83-85` 的 `if (!conn) return;`）
不得计入 `invalid_release_total`。**
初稿错误引用了 `:80-83`，把这条**合法路径**当成了目标 ——
它是 `DbConnGuard::~DbConnGuard`（`db_connection_pool.cpp:114-117`）在没拿到连接时依赖的路径。
若计入，`test_score_api.cpp:117-129`（池耗尽 503 测试）每次超时都会刷一行假告警。

### 6.5 `DbConnGuard` 移动语义（实施中确定：**做**，但需把 `pool_` 改指针）

初稿曾以「无法从函数返回一个 guard」为由计划加移动语义，后经核实该理由不成立：

1. `CMakeLists.txt:8` 确认 C++17。C++17 起 prvalue 返回是**强制拷贝省略**，
   `return DbConnGuard(pool, timeout);` 无需移动构造即可编译。
2. `DbConnGuard::pool_` 原是**引用成员**。引用无法重新绑定，
   因此**移动赋值根本无法实现**（会被隐式删除）。

**最终决定：实现移动语义**（移动构造 + 移动赋值），以支持「从函数返回具名 guard」
与「放入容器」两个真实场景；同时把 `pool_` 从引用改为指针（这是移动赋值的先决条件）。
移动后源 guard 置空（`pool_ = nullptr`），析构判空，防止双重归还。见 `test_db_pool.cpp`。

注：初稿核实发现全仓现有 5 处 `DbConnGuard` 用法（`score_api.cpp:123,138,157,172` 等）
均为局部变量，当下无调用方依赖移动语义；但 DB 中间件改造后 `DataSourceRegistry`
可能需要在容器中管理 guard，故提前补齐，成本极低。

### 6.6 `DbPoolConfig::table` 不在本次移除

初稿曾计划把 `table` 当作“分层污染的死字段”删除。**该判断是错的**，经核实：

| 位置 | 用途 |
|---|---|
| `src/db/db_connection_pool.cpp:39` | 传给 `DbConnection` 构造 |
| `src/db/db_connection.cpp:40` | 存为 `table_` |
| `src/db/db_connection.cpp:134,169,206,245` | **四条 MySQL SQL 语句全部使用** |
| `src/main.cpp:56` | 从 `CPPPOOLS_DB_TABLE` 读入 |
| `src/main.cpp:106` | 启动横幅打印 |

删除它会直接搞坏 MySQL 后端。

它确实是分层污染（连接池配置不应携带业务表名），但正确的治理时机是
**DB 中间件改造**——那时 `db_connection.cpp` 的 score-specific SQL 会整体删除，
表名改为每请求传入。现在动它是对即将被删的代码做无效重构。

**本批不动 `table` 字段。**

## 7. 批 4：统一统计

```cpp
namespace cpppools::pool {

// ObjectPool 与 DbConnectionPool 共用：两者都是"借 → 还"语义。
struct AcquireStats {
    std::string name;
    std::size_t capacity;        // 上限（max_size / pool_size）
    // 已创建/已建立的元素数。必须与 capacity 分开 —— 二者是不同的量：
    //   ObjectPool：已 new 出的对象数（≤ capacity，按需增长）
    //   DbConnectionPool：当前存活连接数（重建失败时 < capacity，即池的退化程度）
    std::size_t size;
    std::size_t in_use;
    std::size_t available;
    std::size_t peak_in_use;
    std::size_t min_available;           // 历史最小空闲（批 4 新增，原清单遗漏）
    std::uint64_t acquire_total;
    std::uint64_t acquire_failed_total;   // 拒绝 + 超时；reset 抛异常不计入（口径固化）
};

}  // namespace cpppools::pool
```

> **批 4 裁剪记录**：原清单里的 `acquire_wait_us_total`（累计等待微秒）**未实现** ——
> 当前没有任何消费方需要平均等待指标，属 YAGNI；将来有需求再加。
> 另：`AcquireStats` 最终留在 `object_pool.h`（未单独建 `pool_stats.h`，
> 见 §12 批 4 的实施偏差记录）；`warn_leaked` 也未抽成共用工具，
> 两个池的析构警告各自内联 fprintf（只有两处，不值得抽）。

**`size` 字段不可省**（初稿漏了）：对 ObjectPool，`capacity`（= `max_size`）与
`total_size`（当前已 new 出的对象数）是**两个不同的量**，而现有可观测面在用后者：

| 消费点 | 用途 |
|---|---|
| `http_server.cpp:467` | `cpppools_object_pool_total` |
| `http_server.cpp:505` | `/api/v1/stats` 的 `memory_pool_total` |
| `http_server.cpp:531` | `/api/v1/config` 的 `memory_pool_total` |

若不加 `size`，这三个指标**无法从其余字段重建**，只能哑火或语义偷换
（把 `capacity` 当 total 输出 → 128 变成一个恒定值，「池实际长到多大」这个信息永久丢失）。

对连接池，`size` 还额外有用：§6.3.1 的空槽使存活连接数可能小于 `pool_size`，
`size` 正好是暴露「池当前退化程度」的指标，比 `rebuild_total` 直观。

**不自洽快照的处理**：见 §5.4 —— `stats()` 加一次锁取一致快照，
而不是各字段分开读 atomic。否则会出现 `in_use + available > size` 这类物理上不可能的数。

`DbConnectionPool` 额外暴露 `invalid_release_total`、`rebuild_total`、`rebuild_failed_total`
（不塞进 `AcquireStats`，因为它们是连接池特有的）。

**旧 getter 基本保留为薄封装**（`pending_tasks()` / `peak_pending_tasks()` /
`free_count()` / `total_count()` / `in_use()` / `available()` /
`total_size()` / `peak_in_use()`），内部转调 `stats()`。

> **批 4 实施偏差**：`min_free_count()` **被删除**而非保留 ——
> 其语义收编进 `stats().base.min_available`（历史最小空闲），
> 仓内调用点同步改为快照字段（当时 grep 确认无残留调用）。

**例外（批 1 实施后确定）**：`ThreadPool::pending_tasks()` 与 `peak_pending_tasks()`
**不转调 `stats()`**，而是直接读 atomic，保持无锁。
同时 `ThreadPool::stats()` 本身也是完全无锁的（`queue_pending` 读 `pending_count_`
而不是 `tasks_.size()`，两者在临界区内恒等）。
**批 4 不得把这两个 getter 改成取锁**，否则会静默回归批 1 刚建立的无锁读。
ObjectPool 的 `stats()` 仍按 §5.4 取一次锁（它有 `in_use + available == size` 这个
等式不变式需要保护，线程池没有）。

理由：`http_server.cpp` 有 20+ 个调用点（`:222,224,449,452,455,458,461,464,467,485,
496,499-506,527,528,531`），`test_score_api.cpp:114,140` 与 `test_pools.cpp` 多处有断言。
保留薄封装可避免为了换统计形式而改一大批与本次改动无关的代码；
且 `stats()` 返回整个结构体，只想读一个字段时写法啰嗦。

**ThreadPool 不使用 `AcquireStats`**。它的「in_use」是排队任务数，
与「借出资源数」语义不同；把 `max_queue_size` 映射成 `capacity`、
`pending` 映射成 `in_use` 是**错误建模**，会让看指标的人误解。
命名约定统一（counter 带 `_total`、峰值带 `peak_`、等待带 `_wait_us`），
但形状按语义分两类，并在 README 说明为何不同。

`/api/v1/stats` 与 `/metrics` 按 `name` 打标签输出，例如：

```
cpppools_pool_in_use{pool="request"} 1
cpppools_pool_in_use{pool="main-db"} 3
cpppools_thread_pool_queue_pending{pool="worker"} 0
```

**指标名直接换为带 `pool` 标签的新名，不双写旧名**。
依据：仓内 grep 确认**无任何非文档消费者**（无 py/js/json/yaml 引用旧指标名）；
`http_server.h:67` 那句「客户端由独立的 SDK 承担」是对**计划中** Python SDK 的前瞻描述，
不是「已有 SDK 在消费指标」的证据。无消费者则无契约可破。

`docs/api/backend_api.md` 的指标表（`:143-153`）与 stats 字段表（`:172-184`、`:195-198`）
需同步重写，列入 §12 批 4。

**为何现在就加 `name()`**：DB 中间件改造后会有 **N 个连接池**（每数据源一个），
`cpppools_db_pool_free` 这个指标必须能区分是哪个数据源的，否则完全无意义。
现在加成本极小；等改造时再加要同时改三个池的构造签名 + 所有指标输出 + 所有测试。

## 8. 已确认的决策（15 条）

前 11 条为设计评审前确认：

1. 三类问题**全做**（真缺陷 + 生产级特性 + 三池统一）—— 这是 DB 改造的前置条件
2. 耗尽策略统一为「有界」，但「等不等」分两种，各有理由（§3）
3. `submit` 返回 `std::optional<std::future<R>>`；已关闭抛 `PoolShutdownError`
4. 双入口 `submit` / `submit_detached` 解决异常可见性
5. 连接健康检查：**仅空闲超阈值时探活**（HikariCP 风格）
6. 统计形状**按语义分两类**（`AcquireStats` + `TaskStats`），不强行统一
7. 批 3 现在做，但**抽出 `ConnectionFactory`**，避免 DB 改造时重写
8. `~ThreadPool()` 调 `shutdown_now()`（丢弃未开始的任务）
9. ObjectPool 到 `max_size` **立即失败**，不做阻塞等待
10. **不抽共同基类**，ObjectPool 与 DbConnectionPool 只共用 `AcquireStats`
11. 启动时连接创建失败 → **fail-fast**，不做部分容忍（已是现状，见 §6.2）

后 4 条为代码评审后新增确认：

12. **`healthy()` 改为虚函数 + 加虚析构**（§6.1.1）—— 不改则 §6.3 的重建逻辑无法测试
13. **任务队列元素换为 `std::packaged_task<void()>`**（§4.3）——
    `std::function` 要求可拷贝，move-only 捕获物存不进去
14. **旧 getter 基本保留为薄封装**（§7）—— 避免改 20+ 个无关调用点；例外：`min_free_count()` 于批 4 删除、收编进 `stats().base.min_available`
15. **指标直接换新名，不双写**（§7）—— 仓内 grep 确认无消费者

评审后**砍掉**的一项（原本列在计划内）：

- 移除 `DbPoolConfig::table` —— 它是活字段，删了会搞坏 MySQL 后端（§6.6）

（`DbConnGuard` 移动语义最终**做了**，见 §6.5。）

## 9. 默认值

| 项 | 默认 | 依据 |
|---|---|---|
| `ThreadPoolConfig::max_queue_size` | 1024 | 足以吸收突发，又不至于在过载时堆积过多内存 |
| `ObjectPool max_size` | `max(128, worker_count * 2)` | ≥ worker_count 保证正常路径不会失败；×2 留余量 |
| `idle_probe_threshold` | 30s | 短于常见 MySQL `wait_timeout`（默认 28800s）与中间设备空闲断连阈值 |
| `PoolShutdownError` | 新异常类型 | 替掉泛用 `std::runtime_error`，便于调用方精确捕获 |
| 重建失败处理 | 保留槽位，下次 acquire 再试 | 瞬时故障自愈 |

**默认值不能只写在文档里**：`ObjectPool max_size` 必须在 `main.cpp:76,98`
**按 `worker_count` 计算**，不得继续写死常量 128（见 §5.1.1）。

## 10. 破坏性变更

### 10.1 编译期破坏（不改就编不过）

| 变更 | 受影响位置 |
|---|---|
| `submit` 返回 `optional` | **实际 7 处调用**（不是“约 10 处”）：`http_server.cpp:251` + `test_thread_pool.cpp:15,16,30,53,69,74`。其中 **`:30` 与 `:74` 是 `futures.emplace_back(pool.submit(...))`**，`emplace_back(optional<future<T>>)` 直接编译失败，需改为 `emplace_back(std::move(*pool.submit(...)))` 或先判 `has_value()` |
| 任务队列元素换为 `packaged_task<void()>` | `thread_pool.h:74`、`thread_pool.cpp:41,51,52` |
| `ThreadPool` 新增 config 构造 | 保留了 `explicit ThreadPool(std::size_t)` 便利构造，所以 `main.cpp:97` 与 `test_thread_pool.cpp:14,23,48,63` **可不改** |
| `ObjectPool` 构造新增 `name` + `max_size` | `main.cpp:98`、`http_server.h/cpp` 成员初始化、`test_pools.cpp` 10 处 |
| `ObjectPool::acquire` 移除变参 | 现有调用点全为无参，**不破坏编译**；仅需删 `reset_object` 的变参分支与 `object_pool.h:34-35` 注释 |
| `DbConnectionPool` 构造改为 `(name, factory, pool_size, idle_probe_threshold)` | `main.cpp:96-101`；**`test_score_api.cpp` 共 10 处 `DbConnectionPool pool(...)`（`:34,45,56,69,81,95,108,118,132,144`）**，另有 `pool.free_count()` `:114,140` 与 `pool.acquire/release` `:120,128`；`test_pools.cpp` 多处 |
| `mode_name()` / `using_mysql()` / `config()` 失去数据源 | 新构造不再接收 `DbPoolConfig` → `config_` 无数据。调用点：`http_server.cpp:222,496,527`、`main.cpp:103`。**处理：加一个 `std::string backend_name` 构造参数供 `mode_name()` 返回**，保住 `/api/v1/stats` 与 `/api/v1/config` 的 `db_mode` 字段，不破坏对外 API |
| `DbConnection::healthy()` 改虚 + 加虚析构 | `db_connection.h:30,39`。`DbConnection` 从聚合类型变为多态类型 |
| 测试文件新增与删除 | **`CMakeLists.txt:186-190`** 的 `cpppools_add_test()` 注册列表必须同步（新增 `test_object_pool`/`test_db_pool`，删除 `test_pools`）|

### 10.2 行为破坏（能编译但行为变了）

| 变更 | 后果 |
|---|---|
| `~ThreadPool()` 改调 `shutdown_now()` | 被丢弃任务的 future 等待方从「拿到结果」变为抛 `std::future_error(broken_promise)`（见 §4.5）|
| `submit_detached` 任务不得捕获裸资源 | 新确立的约束（见 §4.5）。`http_server.cpp:251` 当前按值捕获 `client_fd`，退出时依赖 OS 兜底 |
| `std::bind` → lambda + `std::apply` | **形参为 `int&` 的可调用对象不再可直接提交**，需显式 `std::ref(x)`（见 §4.4）|
| `/metrics` 指标改名 | `cpppools_object_pool_*` / `cpppools_db_pool_*` / `cpppools_thread_pool_*` → 带 `pool` 标签的新名，**不双写**（无消费者，见 §7）|
| `http_server.cpp:268` 必须判空 | 不改则配置不当时空指针解引用崩服务（见 §5.1.1）|
| `docs/api/backend_api.md` | 指标表 `:143-153`、stats 字段表 `:172-184`、示例 `:195-198` 需重写 |

## 11. 测试策略

`test_pools.cpp` 拆分：

| 文件 | 批次 | 覆盖 |
|---|---|---|
| `test_thread_pool.cpp` | 批 1 | 队列满返回 `nullopt`、关闭后两个入口都抛 `PoolShutdownError`、`submit_detached` 异常回调被调用、**回调自身抛异常后池仍存活**、move-only 参数可提交、`shutdown` 排空 vs `shutdown_now` 丢弃、**`shutdown_now` 后 future 抛 broken_promise**、执行耗时统计非零 |
| `test_object_pool.cpp` | 批 2（新） | RAII 归还、`reset()` 保留 capacity、达 `max_size` 返回空 `Ptr`、`reset` 抛异常时计数回滚且对象被销毁、峰值统计、**`init_size==0` 与 `max_size<init_size` 抛 `invalid_argument`**、**析构时 stderr 警告（Release 下也输出）** |
| `test_db_pool.cpp` | 批 3（新） | 超时返回 `nullptr`、重复归还计入 `invalid_release_total` 而 **空指针归还不计入**、空闲超阈值触发探活、`healthy()` 失败触发重建、**重建失败→保留槽位→下次 acquire 重建成功（自愈）**、**acquire 的 timeout 是含探活与重建的总上界**、构造期失败 fail-fast、析构时 stderr 警告 |
| `test_db_connection.cpp` | 批 3（新） | **从 `test_pools.cpp` 迁入三个不属于“池”的用例**：`test_crud_state_is_shared_across_connections`（`:117-142`）、`test_crud_state_is_isolated_between_pools`（`:145-156`）、`test_crud_failure_paths`（`:158-172`）|
| `test_pools.cpp` | 批 3 结束 | 删除（**内容已全部迁出，不得静默丢弃**）|
| `test_metrics.cpp` | 批 4（新） | **`name()` 出现在 `/metrics` 标签中**。加 `name()` 的全部理由就是指标可区分（§7），必须有测试守护；`rebuild_total` / `task_exec_us_total` 等计数正确；旧指标名已移除；HELP/TYPE 必须先于样本行（结构扫描）|

关键测试设计说明：

- **`reset` 抛异常的计数回滚**：需一个 `reset()` 会抛的测试类型，
  断言异常传播后 `in_use() == 0`、`total_size()` 递减（对象被销毁而非回池）。
  这是 §5.3 修复的直接验证。
- **健康检查触发**：靠 §6.1.1 把 `healthy()` 改虚后，测试可定义
  `class FakeConn : public DbConnection` 重写 `healthy()`，并把 `idle_probe_threshold`
  设为 0 以强制探活。**若不改虚函数，这条测试写不出来**。
- **自愈路径**：factory 先返回 nullptr（重建失败）、后返回有效连接，
  断言第二次 `acquire` 成功且 `rebuild_total == 2`。这是批 3 最绕的一条。
- **`shutdown_now` 丢弃语义**：提交一个阻塞任务占住唯一 worker，再提交 N 个任务，
  调 `shutdown_now()`，断言那 N 个任务未被执行（用计数器验证）。

## 12. 实施顺序

每批一个提交。**每批的完成定义**：零告警编译 + 全部测试通过 +
**同批更新 README 对应小节与被改头文件的 doc 注释**。

最后一条不是洁癖：本项目头注释密度极高（是它的特色），注释腐坏的代价比一般项目大。
具体会失效的位置：

| 位置 | 批次后变假 |
|---|---|
| `README.md:114` | 「固定 worker + **无界任务队列**」→ 批 1 |
| `README.md:124,125` | 「预创建 + **按需扩容**」、「`unique_ptr<T, function<void(T*)>>`」→ 批 2 |
| `README.md:225` | 建议用户传 `32` worker，需配合 §5.1.1 的 `max_size` 计算说明 |
| `object_pool.h:34-35,38-42` | 用法注释与 `std::function` deleter 原理 → 批 2 |
| `thread_pool.h:47-48` | 「抛出 `std::runtime_error`」→ 批 1 |
| `db_connection_pool.h:52-53` | 「本项目保持最小核心，不做健康探活/失效重连」→ 批 3 |
| `db_connection_pool.h:95-97` | store 声明顺序保证 → 批 3（已退化为调用方自觉，见 §6.1.2）|

| 批 | 内容 | 文件 |
|---|---|---|
| 1 | ThreadPool 全部改动 | `pool/thread_pool.{h,cpp}`、`test_thread_pool.cpp`、`http_server.cpp`（改用 `submit_detached` + 503 背压）、`main.cpp`、`README.md`（线程池小节）|
| 2 | ObjectPool 全部改动 | `pool/object_pool.h`、`test_object_pool.cpp`（新）、`test_pools.cpp`（移除 ObjectPool 部分）、**`http_server.cpp:268`（判空）**、`http_server.h`、**`main.cpp:76,98`（max_size 按 worker_count 计算）**、**`CMakeLists.txt`**、`README.md` |
| 3 | DbConnectionPool 全部改动 | `db/db_connection_pool.{h,cpp}`、**`db/db_connection.h`（healthy 改虚 + 虚析构）**、`test_db_pool.cpp`（新）、**`test_db_connection.cpp`（新）**、`test_pools.cpp`（删除）、**`test_score_api.cpp`（10 处构造）**、`main.cpp`、**`CMakeLists.txt`**、`README.md` |
| 4 | 统一统计与指标接线 | 三池的 `stats()`、`http_server.cpp`、`test_metrics.cpp`（新）、**`docs/api/backend_api.md`**、`README.md`（新增统一统计章节 §3.4）。**另需在 `task_exception_total` 的 Prometheus HELP 文本里写明它只覆盖 detached（§4.6）**。实施偏差：未新建 `pool_stats.h`（`AcquireStats` 留在 `object_pool.h`）、未抽共用 `update_peak`（仅两处，不值得）——见 §7 裁剪记录 |

按池分批而非按关注点分批：每批 blast radius 锁在一个池，
测试天然按池组织，出问题容易定位。批 4 放最后收口，因为它依赖前三批确定各池最终有哪些指标。

## 13. 与后续 DB 中间件改造的衔接

| 本设计的产出 | 改造时的影响 |
|---|---|
| `ConnectionFactory` | 创建逻辑与配置解耦，**创建逻辑无需在池内重写**。但注意：改造时元素类型仍需从 `DbConnection` 换到 `Database`（中间件设计 §11 第 3 期），**不是「池代码不动」**（初稿如此写过，不成立，见 §6.1）|
| `healthy()` 改为虚函数 | **方向与中间件的 `Database` NVI 基类（`virtual bool healthy() = 0`）天然一致**，不会白做 |
| `PooledConn` 槽位结构 | 元素类型换成 `Database` 后，只需改 `PooledConn::conn` 的类型；路由/自愈/身份标识逻辑不动 |
| `name()` | 直接用于 N 个数据源池的指标标签 |
| `AcquireStats` | 不变；`DataSourceRegistry` 聚合 N 份 |
| `DbPoolConfig` | 改造时整体被 `DataSourceConfig` 取代，届时一并消掉 `table` 字段（本设计不动它，见 §6.6）|

statement cache 留到改造时做 —— 它依赖 prepared statement，而后者依赖 `Database` 抽象。

### 13.1 完成后需回写中间件设计文档

本设计会使 `2026-08-06-db-middleware-design.md` 的两处内容失效，完成后必须同步：

1. **它的 §12 测试表**：写着「`test_object_pool` 从 `test_pools` 拆出」、
   「`test_crud_api` 取代 `test_score_api`」。本设计批 3 已删除 `test_pools`
   并新建 `test_object_pool` / `test_db_pool` / `test_db_connection`，
   中间件设计的测试表需更新（它目前没有 `test_db_pool`）。
2. **它的 §16 已知限制**：写着「**无连接失效自动重建**：连接断开后 `healthy()`
   返回 false，但不会自动重连」。本设计 §6.3 正是要加自动重建，
   这条限制会被**提前消除**。

## 14. 已知限制

- **挂死任务无法中断**：`shutdown` / `shutdown_now` 都会等正在执行的任务结束。
  标准 C++ 无法安全强杀线程，池层面无解。
- **ObjectPool 的失败路径在 `max_size ≥ worker_count` 成立时不可达**，
  但**调用方必须无条件判空** —— 这个不变式由 `main.cpp` 的配置计算维持，
  属**运行期配置而非类型保证**。初稿写成「不可达，保留该路径是为将来预留」，
  容易让实施者省掉判空，已在 §5.1.1 纠正。
- **`stats()` 内的计数器不是无锁读**：为了给出自洽快照，`stats()` 加一次锁
  （§5.4）。这是一致性与无锁之间的有意取舍；`/api/v1/stats` 是低频端点，
  不在热路径上。
- **`task_exception_total` 只覆盖 `submit_detached`**：`submit()` 的异常仍由
  packaged_task 吞进 future。看板上该值为 0 **不等于没有异常**（§4.6）。
- **`shutdown_now` 会让未兑现的 future 抛 `broken_promise`**，且被丢弃任务
  持有的裸资源会泄漏（§4.5）。
- **健康检查有 TOCTOU 窗口**：探活通过后、真正执行 SQL 前，连接仍可能断开。
  这是所有连接池的固有限制，只能靠上层重试缓解。本设计不做自动重试。
- **`invalid_release_total` 只计数不阻止**：野指针归还已经发生时，
  池只能拒绝入队并记录，无法修复调用方的错误。
- **`MockScoreStore` 的生命周期保证发生退化**：factory 化后，
  它从「类内声明顺序保证」（`db_connection_pool.h:95-97`）退化为
  「调用方自觉保证 store 比池活得久」（§6.1.2）。这是抽取 factory 的真实代价。
- **单队列争用未处理**：所有 worker 抢同一把 `mutex_`。属已知的可扩展性上限，
  当前规模不构成瓶颈，需有测量证据后再优化。
- **静态析构期的 stderr 风险**：§5.5 的 `warn_leaked` 向 `stderr` 写入。
  当前三池均为栈对象（`main.cpp:97-101`），无风险；
  若将来改为静态对象，需确认静态析构期 `std::cerr` 仍有效。
- **`acquire` 的真实上界是 deadline + 一次已开始的探活/重建时长**（§6.3.2 初稿
  承诺「探活本身也要能被 deadline 截断」未实现）：`healthy()` 无超时参数，
  MySQL 下 `mysql_ping` / 重建时的 `mysql_real_connect` 无法中断，最坏会击穿
  HTTP 层「`db_acquire_timeout` 内回 503」的契约。依赖连接自身的超时配置缓解。
- **同一次 `acquire` 会尝试所有未试过的空闲槽位**（§6.3.2「继续取下一条空闲连接」）：
  多个空槽会在一次调用里各重建一次、各计一次 `rebuild_failed_total`。
  `tried_this_call` 只防「同一槽位原地重试」，不限制「不同槽位各试一次」。
  持续故障下重建频率 = 空槽数 × QPS，无退避。
  注：空槽全部试过后 acquire 走**条件变量挂起**等 deadline（整体评审修复：
  旧实现此处忙等待，实测 200ms 烧 98% CPU；回归测试
  `test_no_busy_wait_when_all_slots_failed` 用 rusage 守护）。
- **无 graceful stop**（整体评审 I-5 记录）：进程退出靠信号直接终止，析构链
  正常运行期不执行。将来若加停机流程，必须保持「先停 accept → 再 shutdown
  线程池（shutdown_now 会丢弃未开始任务，其捕获的 fd 无人 close，非停机场景
  是真泄漏）→ 最后销毁三个池」的顺序，并接受停机期连接走 PoolShutdownError
  分支的日志输出。
