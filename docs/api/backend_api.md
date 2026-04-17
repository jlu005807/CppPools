# 后端 API 文档

实现文件：`src/server/http_server.cpp`

---

## 1. 通用约定

- 协议：HTTP/1.1
- 方法：当前仅支持 `GET`
- 响应编码：`UTF-8`
- 连接策略：`Connection: close`

---

## 2. 路由总览

| 路径 | 方法 | 说明 |
|---|---|---|
| `/` 或 `/index.html` | GET | 首页 |
| `/app.js` | GET | 首页脚本 |
| `/multi` 或 `/multi_pool.html` | GET | 三池联动可视化页面 |
| `/multi_pool.js` | GET | 三池联动页面脚本 |
| `/api/health` | GET | 健康检查 |
| `/api/stats` | GET | 运行指标 |
| `/api/process?user=<id>` | GET | 模拟一次业务请求 |

---

## 3. API 详情

### 3.1 `GET /api/health`

用于健康探针。

示例响应：

```json
{
  "status": "ok",
  "uptime_ms": 12345,
  "timestamp_hint": "steady_clock"
}
```

### 3.2 `GET /api/stats`

返回线程池、内存池、连接池、请求计数和延迟统计。

示例响应：

```json
{
  "uptime_ms": 1069,
  "total_requests": 4,
  "api_requests": 4,
  "static_requests": 0,
  "error_requests": 0,
  "db_mode": "mock",
  "thread_pool_workers": 4,
  "thread_pool_pending": 0,
  "db_pool_free": 8,
  "db_pool_total": 8,
  "memory_pool_in_use": 0,
  "memory_pool_available": 128,
  "memory_pool_total": 128,
  "latency_samples": 2,
  "p50_latency_ms": 15.000,
  "p95_latency_ms": 15.368,
  "p99_latency_ms": 15.368
}
```

字段说明：

- `db_mode`：`mock` 或 `mysql`
- `thread_pool_pending`：当前等待队列长度
- `db_pool_free / db_pool_total`：连接池空闲/总连接
- `memory_pool_in_use / memory_pool_total`：对象池占用
- `latency_samples`：延迟样本数量（滑动窗口）
- `p50/p95/p99_latency_ms`：处理延迟分位数（毫秒）

### 3.3 `GET /api/process?user=<id>`

模拟一次业务请求：

1. 从对象池获取 `RequestContext`
2. 在线程池执行任务
3. 从 DB 连接池获取连接并查询
4. 返回结构化 JSON

示例响应：

```json
{
  "request_id": 3,
  "thread_id": "4",
  "db_free": 7,
  "memory_pool_in_use": 1,
  "payload": {
    "user_id": 8,
    "score": 41,
    "db_conn_id": 2
  }
}
```

错误示例（连接获取超时）：

```json
{"error":"db connection timeout"}
```

HTTP 状态码：`503 Service Unavailable`

---

## 4. 错误约定

- 非 `GET`：`405 Method Not Allowed`

```json
{"error":"only GET is supported"}
```

- 未匹配路由：`404 Not Found`

```json
{"error":"not found"}
```
