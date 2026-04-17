# CppPools

`CppPools` 是一个面向学习与面试展示的 C++17 项目，围绕三类高频基础组件展开：

- `ThreadPool`：线程复用与异步任务调度
- `ObjectPool<T>`：小对象复用，减少频繁 `new/delete`
- `DBConnectionPool`：数据库连接复用（支持 `mock` / `mysql` 两种模式）

并提供一个轻量 HTTP 服务，将三者串到同一请求链路中：

`HTTP Request -> ThreadPool -> ObjectPool<RequestContext> -> DBConnectionPool -> JSON Response`

---

## 1. 项目结构

```text
CppPools/
├── CMakeLists.txt
├── README.md
├── include/
│   ├── thread_pool/
│   ├── memory_pool/
│   ├── db_pool/
│   └── server/
├── src/
│   ├── thread_pool/
│   ├── db_pool/
│   └── server/
├── demo/
│   ├── main.cpp
│   ├── multi_pools_demo.cpp
│   └── server_main.cpp
├── tests/
│   ├── test_thread_pool.cpp
│   └── test_pools.cpp
├── web/
│   ├── index.html
│   ├── app.js
│   ├── multi_pool.html
│   └── multi_pool.js
├── docs/
│   ├── learning_log.md
│   ├── mysql_setup.sql
│   └── api/
│       ├── backend_api.md
│       └── frontend_api.md
├── build/      # CMake 中间文件
└── output/     # 构建产物目录（可执行文件/库）
```

---

## 2. 构建与输出目录

本项目将构建产物统一放到 `output/`：

- 可执行文件：`output/bin/`
- 库文件：`output/lib/`

### 2.1 CMake 构建

```bash
cmake -S . -B build
cmake --build build
```

### 2.2 运行测试

```bash
ctest --test-dir build --output-on-failure
```

---

## 3. 快速运行

### 3.1 线程池 Demo

```bash
./output/bin/demo_thread_pool
```

### 3.2 三池联动压测 Demo

```bash
./output/bin/demo_multi_pools
# 或
./output/bin/demo_multi_pools <task_count> <worker_count>
```

示例：

```bash
./output/bin/demo_multi_pools 2000 8
```

### 3.3 HTTP 服务 Demo

```bash
./output/bin/demo_server
# 或
./output/bin/demo_server <port> <worker_count>
```

示例：

```bash
./output/bin/demo_server 8080 6
```

访问：

- 首页：`http://127.0.0.1:8080/`
- 三池联动可视化页面：`http://127.0.0.1:8080/multi`

---

## 4. 已实现的关键能力

### 4.1 `/api/stats` 延迟分位数

`/api/stats` 增加了延迟窗口统计字段：

- `latency_samples`
- `p50_latency_ms`
- `p95_latency_ms`
- `p99_latency_ms`

这些值来自最近一段 `/api/process` 请求的处理时延样本。

### 4.2 `/multi` 实时趋势图

`/multi` 页面新增实时折线图，展示：

- `ThreadPool Pending`
- `P95 Latency (ms)`

前端按固定周期轮询 `/api/stats` 并更新折线与资源占用条。

### 4.3 DBPool 支持真实 MySQL

`DBConnectionPool` 支持两种模式：

- `mock`：默认，便于快速演示
- `mysql`：真实 MySQL 连接池

`/api/stats` 中的 `db_mode` 会显示当前模式，仪表盘展示逻辑保持一致。

---

## 5. 启用 MySQL 连接池

> 先准备 MySQL 客户端开发库，再开启 CMake 选项。

### 5.1 开启编译选项

```bash
cmake -S . -B build -DCPPPOOLS_USE_MYSQL=ON
cmake --build build
```

### 5.2 设置运行时环境变量（Windows）

```bash
set CPPPOOLS_DB_MODE=mysql
set CPPPOOLS_DB_HOST=127.0.0.1
set CPPPOOLS_DB_PORT=3306
set CPPPOOLS_DB_USER=root
set CPPPOOLS_DB_PASSWORD=your_password
set CPPPOOLS_DB_NAME=cpppools
set CPPPOOLS_DB_TABLE=user_scores
set CPPPOOLS_DB_POOL_SIZE=8
```

### 5.3 初始化示例表

参考：`docs/mysql_setup.sql`

---

## 6. 文档索引

- 后端 API：`docs/api/backend_api.md`
- 前端 API 与页面交互：`docs/api/frontend_api.md`
- MySQL 初始化脚本：`docs/mysql_setup.sql`

---

## 7. 后续可扩展方向

1. `DBConnectionPool` 增加断线重连与心跳检测
2. 引入统一日志（如 `spdlog`）并增加结构化指标
3. 增加更系统的 benchmark（吞吐、延迟分位数、资源占用）
4. 为 HTTP 层补充 keep-alive、请求解析与异常恢复能力

---

## 8. Windows 一键启动 MySQL 模式

项目提供了脚本：`scripts/run_server_mysql.ps1`，会自动：

- 注入 MySQL `lib/bin` 到 `PATH`
- 设置 `CPPPOOLS_DB_*` 环境变量
- 启动服务端并保持同样仪表盘展示

示例：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\run_server_mysql.ps1 -DbPassword 123456 -Port 8080 -Workers 6
```

如果你的 MySQL 不在默认路径，可额外指定：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\run_server_mysql.ps1 -MySQLRoot "D:\\MySQL\\MySQL Server 8.0" -DbPassword 123456
```

### 8.1 CMake 目标说明（MySQL 模式）

当以 `-DCPPPOOLS_USE_MYSQL=ON` 配置后：

- 标准目标仍是 `demo_server`
- 额外提供标准目标入口 `demo_server_mysql`（依赖 `demo_server`）
- 构建后会自动在 `output/bin` 下生成 `demo_server_mysql(.exe)` 兼容文件名

可直接执行：

```bash
cmake --build build --target demo_server_mysql
```

## 9. New Server Tuning Knobs (Learning Enhancements)

The server now supports extra runtime env vars:

- `CPPPOOLS_DB_ACQUIRE_TIMEOUT_MS` (default `100`)
- `CPPPOOLS_LATENCY_WINDOW_SIZE` (default `1024`)

`GET /api/stats` now also returns:

- `avg_latency_ms`
- `db_acquire_timeout_ms`
- `latency_window_size`

`GET /api/process?user=<id>` now validates `user` strictly.
If `user` is missing or not a non-negative integer, server returns `400 Bad Request`.
