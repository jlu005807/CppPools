# 后端 API 文档

实现文件：`src/server/http_server.cpp`

## 1. 通用约定

- 协议：HTTP/1.1
- 方法：仅支持 `GET`
- 编码：`UTF-8`
- 连接：`Connection: close`

## 2. 路由总览

| 路径 | 方法 | 说明 |
|---|---|---|
| `/` 或 `/index.html` | GET | 前端单页 |
| `/app.js` | GET | 前端脚本 |
| `/api/health` | GET | 健康检查 |
| `/api/stats` | GET | 服务运行指标 |
| `/api/process?user=<id>` | GET | 模拟一次业务请求 |

## 3. 接口详情

### 3.1 `GET /api/health`

用于存活与启动状态检查。

示例：

```json
{
  "status": "ok",
  "uptime_ms": 12345,
  "timestamp_hint": "steady_clock"
}
```

### 3.2 `GET /api/stats`

返回三大池子运行状态与延迟统计。

示例：

```json
{
  "uptime_ms": 1069,
  "total_requests": 12,
  "api_requests": 8,
  "static_requests": 4,
  "error_requests": 0,
  "db_mode": "mysql",
  "thread_pool_workers": 6,
  "thread_pool_pending": 0,
  "db_pool_free": 8,
  "db_pool_total": 8,
  "memory_pool_in_use": 0,
  "memory_pool_available": 128,
  "memory_pool_total": 128,
  "db_acquire_timeout_ms": 100,
  "latency_window_size": 1024,
  "latency_samples": 8,
  "avg_latency_ms": 3.219,
  "p50_latency_ms": 2.771,
  "p95_latency_ms": 5.842,
  "p99_latency_ms": 7.120
}
```

核心字段说明：

- `db_mode`：`mock` 或 `mysql`
- `thread_pool_pending`：线程池等待队列长度
- `db_pool_free/db_pool_total`：连接池空闲数/总连接数
- `memory_pool_in_use/memory_pool_total`：对象池使用中/总量
- `db_acquire_timeout_ms`：连接获取超时配置
- `latency_window_size`：延迟滑动窗口大小
- `avg/p50/p95/p99_latency_ms`：平均值与分位数延迟（毫秒）

### 3.3 `GET /api/process?user=<id>`

执行一次完整链路：

1. 从 `ObjectPool<RequestContext>` 获取请求上下文
2. 从 `DBConnectionPool` 获取连接
3. 执行 mock/mysql 查询
4. 返回 JSON，并记录本次处理时延

请求参数：

- `user`：非负整数

成功响应示例：

```json
{
  "request_id": 3,
  "thread_id": "7",
  "db_free": 7,
  "memory_pool_in_use": 1,
  "payload": {
    "user_id": 8,
    "score": 41,
    "db_conn_id": 2
  }
}
```

错误响应示例：

- 参数非法（HTTP `400`）

```json
{"error":"invalid user parameter, expected non-negative integer"}
```

- 连接获取超时（HTTP `503`）

```json
{"error":"db connection timeout"}
```

## 4. 通用错误

- 非 GET 方法：HTTP `405`

```json
{"error":"only GET is supported"}
```

- 路径不存在：HTTP `404`

```json
{"error":"not found"}
```
