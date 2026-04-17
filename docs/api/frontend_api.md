# 前端 API 与页面说明

---

## 1. 页面入口

- 首页：`/`（`web/index.html` + `web/app.js`）
- 三池联动页：`/multi`（`web/multi_pool.html` + `web/multi_pool.js`）

---

## 2. 首页（`app.js`）

首页功能重点：

- 发送单次请求：调用 `/api/process?user=<id>`
- Burst 并发请求：并发调用多次 `/api/process`
- 周期刷新指标：调用 `/api/stats`
- 背景粒子可视化：用于演示感和交互体验

关键展示指标：

- 最近请求 `request_id`
- 当前请求延迟
- `db_free / db_total`
- `memory_pool_in_use`
- `thread_pool_pending`

---

## 3. 三池联动页（`multi_pool.js`）

三池联动页聚焦于“池化指标 + 趋势图”。

### 3.1 指标来源

`/api/stats` 提供：

- `thread_pool_workers`
- `thread_pool_pending`
- `memory_pool_in_use`
- `memory_pool_available`
- `memory_pool_total`
- `db_pool_free`
- `db_pool_total`
- `total_requests`
- `p95_latency_ms`

### 3.2 可视化能力

- 资源占用条：线程池/内存池/连接池
- 实时折线图：
  - 曲线 1：`ThreadPool Pending`
  - 曲线 2：`P95 Latency (ms)`
- 操作区：
  - 单次请求
  - 并发压测（x20）

### 3.3 轮询策略

- 周期：约 1.5 秒
- 端点：`GET /api/stats`
- 异常处理：保留上次可视化状态，避免闪烁

---

## 4. 前后端对齐点

- `/api/stats` 的分位数字段为：
  - `p50_latency_ms`
  - `p95_latency_ms`
  - `p99_latency_ms`
- `/multi` 页面当前使用 `p95_latency_ms` 作为延迟趋势展示。
- `db_mode`（`mock`/`mysql`）切换后，前端无需改动即可继续展示同一仪表盘。
