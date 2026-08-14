# CppPools 架构参考

SKILL.md 的深化材料：模块地图、请求路径、关键设计决策。
规范性口径以 spec 与 `docs/api/` 为准，本文件只做结构化索引。

## 1. 模块地图

```
include/cpppools/            公开接口（库消费者与 main 只用这些）
├── config/config.h          AppConfig：server / cluster / cache / datasources
├── pool/thread_pool.h       固定 worker + 有界队列（满则拒绝→503 背压）
├── pool/object_pool.h       泛型对象池（RAII 归还 + reset 复用 capacity）
├── db/value|schema|row_request.h   适配层类型系统
├── db/database.h            NVI 基类：public 非虚 CRUD = 校验管道；
│                            protected 纯虚 do_* 给后端；无 DDL（类型级保证）
├── db/database_factory.h    type → 工厂注册表（新后端登记一行即接入）
├── db/datasource_registry.h 多源路由 + 每源连接池 + 每源共享 RowCache 注入
├── db/db_connection_pool.h  有界连接池（元素 = Database）+ DbConnGuard
└── server/http_server.h     HttpServer + ClusterView + HttpServerConfig

src/                         实现 + 私有头（测试经 -Isrc 访问）
├── db/sqlite_database.*     SQLite 后端（自省/WAL/statement cache/读侧规范化）
├── db/statement_cache.h     每连接 prepared statement 缓存
├── db/row_cache.h           行级缓存：16 分片锁 / LRU 双阈值 / TTL 惰性 /
│                            世代号 CAS 回填 / Singleflight / clear(Rejoin)
├── db/caching_database.h    缓存装饰器：写穿透双删失效、投影三语义、
│                            per-request bypass（归属门控通道）
├── server/http_util.*       零拷贝请求行/头解析
├── server/route_table.*     constexpr 声明式路由表（12 端点）
├── server/crud_api.*        通用 CRUD handler + 归属门控接入
├── server/hash_ring.*       murmur3_32 + 一致性环（黄金向量钉死）
└── server/ownership_router.* 归属决策矩阵（307/Fallback/epoch）+ 计数器

sdk/python/cpppools/         零依赖 SDK
├── hashing.py / ring.py     与服务端逐字同口径（spec §3.2 黄金向量对拍）
├── transport.py             urllib 单请求 + 错误分类矩阵
├── errors.py                异常体系（retryable 语义）
└── client.py                ClusterClient：拓扑原子替换 / 黑名单 / 绕行 /
                             Rejoin / epoch 门控 / digest 比对
```

依赖方向：`server → {pool, db} → config`，有向无环。

## 2. 一个请求的完整路径

```
accept（poll 200ms 唤醒，优雅停机检查点）
 └─ ThreadPool::submit_detached(handle_client)      队列满 → accept 层 503
     └─ 解析请求行/头（零拷贝）+ Content-Length 读体（上限 1MB）
         └─ route_table 匹配（constexpr 表 + switch 全覆盖）
             ├─ 集群头解析：X-CppPools-Fallback / Epoch / Rejoin
             │    └─ Rejoin → handle_rejoin（10s CAS 节流清缓存，规则 B）
             └─ CRUD handler（crud_api）
                 ├─ registry.route(table, datasource)   歧义 → 409 不兜底
                 ├─ DbConnGuard 借连接（100ms 超时 → 503）
                 ├─ 归属门控：schema 定路由键 → OwnershipRouter.decide
                 │    ├─ 非 owner 无 Fallback → 307 + Location
                 │    ├─ 就地 bypass → db->set_cache_bypass(true)
                 │    └─ epoch 不符 → bypass + 响应回显 X-CppPools-Epoch
                 └─ Database CRUD = 校验管道（白名单/唯一键/类型）→ do_*
                     └─ CachingDatabase：命中投影 / miss+Singleflight+CAS 回填
                         / 写穿透 + 双删失效（I0）
                             └─ SqliteDatabase（statement cache）→ SQLite
```

## 3. 关键设计决策（改动前必读其理由）

| 决策 | 内容 | 出处 |
|---|---|---|
| NVI 校验管道 | 校验在基类 public 非虚方法内联，后端结构上无法绕过；标识符来自 schema 快照（防注入 = 集合成员判断，非转义） | db-middleware spec |
| where 唯一键 | 每操作至多一行 ⇒ 接口无法批量改删；0 行 = 200（SQL 语义，非 404） | backend_api §1.4/§1.5 |
| 歧义不兜底 | 跨源重名表未显式指定 → 409 + 候选清单；兜底会静默写错库 | backend_api §3.5 |
| 单 owner 路由 | 路由键 = 缓存键 = (源,表,唯一列,值)；datasource 哈希口径 = 显式值或空串（两端必须同口径）；insert 取 schema 列序第一个有值非 NULL 唯一列；REAL 键/NULL/无键不路由 | cluster spec §3.3/§3.6 |
| 307 而非代理转发 | 无出向 HTTP 客户端依赖、不占 worker、无级联故障 | cluster spec §3.6 |
| 提示单调性 | 客户端头只能让节点更保守（不缓存/不重定向），不能更激进 | cluster spec §3.6 |
| epoch 门控 | 每请求带头；不符 → bypass + 回显；SDK 立即刷拓扑、**不重发**（写重发撞 409） | cluster spec §5.4 |
| 写穿透双删 | 前置删 → 落库 → 后置删；失效而非更新（自增主键/读改写结果后端才确定） | cluster spec §4.3 |
| 世代号 CAS 回填 | 回源前记世代，回填在分片锁内原子比对；拦"写后到达的旧回填" | cluster spec §4.3.1 |
| I0 / I1 | 失效永不 bypass；blacklist_ttl ≥ cache_ttl（fail-fast） | cluster spec §5.2 |
| 三规则 | A 归属门控（非 owner 不缓存）/ B 归队清缓存（best-effort）/ C TTL（正确性真正承担者） | cluster spec §5.2 |
| 幂等矩阵 | select/update/delete 可重试；显式键 insert 可重试（409=已生效）；自增 insert 绝不重试 | cluster spec §5.5 |

## 4. 可观测性速查

- `/metrics`（Prometheus）：池指标 `{pool=worker|request|数据源名}`；
  `cache_*` 族 per-datasource（含 `bypass_total{reason}` / `flush_total{reason="rejoin"}`）；
  `cpppools_redirects_total`。
- `/api/v1/stats`：三池快照 + 缓存聚合（cache_enabled/entries/hits/misses）
  + 集群身份（cluster_epoch/node_id）+ 延迟分位。
- `/api/v1/cluster`：SDK 建环唯一信息源（拓扑/哈希参数/TTL/datasource_digest）。
- `X-Request-Id`：所有响应携带，与日志对账。

## 5. 测试矩阵速查

| 层 | 入口 | 钉住什么 |
|---|---|---|
| C++ 单测 ×18 | `ctest --test-dir build` | 三池行为、校验管道全分支、conformance（裸后端+装饰器双跑，MySQL 复用基线）、黄金向量、决策矩阵五行、Rejoin 节流 |
| SDK 单测 ×51 | `unittest discover -s sdk/python/tests` | 黄金向量跨语言、错误矩阵、路由/绕行/全黑/Rejoin/epoch/畸形拓扑 |
| live e2e ×2 | `scripts/cluster_e2e.sh`、`scripts/sdk_e2e.sh` | 稳态 0 个 307 = 两端环一致；杀节点/归队/滚动重启全链路 |
| 压测 | `scripts/loadtest_sdk.py` | 缓存对照、归队风暴观测（docs/benchmarks/ 归档） |

## 6. 已知边界（改动时别踩）

- 服务端 HTTP：Connection: close，无 keep-alive/chunked/TLS；请求行 4095B / 体 1MB。
- SQLite：不支持 BLOB 列 / WITHOUT ROWID；`:memory:` 字面路径强制单槽；单写者模型。
- 集群：缓存命中仍需借连接（pool_size 即并发上限）；故障窗口内替补写入的行，
  owner 归队后读不到（同键最终一致 ≤ TTL）；SDK 无连接复用。
- MySQL：保留未做；接入需补唯一性自省口径专项（COLUMN_KEY='UNI' 与联合唯一/
  前缀索引/多 NULL 的对应关系 —— 缓存放大了误标 unique 的后果）。
- v1.1 候选（压测证据见 docs/benchmarks/2026-08-14）：keep-alive（需双端改造）、
  热键预热（实测无风暴，继续搁置）。
