# 后端 API 文档

实现文件：`src/server/http_server.cpp`、`demo/server_main.cpp`

## 1. 通用约定

- 协议：HTTP/1.1
- 方法：仅支持 `GET`，其他方法返回 `405 Method Not Allowed`
- 编码：`UTF-8`
- 连接：服务端响应统一带 `Connection: close`，单次请求-响应模型
- 路径不存在：`404 Not Found`，body 为 `{"error":"not found"}`

## 2. 路由总览

| 路径 | 方法 | 说明 |
|---|---|---|
| `/` 或 `/index.html` | GET | 前端单页（静态资源） |
| `/app.js` | GET | 前端脚本（静态资源） |
| `/api/health` | GET | 健康检查 |
| `/api/stats` | GET | 服务运行指标（池子状态 + 延迟分位） |
| `/api/process?user=<id>&work_ms=<ms>&hold_ms=<ms>` | GET | 模拟一次业务请求 |

静态资源仅放行白名单（`/index.html`、`/app.js`），其它路径返回 `404`。

## 3. 接口详情

### 3.1 `GET /api/health`

用于存活与启动状态检查，不涉及池子或数据库。

响应字段：

| 字段 | 类型 | 说明 |
|---|---|---|
| `status` | string | 固定为 `"ok"` |
| `uptime_ms` | int | 进程已运行时长（毫秒，基于 `steady_clock`） |
| `timestamp_hint` | string | 固定为 `"steady_clock"`，提示时间来源 |

示例：

```json
{
  "status": "ok",
  "uptime_ms": 12345,
  "timestamp_hint": "steady_clock"
}
```

### 3.2 `GET /api/stats`

返回三大池子运行状态、累计计数器、滑动窗口延迟分位。所有字段均为近似实时值（多个 atomic 独立读取，不构成原子快照）。

响应字段：

| 字段 | 类型 | 说明 |
|---|---|---|
| `uptime_ms` | int | 进程已运行时长 |
| `total_requests` | int | 累计接受请求总数 |
| `api_requests` | int | `/api/*` 请求计数 |
| `static_requests` | int | 静态资源请求计数 |
| `error_requests` | int | 累计错误响应计数（400/404/405/500/503 等） |
| `process_requests` | int | `/api/process` 累计调用数 |
| `active_process_requests` | int | 当前正在处理中的 `/api/process` 数量 |
| `db_timeout_requests` | int | 因连接获取超时返回 `503` 的次数 |
| `db_mode` | string | `"mock"` 或 `"mysql"` |
| `thread_pool_workers` | int | 线程池工作线程数 |
| `thread_pool_pending` | int | 当前线程池待处理任务数 |
| `peak_thread_pool_pending` | int | 线程池待处理任务的历史峰值 |
| `db_pool_free` | int | 当前连接池空闲连接数 |
| `db_pool_total` | int | 连接池总连接数（启动时固定） |
| `min_db_pool_free` | int | 连接池空闲数的历史最小值 |
| `memory_pool_in_use` | int | 对象池借出中的对象数 |
| `memory_pool_available` | int | 对象池当前空闲对象数 |
| `memory_pool_total` | int | 对象池总对象数（含按需扩容） |
| `peak_memory_pool_in_use` | int | 对象池借出数历史峰值 |
| `peak_active_process_requests` | int | `active_process_requests` 历史峰值 |
| `db_acquire_timeout_ms` | int | 连接池 `acquire` 超时配置（毫秒） |
| `latency_window_size` | int | 延迟统计滑动窗口大小 |
| `latency_samples` | int | 当前窗口内实际样本数 |
| `avg_latency_ms` | double | 窗口内 `/api/process` 平均延迟 |
| `p50_latency_ms` | double | 50 分位延迟（最近邻秩） |
| `p95_latency_ms` | double | 95 分位延迟 |
| `p99_latency_ms` | double | 99 分位延迟 |

示例：

```json
{
  "uptime_ms": 1069,
  "total_requests": 12,
  "api_requests": 8,
  "static_requests": 4,
  "error_requests": 0,
  "process_requests": 6,
  "active_process_requests": 0,
  "db_timeout_requests": 0,
  "db_mode": "mysql",
  "thread_pool_workers": 6,
  "thread_pool_pending": 0,
  "peak_thread_pool_pending": 3,
  "db_pool_free": 8,
  "db_pool_total": 8,
  "min_db_pool_free": 5,
  "memory_pool_in_use": 0,
  "memory_pool_available": 128,
  "memory_pool_total": 128,
  "peak_memory_pool_in_use": 4,
  "peak_active_process_requests": 4,
  "db_acquire_timeout_ms": 100,
  "latency_window_size": 1024,
  "latency_samples": 6,
  "avg_latency_ms": 3.219,
  "p50_latency_ms": 2.771,
  "p95_latency_ms": 5.842,
  "p99_latency_ms": 7.120
}
```

### 3.3 `GET /api/process`

执行一次完整链路：

1. 从 `ObjectPool<RequestContext>` 借出请求上下文（RAII 自动归还）
2. 可选 `work_ms`：在借连接前 sleep，模拟 CPU/业务耗时
3. 从 `DBConnectionPool` 借连接（最多等待 `db_acquire_timeout_ms`）
4. 可选 `hold_ms`：拿到连接后 sleep，模拟连接被业务占用
5. 调用 mock/mysql 查询
6. 记录本次处理时延到滑动窗口

请求参数：

| 参数 | 必填 | 类型 | 取值 | 说明 |
|---|---|---|---|---|
| `user` | 是 | int | 非负整数 | 业务用户 ID |
| `work_ms` | 否 | uint | 0–5000 | 借连接前 sleep 毫秒数（超过 5000 会被夹到 5000） |
| `hold_ms` | 否 | uint | 0–5000 | 借到连接后 sleep 毫秒数（超过 5000 会被夹到 5000） |

成功响应字段：

| 字段 | 类型 | 说明 |
|---|---|---|
| `request_id` | int | accept 主循环分配的递增编号 |
| `thread_id` | string | 实际执行该请求的 worker 线程 ID |
| `work_ms` | int | 实际生效的 `work_ms`（截断后） |
| `hold_ms` | int | 实际生效的 `hold_ms`（截断后） |
| `db_free` | int | 查询完成时连接池空闲数快照 |
| `memory_pool_in_use` | int | 查询完成时对象池借出数快照 |
| `thread_pool_pending` | int | 查询完成时线程池待处理任务数快照 |
| `active_process_requests` | int | 查询完成时仍在处理中的请求数快照 |
| `payload` | object | 数据库查询结果（见下方） |

`payload` 字段：

- mock 模式：`{"user_id":<id>, "score":<int>, "db_conn_id":<池内连接编号>}`
- mysql 模式：`{"user_id":<id>, "score":<int>, "db_conn_id":<mysql_thread_id>}`
- mysql 异常时还会包含 `"db_error"` 或 `"db_error_code"` 字段

成功响应示例：

```json
{
  "request_id": 3,
  "thread_id": "7",
  "work_ms": 80,
  "hold_ms": 120,
  "db_free": 7,
  "memory_pool_in_use": 1,
  "thread_pool_pending": 0,
  "active_process_requests": 1,
  "payload": {
    "user_id": 8,
    "score": 41,
    "db_conn_id": 2
  }
}
```

错误响应：

- `user` 缺失或非法（HTTP `400`）：

```json
{"error":"invalid user parameter, expected non-negative integer"}
```

- `work_ms`/`hold_ms` 非法（HTTP `400`）：

```json
{"error":"invalid work_ms parameter, expected non-negative integer"}
{"error":"invalid hold_ms parameter, expected non-negative integer"}
```

- 连接获取超时（HTTP `503`）：

```json
{"error":"db connection timeout"}
```

## 4. 通用错误

| HTTP 状态 | 触发条件 | 响应体 |
|---|---|---|
| `400 Bad Request` | 参数非法 | `{"error":"invalid <param> parameter, ..."}` |
| `404 Not Found` | 路径不在白名单/路由表中 | `{"error":"not found"}` |
| `405 Method Not Allowed` | 非 GET 方法 | `{"error":"only GET is supported"}` |
| `500 Internal Server Error` | 静态资源读取失败 | `<filename> not found`（text/plain） |
| `503 Service Unavailable` | 连接池获取超时 | `{"error":"db connection timeout"}` |

## 5. 运行期可调环境变量

`demo_server` 启动时读取以下环境变量（见 `demo/server_main.cpp`）：

| 变量 | 默认值 | 说明 |
|---|---|---|
| `CPPPOOLS_DB_MODE` | `mock` | `mock` 或 `mysql` |
| `CPPPOOLS_DB_HOST` | `127.0.0.1` | MySQL 主机 |
| `CPPPOOLS_DB_PORT` | `3306` | MySQL 端口 |
| `CPPPOOLS_DB_USER` | `root` | MySQL 用户 |
| `CPPPOOLS_DB_PASSWORD` | `（空）` | MySQL 密码 |
| `CPPPOOLS_DB_NAME` | `cpppools` | 数据库名 |
| `CPPPOOLS_DB_TABLE` | `user_scores` | 表名 |
| `CPPPOOLS_DB_POOL_SIZE` | `8` | 连接池大小 |
| `CPPPOOLS_DB_ACQUIRE_TIMEOUT_MS` | `100` | 连接获取超时（毫秒） |
| `CPPPOOLS_LATENCY_WINDOW_SIZE` | `1024` | 延迟滑动窗口样本数上限 |

命令行参数：

```bash
./demo_server [port=8080] [worker_count=6]
```
