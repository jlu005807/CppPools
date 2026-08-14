# CppPools — 单节点三级池化 API 服务

> C++17 / MySQL / Socket。一个把客户端 HTTP 请求经三级池化链路转发到后端真实数据库的单节点服务。

## 定位

CppPools 是**客户端与后端数据库之间的接入层**：

```
HTTP 客户端 (Python SDK)
      │
      ▼
┌─────────────────────────────────────────────┐
│ CppPools 单节点服务                          │
│                                             │
│  ThreadPool    ── 请求调度，accept 不阻塞     │
│       ▼                                     │
│  ObjectPool    ── 复用 RequestContext        │
│       ▼                                     │
│  DbConnectionPool ── 复用数据库连接 + 超时     │
└─────────────────────────────────────────────┘
      │
      ▼
   MySQL（或 mock 后端）
```

三级池化的意义：**把「昂贵资源的创建」与「请求的到来」解耦**。线程、请求上下文、
数据库连接都在启动期预建，请求路径上只做借还，并且借不到时以超时（`503`）显式失败，
而不是无限等待。

**当前是单节点实现**，但模块边界为后续演进到多节点留了空间（见「演进方向」）。

- 后端 API 契约：[docs/api/backend_api.md](docs/api/backend_api.md)
- 客户端：暂无，计划以独立 Python SDK 实现（本仓库只提供服务端）

---

## 1. 项目结构

```text
CppPools/
├── CMakeLists.txt                          # 构建入口
├── README.md
├── LICENSE                                 # MIT
├── .gitignore
├── .clang-format
│
├── include/cpppools/                       # 公开头文件（对外接口）
│   ├── pool/                               #   namespace cpppools::pool
│   │   ├── thread_pool.h                   #     固定 worker + **有界队列与 503 拒绝** + 双提交入口
│   │   └── object_pool.h                   #     泛型对象池（RAII 归还 + reset 复用）
│   ├── db/                                 #   namespace cpppools::db
│   │   ├── score_record.h                  #     领域模型 + 操作状态枚举
│   │   ├── db_connection.h                 #     连接抽象（mock / mysql 双后端）
│   │   ├── mock_score_store.h              #     mock 后端的共享存储
│   │   └── db_connection_pool.h            #     有界连接池 + DbConnGuard
│   └── server/                             #   namespace cpppools::server
│       ├── api_response.h                  #     响应模型 + HTTP 报文组装
│       └── http_server.h                   #     accept 循环 + 分发 + 遥测
│
├── src/                                    # 实现 + 私有头文件
│   ├── main.cpp                            #   服务进程入口
│   ├── pool/thread_pool.cpp
│   ├── db/
│   │   ├── db_connection.cpp
│   │   ├── mock_score_store.cpp
│   │   └── db_connection_pool.cpp
│   └── server/
│       ├── http_util.{h,cpp}               #   私有：零拷贝请求解析
│       ├── route_table.{h,cpp}             #   私有：constexpr 声明式路由表
│       ├── score_api.{h,cpp}               #   私有：4 个 CRUD handler
│       ├── api_response.cpp
│       └── http_server.cpp
│
├── test/                                   # 8 个 CTest 用例（见 §5）
├── third_party/                            # vendored 依赖（nlohmann/json 单头文件）
├── docs/
│   ├── api/backend_api.md                  #   API 契约
│   ├── mysql_setup.sql                     #   MySQL 初始化脚本
│   └── learning_log.md
├── scripts/
│   ├── build.sh                            #   构建 + 测试（Linux/macOS）
│   ├── run_server.sh                       #   启动服务（Linux/macOS）
│   └── run_server_mysql.ps1                #   启动服务（Windows）
└── build/                                  # 构建输出（已 gitignore）
```

### 头文件的公开 / 私有边界

- `include/cpppools/<模块>/` —— 对外接口。库的消费者与 `main.cpp` 只用这些。
- `src/<模块>/` —— 实现，以及**私有头文件**。
  `http_util` / `route_table` / `score_api` 是实现细节，不对外暴露；
  测试通过额外的 `-Isrc` 直接访问它们（测试相对普通消费者的合理特权）。

### 模块依赖

```
server ──> pool
   └─────> db
```

有向无环，`pool` 与 `db` 互不依赖，都只依赖标准库。

**为什么 `DbConnectionPool` 归在 `db/` 而不是 `pool/`**：它虽然是「三大池子」之一，
但构造时直接依赖 `DbConnection` 的两种后端形态，与连接抽象是一个内聚单元。
按依赖关系而非叙事口号来切分模块。

## 2. 三级池化实现

### 2.1 ThreadPool

[include/cpppools/pool/thread_pool.h](include/cpppools/pool/thread_pool.h)

- **有界任务队列**（默认 1024，`CPPPOOLS_MAX_QUEUE_SIZE` 可调）+ 拒绝策略，标准生产者/消费者模型。
  队列满时 `submit` 返回 `std::nullopt`，accept 循环**立即回 503** —— 真实背压，而非无限堆积。
- `std::mutex` + `std::condition_variable`，`wait(lock, predicate)` 防虚假唤醒。
- **两个提交入口**：
  - `submit(F, Args...)` → `std::optional<std::future<R>>`，异常由 future 携带；
  - `submit_detached(F, Args...)` → `bool`，异常交给 `on_task_exception` 回调 + 计数。

  为何必须两个：`packaged_task::operator()` 会把异常**存进 future 而不重抛**，
  调用方一旦丢弃 future（HTTP 层正是如此），异常就永久静默。

  注意 `task_exception_total` **只覆盖 `submit_detached`**：`submit()` 的异常被
  `packaged_task` 吞进 future，池看不到，因此该值为 0 **不等于没有异常**。
- 队列元素是 `std::packaged_task<void()>` 而非 `std::function<void()>`：
  后者要求目标**可拷贝**，捕获了 move-only 参数的 lambda 存不进去。
  因此 `submit(f, std::unique_ptr<X>{})` 现在可用。
- `shutdown()` 排空队列后 join；`shutdown_now()` **丢弃未开始**的任务后 join；
  **析构走 `shutdown_now()`**（析构不应无限期阻塞）。
  两者都会等*正在执行*的任务跑完 —— 标准 C++ 无法安全强杀线程，这一点池层面无解。
- `stats()` 一次取回全部指标；`pending_tasks()` 等旧 getter 保留为薄封装。
  worker 线程命名为 `<name>-<index>`，便于 lldb / `ps -M` 定位。

### 2.2 ObjectPool\<T\>

[include/cpppools/pool/object_pool.h](include/cpppools/pool/object_pool.h)

- 构造签名 `(name, init_size, max_size)`：**有界池**，`max_size` 是硬上限。
  达到上限且无空闲时 `acquire()` 立即返回**空 Ptr**（不阻塞）—— 与线程池
  的“拒绝”语义对齐；`main.cpp` 按 `max(128, worker_count * 2)` 计算容量，
  保证不低于并发需求。该不变式是**运行期配置而非类型保证**，
  因此调用方必须判空（`http_server.cpp` 已加）。
- 预创建 `init_size` 个，未达上限才扩容，避免每个请求都 `new/delete`。
- `acquire()` 返回 `unique_ptr<T, PoolDeleter<T>>`：**无状态** deleter（只存池指针），
  析构时自动归还 —— RAII。不用 `std::function`：它的 SSO 无标准保证，
  理论上每次 acquire 可能堆分配，与池化初衷相悖。
- **异常安全**：出池后的 `reset()` 若抛异常，对象被**销毁**并回滚计数，
  不会泄漏、也不会把“部分重置”的脏对象交给下一个调用方。
- `stats()` 一次锁取一致快照（保护 `in_use + available == size` 不变式）；
  旧 getter 保留为薄封装。
- 析构期 `assert(in_use_ == 0)` + **Release 下也写 stderr 警告**，
  拦截「池先销毁、对象后归还」的生命周期错误。

### 2.3 DbConnectionPool

[include/cpppools/db/db_connection_pool.h](include/cpppools/db/db_connection_pool.h)

- **factory 化构造**：`(name, factory, pool_size/config, idle_probe_threshold)`。
  建连逻辑由调用方的 `ConnectionFactory` 提供（mock 存储 / MySQL 配置都在 factory 里），
  池只负责借还、探活与重建。这是为 DB 中间件改造预留的扩展点：
  届时元素类型换成 `Database`，只需换 factory 实现。
- **健康检查与失效重建**：借出的连接若空闲超过阈值才探活（HikariCP 风格，
  热连接零开销）；探活失败则销毁并用 factory 重建。重建失败**保留空槽**，
  下次 acquire 再试 —— 数据库恢复后自愈，而不是永久少一个槽位。
- **有界池 + 绝对 deadline**：`acquire(timeout)` 基于 `wait_until`，
  timeout 是绝对 deadline（不每次重试重新计满），超时返回 nullptr。
  真实上界 = timeout + 一次已开始的探活/重建时长（`healthy()` 无超时参数）。
  同一次 acquire 会尝试所有未试过的空闲槽位，但同一槽位不原地重试。
- `release()` 用 `unordered_map` 反查槽位校验归还合法性；
  重复归还 / 野指针归还被拒绝、计入 `invalid_release_total` 并打 stderr
  （不再静默丢弃）。空指针归还是合法路径（guard 没拿到连接时）。
- `DbConnGuard` 把「借→还」绑到栈对象，业务抛异常也能自动归还；可移动。
- `free_count()` 维护 atomic 镜像，指标查询无锁读。
- `DbConnection::healthy()` 是**虚函数**：健康检查路径必须能被测试注入
  （mock 恒真、MySQL 要真连接，都制造不出“探活失败”场景）。

### 2.4 请求在三池间的接力

```
accept 主循环（主线程）
  └─ ThreadPool::submit_detached(handle_client)  ← 主线程立刻回到 accept
       │  队列已满 → 在 accept 循环直接回 503（真实背压，不无限堆积）
       │  池已关闭（停机竞态）→ 直接关这条连接
       └─ 解析请求行（零拷贝 string_view）
            └─ find_route() 查 constexpr 路由表
                 └─ handler
                      ├─ ObjectPool::acquire()   ← RAII 归还
                      └─ DbConnGuard(pool, 100ms) ← RAII 归还，超时则 503
                           └─ 查询 mock / MySQL
```

---

## 3. 网络与可观测性

### 3.1 自研 Socket 级 HTTP/1.1

[src/server/http_server.cpp](src/server/http_server.cpp)

- 直接基于 BSD socket / Winsock：`socket → bind → listen → accept`，不依赖第三方 HTTP 库。
- 模型：一次 `recv` + 一次 `send` + 主动 `close`。仅 GET / POST，参数全走 query string，
  因此无需实现 Content-Length 感知的循环读取。
- **过载背压**：线程池队列满时，accept 循环直接回 503（JSON 错误体）并关连接，
  而不是默默丢弃让客户端挂到超时；服务正在停机时则只关连接。
- 跨平台抽象：`SocketType` / `close_socket()` 屏蔽 `SOCKET` vs `int`。
- 路由由 `constexpr` 表集中声明；`RouteId` 是 `enum class`，配合 `switch` 全覆盖分发，
  **新增路由却忘接 handler 会被 `-Werror=switch` 拦成编译错误**，而非运行期 404。

### 3.2 RAII 贯穿到 HTTP 层

```cpp
// socket 与遥测计数器都交给 RAII，异常路径同样成立
SocketGuard guard(client_fd);                       // 析构必关 fd
const auto finalize = make_scope_exit([&] { ... }); // 必定递减 active 计数
DbConnGuard db(ctx.db_pool, ctx.db_timeout);        // 析构必归还连接
```

### 3.3 指标端点

- `GET /health` — 存活探针（纯文本，无版本前缀）
- `GET /metrics` — Prometheus exposition 文本。池指标统一带 `pool` 标签
  （为多数据源改造预留），指标名于批 4 直接换新、不双写旧名，详见 `docs/api/backend_api.md`
- `GET /api/v1/stats` — 三池瞬时值 + 历史峰值 + 累计计数器 + P50/P95/P99
- `GET /api/v1/get-config` — 启动期配置快照

### 3.4 统一统计：两类形状，不强行拉平

三池的统计按语义分两类（`stats()` 一次锁取一致快照）：

- **借→还池**（ObjectPool / DbConnectionPool）共用 `AcquireStats`：
  `capacity / size / in_use / available / peak_in_use / min_available /
  acquire_total / acquire_failed_total`。
  等式不变式因池而异：对象池是 `in_use + available == size`；
  连接池是 `in_use + available == capacity` 且 `size <= capacity`
  （重建失败的空槽占槽位、不计存活）。
- **线程池**用自己的 `TaskStats`：它不是借→还池，“待处理任务数”与
  “借出资源数”语义不同，把 `max_queue_size` 映射成 `capacity` 是错误建模。

命名约定统一：counter 带 `_total`、峰值带 `peak_`、历史最小带 `min_`。

两个易误读的口径：

- `cpppools_thread_pool_task_exception_total` **仅覆盖 `submit_detached`**：
  `submit()` 的异常由 future 携带、不计入。值为 0 不等于没发生过异常（HELP 文本已写明）。
- `cpppools_pool_available{pool="db"}` 含重建失败的空槽；存活连接数看 `cpppools_pool_size`。

---

## 4. 快速开始

### 4.1 环境要求

- 支持 C++17 的编译器（GCC 7+ / Clang 6+ / MSVC 2017+ / MinGW-w64）
- CMake 3.16+
- 可选：MySQL 8.0 客户端开发包（仅 MySQL 后端需要）

### 4.2 mock 后端（零外部依赖）

```bash
# Linux / macOS：一条命令完成构建 + 测试
scripts/build.sh

# 或手动
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure   # 预期 5/5 通过
```

```powershell
# Windows
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

产物（`build/bin/`）：

| 产物 | 说明 |
|---|---|
| `cpppools_server` | 三级池化 API 服务 |
| `test_thread_pool` / `test_object_pool` / `test_db_pool` / `test_db_connection` / `test_score_store` / `test_http_util` / `test_metrics` / `test_score_api` | 单元测试 |

### 4.3 启动并验证

```bash
scripts/run_server.sh --port 8080 --workers 6
# 或直接：./build/bin/cpppools_server 8080 6

curl http://127.0.0.1:8080/health
curl -X POST 'http://127.0.0.1:8080/api/v1/create-score?user=42&score=99'   # 201
curl        'http://127.0.0.1:8080/api/v1/get-score?user=42'                # 200
curl -X POST 'http://127.0.0.1:8080/api/v1/update-score?user=42&score=88'   # 200
curl -X POST 'http://127.0.0.1:8080/api/v1/delete-score?user=42'            # 204
curl        'http://127.0.0.1:8080/api/v1/get-score?user=42'                # 404
curl  http://127.0.0.1:8080/api/v1/stats
```

### 4.4 观察池化行为

服务端不提供人造的压测端点，压测由客户端 SDK 在真实场景下进行。
观察池子状态用 `/api/v1/stats` 与 `/metrics`：

```bash
# 观察连接池耗尽（503）需要 worker 数 > 连接池大小
scripts/run_server.sh --workers 32          # 池大小默认 8
for i in $(seq 1 200); do
  curl -s "http://127.0.0.1:8080/api/v1/get-score?user=$i" -o /dev/null &
done; wait
curl -s http://127.0.0.1:8080/api/v1/stats  # 看 db_timeout_requests 与 min_db_pool_free
```

注意现在 `/metrics` 上会同时存在两种 503，用指标区分：
`cpppools_db_acquire_timeout_total`（连接池耗尽）与
`cpppools_rejected_requests_total`（线程池队列满）。

### 4.5 MySQL 后端

```bash
# 1) 初始化库表
mysql -uroot -p < docs/mysql_setup.sql

# 2) 构建并启动
scripts/build.sh --mysql
scripts/run_server.sh --mysql --password 123456

# 3) 确认后端已切换
curl -s http://127.0.0.1:8080/api/v1/get-config
```

```powershell
# Windows
cmake -S . -B build -G Ninja -DCPPPOOLS_USE_MYSQL=ON `
  -DMYSQL_INCLUDE_DIR="C:/Program Files/MySQL/MySQL Server 8.0/include" `
  -DMYSQL_LIBRARY="C:/Program Files/MySQL/MySQL Server 8.0/lib/libmysql.lib"
cmake --build build
powershell -ExecutionPolicy Bypass -File .\scripts\run_server_mysql.ps1 -DbPassword 123456
```

> mock 后端是**开发与测试后端**（零依赖、可复现，全部测试靠它）；
> MySQL 才是本服务的目标场景。两者共用完全相同的 API 语义与响应字段。

### 4.6 运行期环境变量

| 变量 | 默认值 | 说明 |
|---|---|---|
| `CPPPOOLS_DB_MODE` | `mock` | `mock` 或 `mysql` |
| `CPPPOOLS_DB_HOST` / `CPPPOOLS_DB_PORT` | `127.0.0.1` / `3306` | MySQL 地址 |
| `CPPPOOLS_DB_USER` / `CPPPOOLS_DB_PASSWORD` | `root` / `（空）` | MySQL 凭据 |
| `CPPPOOLS_DB_NAME` / `CPPPOOLS_DB_TABLE` | `cpppools` / `user_scores` | 库表名 |
| `CPPPOOLS_DB_POOL_SIZE` | `8` | 连接池大小 |
| `CPPPOOLS_MAX_QUEUE_SIZE` | `1024` | 线程池任务队列上限；满则回 `503`（真实背压）|
| `CPPPOOLS_DB_ACQUIRE_TIMEOUT_MS` | `100` | 连接获取超时 |
| `CPPPOOLS_LATENCY_WINDOW_SIZE` | `1024` | 延迟窗口样本上限 |

---

## 5. 测试

| 用例 | 覆盖 |
|---|---|
| `test_thread_pool` | 17 用例：有界队列拒绝 / 双提交入口 / move-only 参数 / shutdown 与 shutdown_now / TaskStats / 线程命名 |
| `test_object_pool` | 有界 max_size 与立即失败、异常安全窗口（reset 抛出时销毁并回滚）、无状态 deleter、一致快照 |
| `test_db_pool` | 构造 fail-fast、探活/重建/自愈、绝对 deadline、guard 移动、非法归还计数、退化池统计 |
| `test_db_connection` | CRUD 跨连接可见性、跨存储隔离、失败路径（自 test_pools 迁入）|
| `test_score_store` | CRUD 四操作的命中/未命中语义、8 线程并发写入 |
| `test_http_util` | 请求行解析（含截断防御）、query 解析、路由三态匹配、HTTP 报文组装 |
| `test_metrics` | /metrics 与 /api/v1/stats 的格式契约：pool 标签、旧指标名已移除、task_exception 的 detached-only 警告、stats 新字段 |
| `test_score_api` | 4 个 handler 的 201/200/204/400/404/409/503 全分支 |

```bash
ctest --test-dir build --output-on-failure
```

测试基于 `assert`，因此 CMake 对测试目标显式加了 `-UNDEBUG`——
否则 Release 配置下断言会被消除，测试将退化成空 `main` 并全部「通过」。

---

## 6. 演进方向（暂不实现）

当前为单节点。若要扩展为分布式，现有边界提供的着手点：

| 方向 | 现有基础 |
|---|---|
| 多节点 + 一致性哈希路由 | `route_table` 已把路由与 handler 解耦，可在前面加一层转发 |
| 无状态水平扩容 | 服务本身无跨请求状态（mock 存储除外，MySQL 后端下天然无状态） |
| 健康检查驱动的摘除 | `/health` 与 `/metrics` 已就绪，契约独立于业务 API 版本 |
| 连接池按后端分片 | `DbPoolConfig` 已是值对象，可扩展为多目标配置 |

暂不引入的东西及原因：keep-alive、异步 I/O、调度器隔离 —— 当前是
「固定线程池 + 阻塞 thread-per-connection」模型，阻塞本身就是这个模型的一部分，
引入这些会改变整体架构，应在真正需要时作为独立演进项处理。

---

## 7. 已知限制

- 请求行上限 4095 字节（超出返回 `414`），不支持 keep-alive 与 chunked。
- 请求体不被读取；写操作参数必须走 query string。
- `/api/v1/stats` 的多个计数器分别读取，**不构成原子快照**。
- MySQL 连接失效后不会自动重建（`healthy()` 只探活，无重连逻辑）。
- MySQL 后端未做 prepared statement；`user`/`score` 已收敛为 int 故无注入面，
  但表名来自环境变量，信任边界是运维方。
