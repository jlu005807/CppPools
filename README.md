# CppPools — 并发资源池化服务

> C++17 / MySQL / Socket，一个用于面试展示与并发学习的最小可运行项目。

围绕简历上的三句话展开：

1. **池化架构设计**：用 C++17 构建 `ThreadPool / ObjectPool / DBConnectionPool` 三级池化协作链路，
   实现 HTTP 请求调度到 DB 查询的端到端处理。
2. **网络与可观测性**：自研 Socket 级轻量 HTTP/1.1 服务；封装 RAII Guard 与连接超时机制，
   实时输出 P99 延迟及池化状态指标，支撑运行期调参。
3. **工程化实践**：基于 CMake + CTest 搭建工程，支持 **Mock / MySQL 双模式**无缝编译切换，
   提供一键部署脚本便于环境拉起与演示。

链路一图流：

```
HTTP 请求 ──> ThreadPool 调度 ──> ObjectPool 复用 RequestContext ──> DBConnectionPool 查询 ──> JSON 响应
   ▲                                                                                              │
   └──────────────────────────  /api/stats 实时回传三池指标 + P50/P95/P99  ──────────────────────┘
```

前端只保留一个中文页面 `web/index.html`：三池卡片 + 简单 HTTP 操作，方便观察池化效果。

---

## 1. 池化架构设计（C++17 三级池化协作链路）

### 1.1 ThreadPool — 线程池

文件：[include/thread_pool/thread_pool.h](include/thread_pool/thread_pool.h)、[src/thread_pool/thread_pool.cpp](src/thread_pool/thread_pool.cpp)

核心要点：

- 固定 worker + 无界任务队列，标准的 **生产者 / 消费者** 模型。
- 同步原语：`std::mutex` + `std::condition_variable`，`wait(lock, predicate)` 防虚假唤醒。
- `submit(F, Args...)` 用 `std::packaged_task` + `std::future` 把"返回值/异常"安全传回调用线程。
- `shutdown()` 仅阻止新任务入队，已入队任务执行完后 worker 才退出。
- `pending_tasks()` 通过 `std::atomic<size_t>` 镜像计数，让 `/api/stats` 高频轮询时**无锁读**。

### 1.2 ObjectPool\<T\> — 对象池

文件：[include/memory_pool/object_pool.h](include/memory_pool/object_pool.h)

核心要点：

- 预创建 + 按需扩容，避免每个请求都 `new RequestContext` / `delete`。
- `acquire()` 返回 `std::unique_ptr<T, std::function<void(T*)>>`：
  自定义 deleter 在 `unique_ptr` 析构时把对象 push 回 free list，**RAII 自动归还**。
- 重置策略 `*obj = T(args...)`：教学场景下足够简单；注释里给出了 `obj->reset()` 的性能更优替代。
- 析构期通过 `assert(in_use_ == 0)` 拦截"池先销毁、对象后归还"这类生命周期错误。

### 1.3 DBConnectionPool — 数据库连接池

文件：[include/db_pool/db_connection_pool.h](include/db_pool/db_connection_pool.h)、[src/db_pool/db_connection_pool.cpp](src/db_pool/db_connection_pool.cpp)

核心要点：

- 有界连接池，`acquire(timeout)` 基于 `condition_variable::wait_for + predicate`：
  - 拿到连接 → 返回原始指针；
  - **超时返回 nullptr**，避免业务线程在资源耗尽时无限阻塞。
- `release()` 通过 `unordered_set` 校验归还合法性，拒绝重复归还 / 野指针归还。
- `DBConnGuard` 把"借→还"绑到栈对象上，业务函数即使抛异常也能自动 `release()`。
- `free_count()` 同样维护 `std::atomic` 镜像，stats getter 无锁读。

### 1.4 三池在 HttpServer 中的接力

请求进入 `route_request("/api/process")` 后：

```
ThreadPool: accept 主循环 -> submit(handle_client)
        │
        ▼
ObjectPool:  auto req_ctx = request_pool_.acquire();   // 复用 RequestContext，作用域结束自动归还
        │
        ▼
DBConnectionPool: DBConnGuard guard(db_pool_, timeout); // RAII 借连接，超时则返回 503
        │
        ▼
guard.get()->query_user_score(user_id);  // mock 或 MySQL
```

---

## 2. 网络与可观测性（自研 Socket 级 HTTP/1.1）

### 2.1 自研 HTTP/1.1 服务

文件：[include/server/http_server.h](include/server/http_server.h)、[src/server/http_server.cpp](src/server/http_server.cpp)

- 直接基于 BSD socket / Winsock：`socket → bind → listen → accept`；不依赖任何第三方 HTTP 库。
- 路由模型：`一次 recv + 一次 send + 主动 close`，**仅支持 GET**，其余方法 405。
- 跨平台抽象：`SocketType` / `close_socket()` 屏蔽 `SOCKET` vs `int`、`closesocket()` vs `close()`。
- 静态资源走白名单：`/`、`/index.html`、`/app.js`，避免任意文件读取。

### 2.2 RAII Guard 与连接超时

```cpp
// src/server/http_server.cpp 内 /api/process 路径片段
auto req_ctx = request_pool_.acquire();                  // ObjectPool RAII
DBConnGuard guard(db_pool_, config_.db_acquire_timeout); // DBConnectionPool RAII + 超时
if (!guard.valid()) {
    db_timeout_requests_.fetch_add(1, std::memory_order_relaxed);
    return build_http_response(json_error("db connection timeout"), ..., 503, "Service Unavailable");
}
```

业务异常或提前 `return` 都不需要手动 `release()`：栈对象析构时一定执行。

### 2.3 实时指标

- `/api/health` — 进程存活探针。
- `/api/stats` — 三池快照 + 历史峰值 + 滑动窗口延迟分位（avg / p50 / **p95 / p99**）。
  分位用 [Nearest Rank 法](src/server/http_server.cpp)，样本来自最近 `latency_window_size` 次 `/api/process`。
- `/api/process?user=<id>&work_ms=<ms>&hold_ms=<ms>` — 模拟一次完整链路，
  返回体里直接附带池子瞬时快照，前端可一眼看出请求对池子的冲击。

完整字段说明：[docs/api/backend_api.md](docs/api/backend_api.md)。

---

## 3. 工程化实践（CMake + CTest + 双模式 + 一键脚本）

- **CMake** 入口 [CMakeLists.txt](CMakeLists.txt)：
  - `CMAKE_CXX_STANDARD 17`，关闭方言扩展，跨平台一致。
  - 产物统一输出到 `output/bin` 与 `output/lib`。
  - `option(CPPPOOLS_USE_MYSQL ...)` 一键切换 Mock / MySQL 编译路径。
- **CTest** 注册了 `test_thread_pool` 与 `test_pools` 两个用例，`ctest --output-on-failure` 一键回归。
- **双模式**：Mock 模式下零外部依赖即可学习；MySQL 模式链接官方 MySQL C API，复用同一套池实现。
- **一键脚本** [scripts/run_server_mysql.ps1](scripts/run_server_mysql.ps1)：Windows 下三步内拉起 MySQL 模式服务。

---

## 4. 项目结构

```text
CppPools/
├── CMakeLists.txt                  # CMake 构建入口
├── README.md                       # 本文档
├── include/
│   ├── thread_pool/thread_pool.h   # 线程池接口（submit / future / shutdown）
│   ├── memory_pool/object_pool.h   # 泛型对象池（RAII 自动归还）
│   ├── db_pool/
│   │   ├── db_connection_pool.h    # 连接池 + DBConnGuard
│   │   └── mock_db_connection.h    # mock / mysql 双模式统一连接抽象
│   └── server/http_server.h        # HTTP 服务接口与统计结构
├── src/
│   ├── thread_pool/thread_pool.cpp
│   ├── db_pool/db_connection_pool.cpp
│   ├── db_pool/mock_db_connection.cpp
│   └── server/http_server.cpp      # accept 循环 / 路由 / stats
├── demo/
│   ├── main.cpp                    # 线程池基础 demo
│   ├── multi_pools_demo.cpp        # 三池联动命令行 demo
│   └── server_main.cpp             # HTTP 服务入口（读环境变量）
├── tests/
│   ├── test_thread_pool.cpp
│   └── test_pools.cpp              # 对象池 + 连接池
├── web/
│   ├── index.html                  # 中文单页：三池卡片 + 请求面板
│   └── app.js                      # 调用 /api/process 与 /api/stats
├── docs/
│   ├── api/backend_api.md
│   ├── api/frontend_api.md
│   ├── mysql_setup.sql             # MySQL 初始化脚本
│   └── learning_log.md
├── scripts/run_server_mysql.ps1    # Windows 一键脚本
└── output/                         # 构建产物（bin / lib）
```

---

## 5. 运行教程

### 5.1 准备环境

- C++ 编译器：支持 C++17（GCC 7+/Clang 6+/MSVC 2017+/MinGW-w64）。
- CMake 3.16+，可选 Ninja / Make / Visual Studio。
- 可选：MySQL 8.0 客户端开发包（仅 MySQL 模式需要）。

> Windows 下推荐 PowerShell + MinGW 或 Visual Studio。
> Linux / macOS 直接用系统包管理器装 `cmake` + `g++` 即可。

### 5.2 Mock 模式（零外部依赖）

#### 步骤 1：配置 + 构建

```powershell
# Windows PowerShell
Set-Location D:\Internt_of_Thing\CppPools
cmake -S . -B build
cmake --build build
```

```bash
# Linux / macOS
cd /path/to/CppPools
cmake -S . -B build
cmake --build build -j
```

构建产物：

- `output/bin/demo_thread_pool` — 线程池入门 demo
- `output/bin/demo_multi_pools` — 三池联动命令行 demo
- `output/bin/demo_server`      — HTTP 服务
- `output/bin/test_thread_pool` / `output/bin/test_pools` — 单元测试

#### 步骤 2：跑测试

```powershell
ctest --test-dir build --output-on-failure
```

预期输出：`100% tests passed, 0 tests failed out of 2`。

#### 步骤 3：体验三个 demo

```powershell
# (1) 线程池基础：提交任务、等待 future
./output/bin/demo_thread_pool

# (2) 三池联动命令行：500 任务默认；可改任务数 / worker 数
./output/bin/demo_multi_pools 1000 6

# (3) HTTP 服务：默认 127.0.0.1:8080，可改端口 / worker 数
./output/bin/demo_server 8080 6
```

#### 步骤 4：浏览器访问

打开 <http://127.0.0.1:8080/> ，可以看到三池观测台：

- 顶部胶囊显示服务运行时长 + db_mode。
- 中部三张卡片实时反映 ThreadPool / ObjectPool / DBConnectionPool 状态。
- "发送 1 次" / "并发压测"按钮直接触发请求，观察池子瞬时变化与 P95 延迟。

#### 步骤 5：用 curl 验证 API

```powershell
# 单次请求
Invoke-RestMethod "http://127.0.0.1:8080/api/process?user=42&work_ms=80&hold_ms=120"

# 池子快照
Invoke-RestMethod "http://127.0.0.1:8080/api/stats" |
  Select-Object thread_pool_workers, db_pool_free, memory_pool_in_use, p95_latency_ms, p99_latency_ms
```

```bash
curl 'http://127.0.0.1:8080/api/process?user=42&work_ms=80&hold_ms=120'
curl 'http://127.0.0.1:8080/api/stats'
```

### 5.3 MySQL 模式（Windows + MinGW 示例）

#### 步骤 1：MySQL 客户端就位

确保有 `mysql.h` 头文件与 `libmysql.lib`（或 `libmysql.dll`）。
官方 MySQL Server 安装后默认路径：

- 头文件：`C:/Program Files/MySQL/MySQL Server 8.0/include`
- 库文件：`C:/Program Files/MySQL/MySQL Server 8.0/lib/libmysql.lib`

#### 步骤 2：构建（重新生成 build/）

```powershell
Set-Location D:\Internt_of_Thing\CppPools
Remove-Item -Recurse -Force .\build -ErrorAction SilentlyContinue

cmake -S . -B build -G Ninja `
  -DCPPPOOLS_USE_MYSQL=ON `
  -DMYSQL_INCLUDE_DIR="C:/Program Files/MySQL/MySQL Server 8.0/include" `
  -DMYSQL_LIBRARY="C:/Program Files/MySQL/MySQL Server 8.0/lib/libmysql.lib"

cmake --build build -v
```

> 编译期会把 `libmysql.dll` 自动拷到 `output/bin`，无需手动配 PATH。
> 同时会生成 `demo_server_mysql(.exe)` 别名，便于一键脚本识别。

#### 步骤 3：初始化数据库

```powershell
Get-Content .\docs\mysql_setup.sql |
  & "C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe" -uroot -p123456
```

`docs/mysql_setup.sql` 会建库 `cpppools` 与表 `user_scores`，并插入示例数据。

#### 步骤 4：一键拉起

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\run_server_mysql.ps1 `
  -DbPassword 123456 -Port 8080 -Workers 6
```

#### 步骤 5：验证 db_mode = mysql

```powershell
Invoke-RestMethod http://127.0.0.1:8080/api/stats |
  Select-Object db_mode, db_pool_total, p95_latency_ms
```

返回 `db_mode=mysql` 即代表真实连接池已生效。

### 5.4 运行期可调环境变量

`demo_server` / `demo_multi_pools` 读取下列变量（无需重新编译）：

| 变量 | 默认值 | 说明 |
|---|---|---|
| `CPPPOOLS_DB_MODE` | `mock` | `mock` 或 `mysql` |
| `CPPPOOLS_DB_HOST` / `CPPPOOLS_DB_PORT` | `127.0.0.1` / `3306` | MySQL 地址 |
| `CPPPOOLS_DB_USER` / `CPPPOOLS_DB_PASSWORD` | `root` / `` | MySQL 凭据 |
| `CPPPOOLS_DB_NAME` / `CPPPOOLS_DB_TABLE` | `cpppools` / `user_scores` | 库表名 |
| `CPPPOOLS_DB_POOL_SIZE` | `8` | 连接池大小 |
| `CPPPOOLS_DB_ACQUIRE_TIMEOUT_MS` | `100` | 连接获取超时（毫秒） |
| `CPPPOOLS_LATENCY_WINDOW_SIZE` | `1024` | 延迟滑动窗口样本数上限 |

详见 [docs/api/backend_api.md](docs/api/backend_api.md#5-运行期可调环境变量)。

---

## 6. 学习路径建议

按这个顺序读源码，能快速理清三池协作脉络：

1. [include/thread_pool/thread_pool.h](include/thread_pool/thread_pool.h) — submit / future / cv 范式。
2. [include/memory_pool/object_pool.h](include/memory_pool/object_pool.h) — unique_ptr + custom deleter 的 RAII。
3. [include/db_pool/db_connection_pool.h](include/db_pool/db_connection_pool.h) + `.cpp` — wait_for 超时 + DBConnGuard。
4. [src/server/http_server.cpp](src/server/http_server.cpp) — accept 循环 / 路由 / 三池接力。
5. [demo/server_main.cpp](demo/server_main.cpp) — 三池由 `main` 统一持有，服务内部只持引用。

## 7. API 文档

- [后端 API 文档](docs/api/backend_api.md)
- [前端页面 / 接口调用文档](docs/api/frontend_api.md)
