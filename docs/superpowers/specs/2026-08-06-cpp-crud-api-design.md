# CppPools 后端 API 改造设计（移除前端 + CRUD/状态接口）

- 日期：2026-08-06
- 状态：已确认，待实施
- 范围：删除全部前端资源；新增 4 个 CRUD 接口与 4 个状态接口；路径统一到 `/api/v1`

## 1. 背景与目标

CppPools 原本自带一个中文单页观测台（`web/index.html` + `web/app.js`）作为三池状态的可视化入口。
现决定移除前端，改由后续独立编写的 **Python SDK** 作为唯一客户端。因此本次改造有两个目标：

1. 删除全部 HTML/CSS/JS 与其配套文档，但**完整保留并强化后端 API 契约**。
2. 把 API 从「仅 GET + 一个演示端点」扩展为「4 个 CRUD + 4 个状态查询」，
   使其成为一个可被 SDK 正常驱动的服务。

Alluxio Dora 的 HTTP API 作为**风格参照**，但不逐条照搬——两者语言与并发模型不同，详见第 6 节。

## 2. 实施分步

本设计分两次提交落地：

- **Step 1**：删除前端。移除 `web/`、前端文档、以及服务端的静态资源路由。
- **Step 2**：完善后端 API。新增数据层、路由表、CRUD 与状态接口。

Python SDK 不在本次范围内，仅要求 API 契约对 SDK 友好（稳定的 snake_case JSON + 可预测状态码）。

## 3. 端点契约

### 3.1 CRUD（4 个）

资源为既有的 `user_scores` 表（`user_id`, `score`, `updated_at`），Mock 与 MySQL 双模式共用同一套语义。

| 方法 | 路径 | 必需参数 | 成功 | 失败 |
|---|---|---|---|---|
| POST | `/api/v1/create-score` | `user`, `score` | `201` + 记录 | `409` 已存在 |
| GET  | `/api/v1/get-score`    | `user`          | `200` + 记录 | `404` 不存在 |
| POST | `/api/v1/update-score` | `user`, `score` | `200` + 记录 | `404` 不存在 |
| POST | `/api/v1/delete-score` | `user`          | `204` 无响应体 | `404` 不存在 |

记录序列化形如：

```json
{"user_id": 42, "score": 99, "updated_at_ms": 1775433600000}
```

`user` 与 `score` 均要求非负整数，非法或缺失返回 `400`。

### 3.2 状态查询（4 个）

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/health` | 纯文本 `cpppools worker is active`，无版本前缀 |
| GET | `/metrics` | Prometheus exposition 文本 |
| GET | `/api/v1/stats` | 三池 JSON 快照（沿用原 `/api/stats` 实现） |
| GET | `/api/v1/get-config` | 启动期配置快照 + 版本号 |

`/health` 与 `/metrics` 不带版本前缀，与 Alluxio 一致——这两个是面向基础设施（探针 / 采集器）的端点，
其契约独立于业务 API 版本演进。

### 3.3 演示端点（1 个，迁移）

`GET /api/v1/process?user&work_ms&hold_ms` — 原 `/api/process`，保留三池压力演示能力。

### 3.4 下线端点

`/`、`/index.html`、`/app.js`、`/api/health`、`/api/stats`、`/api/process`。

因前端即将删除且 Python SDK 尚未编写，当前是唯一的零成本破坏性改版窗口，故一次性统一路径。

## 4. 响应约定

**字段命名统一 snake_case**。不采用 Alluxio 的 camelCase：既有 `/api/stats` 的 26 个字段本就是
snake_case，且 Python SDK 侧 snake_case 更符合 PEP 8。

**错误响应体**：

```json
{"message": "score not found for user 42", "status_code": 404}
```

不含 `exception` 字段——Alluxio 会回传 Java 异常类名，但在 C++ 侧暴露内部类型名属信息泄露。

**状态码约定**：

| 码 | 触发条件 |
|---|---|
| 200 | 读取成功 / 更新成功 |
| 201 | 创建成功 |
| 204 | 删除成功（无响应体） |
| 400 | 必需参数缺失或非法 |
| 404 | 路径不存在，或资源不存在 |
| 405 | 路径存在但 HTTP 方法不匹配 |
| 409 | 创建时资源已存在 |
| 503 | 连接池获取超时 |

`405` 的正确判定依赖路由表——现有实现对任何非 GET 请求一律返回 405，包括不存在的路径，属缺陷。

## 5. 架构设计

### 5.1 分层

```
handle_client (socket I/O)
    └─ route_request  ── find_route(path, method) ─→ constexpr RouteSpec 表
            └─ handler(RouteContext) -> ApiResponse      ← 不接触 socket，可单测
                    └─ MockDBConnection CRUD -> ScoreResult
                            ├─ mock  : MockScoreStore（shared_mutex + unordered_map）
                            └─ mysql : SQL
```

`ApiResponse { int status_code; std::string status_text; std::string content_type; std::string body; }`
是分层的关键——handler 返回纯数据，由 `http_server` 负责拼 HTTP 报文。这让 CRUD 逻辑可以
脱离网络单测。

### 5.2 数据模型

```cpp
struct ScoreRecord { int user_id; int score; long long updated_at_ms; };
enum class DBOpStatus { kOk, kNotFound, kAlreadyExists, kError };
struct ScoreResult { DBOpStatus status; ScoreRecord record; std::string error; };
```

现有 `query_user_score` 直接返回拼好的 JSON 字符串，把数据访问与序列化耦合在一起，
无法表达 404/409。改为返回 `ScoreResult` 是 CRUD 语义成立的前提。

### 5.3 Mock 模式必须有状态

现有 `query_user_score_mock` 是纯函数 `(user_id * 17 + id_ * 3) % 101`，无状态。
但 CRUD 要求「用连接 3 写入、用连接 7 能读到」，因此新增进程级共享存储：

```cpp
class MockScoreStore {
    mutable std::shared_mutex mutex_;                 // C++17：读并发
    std::unordered_map<int, ScoreRecord> records_;
};
```

由 `DBConnectionPool` 持有并注入各 mock 连接，**不使用单例**——沿用项目既有的
「池对象由 main / pool 持有生命周期」所有权风格。`get-score` 读多写少，故用
`shared_lock` 并发读、`unique_lock` 独占写。

### 5.4 路由表

```cpp
using RouteHandler = ApiResponse (*)(const RouteContext&);   // 函数指针，非 std::function
struct RouteSpec {
    std::string_view path;  HttpMethod method;  RouteId id;
    std::array<std::string_view, 2> required_params;  std::size_t required_count;
};
enum class MatchStatus { kMatched, kMethodMismatch, kNoSuchPath };
```

9 条路由用 `constexpr std::array` 线性扫描，不用哈希表——规模下线性扫描更快，
且表在编译期成型、进程启动零构造开销。

`enum class RouteId` 配合 `switch` 全覆盖分发：新增路由却忘记接 handler 会被
`-Wswitch` 拦成**编译错误**，而非运行期 404。

## 6. 明确不从 Alluxio 搬运的部分

| Alluxio 做法 | 不搬的理由 | CppPools 做法 |
|---|---|---|
| `OpTag.LIGHT/HEAVY` 双调度器 | 那是 Netty event-loop 不可阻塞的补偿；CppPools 是固定线程池 + 阻塞 thread-per-connection，阻塞本身即模型的一部分 | 单一 ThreadPool |
| 枚举内 `new XxxAction()` + 虚派发 | 依赖 GC 与堆分配的多态注册 | constexpr 表 + 函数指针，零堆分配、零 vtable |
| `AlluxioRuntimeException` 映射 HTTP 码 | 异常驱动控制流在 C++ 代价高且不合惯例 | 状态枚举按值返回，热路径无 throw |
| `HashMap<String, Endpoint>` 路由 | 9 条路由用哈希表属浪费 | constexpr 线性扫描 |
| Jackson 反射序列化 | C++ 无反射 | 手写 ostringstream，沿用项目现有风格 |
| camelCase 字段 | 与既有 stats 字段及 Python SDK 惯例冲突 | 统一 snake_case |
| `String` 值拷贝 | Java 无零拷贝视图 | `std::string_view` 全程零拷贝解析 |

## 7. 零拷贝解析与生命周期契约

```cpp
struct RequestLine { HttpMethod method; std::string_view target; };
std::optional<RequestLine> parse_request_line(std::string_view raw);
std::pair<std::string_view, std::string_view> split_target(std::string_view target);
std::optional<std::string_view> find_query_value(std::string_view query, std::string_view key);
std::optional<int> parse_int_non_negative(std::string_view s);
```

两项顺带修复：

1. **参数歧义**：现有 `get_query_value` 对「参数不存在」与「存在但值为空」都返回空字符串，
   无法区分，导致 `?user=` 被误判为缺参。改用 `std::optional` 后可正确返回 400。
2. **去掉一次 4KB 拷贝**：现有 `std::istringstream req_stream(request)` 会复制整个请求。
   改为在 `string_view` 上直接切分请求行，解析全程不分配。

**生命周期契约（须写入注释）**：所有 `string_view` 均借自 `handle_client` 栈上的
`std::string request`，仅在 `route_request` 调用期内有效，禁止跨线程传递或存入成员变量。

## 8. MySQL CRUD 的 SQL 与一处隐蔽陷阱

| 操作 | SQL | 状态判定 |
|---|---|---|
| create | `INSERT INTO \`t\` (user_id, score) VALUES (id, s)` | `mysql_errno() == 1062`（ER_DUP_ENTRY）→ `kAlreadyExists` |
| get | `SELECT score, UNIX_TIMESTAMP(updated_at)*1000 FROM \`t\` WHERE user_id=id LIMIT 1` | 0 行 → `kNotFound` |
| update | `UPDATE \`t\` SET score=s WHERE user_id=id` | `mysql_affected_rows() == 0` → `kNotFound` |
| delete | `DELETE FROM \`t\` WHERE user_id=id` | `mysql_affected_rows() == 0` → `kNotFound` |

**陷阱**：`mysql_affected_rows()` 默认返回**实际改变**的行数而非匹配行数。
把 score 更新为与原值相同时返回 0，会被误判成 `kNotFound`。

**解法**：`mysql_real_connect` 的 flags 参数传 `CLIENT_FOUND_ROWS`，语义改为返回匹配行数。
这是 update 语义正确的前提，须在代码中注释说明。

参数仍用字符串拼接而非 prepared statement：`user` 与 `score` 都已被
`parse_int_non_negative` 收敛为 int，无注入面；引入 stmt API 会显著增加代码量，
对教学定位的项目不划算。表名来自环境变量，信任边界是运维方。

## 9. `/metrics` 格式

遵循 Prometheus 命名约定（counter 带 `_total`、单位入名）：

```
# HELP cpppools_requests_total Total HTTP requests received.
# TYPE cpppools_requests_total counter
cpppools_requests_total 42
# TYPE cpppools_db_acquire_timeout_total counter
cpppools_db_acquire_timeout_total 0
# TYPE cpppools_thread_pool_pending gauge
cpppools_thread_pool_pending 3
# TYPE cpppools_db_pool_free gauge
cpppools_db_pool_free 8
# TYPE cpppools_object_pool_in_use gauge
cpppools_object_pool_in_use 1
# TYPE cpppools_process_latency_ms summary
cpppools_process_latency_ms{quantile="0.5"} 2.771
cpppools_process_latency_ms{quantile="0.95"} 5.842
cpppools_process_latency_ms{quantile="0.99"} 7.120
```

## 10. 顺带修复的既有缺陷

本次必须改动 `handle_client` 才能支持 POST，因此一并修掉两个已确证缺陷：

1. **socket fd 泄漏**：`route_request` 抛异常时会跳过 `close_socket`，且异常被
   `packaged_task` 吞入被丢弃的 future，故障完全静默。改为 socket RAII + `catch(...)` 兜底 500。
2. **`finalize_latency` 计数漂移**：现在手工在 5 个 return 前各调一次，异常路径会漏，
   导致 `active_process_requests_` 只增不减、永久失真。改为 scope guard。

一个以 RAII 为主题的项目，HTTP 层却在手工配对资源——修掉它同时强化项目主线叙事。

另外，删除 `web_root` 构造参数后，服务不再依赖「必须以项目根为 CWD 启动」，
顺带消除了原静态资源相对路径带来的启动约束。

## 11. 文件变更清单

### 删除

- `web/index.html`、`web/app.js`
- `docs/api/frontend_api.md`
- `docs/algorithmic_philosophy.md`（前端流场可视化的遗留文档，已确认 `web/` 中无任何
  canvas / p5 代码引用，属孤儿文档）

### 新增

| 文件 | 用途 |
|---|---|
| `include/db_pool/score_record.h` | 数据类型（header-only） |
| `include/db_pool/mock_score_store.h` / `src/db_pool/mock_score_store.cpp` | mock 共享存储 |
| `include/server/http_util.h` / `src/server/http_util.cpp` | 可测的零拷贝解析函数 |
| `include/server/route_table.h` / `src/server/route_table.cpp` | 声明式路由表 |
| `include/server/score_api.h` / `src/server/score_api.cpp` | 4 个 CRUD handler |
| `tests/test_http_util.cpp` | 解析与路由匹配单测 |
| `tests/test_score_api.cpp` | CRUD 语义单测 |

### 修改

| 文件 | 改动 |
|---|---|
| `include/db_pool/mock_db_connection.h` / `.cpp` | 新增 CRUD 四方法；`CLIENT_FOUND_ROWS` |
| `include/db_pool/db_connection_pool.h` / `.cpp` | 持有并注入 `MockScoreStore` |
| `include/server/http_server.h` / `.cpp` | 删静态资源逻辑；接路由表；加 health/metrics/get-config；修 fd 泄漏与计数漂移 |
| `demo/server_main.cpp` | 移除 `web_root` 实参 |
| `CMakeLists.txt` | `project(... VERSION 0.2.0)` + 版本宏；抽 `http_server` 静态库；注册 2 个新测试 |
| `docs/api/backend_api.md`、`README.md` | 按新契约重写 |

## 12. 测试计划

| 用例 | 覆盖内容 |
|---|---|
| `test_http_util.cpp`（新） | `parse_request_line` / `split_target` / `find_query_value` 的缺失与空值区分；`parse_int_non_negative` 边界；分位数在 n=1、n=2 的边界；路由匹配三态（命中 / 方法不匹配 / 路径不存在） |
| `test_score_api.cpp`（新） | CRUD 四端点全部分支：创建成功 201、重复创建 409、读取 404、更新不存在 404、删除不存在 404、参数缺失 400、参数非法 400 |
| `test_pools.cpp` | 保持不变 |
| `test_thread_pool.cpp` | 保持不变 |

CTest 用例从 2 个增至 4 个。HTTP 层此前**零测试覆盖**（558 行业务路由无任何用例，
且工具函数全在匿名 namespace 中不可测），本次将其纳入覆盖是主要质量收益之一。

## 13. 已知未验证项

- 本机未安装 CMake，完整 `cmake + ctest` 流程无法在此环境验证；实施期改用
  `clang++` 直接编译链接来运行测试。CMake 配置的正确性需在装有 CMake 的环境复核。
- 本机无 MySQL 客户端库，MySQL 模式的 CRUD 分支（含 `CLIENT_FOUND_ROWS` 行为）
  **无法在此环境实测**，仅能保证 mock 模式通过。需在真实 MySQL 环境回归验证。

---

## 14. 实施结果与偏差（事后补记）

本节记录实际交付与上文设计的差异，以及评审后追加的修复。

### 14.1 与设计的偏差

| 项 | 设计 | 实际 | 原因 |
|---|---|---|---|
| 测试文件数 | 2 个 | **3 个** | 额外拆出 `test_score_store.cpp`，避免把存储层与 handler 层的断言混在一个文件 |
| 响应模型位置 | `score_api.h` 内 | 独立 `api_response.h/.cpp` | 评审指出 `build_http_response` 作为 private 成员无法测试，抽成自由函数后 204/405 协议细节才能被断言覆盖 |
| CTest 用例数 | 4 | **5** | 同上 |

### 14.2 评审后追加的修复

两个阻塞级缺陷（均已在真实服务上验证）：

1. **SIGPIPE 未防护**：向已断开的连接 `send` 会收到 SIGPIPE，默认处置是
   **终止整个进程**——一个 curl 超时就能搞挂服务。修法：macOS/BSD 用
   per-socket 的 `SO_NOSIGPIPE`，Linux 用 `MSG_NOSIGNAL`，均不修改全局信号处置。
   同时给 `recv` / `send` 加上 `EINTR` 重试。

2. **请求行截断被静默接受**：`parse_request_line` 原有两处宽容（无 CRLF 就当完整行、
   无第二个空格就当缺 HTTP 版本），叠加后丢失了完整性检测：超长 URL 被 4096
   缓冲截断在 `score=98765` 中间会变成 `score=98` 并通过校验，**持久化错误数据
   并返回 201**。修法：未见行终结符一律报错，读满缓冲时返回 `414`。

其余：`~ScopeExit` 改为 `noexcept` 并内部吃异常（否则展开期再抛会 `std::terminate`，
恰好抵消引入它的初衷）；204 不再发送 `Content-Length`（RFC 7230 3.3.2 MUST NOT）；
405 补上 `Allow` 头（RFC 7231 6.5.5 MUST）；MySQL 写后回读失败不再把已成功的写
改判为 404；delete 去掉无用的前置回读；清除死代码 `trim()` 与 `RequestContext::path`。

**重要**：设计中声称的「新增路由忘接 handler 会编译报错」当时并不成立，
因为 CMake 未开启 `-Wall`（`-Wswitch` 未启用且无 `-Werror`）。已通过
`cmake/CompilerWarnings.cmake` 统一开启告警并把 `-Werror=switch` 升为错误，该承诺方真正生效。

### 14.3 后续的结构重整（独立提交）

本设计落地后又做了一次结构重整，与上文的文件路径已不一致，以 README 为准：

- 布局改为 merged（头源同置），include 根为 `src/`，路径形如 `cpppools/<模块>/<文件>.h`
- 引入命名空间 `cpppools::pool` / `cpppools::db` / `cpppools::server`
- `MockDBConnection` → `DbConnection`（名字与“连真实数据库”的定位对齐），
  并把 `DBXxx` 类名统一为 `DbXxx`
- `demo/` → `apps/`，产物改名 `cpppools-server` / `cpppools-bench`；
  删除与 `test_thread_pool` 完全重叠的 `demo/main.cpp`
- 新增 `cmake/CompilerWarnings.cmake`、`.clang-format`、
  `scripts/build.sh`、`scripts/run_server.sh`
