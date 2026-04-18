# CppPools

一个面向学习与面试展示的 **C++17 高并发基础组件项目**。
项目核心是“三大池化组件 + 网络服务串联”：

- `ThreadPool`：线程复用与异步任务调度
- `ObjectPool<T>`：小对象复用，减少频繁 `new/delete`
- `DBConnectionPool`：数据库连接复用（支持 `mock/mysql`）
- `HttpServer`：将三类池子接入同一请求链路，并提供可视化页面

请求链路：

`HTTP Request -> ThreadPool -> ObjectPool<RequestContext> -> DBConnectionPool -> JSON Response`

---

## 1. 项目结构（含注释）

```text
CppPools/
├── CMakeLists.txt                # CMake 构建入口（目标、依赖、MySQL 选项）
├── README.md                     # 项目说明文档
├── include/
│   ├── thread_pool/
│   │   └── thread_pool.h         # 线程池接口（submit/future/shutdown）
│   ├── memory_pool/
│   │   └── object_pool.h         # 通用对象池模板（RAII 自动归还）
│   ├── db_pool/
│   │   ├── db_connection_pool.h  # 连接池接口与 RAII Guard
│   │   └── mock_db_connection.h  # mock/mysql 统一连接抽象
│   └── server/
│       └── http_server.h         # HTTP 服务接口与统计结构
├── src/
│   ├── thread_pool/
│   │   └── thread_pool.cpp       # 线程池实现
│   ├── db_pool/
│   │   ├── db_connection_pool.cpp# 连接池实现（超时获取、归还校验）
│   │   └── mock_db_connection.cpp# mock 查询 + MySQL 查询实现
│   └── server/
│       └── http_server.cpp       # 路由、指标统计、静态资源服务
├── demo/
│   ├── main.cpp                  # 线程池基础 demo
│   ├── multi_pools_demo.cpp      # 命令行三池联动压测 demo
│   └── server_main.cpp           # HTTP 服务启动入口
├── tests/
│   ├── test_thread_pool.cpp      # 线程池测试
│   └── test_pools.cpp            # 对象池/连接池测试（超时、RAII、重复归还）
├── web/
│   ├── index.html                # 主可视化页面
│   ├── app.js                    # 主页面交互逻辑
│   ├── multi_pool.html           # 三池联动仪表盘页面
│   └── multi_pool.js             # pending/latency 实时图逻辑
├── scripts/
│   └── run_server_mysql.ps1      # Windows 一键启动 MySQL 模式脚本
├── docs/
│   ├── api/
│   │   ├── backend_api.md        # 后端 API 文档
│   │   └── frontend_api.md       # 前端 API 文档
│   ├── mysql_setup.sql           # MySQL 初始化脚本
│   └── learning_log.md           # 学习迭代记录
├── build/                        # CMake 中间构建目录（可删可重建）
└── output/                       # 最终产物目录（bin/lib）
```

---

## 2. 各模块实现说明

### 2.1 ThreadPool（线程池）

功能：

- 固定工作线程
- `submit(...)` 提交任意可调用对象
- 返回 `std::future` 获取异步结果
- `shutdown()` 优雅关闭，先清空任务再退出线程

关键实现点：

- `std::queue<std::function<void()>>` + `mutex + condition_variable`
- 任务通过 `std::packaged_task` 封装，异常可透传给 `future`

---

### 2.2 ObjectPool<T>（对象池）

功能：

- 复用高频对象，降低堆分配压力
- `acquire()` 返回带自定义 deleter 的 `unique_ptr`
- 句柄析构时自动归还对象（RAII）

关键实现点：

- 空闲队列 `free_list_`
- 占用计数 `in_use_`
- 析构时断言检查（调试期可捕获“对象池先析构”的错误用法）

---

### 2.3 DBConnectionPool（数据库连接池）

功能：

- 支持 `mock/mysql` 两种模式
- `acquire(timeout)` 超时获取连接
- `DBConnGuard` 自动归还连接
- 防御性处理重复归还（避免连接重复入队）

关键实现点：

- `free_conns_`（空闲连接队列）
- `in_use_conns_`（借出连接集合）
- `condition_variable::wait_for` 做超时等待

---

### 2.4 HttpServer（服务编排层）

功能：

- 处理 `GET` 请求并路由
- 提供静态页面：`/`、`/multi`
- 提供 API：`/api/health`、`/api/stats`、`/api/process`
- 统计延迟窗口并输出 `avg/p50/p95/p99`

关键实现点：

- `/api/process?user=<id>` 严格参数校验（非法返回 `400`）
- `HttpServerConfig` 支持运行时可调：
  - `db_acquire_timeout`
  - `latency_window_size`

---

## 3. 构建与运行指南

### 3.1 通用构建（mock 模式）

```bash
cmake -S . -B build
cmake --build build
```

产物位置：

- 可执行文件：`output/bin`
- 库文件：`output/lib`

---

### 3.2 运行测试

```bash
# 方式一（推荐）
ctest --test-dir build --output-on-failure

# 方式二（直接运行）
./output/bin/test_thread_pool
./output/bin/test_pools
```

---

### 3.3 运行 Demo

线程池 demo：

```bash
./output/bin/demo_thread_pool
```

三池联动命令行 demo：

```bash
./output/bin/demo_multi_pools
# 或
./output/bin/demo_multi_pools <task_count> <worker_count>
```

HTTP 服务：

```bash
./output/bin/demo_server
# 或
./output/bin/demo_server <port> <worker_count>
```

访问：

- `http://127.0.0.1:8080/`
- `http://127.0.0.1:8080/multi`

---

## 4. MySQL 模式（Windows + MinGW）

> 关键：请使用 `libmysql.lib`，不要使用 `mysqlclient.lib`（MinGW 下常导致链接失败）。

### 4.1 构建

```powershell
Set-Location D:\Internt_of_Thing\CppPools
Remove-Item -Recurse -Force .\build -ErrorAction SilentlyContinue

cmake -S . -B build -G Ninja `
  -DCPPPOOLS_USE_MYSQL=ON `
  -DMYSQL_INCLUDE_DIR="C:/Program Files/MySQL/MySQL Server 8.0/include" `
  -DMYSQL_LIBRARY="C:/Program Files/MySQL/MySQL Server 8.0/lib/libmysql.lib"

cmake --build build -v
```

### 4.2 初始化数据库

```powershell
Get-Content .\docs\mysql_setup.sql | & "C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe" -uroot -p123456
```

### 4.3 启动服务（推荐脚本）

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\run_server_mysql.ps1 -DbPassword 123456 -Port 8080 -Workers 6
```

快速自检：

```powershell
Invoke-RestMethod http://127.0.0.1:8080/api/stats | Select-Object db_mode,db_pool_total,p95_latency_ms
```

若 `db_mode=mysql`，说明真实连接池已生效。

---

## 5. API 概览

- `GET /api/health`：健康检查
- `GET /api/stats`：运行指标（线程池、内存池、连接池、延迟分位数）
- `GET /api/process?user=<id>`：业务请求模拟（非法参数返回 `400`）

详细说明见：

- `docs/api/backend_api.md`
- `docs/api/frontend_api.md`

---

## 6. 下一步可扩展方向

1. 连接池健康检查与断线重连
2. 更完整 benchmark（吞吐、延迟分位、资源占用）
3. 引入结构化日志（如 `spdlog`）
4. HTTP keep-alive 与更完整请求解析
