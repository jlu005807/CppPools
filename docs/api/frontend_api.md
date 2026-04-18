# 前端页面与接口调用文档

实现文件：`web/index.html`、`web/app.js`

## 1. 页面定位

前端只保留一个中文单页：`/`（即 `web/index.html`）。

目标是“最小操作 + 指标可见”，突出后端三大池子与网络 API，而不是复杂前端交互。

## 2. 页面能力

页面提供两个操作按钮：

- `发送单次请求`：调用 `/api/process?user=<id>`
- `并发请求 x5`：并发调用 5 次 `/api/process`

页面自动轮询：

- 每 1 秒调用一次 `/api/stats`
- 实时更新线程池、内存池、连接池和延迟指标

## 3. 前端使用的后端接口

### 3.1 `GET /api/process?user=<id>`

用途：触发一次后端业务处理链路，返回本次请求结果。

前端行为：

- 显示 JSON 响应到结果面板
- 显示请求耗时（浏览器侧测量）

### 3.2 `GET /api/stats`

用途：获取实时统计指标。

前端读取字段：

- `thread_pool_workers`
- `thread_pool_pending`
- `db_pool_free`
- `db_pool_total`
- `memory_pool_in_use`
- `memory_pool_available`
- `total_requests`
- `avg_latency_ms`
- `p95_latency_ms`
- `p99_latency_ms`

## 4. 交互与容错

- 输入校验：`user` 必须是非负整数
- 网络异常：保留页面可操作，不会导致页面崩溃
- 指标拉取失败：忽略本次刷新，等待下一次轮询

## 5. 访问方式

启动 `demo_server` 后访问：

- `http://127.0.0.1:8080/`

如果修改端口，例如 `demo_server 8090 6`，则访问：

- `http://127.0.0.1:8090/`
