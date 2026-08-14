# CppPools — 多数据源数据库中间件（三级池化 + 分布式集群）

> C++17 / SQLite（MySQL 后端规划中）/ 自研 Socket HTTP / Python SDK。
> 客户端发送通用 JSON CRUD 请求，服务端按表名透明路由到配置的数据源 ——
> 三大池子（线程池 / 对象池 / 连接池）是它真实的运行时骨架。
> 多节点部署时按一致性哈希把每个键路由到唯一 owner，带行级写穿透缓存。

## 定位

```
SDK / curl
  │  建环（/api/v1/cluster）→ 按 (源,表,唯一列,值) 直连 owner
  ▼
┌─────────────────── CppPools 节点 × N（单节点即退化形态）────────────┐
│  ThreadPool → ObjectPool → 校验管道 → 表路由 → 归属门控（307/epoch）  │
│       → RowCache（行级写穿透缓存，每数据源共享）                    │
│       → DbConnectionPool × 数据源数                                 │
└───────────────────────────────────────────────────────────┘
  ▼
SQLite（已实现） / MySQL（规划中）…… 每节点各自的数据源（真理之源）
```

三条主线：

1. **三级池化**：把「昂贵资源的创建」与「请求的到来」解耦。线程、请求上下文、
   数据库连接都在启动期预建，请求路径上只做借还，借不到时以超时（`503`）
   显式失败，而不是无限等待。
2. **数据库中间件**：客户端不必知道底层是 SQLite 还是 MySQL（透明性）；
   但客户端应当知道数据去了哪个逻辑库（响应回传 `datasource`，
   歧义表必须显式指定，绝不兜底猜）。
3. **分布式集群（可选）**：配置 `cluster` 段后，每个键归唯一 owner 节点
   （单 owner ⇒ 无需失效广播/分布式锁）；非 owner 回 307；节点故障由
   SDK 黑名单 + attempt 绕行承担；缓存行级写穿透，稳态同键强一致。
   不配 `cluster`/`cache` 段 = 单节点无缓存，行为逐字节不变。

- API 契约：[docs/api/backend_api.md](docs/api/backend_api.md)
- SDK 文档：[docs/api/sdk.md](docs/api/sdk.md)（`sdk/python/`，零依赖）
- 设计文档：`docs/superpowers/specs/2026-08-06-db-middleware-design.md`（中间件）、
  `docs/superpowers/specs/2026-08-11-cluster-cache-design.md`（集群与缓存）

---

## 1. 项目结构

```text
CppPools/
├── CMakeLists.txt                          # 构建入口
├── README.md
├── LICENSE                                 # MIT
│
├── config/
│   └── cpppools.example.json               # 配置示例（server + cluster + cache + datasources）
│
├── include/cpppools/                       # 公开头文件（对外接口）
│   ├── config/config.h                     #   JSON 配置类型与加载（含 cluster/cache 段）
│   ├── pool/                               #   namespace cpppools::pool
│   │   ├── thread_pool.h                   #     固定 worker + 有界队列与 503 拒绝 + 双提交入口
│   │   └── object_pool.h                   #     泛型对象池（RAII 归还 + reset 复用）
│   ├── db/                                 #   namespace cpppools::db
│   │   ├── value.h / schema.h / row_request.h  # 适配层类型系统（Value/Schema/CRUD 请求）
│   │   ├── database.h                      #     NVI 基类 + 内联校验管道
│   │   ├── database_factory.h              #     后端工厂注册表（type → 工厂函数）
│   │   ├── datasource_registry.h           #     多数据源 + table→datasource 路由 + 共享缓存注入
│   │   └── db_connection_pool.h            #     有界连接池 + DbConnGuard
│   └── server/                             #   namespace cpppools::server
│       ├── api_response.h                  #     响应模型 + HTTP 报文组装
│       └── http_server.h                   #     accept 循环 + 分发 + 归属门控 + 遥测
│
├── src/                                    # 实现 + 私有头文件
│   ├── main.cpp                            #   服务进程入口（配置驱动 + 优雅停机）
│   ├── config/config.cpp
│   ├── pool/thread_pool.cpp
│   ├── db/
│   │   ├── database_factory.cpp
│   │   ├── sqlite_database.cpp             #   SQLite 后端（自省/stmt 缓存/WAL）
│   │   ├── statement_cache.h               #   私有：每连接 prepared statement 缓存
│   │   ├── row_cache.h                     #   私有：行级缓存（分片锁/LRU/TTL/世代号/Singleflight）
│   │   ├── caching_database.h              #   私有：缓存装饰器（写穿透双删失效）
│   │   ├── datasource_registry.cpp
│   │   └── db_connection_pool.cpp
│   └── server/
│       ├── http_util.{h,cpp}               #   私有：零拷贝请求/头解析
│       ├── route_table.{h,cpp}             #   私有：constexpr 声明式路由表
│       ├── crud_api.{h,cpp}                #   私有：通用行级 CRUD + 归属门控接入
│       ├── hash_ring.{h,cpp}               #   私有：murmur3 + 一致性哈希环（黄金向量钉死）
│       ├── ownership_router.{h,cpp}        #   私有：归属决策矩阵（307/Fallback/epoch）
│       ├── api_response.cpp
│       └── http_server.cpp
│
├── sdk/python/                             # Python SDK（零依赖，见 docs/api/sdk.md）
│   ├── cpppools/                           #   hashing/ring/transport/errors/client
│   └── tests/                              #   51 单测 + 三节点 live e2e
│
├── test/                                   # 18 个 CTest 用例（见 §7）
├── third_party/                            # vendored 依赖（nlohmann/json 单头文件）
├── docs/
│   ├── api/backend_api.md                  #   API 契约
│   ├── api/sdk.md                          #   SDK 文档
│   ├── benchmarks/                         #   性能基线与验收归档
│   ├── mysql_setup.sql                     #   MySQL 初始化脚本（MySQL 后端启用后用）
│   ├── learning_log.md
│   └── superpowers/                        #   设计 spec 与实施计划
├── scripts/
│   ├── build.sh                            #   构建 + 测试（Linux/macOS）
│   ├── run_server.sh                       #   启动服务（自动种子演示库）
│   ├── init_demo_db.sql                    #   演示库建表脚本
│   ├── cluster_e2e.sh                      #   双节点集群 e2e（307/Fallback/epoch）
│   ├── sdk_e2e.sh                          #   三节点 SDK e2e（杀节点/归队/滚动重启）
│   └── bench_baseline.py                   #   性能基线采集
└── build/                                  # 构建输出（已 gitignore）
```

### 头文件的公开 / 私有边界

- `include/cpppools/<模块>/` —— 对外接口。库的消费者与 `main.cpp` 只用这些。
- `src/<模块>/` —— 实现，以及**私有头文件**。
  `http_util` / `route_table` / `crud_api` / `statement_cache` /
  `hash_ring` / `ownership_router` / `row_cache` / `caching_database`
  是实现细节；测试通过额外的 `-Isrc` 直接访问它们（测试相对普通消费者的合理特权）。

### 模块依赖

```
server ──> pool
   └─────> db ──> config
```

有向无环。`DbConnectionPool` 归在 `db/`：它是三大池子之一，
但与连接抽象（`Database`）是一个内聚单元 —— 按依赖关系而非叙事口号切模块。

---

## 2. 三级池化实现

### 2.1 ThreadPool

[include/cpppools/pool/thread_pool.h](include/cpppools/pool/thread_pool.h)

- **有界任务队列**（默认 1024，`CPPPOOLS_MAX_QUEUE_SIZE` 可调）+ 拒绝策略。
  队列满时 `submit` 返回 `std::nullopt`，accept 循环**立即回 503** —— 真实背压。
- `std::mutex` + `std::condition_variable`，`wait(lock, predicate)` 防虚假唤醒。
- **两个提交入口**：
  - `submit(F, Args...)` → `std::optional<std::future<R>>`，异常由 future 携带；
  - `submit_detached(F, Args...)` → `bool`，异常交给 `on_task_exception` 回调 + 计数。

  为何必须两个：`packaged_task::operator()` 把异常**存进 future 而不重抛**，
  调用方一旦丢弃 future（HTTP 层正是如此），异常就永久静默。
  注意 `task_exception_total` **只覆盖 `submit_detached`**，值为 0 不等于没异常。
- 队列元素是 `std::packaged_task<void()>` 而非 `std::function<void()>`：
  后者要求目标**可拷贝**，move-only 参数存不进去。
- `shutdown()` 排空队列后 join；`shutdown_now()` 丢弃未开始任务后 join；
  **析构走 `shutdown_now()`**（析构不应无限期阻塞）。
- `stats()` 一次取回全部指标；worker 线程命名 `<name>-<index>`，便于定位。

### 2.2 ObjectPool\<T\>

[include/cpppools/pool/object_pool.h](include/cpppools/pool/object_pool.h)

- 构造 `(name, init_size, max_size)`：**有界池**，达到上限且无空闲时
  `acquire()` 立即返回**空 Ptr**（不阻塞）；`main.cpp` 按
  `max(128, worker_count * 2)` 计算容量。该不变式是**运行期配置而非类型保证**，
  调用方必须判空（`http_server.cpp` 已加，耗尽回 503）。
- `acquire()` 返回 `unique_ptr<T, PoolDeleter<T>>`：**无状态** deleter，
  析构自动归还 —— RAII。
- **异常安全**：出池后的 `reset()` 若抛异常，对象被**销毁**并回滚计数，
  不会把"部分重置"的脏对象交给下一个调用方。
- `stats()` 一次锁取一致快照；析构期拦截「池先销毁、对象后归还」的错误。

### 2.3 DbConnectionPool

[include/cpppools/db/db_connection_pool.h](include/cpppools/db/db_connection_pool.h)

- **池元素是 `Database`**（连接 + schema 快照 + statement cache 三合一），
  每个数据源一个池。建连由 `ConnectionFactory` 提供（经
  `DataSourceRegistry` 按配置生成），池只负责借还、探活与重建。
- **健康检查与失效重建**：借出的连接若空闲超过阈值才探活（HikariCP 风格，
  热连接零开销；SQLite 探活 = cached `SELECT 1`）；探活失败则销毁并用
  factory 重建。重建失败**保留空槽**，下次 acquire 再试 —— 数据库恢复后自愈。
- **有界池 + 绝对 deadline**：`acquire(timeout)` 基于 `wait_until`，
  timeout 是绝对 deadline（不每次重试重新计满）。
  同一次 acquire 会尝试所有未试过的空闲槽位，但同一槽位不原地重试；
  空槽全部试过后**条件变量挂起**等待（不是忙等待 —— 该缺陷曾实测
  200ms 烧 98% CPU，已修复并有 rusage 回归测试）。
- `release()` 用反查表校验归还合法性；重复 / 野指针归还被拒绝、计数并打 stderr。
- `DbConnGuard` 把「借→还」绑到栈对象，业务抛异常也能自动归还；可移动。

### 2.4 请求在三池间的接力

```
accept 主循环（主线程）
  └─ ThreadPool::submit_detached(handle_client)  ← 主线程立刻回到 accept
       │  队列已满 → 在 accept 循环直接回 503（真实背压）
       └─ 解析请求行（零拷贝 string_view）→ 读请求体（Content-Length 模型）
            └─ find_route() 查 constexpr 路由表
                 └─ handler
                      ├─ ObjectPool::acquire()     ← RAII 归还，耗尽 503
                      ├─ JSON 解析 + 校验管道 + 路由
                      └─ DbConnGuard(pool, 100ms)  ← RAII 归还，超时 503
                           └─ Database CRUD（prepared statement 复用）
```

---

## 3. 数据库适配层

### 3.1 校验管道（安全核心）

`Database` 基类用 **NVI 惯用法**把校验做进公开接口：
`select/insert/update/remove` 是**非虚**的，内部先过校验管道再调
子类的 `do_*` —— 新增后端在结构上不可能绕过校验。

管道规则：

1. 表名 / 列名必须在**启动时自省的 schema 白名单**内 ——
   SQL 里的标识符来自 schema 快照而非客户端输入，注入面在入口处被
   集合成员判断消除（不依赖转义）。
2. `where` 列必须是**主键 / 唯一键** —— 每个操作至多影响一行，
   中间件在结构上无法被用来批量改删。
3. `where` 值不得为 NULL；列值**严格类型校验**（整数↛文本、小数↛整数），
   唯一放行：整数可赋给 REAL 列（无损）。
4. 接口**没有任何建表 / 删表方法** —— "不能改关系模型"是类型级保证。

### 3.2 多数据源路由（DataSourceRegistry）

- 启动时逐源探连 + 自省，每源建一个连接池，建立 `table → [datasource]` 索引。
- 路由：显式 `datasource` 优先；表名唯一 → 自动路由；
  **跨源歧义且未指定 → 409 + 候选清单，绝不按配置顺序兜底猜**
  （兜底会造成 affected_rows:1 的静默写错库）。歧义表在启动日志告警。
- 扩展新后端 = 实现一个 `Database` 子类 + 在工厂注册表登记一行，
  基类 / 路由 / HTTP 层零改动。

### 3.3 SQLite 后端

- 打开：`READWRITE` 不带 `CREATE`（中间件不建库）；文件库启用
  WAL + `busy_timeout`（多读单写并发；嵌入式后端的连接池收益主要是
  statement 缓存与并发读，而非写吞吐）。
- 自省：按 SQLite 亲和规则映射类型（NUMERIC→real）；rowid 别名
  （恰好 `INTEGER` 的独列 PK）记为自增；partial / 联合唯一索引不标列级 unique；
  BLOB 亲和列直接拒绝该数据源（Value 不表达 BLOB）。
- 执行：每表每操作固定 SQL + prepared statement 缓存（键空间被 schema 限定）；
  select 全列取回后投影；insert 缺省列绑 NULL（自增主键自动生成）；
  update 读改写（SET 全列，缺省以现值回填）。
- 读侧按 schema 口径规范化：外部写入的异构数据（NUMERIC 列的整数存储）
  提升为正确类型，无法修正的报可读错误。
- 状态码映射：UNIQUE/PK 冲突 → 409；NOT NULL/CHECK 违例 → 400；
  其余后端错 → 500 透传消息。
- `:memory:` 字面路径强制单槽（每 open 一个独立内存库的陷阱）；
  `file::memory:?cache=shared` 共享内存库支持多槽。

---

## 4. 分布式集群与行级缓存（可选）

设计全文见 `docs/superpowers/specs/2026-08-11-cluster-cache-design.md`；
API 契约见 `docs/api/backend_api.md` §3.6/§5.5/§5.6。

### 4.1 单 owner 路由

路由键 = 缓存键 = `(datasource, table, 唯一键列, 键值)`，在一致性哈希环
（murmur3_32 seed=0 无符号，100 vnodes/节点，长度前缀编码）上归唯一
owner。同一键的增删改查都经同一节点的同一份缓存副本 —— **无需失效广播、
无需分布式锁**，稳态同键强一致。SDK 与服务端各自建环、逐字同参，
由 spec 冻结的黄金向量两端共同钉死（跨语言一致是路由契约的根）。

服务端权威自判归属：非 owner 且未带 Fallback 头 → **307 重定向**到
owner（不做代理转发：零出向依赖、零线程占用）。拓扑变更用 `epoch`
门控：视图落后的一端自动退化为"正确但无加速"。

### 4.2 行级缓存（RowCache + 装饰器）

- **每数据源一个共享实例**（跨连接；per-connection 缓存会打破单节点内
  的强一致），16 分片锁；LRU 双阈值（条目数 + 字节数）+ 单条上限。
- **写穿透 + 双删失效**：写直达 DB，成功后删键（而非更新：自增主键/
  读改写结果都在后端才确定，删掉让下次读回源显然正确）；失效永不 bypass。
- **回填防陈旧**：分片世代号 CAS（回源前记世代，回填时锁内原子比对）——
  写后到达的旧回填直接丢弃；同键并发 miss 由 Singleflight 合并为一次回源。
- 只缓存 kOk 非空单行；TTL 惰性过期；REAL 键列不缓存不路由。
- 故障语义：owner 不可达时 SDK 绕行，替补节点就地处理但**不缓存**
  （规则 A）；归队后由 TTL + Rejoin 清缓存收敛（规则 B/C）。
- 运行期 kill-switch：`POST /api/v1/cache/disable|enable`（疑似脏读时
  第一动作是关缓存而非重启）。

### 4.3 SDK 与可观测性

`sdk/python/`（零依赖）：建环、直连 owner、307 跟随、黑名单 + attempt
绕行 + Fallback 头、全黑清空重试一轮、回切带 Rejoin 头、epoch 不符立即
刷拓扑、错误分类重试（自增 insert 非幂等不重试）。详见 docs/api/sdk.md。

指标：`cache_*` 族（hits/misses/entries/bytes/evictions/invalidations/
singleflight/bypass{reason}/flush{reason}，per-datasource）、
`cpppools_redirects_total`；验收归档见 `docs/benchmarks/`（命中率 99.97%、
DB 读 QPS 降 99.8%、服务端读 p99 降 80%、写退化 ≤2.4%）。

---

## 5. 网络与可观测性

### 5.1 自研 Socket 级 HTTP/1.1

[src/server/http_server.cpp](src/server/http_server.cpp)

- 直接基于 BSD socket / Winsock，不依赖第三方 HTTP 库。
- 模型：一次读取 + 一次响应 + 主动关闭。仅 GET / POST。
  CRUD 端点的 JSON 请求体按 **Content-Length** 循环读取（上限 1 MB，
  超限 413；读体 5 秒超时防"声明 body 却不发"钉死 worker；
  路由前预校验请求行）。所有响应带 `X-Request-Id`（对账用）。
- **过载背压**：线程池队列满时，accept 循环直接回 503 并关连接。
- **优雅停机**：SIGTERM/SIGINT → 停 accept（poll 200ms 唤醒）→ 排空
  在途请求（上限 5s）。
- 路由由 `constexpr` 表集中声明；`RouteId` 配 `switch` 全覆盖分发，
  **新增路由忘接 handler 会被 `-Werror=switch` 拦成编译错误**。
- 未预期异常在 `handle_client` 兜底为 500（连接不会静默悬挂）。

### 5.2 指标端点

- `GET /health` — 存活探针（纯文本）
- `GET /metrics` — Prometheus exposition 文本；池指标统一带 `pool` 标签
  （`worker` / `request` / 各数据源名）
- `GET /api/v1/stats` — 三池瞬时值 + 历史峰值 + 累计计数器 + 缓存聚合
  + 集群身份 + P50/P95/P99
- `GET /api/v1/get-config` — 启动期配置快照（不含凭据）
- `GET /api/v1/cluster` — 拓扑/哈希参数/TTL/配置摘要（SDK 建环唯一信息源）

字段明细见 `docs/api/backend_api.md` §5。

### 5.3 统一统计：两类形状，不强行拉平

- **借→还池**（ObjectPool / DbConnectionPool）共用 `AcquireStats`。
  等式不变式因池而异：对象池 `in_use + available == size`；
  连接池 `in_use + available == capacity` 且 `size <= capacity`
  （重建失败的空槽占槽位、不计存活）。
- **线程池**用自己的 `TaskStats`：它不是借→还池，
  把 `max_queue_size` 映射成 `capacity` 是错误建模。

命名约定：counter 带 `_total`、峰值带 `peak_`、历史最小带 `min_`。

---

## 6. 快速开始

### 6.1 环境要求

- C++17 编译器（GCC 7+ / Clang 6+ / MSVC 2017+ / MinGW-w64）
- CMake 3.16+
- SQLite3 开发库（macOS SDK / 主流 Linux 发行包自带）
- Python 3.8+（仅 SDK 与其测试需要，零第三方依赖）

### 6.2 构建与测试

```bash
# Linux / macOS：一条命令完成构建 + 测试
scripts/build.sh

# 或手动
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure   # 预期 18/18 通过
python3 -m unittest discover -s sdk/python/tests   # SDK 单测，预期 51 全绿
```

产物：`build/bin/cpppools_server` 与 18 个测试二进制。

### 6.3 启动并验证（单节点）

```bash
# 首次使用：cp config/cpppools.example.json config/cpppools.json
# run_server.sh 会在库文件不存在时用 scripts/init_demo_db.sql 种子演示库
scripts/run_server.sh
# 或直接：./build/bin/cpppools_server --config config/cpppools.json
# 配置路径优先级：--config > CPPPOOLS_CONFIG 环境变量 > config/cpppools.json

curl http://127.0.0.1:8080/health
curl -X POST http://127.0.0.1:8080/api/v1/insert -d '{"table":"user_scores","values":{"user_id":42,"score":99,"updated_at_ms":0}}'   # 201
curl -X POST http://127.0.0.1:8080/api/v1/select -d '{"table":"user_scores","columns":["score"],"where":{"column":"user_id","value":42}}'  # 200
curl -X POST http://127.0.0.1:8080/api/v1/update -d '{"table":"user_scores","set":{"score":88},"where":{"column":"user_id","value":42}}'   # 200
curl -X POST http://127.0.0.1:8080/api/v1/delete -d '{"table":"user_scores","where":{"column":"user_id","value":42}}'                       # 200
curl        http://127.0.0.1:8080/api/v1/schema
curl        http://127.0.0.1:8080/api/v1/stats
```

多数据源：在配置文件的 `datasources` 数组里加源即可；
表名跨源重复时请求需带 `"datasource":"<源名>"`（否则 409）。

### 6.4 集群与缓存

配置文件加 `cluster`（节点身份 + 拓扑 + epoch）与 `cache`（`enabled:true`）
两段（格式见 `docs/api/backend_api.md` §6 与 `config/cpppools.example.json`）；
每个节点各自启动，`node_id` 不同、`nodes` 列表相同。集群验证：

```bash
bash scripts/cluster_e2e.sh   # 双节点：路由分布/307 跟随/Fallback/epoch
bash scripts/sdk_e2e.sh       # 三节点：杀节点绕行/归队清缓存/滚动重启
```

客户端用 SDK 直连 owner（裸 curl 需自己处理 307，加 `-L`）：

```python
import sys; sys.path.insert(0, "sdk/python")
from cpppools.client import ClusterClient
client = ClusterClient(["http://127.0.0.1:8080"])
client.insert("user_scores", {"user_id": 42, "score": 99, "updated_at_ms": 0})
```

### 6.5 观察池化行为

```bash
# 观察连接池耗尽（503）需要 worker 数 > 连接池大小
scripts/run_server.sh --workers 32
for i in $(seq 1 200); do
  curl -s -X POST "http://127.0.0.1:8080/api/v1/select" \
    -d "{\"table\":\"user_scores\",\"where\":{\"column\":\"user_id\",\"value\":$i}}" \
    -o /dev/null &
done; wait
curl -s http://127.0.0.1:8080/api/v1/stats  # 看 db_timeout_requests 与 min_db_pool_free
```

两种 503 用指标区分：`cpppools_db_acquire_timeout_total`（连接池耗尽）
与 `cpppools_rejected_requests_total`（线程池队列满）。

### 6.6 运行期环境变量

数据源建连完全由 JSON 配置驱动，不再有数据库连接环境变量。剩余：

| 变量 | 默认值 | 说明 |
|---|---|---|
| `CPPPOOLS_MAX_QUEUE_SIZE` | `1024` | 线程池任务队列上限；满则回 `503`（真实背压）|
| `CPPPOOLS_DB_ACQUIRE_TIMEOUT_MS` | `100` | 连接获取超时 |
| `CPPPOOLS_LATENCY_WINDOW_SIZE` | `1024` | 延迟窗口样本上限（配置文件优先） |

---

## 7. 测试

| 用例 | 覆盖 |
|---|---|
| `test_thread_pool` | 17 用例：有界队列拒绝 / 双提交入口 / move-only 参数 / shutdown 与 shutdown_now / TaskStats / 线程命名 |
| `test_object_pool` | 有界 max_size 与立即失败、异常安全窗口（reset 抛出时销毁并回滚）、无状态 deleter、一致快照 |
| `test_db_pool` | 构造 fail-fast、探活/重建/自愈、绝对 deadline、无忙等待（rusage）、guard 移动、非法归还计数、并发借还（TSan 干净） |
| `test_config` | JSON 配置解析：缺字段、类型错、password_env、多 default 冲突、cluster/cache 段与不变量 I1 |
| `test_schema_validator` | NVI 校验管道全拒绝分支（表/列/where 唯一键/NULL/类型/重复列）、工厂注册 |
| `test_statement_cache` | 每连接 statement 缓存：命中/淘汰/计数 |
| `test_database_conformance` | SQLite 后端一致性矩阵（自省/类型往返/冲突/读改写/脏数据受控）——裸后端与套缓存装饰器**双跑**；MySQL 后端的复用基线 |
| `test_routing` | 多数据源：自动路由、歧义 409、显式消歧、跨源隔离、构建错误 |
| `test_http_util` | 请求行解析（含截断防御）、Content-Length/通用头解析、路由三态匹配、报文组装 |
| `test_http_server` | handle_client 端到端（socketpair）：对象池耗尽 503、DB 超时 503、请求体边界、X-Request-Id、优雅停机、计数器口径 |
| `test_metrics` | /metrics 与 /api/v1/stats 的格式契约：pool 标签、task_exception 的 detached-only 警告、stats 字段 |
| `test_crud_api` | 通用 CRUD 状态码矩阵：201/200/400/409/歧义 409/JSON 异常面/脏 UTF-8 容错/schema 结构 |
| `test_hash_ring` | murmur3 + 环：spec 黄金向量（含碰撞 tie-break/回绕），与 Python SDK 跨语言同源 |
| `test_cluster_api` | /api/v1/cluster 契约、单节点退化、epoch/TTL/digest 回显 |
| `test_row_cache` | 命中/miss/TTL/双阈值 LRU/超限拒收/世代号拦回填（确定性构造）/Singleflight/clear |
| `test_caching_database` | 装饰器语义：**跨连接/跨槽位共享缓存**（B1 守卫）、写后失效、投影三语义、per-request bypass |
| `test_cache_metrics` | cache_* 指标族、kill-switch 端点行为、Rejoin 节流 |
| `test_ownership` | 归属决策矩阵五行（路由器级 + HTTP 级）、B5 失效专项、307/epoch/Fallback 头组合 |

另：SDK 51 单测（fake 集群）+ 两个 live e2e 脚本（双节点 `cluster_e2e.sh`、
三节点 `sdk_e2e.sh`：稳态 0 个 307 = 两端环一致的硬证据）。

```bash
ctest --test-dir build --output-on-failure
```

测试基于 `assert`，CMake 对测试目标显式加 `-UNDEBUG` ——
否则 Release 下断言被消除，测试退化成空 `main` 全部假绿。
关键行为另有变异测试守护（删除某个检查必然导致测试失败）。

---

## 8. 演进方向

| 方向 | 现有基础 |
|---|---|
| **MySQL 后端**（暂缓，保留） | 工厂注册表已留位：实现 `Database` 子类 + 注册一行；`test_database_conformance` 断言矩阵直接复用；另有唯一性自省口径专项待做（`COLUMN_KEY='UNI'` 与联合唯一/前缀索引的对应） |
| 集群 v1.1 | SDK keep-alive（requests 注入）、热键账本 + 归队预热（判据：归队后 miss 率/DB QPS 尖峰）、缓存前置到池外（命中免借连接，触发条件见 spec §4.1.2） |
| Redis 共享缓存模式 | 同一缓存抽象换后端；该模式下环退化为负载均衡（spec §10 扩展位） |
| 数据源级只读标记 | 配置加字段即可，写操作在路由层拒绝 |
| 多条件 where / 比较运算符 | `WhereClause` 可扩展为条件数组，基类接口不动 |

暂不引入：keep-alive、异步 I/O、调度器隔离 —— 当前是
「固定线程池 + 阻塞 thread-per-connection」模型，引入这些会改变整体架构，
应在真正需要时作为独立演进项处理。

---

## 9. 已知限制

- 请求行上限 4095 字节（超出 `414`），请求体上限 1 MB（超出 `413`）；
  不支持 keep-alive、chunked、TLS。
- `where` 仅支持**单列等值**且必须是主键 / 唯一键；无分页、联表、事务、批量
  （刻意收窄：中间件只提供单条记录的增删改查）。
- 接口无 DDL 能力：表结构由使用方预置，schema 变更后需重启服务。
- SQLite：不支持 BLOB 列与 WITHOUT ROWID 表（启动期显式拒绝/已知边界）；
  `:memory:` 字面路径强制单槽；写并发受 SQLite 单写者模型限制（WAL 下多读单写）。
- `/api/v1/stats` 的跨池字段逐个读取，池内是一致快照、跨池不构成全局原子快照。
- 连接探活失败后由 factory 重建；`:memory:` 数据源重建会得到空库（固有属性，仅影响该用法）。
- 集群：缓存命中仍需借连接（`pool_size` 即并发上限，spec §4.1.2）；
  故障窗口内经替补写入的数据，owner 归队后读 owner 库读不到（同键最终一致，
  旧读窗口 ≤ TTL，spec §5.1 例外②）；SDK 无连接复用（v1.1）；
  节点间配置一致性无强制手段，由 `datasource_digest` 比对提供检测。
