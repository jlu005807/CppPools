# 后端 API 文档

CppPools 是一个**多数据源数据库中间件**：客户端发送通用的行级 CRUD 请求
（JSON 请求体），服务端按表名路由到配置的 SQLite / MySQL 数据源，
客户端不需要知道底层数据库类型。

本文档是对外 API 契约。设计依据：
`docs/superpowers/specs/2026-08-06-db-middleware-design.md`。

---

## 1. 通用约定

### 1.1 路径与请求形态

- 业务端点带 `/api/v1/` 前缀；`/health`、`/metrics` 是基础设施端点，不带版本前缀。
- 四个 CRUD 统一 **POST + JSON 请求体**（`Content-Length` 模型）：
  值可以是任意字符串（含 `&` `=` 空格 中文），query string 不胜任；
  insert 还需一次传多列。
- 请求体上限 1 MB，超出返回 `413`；请求行上限 4095 字节，超出返回 `414`。
- 不支持 keep-alive、chunked、TLS；每连接"一次读取 + 一次响应 + 主动关闭"。

### 1.2 值类型（严格校验）

JSON 值与数据库列类型的映射是**严格的**，不做隐式转换：

| JSON 类型 | 中间件 Value | 可写入的列类型 |
|---|---|---|
| 整数 | integer | INTEGER、REAL（整数可无损表示为实数） |
| 浮点数 | real | REAL |
| 字符串 | string | TEXT |
| null | NULL | 仅 nullable 列 |
| 布尔 / 对象 / 数组 | —— | 拒绝（400） |

### 1.3 错误响应体

所有非 2xx 响应统一为：

```json
{"message":"<可读消息>","status_code":400}
```

### 1.4 状态码矩阵

| 情形 | 状态码 | 说明 |
|---|---|---|
| insert 成功 | `201` | 回传 `inserted_id` |
| select / update / delete 成功 | `200` | 包括 0 行匹配 |
| select 无匹配 | `200` + `rows:[]` | **不是 404**（SQL 语义：查询成功但无结果） |
| update / delete 匹配 0 行 | `200` + `affected_rows:0` | 同上，不解释为"资源不存在" |
| 表/列不存在、where 列非唯一键、类型不匹配、缺必填列（NOT NULL 违例）、JSON 格式错 | `400` | 契约违反 |
| 表名跨数据源歧义且未指定 datasource | `409` | 消息列出候选数据源 |
| insert 唯一键 / 主键冲突 | `409` | |
| 连接池获取超时 | `503` | 消息 `db connection timeout` |
| 后端错误 | `500` | 透传后端消息（JSON 转义） |

400 留给真正的契约违反；0 行不是错误 —— 中间件如实转达数据库语义。

### 1.5 安全模型（客户端视角）

- **where 必须是主键 / 唯一键**：每个操作至多影响一行，接口在结构上
  无法被用来批量改删。
- **表名 / 列名必须在启动时自省出的 schema 白名单内**：不在白名单即 400，
  SQL 注入面在入口处被集合成员判断消除。
- **接口没有建表/删表能力**：关系模型由使用方在库内预置，中间件只读写行。
  schema 变更后需重启服务（启动时自省一次）。

---

## 2. 路由总览（9 个端点）

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/v1/select` | 查一行 |
| POST | `/api/v1/insert` | 插一行 |
| POST | `/api/v1/update` | 改一行 |
| POST | `/api/v1/delete` | 删一行 |
| GET | `/api/v1/schema` | 自省：各数据源的表与列 |
| GET | `/api/v1/stats` | 三池运行指标 + 延迟分位 |
| GET | `/api/v1/get-config` | 启动期配置快照 |
| GET | `/health` | 存活探针（纯文本） |
| GET | `/metrics` | Prometheus exposition 文本 |

---

## 3. 通用 CRUD 接口

四个端点共用的请求字段：

| 字段 | 类型 | 必需 | 说明 |
|---|---|---|---|
| `table` | string | 是 | 表名（必须在 schema 白名单内） |
| `where` | object | select/update/delete 必需 | `{"column":"<列名>","value":<值>}`；列必须是 PK/UNIQUE；值不得为 null |
| `datasource` | string | 否 | 显式指定数据源（见 §3.5 路由规则）；非字符串一律 400 |

所有成功响应都回传 `"datasource":"<实际命中的数据源名>"`，
便于客户端确认数据写入位置。

### 3.1 `POST /api/v1/select`

额外字段：`columns`（string 数组，可选；缺省或空数组 = 全部列；
返回列顺序 = 请求顺序）。

```json
// 请求
{"table":"user_scores","columns":["score"],"where":{"column":"user_id","value":42}}
// 200
{"datasource":"main","columns":["score"],"rows":[[99]],"row_count":1}
// 无匹配也是 200
{"datasource":"main","columns":["score"],"rows":[],"row_count":0}
```

### 3.2 `POST /api/v1/insert`

额外字段：`values`（object，必需，列名 → 值；自增主键列可缺省由数据库生成）。

```json
// 请求
{"table":"user_scores","values":{"user_id":42,"score":99}}
// 201
{"datasource":"main","affected_rows":1,"inserted_id":42}
```

`inserted_id`：数据库生成的自增主键（SQLite 为 rowid 别名列）。
表没有自增主键时为 `null`；显式传入主键时回传该值。

### 3.3 `POST /api/v1/update`

额外字段：`set`（object，必需，列名 → 新值）。

```json
// 请求
{"table":"user_scores","set":{"score":88},"where":{"column":"user_id","value":42}}
// 200
{"datasource":"main","affected_rows":1}
```

未出现在 `set` 里的列保持原值。

### 3.4 `POST /api/v1/delete`

```json
// 请求
{"table":"user_scores","where":{"column":"user_id","value":42}}
// 200
{"datasource":"main","affected_rows":1}
```

### 3.5 多数据源路由规则

配置文件中可定义多个数据源（类型可相同，如同构分片）。请求按表名路由：

| 情形 | 行为 |
|---|---|
| 请求显式带 `datasource` | 直接定位该数据源（**优先级最高**） |
| 表名只在一个数据源出现 | 自动路由 |
| 表名在多个数据源出现，请求未指定 | **409**，消息列出候选数据源名 |
| 表名不在任何数据源 | 400 |
| 指定的 `datasource` 不存在 / 该源没有此表 | 400 |

刻意不做"按配置顺序兜底选第一个"：若两个分片都有 `orders`，兜底会把
`update` 静默写进其中一个库而另一个库的行不动，客户端却收到
`affected_rows:1` —— 无报错的数据错乱。歧义表会在**启动日志**里告警。

---

## 4. `GET /api/v1/schema`

返回所有数据源的表与列元信息。**不暴露后端类型**（SQLite/MySQL 是
客户端不该依赖的实现细节）。

```json
{"datasources":[
  {"name":"main","tables":[
    {"name":"user_scores","columns":[
      {"name":"user_id","type":"integer","nullable":false,"primary_key":true,"unique":true},
      {"name":"score","type":"integer","nullable":false,"primary_key":false,"unique":false}
    ]}
  ]}
]}
```

`type` 取值：`integer` / `real` / `text`。`unique` 覆盖主键与单列
UNIQUE 约束（联合唯一索引不使任何单列唯一，因此不会出现在 `unique:true`
的判据里）。

---

## 5. 状态查询接口

### 5.1 `GET /health`

纯文本 `cpppools worker is active`，`200`。契约刻意简单，供探针使用。

### 5.2 `GET /metrics`

Prometheus exposition 文本。池指标统一带 `pool` 标签：
`"worker"`（线程池）、`"request"`（请求上下文对象池）、
**数据源名**（每个数据源的连接池，如 `"main"`）。

指标族：

- `cpppools_requests_total / api_requests_total / error_requests_total /
  rejected_requests_total / active_requests / uptime_ms /
  db_acquire_timeout_total` — 请求面计数。
- `cpppools_thread_pool_*{pool="worker"}` — workers / queue_capacity /
  queue_pending / peak_queue_pending / submitted_total / rejected_total /
  completed_total / task_exception_total / task_exec_us_total /
  peak_task_exec_us。注意 `task_exception_total` **仅覆盖 submit_detached**。
- `cpppools_pool_{capacity,size,in_use,available,peak_in_use,min_available,
  acquire_total,acquire_failed_total}{pool="..."}` — 借→还两池共用形状。
  连接池口径：`available` 含重建失败的空槽；`size` 只数存活连接，
  等式是 `in_use + available == capacity` 且 `size <= capacity`。
- `cpppools_db_pool_{invalid_release_total,rebuild_total,
  rebuild_failed_total}{pool="<数据源名>"}` — 连接池特有计数。
- `cpppools_api_latency_ms{quantile="0.5|0.95|0.99"}` + `_avg` /
  `_samples` — 业务 API 延迟分位。

两种 503 用指标区分：`cpppools_db_acquire_timeout_total`（连接池耗尽）
与 `cpppools_rejected_requests_total`（线程池队列满，accept 层背压）。

### 5.3 `GET /api/v1/stats`

JSON。一次锁取每池一致快照后拼装（池内是原子快照；跨池字段逐个读取）。

主要字段：

- 请求面：`uptime_ms / total_requests / api_requests / error_requests /
  rejected_requests / active_requests / peak_active_requests /
  db_timeout_requests`
- 数据源：`datasources`（数据源数量）
- 线程池：`thread_pool_workers / thread_pool_queue_capacity /
  thread_pool_pending / peak_thread_pool_pending /
  thread_pool_submitted_total / thread_pool_rejected_total /
  thread_pool_completed_total / thread_pool_task_exception_total /
  thread_pool_task_exec_us_total / thread_pool_peak_task_exec_us`
- 连接池（**全部数据源池聚合**；分池明细见 /metrics 的 pool 标签）：
  `db_pool_free / db_pool_total / db_pool_alive / min_db_pool_free /
  db_pool_acquire_total / db_pool_acquire_failed_total /
  db_pool_invalid_release_total / db_pool_rebuild_total /
  db_pool_rebuild_failed_total`
- 对象池：`memory_pool_in_use / memory_pool_available / memory_pool_total /
  memory_pool_capacity / peak_memory_pool_in_use /
  memory_pool_acquire_total / memory_pool_acquire_failed_total`
- 延迟：`db_acquire_timeout_ms / latency_window_size / latency_samples /
  avg_latency_ms / p50_latency_ms / p95_latency_ms / p99_latency_ms`

### 5.4 `GET /api/v1/get-config`

启动期配置快照（不含任何凭据）：`version / datasources（名称数组）/
db_pool_total / db_acquire_timeout_ms / thread_pool_workers /
thread_pool_queue_capacity / memory_pool_total / memory_pool_capacity /
latency_window_size / max_request_bytes / endpoint_count`。

---

## 6. 运行期配置与环境变量

数据源建连完全由 **JSON 配置文件**驱动，路径优先级：
`--config <path>` > `CPPPOOLS_CONFIG` 环境变量 > `config/cpppools.json`。
格式见 `config/cpppools.example.json`（`server` 段 + `datasources` 数组）。
配置错误（含数据源不可连）在启动期 fail-fast。

剩余环境变量：

| 变量 | 默认值 | 说明 |
|---|---|---|
| `CPPPOOLS_MAX_QUEUE_SIZE` | `1024` | 线程池任务队列上限；满则回 `503` |
| `CPPPOOLS_DB_ACQUIRE_TIMEOUT_MS` | `100` | 连接获取超时 |
| `CPPPOOLS_LATENCY_WINDOW_SIZE` | `1024` | 延迟窗口样本上限（配置文件 `server.latency_window_size` 优先） |

---

## 7. 客户端调用示例（Python）

```python
import json
import urllib.request

BASE = "http://127.0.0.1:8080"

def post(path, payload):
    req = urllib.request.Request(
        BASE + path, data=json.dumps(payload).encode(), method="POST")
    try:
        with urllib.request.urlopen(req) as resp:
            return resp.status, json.loads(resp.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read())

# 插入（201；回传 inserted_id）
print(post("/api/v1/insert", {"table": "user_scores",
                              "values": {"user_id": 42, "score": 99}}))
# 查询（200；无匹配时 rows 为空数组）
print(post("/api/v1/select", {"table": "user_scores",
                              "columns": ["score"],
                              "where": {"column": "user_id", "value": 42}}))
# 更新 / 删除（200 + affected_rows）
print(post("/api/v1/update", {"table": "user_scores",
                              "set": {"score": 88},
                              "where": {"column": "user_id", "value": 42}}))
print(post("/api/v1/delete", {"table": "user_scores",
                              "where": {"column": "user_id", "value": 42}}))

# 错误处理：错误体统一 {"message", "status_code"}
code, body = post("/api/v1/select", {"table": "ghost",
                                     "where": {"column": "id", "value": 1}})
assert code == 400 and "message" in body
```
