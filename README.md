# CppPools

一个用于学习与面试展示的 C++17 项目，核心聚焦：

- 线程池（`ThreadPool`）
- 内存对象池（`ObjectPool<T>`）
- 数据库连接池（`DBConnectionPool`，支持 `mock/mysql`）
- 基于 Socket 的轻量 HTTP 服务（`HttpServer`）

项目链路：

`HTTP 请求 -> 线程池调度 -> 对象池复用 RequestContext -> 连接池查询 -> JSON 响应`

前端现在只保留一个中文页面：`web/index.html`，用于最小化操作与指标观察。

## 项目结构（含注释）

```text
CppPools/
├── CMakeLists.txt                      # CMake 构建入口（目标、选项、输出目录）
├── README.md                           # 项目说明
├── include/
│   ├── thread_pool/thread_pool.h       # 线程池接口（submit/future/shutdown）
│   ├── memory_pool/object_pool.h       # 泛型对象池（RAII 自动归还）
│   ├── db_pool/
│   │   ├── db_connection_pool.h        # 连接池接口 + DBConnGuard
│   │   └── mock_db_connection.h        # mock/mysql 统一连接抽象
│   └── server/http_server.h            # HTTP 服务接口与统计结构
├── src/
│   ├── thread_pool/thread_pool.cpp     # 线程池实现
│   ├── db_pool/
│   │   ├── db_connection_pool.cpp      # 连接池实现（超时、归还校验）
│   │   └── mock_db_connection.cpp      # mock 查询 + MySQL 查询实现
│   └── server/http_server.cpp          # HTTP 路由、静态资源、统计指标
├── demo/
│   ├── main.cpp                        # 线程池基础 demo
│   ├── multi_pools_demo.cpp            # 命令行三池联动 demo
│   └── server_main.cpp                 # HTTP 服务启动入口
├── tests/
│   ├── test_thread_pool.cpp            # 线程池测试
│   └── test_pools.cpp                  # 对象池/连接池测试
├── web/
│   ├── index.html                      # 单页中文前端
│   └── app.js                          # 单页交互逻辑（调用 /api/process 与 /api/stats）
├── docs/
│   ├── api/
│   │   ├── backend_api.md              # 后端 API 文档
│   │   └── frontend_api.md             # 前端页面与接口调用文档
│   ├── mysql_setup.sql                 # MySQL 初始化脚本
│   └── learning_log.md                 # 学习迭代记录
├── scripts/run_server_mysql.ps1        # Windows 一键启动 MySQL 模式
├── build/                              # CMake 中间构建目录
└── output/                             # 构建产物（bin/lib）
```

## 模块实现说明

### 1) 线程池 ThreadPool

- 固定工作线程 + 任务队列
- `submit(...)` 返回 `std::future`
- 使用 `mutex + condition_variable` 实现生产者/消费者
- `shutdown()` 优雅退出，避免任务丢失

### 2) 内存池 ObjectPool<T>

- 预创建对象并复用，减少频繁 `new/delete`
- `acquire()` 返回带自定义 deleter 的 `unique_ptr`
- 作用域结束自动归还（RAII）

### 3) 数据库连接池 DBConnectionPool

- 支持 `mock` 与 `mysql` 两种模式
- `acquire(timeout)` 支持超时等待
- `DBConnGuard` 自动归还连接
- 防止重复归还导致连接重复入队

### 4) HTTP 服务 HttpServer

- 当前只支持 `GET`
- 静态资源：`/`、`/index.html`、`/app.js`
- API：`/api/health`、`/api/stats`、`/api/process?user=<id>`
- 指标包含：`avg/p50/p95/p99` 延迟分位

## 构建与运行

### 1) 默认构建（Mock DB）

```bash
cmake -S . -B build
cmake --build build
```

产物输出：

- 可执行文件：`output/bin`
- 库文件：`output/lib`

### 2) 运行测试

```bash
ctest --test-dir build --output-on-failure
```

或直接运行：

```bash
./output/bin/test_thread_pool
./output/bin/test_pools
```

### 3) 运行 Demo

```bash
# 线程池 demo
./output/bin/demo_thread_pool

# 三池联动命令行 demo
./output/bin/demo_multi_pools

# HTTP 服务
./output/bin/demo_server
# 可选参数：端口、worker 数
./output/bin/demo_server 8080 6
```

浏览器访问：

- `http://127.0.0.1:8080/`

## MySQL 模式（Windows + MinGW）

```powershell
Set-Location D:\Internt_of_Thing\CppPools
Remove-Item -Recurse -Force .\build -ErrorAction SilentlyContinue

cmake -S . -B build -G Ninja `
  -DCPPPOOLS_USE_MYSQL=ON `
  -DMYSQL_INCLUDE_DIR="C:/Program Files/MySQL/MySQL Server 8.0/include" `
  -DMYSQL_LIBRARY="C:/Program Files/MySQL/MySQL Server 8.0/lib/libmysql.lib"

cmake --build build -v
```

初始化数据库：

```powershell
Get-Content .\docs\mysql_setup.sql | & "C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe" -uroot -p123456
```

启动（推荐脚本）：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\run_server_mysql.ps1 -DbPassword 123456 -Port 8080 -Workers 6
```

验证：

```powershell
Invoke-RestMethod http://127.0.0.1:8080/api/stats | Select-Object db_mode,db_pool_total,p95_latency_ms
```

如果返回 `db_mode=mysql`，说明真实连接池已启用。

## API 文档

- [后端 API 文档](docs/api/backend_api.md)
- [前端页面/API 调用文档](docs/api/frontend_api.md)
