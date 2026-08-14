# 后端 API 文档

CppPools 是一个**单节点的三级池化 API 服务**：把客户端 HTTP 请求经
`ThreadPool → ObjectPool → DBConnectionPool` 三级池化链路，转发到后端真实数据库。

本服务是**纯 API 服务**，不托管任何静态资源；客户端由独立 SDK（计划中的 Python SDK）承担。

## 1. 通用约定

- 协议：HTTP/1.1
- 方法：**读操作用 `GET`，写操作用 `POST`**；其余方法返回 `405`
- **所有参数一律走 query string**，服务端不解析请求体。请求体必须为空
- 编码：`UTF-8`；JSON 字段统一 **snake_case**
- 连接：响应统一带 `Connection: close`，单次请求-响应模型，不支持 keep-alive
- 请求行上限 **4095 字节**，超过返回 `414`

### 1.1 路径版本约定

| 前缀 | 用途 |
|---|---|
| 无前缀（`/health`、`/metrics`） | 面向基础设施（存活探针 / 指标采集器），契约独立于业务 API 版本演进 |
| `/api/v1/*` | 业务 API，随版本演进 |

### 1.2 错误响应体

所有错误（4xx / 5xx）统一为：

```json
{"message": "score not found", "status_code": 404}
```

`204 No Content` 无响应体。

### 1.3 状态码约定

| 码 | 含义 |
|---|---|
| `200 OK` | 读取成功 / 更新成功 |
| `201 Created` | 创建成功 |
| `204 No Content` | 删除成功（无响应体，且不含 `Content-Length`） |
| `400 Bad Request` | 必需参数缺失、或参数非法（非整数 / 负数 / 空值） |
| `404 Not Found` | 路径不在路由表中，或目标记录不存在 |
| `405 Method Not Allowed` | 路径存在但方法不匹配（响应带 `Allow` 头） |
| `409 Conflict` | 创建时记录已存在 |
| `414 URI Too Long` | 请求行超过 4095 字节 |
| `500 Internal Server Error` | 数据库操作失败或未预期异常 |
| `503 Service Unavailable` | 三种成因：① 连接池获取超时；② **线程池队列已满（过载）**——在进入处理链之前就被拒绝；③ **请求对象池耗尽**——进入了处理链但借不到上下文 |

## 2. 路由总览（8 个端点）

API 面故意保持极小：**4 个 CRUD + 4 个健康/状态查询**。压测由客户端 SDK 在真实场景下进行，
服务端不提供任何人造的压测/演示端点。

| 方法 | 路径 | 必需参数 | 说明 |
|---|---|---|---|
| GET | `/health` | — | 存活探针（纯文本） |
| GET | `/metrics` | — | Prometheus 指标（纯文本） |
| GET | `/api/v1/stats` | — | 三池运行指标 + 延迟分位 |
| GET | `/api/v1/get-config` | — | 启动期配置快照 |
| POST | `/api/v1/create-score` | `user`, `score` | 新建记录 |
| GET | `/api/v1/get-score` | `user` | 查询记录 |
| POST | `/api/v1/update-score` | `user`, `score` | 更新记录 |
| POST | `/api/v1/delete-score` | `user` | 删除记录 |

## 3. CRUD 接口

资源为 `user_scores` 表（`user_id` 主键、`score`、`updated_at`）。
Mock 与 MySQL 两种后端共用完全相同的语义与响应字段。

记录序列化格式：

```json
{"user_id": 42, "score": 99, "updated_at_ms": 1785995899243}
```

`updated_at_ms` 为 epoch 毫秒。Mock 后端在写入时取本地时钟；
MySQL 后端取 `UNIX_TIMESTAMP(updated_at) * 1000`。

> 三级池化就发生在这四个接口的请求路径上：
> ThreadPool 调度 → ObjectPool 借出接收缓冲与响应字符串 → DbConnectionPool 借出连接。

### 3.1 `POST /api/v1/create-score?user=<id>&score=<n>`

创建一条记录。**不做 upsert**：记录已存在时返回 `409` 且不修改原值。

| 情形 | 状态码 | 响应体 |
|---|---|---|
| 创建成功 | `201` | 记录 JSON |
| 记录已存在 | `409` | `{"message":"score already exists","status_code":409}` |
| 参数非法/缺失 | `400` | 错误体 |
| 连接池超时 | `503` | `{"message":"db connection timeout","status_code":503}` |

线程池队列已满时的 `503` **不属于任何端点**——它发生在进入处理链之前，
accept 循环直接响应：`{"message":"server overloaded","status_code":503}`。

三种 503 的区分方式（指标口径不同）：

| 成因 | 位置 | 计入的指标 | 计入 `total_requests` | 响应体 |
|---|---|---|---|---|
| 线程池队列满 | accept 循环，未进处理链 | `rejected_requests` | 否（拒绝率 = `rejected / (total + rejected)`）| `server overloaded` |
| 请求对象池耗尽 | worker 内，已进处理链 | `error_requests` | 是 | `request pool exhausted` |
| 连接池超时 | handler 内，已进处理链 | `error_requests` | 是 | `db connection timeout` |

```bash
curl -X POST 'http://127.0.0.1:8080/api/v1/create-score?user=42&score=99'
```

### 3.2 `GET /api/v1/get-score?user=<id>`

| 情形 | 状态码 |
|---|---|
| 命中 | `200` + 记录 JSON |
| 记录不存在 | `404` |

```bash
curl 'http://127.0.0.1:8080/api/v1/get-score?user=42'
```

### 3.3 `POST /api/v1/update-score?user=<id>&score=<n>`

覆盖 `score` 并刷新 `updated_at_ms`。**不做插入**：记录不存在时返回 `404`。

把 `score` 更新为与原值相同时仍返回 `200`（MySQL 后端通过连接标志
`CLIENT_FOUND_ROWS` 保证这一语义，否则 `affected_rows` 为 0 会被误判成 `404`）。

```bash
curl -X POST 'http://127.0.0.1:8080/api/v1/update-score?user=42&score=88'
```

### 3.4 `POST /api/v1/delete-score?user=<id>`

| 情形 | 状态码 |
|---|---|
| 删除成功 | `204`（无响应体） |
| 记录不存在 | `404` |

```bash
curl -X POST 'http://127.0.0.1:8080/api/v1/delete-score?user=42'
```

## 4. 状态查询接口

### 4.1 `GET /health`

存活探针。返回 `text/plain`，固定内容 `cpppools worker is active`。
刻意不用 JSON —— 契约越简单越不容易随业务演进而破坏。

### 4.2 `GET /metrics`

Prometheus exposition 文本格式。counter 带 `_total` 后缀，单位写入指标名。

> 批 4 起池指标**全部带 `pool` 标签**（为多数据源改造预留：届时会有 N 个连接池，
> 无标签无法区分数据源），且指标名**直接换新、不双写旧名**
> （`cpppools_db_pool_free` / `cpppools_object_pool_*` / `cpppools_thread_pool_pending` 等已移除）。

| 指标 | 类型 | 说明 |
|---|---|---|
| `cpppools_uptime_ms` | gauge | 进程运行时长 |
| `cpppools_requests_total` | counter | 累计请求数 |
| `cpppools_api_requests_total` | counter | 累计 API 请求数 |
| `cpppools_error_requests_total` | counter | 累计 4xx/5xx 响应数（已进入处理链但出错）|
| `cpppools_rejected_requests_total` | counter | 在 accept 循环被拒的连接数（**未进入处理链**）：线程池队列已满，或提交本身失败（如分配失败）|
| `cpppools_db_acquire_timeout_total` | counter | 借连接超时导致 503 的**请求**数（池级计数是 `cpppools_pool_acquire_failed_total`）|
| `cpppools_active_requests` | gauge | 当前处理中的请求数 |
| `cpppools_thread_pool_workers{pool}` | gauge | 工作线程数 |
| `cpppools_thread_pool_queue_capacity{pool}` | gauge | 任务队列上限 |
| `cpppools_thread_pool_queue_pending{pool}` / `_peak_queue_pending` | gauge | 待处理任务数 / 历史峰值 |
| `cpppools_thread_pool_submitted_total{pool}` / `_rejected_total` / `_completed_total` | counter | 入队 / 队列满拒绝 / 完成的任务数 |
| `cpppools_thread_pool_task_exception_total{pool}` | counter | 任务异常数。**仅覆盖 `submit_detached`**；`submit()` 的异常由 future 携带、不计入。值为 0 不等于没发生过异常 |
| `cpppools_thread_pool_task_exec_us_total{pool}` / `_peak_task_exec_us` | counter/gauge | 任务执行耗时累计 / 单任务最长（µs）|
| `cpppools_pool_capacity{pool}` | gauge | 池硬上限 |
| `cpppools_pool_size{pool}` | gauge | 已分配对象 / 存活连接数（≤ capacity；DB 池的重建失败空槽不计，暴露退化程度）|
| `cpppools_pool_in_use{pool}` / `_available` / `_peak_in_use` / `_min_available` | gauge | 借出中 / 空闲（DB 池含空槽）/ 历史峰值 / 历史最小空闲 |
| `cpppools_pool_acquire_total{pool}` / `_acquire_failed_total` | counter | acquire 调用数 / 失败数（对象池达上限；连接池超时）。失败率 = failed / total |
| `cpppools_db_pool_invalid_release_total{pool}` | counter | 重复归还 / 野指针归还次数 |
| `cpppools_db_pool_rebuild_total{pool}` / `_rebuild_failed_total` | counter | 探活失败后重建成功 / 失败（空槽保留待自愈）次数 |
| `cpppools_api_latency_ms{quantile}` | summary | P50 / P95 / P99 延迟 |

`pool` 标签取值：`"worker"`（线程池）、`"request"`（请求上下文对象池）、`"db"`（数据库连接池）。

### 4.3 `GET /api/v1/stats`

三池运行状态 + 累计计数器 + 滑动窗口延迟分位。

> 所有字段均为近似实时值：每个池内部是一次锁的一致快照，但多个池与
> HTTP 层计数器之间独立读取，**整体不构成原子快照**。
> 因此可能出现 `active_requests > total_requests` 这类瞬时不自洽的组合。

| 字段 | 类型 | 说明 |
|---|---|---|
| `uptime_ms` | int | 进程已运行时长 |
| `total_requests` | int | 累计接受请求总数 |
| `api_requests` | int | `/api/v1/*` 请求计数 |
| `error_requests` | int | 累计错误响应计数（已进入处理链但出错）|
| `rejected_requests` | int | 因线程池队列已满被拒的连接数（未进入处理链，指向容量不足）。**不计入 `total_requests`**，故拒绝率 = `rejected / (total + rejected)` |
| `active_requests` | int | 当前正在处理中的请求数 |
| `peak_active_requests` | int | 并发处理数历史峰值 |
| `db_timeout_requests` | int | 因连接获取超时返回 `503` 的次数 |
| `db_mode` | string | `"mock"` 或 `"mysql"` |
| `thread_pool_workers` | int | 线程池工作线程数 |
| `thread_pool_queue_capacity` | int | 任务队列上限 |
| `thread_pool_pending` | int | 当前待处理任务数 |
| `peak_thread_pool_pending` | int | 待处理任务历史峰值 |
| `thread_pool_submitted_total` / `_rejected_total` / `_completed_total` | int | 入队 / 队列满拒绝 / 完成的任务数 |
| `thread_pool_task_exception_total` | int | 任务异常数（**仅 `submit_detached`**，见 /metrics 同名指标说明）|
| `thread_pool_task_exec_us_total` / `thread_pool_peak_task_exec_us` | int | 任务执行耗时累计 / 单任务最长（µs）|
| `db_pool_free` / `db_pool_total` | int | 连接池空闲数（**含重建失败的空槽**）/ 槽位总数 |
| `db_pool_alive` | int | 存活连接数（空槽不计，暴露池的退化程度）|
| `min_db_pool_free` | int | 连接池空闲数历史最小值 |
| `db_pool_acquire_total` / `db_pool_acquire_failed_total` | int | 连接 acquire 调用数 / 超时失败数 |
| `db_pool_invalid_release_total` | int | 重复归还 / 野指针归还次数 |
| `db_pool_rebuild_total` / `db_pool_rebuild_failed_total` | int | 探活失败后重建成功 / 失败次数 |
| `memory_pool_in_use` / `_available` / `_total` | int | 对象池借出 / 空闲 / 已分配数 |
| `memory_pool_capacity` | int | 对象池硬上限（达上限 acquire 返回空，HTTP 层回 503）|
| `memory_pool_acquire_total` / `memory_pool_acquire_failed_total` | int | 对象 acquire 调用数 / 达上限失败数 |
| `peak_memory_pool_in_use` | int | 对象池借出历史峰值 |
| `db_acquire_timeout_ms` | int | 连接获取超时配置 |
| `latency_window_size` / `latency_samples` | int | 延迟窗口容量 / 当前样本数 |
| `avg_latency_ms` | double | 窗口内平均延迟 |
| `p50_latency_ms` / `p95_latency_ms` / `p99_latency_ms` | double | 分位延迟（最近邻秩法） |

> 池的峰值指标（`min_db_pool_free` / `peak_memory_pool_in_use` / `peak_thread_pool_pending`）
> 由各个池自己在借出时统计，因此覆盖所有请求路径，不会因新增端点而漏记。

### 4.4 `GET /api/v1/get-config`

启动期配置快照，供客户端自省运行参数而无需读服务端配置文件。

```json
{
  "version": "0.3.0",
  "db_mode": "mock",
  "db_pool_total": 8,
  "db_acquire_timeout_ms": 100,
  "thread_pool_workers": 6,
  "thread_pool_queue_capacity": 1024,
  "memory_pool_total": 128,
  "memory_pool_capacity": 128,
  "latency_window_size": 1024,
  "max_request_bytes": 4096,
  "endpoint_count": 8
}
```

## 5. 运行期可调环境变量

| 变量 | 默认值 | 说明 |
|---|---|---|
| `CPPPOOLS_DB_MODE` | `mock` | `mock` 或 `mysql` |
| `CPPPOOLS_DB_HOST` / `CPPPOOLS_DB_PORT` | `127.0.0.1` / `3306` | MySQL 地址 |
| `CPPPOOLS_DB_USER` / `CPPPOOLS_DB_PASSWORD` | `root` / `（空）` | MySQL 凭据 |
| `CPPPOOLS_DB_NAME` / `CPPPOOLS_DB_TABLE` | `cpppools` / `user_scores` | 库表名 |
| `CPPPOOLS_DB_POOL_SIZE` | `8` | 连接池大小 |
| `CPPPOOLS_MAX_QUEUE_SIZE` | `1024` | 线程池任务队列上限（满则回 503）|
| `CPPPOOLS_DB_ACQUIRE_TIMEOUT_MS` | `100` | 连接获取超时（毫秒）|
| `CPPPOOLS_LATENCY_WINDOW_SIZE` | `1024` | 延迟滑动窗口样本数上限 |

命令行参数：

```bash
./cpppools_server [port=8080] [worker_count=6]
```

> **容量提示**：当 `worker_count <= CPPPOOLS_DB_POOL_SIZE` 时，线程池会先成为瓶颈，
> 连接池永远打不空，`503` 通路实际不可达。用 SDK 压测想触发连接池耗尽时，
> 需令 `worker_count > db_pool_size`（例如 `./cpppools_server 8080 32`，池大小仍为 8）。

## 6. 客户端调用示例（Python）

```python
import requests

BASE = "http://127.0.0.1:8080"

# 创建
r = requests.post(f"{BASE}/api/v1/create-score", params={"user": 42, "score": 99})
assert r.status_code == 201, r.text
print(r.json())          # {'user_id': 42, 'score': 99, 'updated_at_ms': ...}

# 查询
r = requests.get(f"{BASE}/api/v1/get-score", params={"user": 42})
print(r.status_code, r.json())

# 更新
requests.post(f"{BASE}/api/v1/update-score", params={"user": 42, "score": 88})

# 删除（204 无响应体）
r = requests.post(f"{BASE}/api/v1/delete-score", params={"user": 42})
assert r.status_code == 204 and r.content == b""

# 错误处理：错误体统一 {"message", "status_code"}
r = requests.get(f"{BASE}/api/v1/get-score", params={"user": 42})
if r.status_code >= 400:
    err = r.json()
    raise RuntimeError(f"{err['status_code']}: {err['message']}")
```

> 写操作请用 `params=` 传查询参数，**不要用 `json=` 或 `data=`**：
> 服务端不解析请求体，且发送非空请求体可能导致响应被 RST 打断。
