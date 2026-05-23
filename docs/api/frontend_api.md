# 前端页面与接口调用文档

实现文件：`web/index.html`、`web/app.js`

## 1. 页面定位

前端只保留一个中文单页：`/`（即 `web/index.html`）。

设计目标："最小操作 + 三池可见"——围绕后端 `ThreadPool / ObjectPool / DBConnectionPool` 三大池子做一个轻量观测台。

## 2. 页面结构

页面分四块（从上到下）：

1. **页头**：标题 + 服务状态胶囊（运行时长 / DB 模式 / 服务在线指示点）。
2. **HTTP 请求卡**：4 个输入框（user / work_ms / hold_ms / 并发数）+ 3 个按钮 + 状态条。
3. **三池子卡片网格**：线程池 / 对象池 / 连接池各一张，每张含一个进度条 + 4 个数值指标。
4. **最近响应面板**：展示单次请求 JSON 或并发压测聚合摘要。

## 3. 输入与按钮

| 输入框 | 字段 | 范围 | 默认 | 作用 |
|---|---|---|---|---|
| 用户 ID | `userId` | 0–1,000,000 | `1` | 单次或并发请求的起始 `user` |
| work_ms (CPU) | `workMs` | 0–5000 | `80` | 透传给 `/api/process?work_ms=` |
| hold_ms (占用连接) | `holdMs` | 0–5000 | `120` | 透传给 `/api/process?hold_ms=` |
| 并发数 | `burstCount` | 1–80 | `20` | "并发压测"一次提交的请求数 |

| 按钮 | 行为 |
|---|---|
| 发送 1 次 | 调用一次 `/api/process`，结果写入"最近响应" |
| 并发压测 | 用 `Promise.allSettled` 并发提交 `burstCount` 个 `/api/process`，user 依次取 `userId+i`；期间以 250ms 间隔轮询 `/api/stats`，完成后写入聚合摘要 |
| 立即刷新 | 主动拉一次 `/api/stats`，更新三张卡片 |

## 4. 三池子卡片展示字段

每张卡片自带语义色条（蓝/绿/橙）和占用比例进度条。

### 4.1 线程池 ThreadPool（蓝）

| 指标 | 来自 `/api/stats` 字段 |
|---|---|
| Workers | `thread_pool_workers` |
| 待处理 | `thread_pool_pending` |
| 活跃 | `active_process_requests` |
| P95 ms | `p95_latency_ms` |

进度条：`active_process_requests / thread_pool_workers`。

### 4.2 对象池 ObjectPool（绿）

| 指标 | 来自 `/api/stats` 字段 |
|---|---|
| 使用中 | `memory_pool_in_use` |
| 可用 | `memory_pool_available` |
| 总量 | `memory_pool_total` |
| 峰值 | `peak_memory_pool_in_use` |

进度条：`memory_pool_in_use / memory_pool_total`。

### 4.3 连接池 DBConnectionPool（橙）

| 指标 | 来自 `/api/stats` 字段 |
|---|---|
| 空闲 | `db_pool_free` |
| 总量 | `db_pool_total` |
| 最小空闲 | `min_db_pool_free` |
| 超时 | `db_timeout_requests` |

进度条：`(db_pool_total − db_pool_free) / db_pool_total`。卡片副标题展示 `db_mode`。

## 5. 自动轮询

- 页面加载后立即拉取一次 `/api/stats`，并以 **1500ms** 周期轮询。
- "并发压测"执行期间叠加 **250ms** 快速轮询，结束后撤销。
- 页面失焦（`visibilitychange` → hidden）时**自动暂停轮询**，回到前台再恢复。
- 连续 2 次拉取失败，服务状态胶囊变红显示"服务异常 / 无法连接"。

## 6. 前端使用的后端接口

### 6.1 `GET /api/process`

```
/api/process?user=<userId>&work_ms=<workMs>&hold_ms=<holdMs>
```

- 单次请求 `AbortController` 超时 **9s**；并发请求超时 **15s**。
- 成功：JSON 渲染到响应面板，状态条显示"成功 · Xms"。
- 失败：状态条显示"失败 · 状态码 · Xms"，面板显示 `HTTP <code>` + 原始 body。

### 6.2 `GET /api/stats`

- 超时 **3s**，拉取失败静默重试。
- 一次响应同时刷新三张卡片的所有指标和进度条。

## 7. 可访问性与体验

- 所有输入框、按钮高度 ≥44px，符合触控目标尺寸要求。
- 输入框 `:focus-visible` 加蓝色环形高亮，键盘导航明确。
- 状态条使用 `role="status"` + `aria-live="polite"`，让屏幕阅读器在请求成功/失败时获得通知。
- 颜色不作为唯一信息载体——状态条同时用色与文字（"成功"/"失败"/"忙"）表达。
- 自动适配 `prefers-color-scheme`（亮色/暗色双套配色）。
- 自动尊重 `prefers-reduced-motion`，关闭动效。
- 数字使用 `font-variant-numeric: tabular-nums`，刷新时不抖动。

## 8. 访问方式

```
http://127.0.0.1:8080/
```

修改端口时，例如 `demo_server 8090 6`：

```
http://127.0.0.1:8090/
```
