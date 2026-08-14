# DB 中间件改造实施计划

**Goal:** 期 3 —— SQLite 后端落地（真实 SQL、真实自省、statement cache），并把 `DbConnectionPool` 的元素类型从 `DbConnection` 切换为 `Database`，删除 mock 后端。

**Architecture:** `SqliteDatabase : Database`（NVI 子类）封装 sqlite3 连接 + 自省 + prepared statement 缓存；连接池元素切为 `unique_ptr<Database>`，探活走 `Database::healthy()`；score 四端点临时改写为 Database 实现（端点本身留到期 5 被通用 CRUD 取代）。

**Tech Stack:** C++17、系统 SQLite3（SDK 3.51 已就位，`-lsqlite3`）、assert + CTest。

**设计依据：** `docs/superpowers/specs/2026-08-06-db-middleware-design.md` §6/§10/§11/§12/§13。

**执行纪律（沿用）：** 每 Task 一组 RED→GREEN；期末 t.sh 全量 + CMake 零告警 + CTest + e2e；code-reviewer 评审→修正→复验→提交（作者 `jlu005807 <1289406660@qq.com>`，`git -c` 覆盖）。

---

## 关键设计决策（开工前定稿）

| # | 决策 | 理由 |
|---|---|---|
| D1 | **mock 提前删除**（spec 原定 期5）：`db_connection.{h,cpp}`、`mock_score_store.{h,cpp}` 本期删；score_api 改写为 Database 实现 | 池元素切为 `Database` 后 `DbConnection` 失去最后消费者；用户已明确"mock 可删除、用 SQLite" |
| D2 | **update = 读改写**：UPDATE SQL 固定 SET 全部非 where 列；缺省列先用该表缓存的 SELECT 读当前值回填 | 保持"每表每操作一条固定 SQL"（statement cache 键空间有界的前提，见 spec §9.1 定稿）；单行场景多一次 SELECT 可接受 |
| D3 | **insert 全列固定 SQL**：缺省列绑定 NULL；INTEGER PK（rowid 别名）收 NULL → SQLite 自动生成 rowid | 同上；NOT NULL 无默认列缺省 → SQLITE_CONSTRAINT_NOTNULL → kBackendError（500，spec §7.3 口径，已知接受） |
| D4 | **select 全列固定 SQL**：投影（列子集 + 请求顺序）在取回后由适配层完成 | 同上 |
| D5 | **INTEGER PRIMARY KEY（rowid 别名）口径**：`nullable=false, autoincrement=true`（虽然 PRAGMA table_info 报 notnull=0） | 诚实映射会让 insert NULL 被放行并由 SQLite 静默换 rowid——惊喜行为；显式 NULL 应拒绝，想要自增就省略该列 |
| D6 | **UNIQUE 判定**：PK 列 unique=true；`PRAGMA index_list`（跳过 origin='pk'）+ `index_info`，仅**单列** UNIQUE 索引记 unique；多列联合唯一不使任何单列唯一 | where 语义是单列等值，联合唯一无法支撑 |
| D7 | **BLOB 亲和列拒绝**：数据源创建失败，error 指明 `table.column` | Value 无 kBlob（YAGNI）；静默转 string 会损坏数据，显式失败 |
| D8 | **NUMERIC 亲和 → kReal**（整数按既有放行规则可写入）；类型映射按 SQLite 亲和规则（INT→Integer，CHAR/CLOB/TEXT→Text，REAL/FLOA/DOUB→Real，BLOB/空→拒绝，其余→Numeric→kReal） | DATETIME 等声明是 NUMERIC 亲和，不能误判 |
| D9 | **`:memory:`**：配置 path 为 `:memory:` → 强制 pool_size=1 + 警告（spec §10.1）；测试用 `file::memory:?cache=shared`（URI 模式）共享内存库 | 每 open 独立内存库 = 随机失败 |
| D10 | **WAL + busy_timeout 仅文件库**（`sqlite3_db_filename` 非空判定）；busy_timeout = 3000ms（与 db_acquire_timeout 默认 100ms 区分：前者是 SQLite 锁等待，后者是池等待） | spec §10.2；:memory: 不支持 WAL |
| D11 | **打开标志**：`SQLITE_OPEN_READWRITE`（不带 CREATE）——中间件不建库；`file:` 前缀加 `SQLITE_OPEN_URI`；失败返回可读 error | "不操作 schema"的边界自然延伸 |
| D12 | **inserted_id**：表有 rowid 别名列（INTEGER PK，`pk==1` 且 kind Integer）→ `sqlite3_last_insert_rowid()`，否则 nullopt | 决策③；无 rowid 表（WITHOUT ROWID）返回 nullopt |
| D13 | **错误码映射**：`SQLITE_CONSTRAINT_UNIQUE/PRIMARYKEY` → kAlreadyExists（HTTP 409）；其余 → kBackendError + `sqlite3_errmsg` | spec §7.3/§12 |
| D14 | **healthy()** = 执行 `SELECT 1`（cached stmt）；失败即不健康 | 廉价、真实 |
| D15 | **池配置精简**：`DbPoolConfig` 删 mock/mysql 字段，保留 `pool_size` + `backend`（"sqlite"/"mysql"）+ `table`（score 过渡期端点用）；便利构造（内置 mock factory）删除 | mock 已删；mysql 期6 |
| D16 | **main 接线**：从配置 datasources 取默认数据源（`is_default`，或唯一数据源；0 个/多个无 default → fail-fast），经 `create_database` 验证可连后，池 factory 每槽位 `create_database` 新建实例（每连接一份 Schema 拷贝 + statement cache） | 期4 才上多数据源 registry；本期单源 |

## 文件结构

| 文件 | 职责 | 动作 |
|---|---|---|
| `src/db/sqlite_database.h/.cpp` | sqlite3 RAII 连接、SqliteStmt 封装、自省、四个 do_*、healthy、factory 注册 `sqlite` | 新建 |
| `test/test_database_conformance.cpp` | 真实 SQLite conformance（本期最重要验证，spec §12） | 新建 |
| `include/cpppools/db/db_connection_pool.h` + `src/db/db_connection_pool.cpp` | 元素类型 → `Database`；DbConnGuard → `Database*`；DbPoolConfig 精简 | 修改 |
| `test/test_db_pool.cpp` | FakeConn → FakeDatabase（可控 healthy） | 修改 |
| `src/server/score_api.{h,cpp}` | 四端点改写为 Database CRUD（user_scores 表，where=user_id） | 修改 |
| `test/test_score_api.cpp` / `test/test_http_server.cpp` | 池构造切换；临时 SQLite 文件 fixture | 修改 |
| `src/main.cpp` | D16 接线；删 env 建连/mock_store | 修改 |
| `scripts/init_demo_db.sql` + `run_server.sh` 种子 | 演示库建表（middleware 不建表，demo 需要预置） | 新建/修改 |
| `config/cpppools.example.json` | sqlite_path 指向 `data/cpppools_demo.db` | 修改 |
| 删除：`db_connection.{h,cpp}`、`mock_score_store.{h,cpp}`、`test/test_score_store.cpp` | — | 删除 |
| `CMakeLists.txt` / `output/t.sh` | sqlite_database.cpp 源文件 + 全部目标 `-lsqlite3` | 修改 |
| `scripts/e2e.sh` | score 端点在真实 SQLite 上的冒烟 + 配置回显 | 修改 |

## 任务分解

### Task 1: SqliteDatabase 核心 + conformance 测试（RED→GREEN）

- [ ] 1.1 写 `test_database_conformance.cpp`（fixture：临时文件库 + sqlite3_exec 建表建索引；断言矩阵见下）
- [ ] 1.2 确认 RED（sqlite_database 不存在 → 编译失败）
- [ ] 1.3 实现 `sqlite_database.h/.cpp`：RAII 连接/stmt、打开（D9/D10/D11）、自省（D5/D6/D7/D8）、do_*（D2/D3/D4/D12/D13）、healthy（D14）、statement cache 接入（`StatementCache<std::unique_ptr<SqliteStmt>>`）
- [ ] 1.4 注册 `sqlite` 类型到工厂（`register_database_type` 在 sqlite_database.cpp 的注册钩子，main 显式调用 `register_sqlite_backend()`——避免全局静态初始化顺序依赖）
- [ ] 1.5 GREEN + 零告警

**conformance 断言矩阵**（每表建：`users(id INTEGER PK, name TEXT NOT NULL, age INTEGER, score REAL, tag TEXT UNIQUE)`）：
insert 往返 / inserted_id 回传 / 显式 PK 插入 / UNIQUE 冲突 kAlreadyExists / select 全列与投影子集（含顺序） / 0 行 select 空集 kOk / update 部分列（读改写，未动列保持） / update 0 行 kOk+0 / delete + 0 行 / NULL 往返（age） / REAL 收整数 / 类型不匹配到不了后端（校验层） / BLOB 列表创建失败 / 自省：PK/UNIQUE/nullable/autoincrement 判定（含联合唯一索引不记 unique） / `:memory:` shared-cache 两连接互见 / healthy()。

### Task 2: 连接池元素切换（RED→GREEN）

- [ ] 2.1 改 `test_db_pool.cpp`：FakeDatabase（继承 Database，可控 healthy，记录构造次数）替换 FakeConn → RED
- [ ] 2.2 `db_connection_pool.h/.cpp`：`ConnectionFactory = function<unique_ptr<Database>(size_t)>`；PooledConn/conn_to_slot_/DbConnGuard 类型切换；DbPoolConfig 精简（D15）；删便利构造
- [ ] 2.3 GREEN + TSan（并发用例已在池测试里）

### Task 3: 服务器接线 + mock 删除 + e2e（GREEN→验证）

- [ ] 3.1 score_api 改写为 Database 实现（ScoreResult 形状不变；select 无行 → 404；UNIQUE 冲突 → 409；校验错 → 400；后端错 → 500）
- [ ] 3.2 删 `db_connection.{h,cpp}` / `mock_score_store.{h,cpp}` / `test_score_store.cpp`；test_score_api、test_http_server 切换到临时 SQLite fixture
- [ ] 3.3 main.cpp D16 接线；`init_demo_db.sql` + run_server.sh 种子；示例配置更新
- [ ] 3.4 全量：t.sh 全绿 + CMake 零告警 + CTest + e2e（真实 SQLite 冒烟）+ TSan
- [ ] 3.5 提交 → code-reviewer 评审 → 修正 → 复验 → 提交

## 验证基线（期末）

- `output/t.sh` 全部测试二进制 PASS（test_score_store 删除后为 13→12+2 个）
- CMake 全量重编零告警；CTest 全绿
- e2e：配置回显 + score CRUD 真实落库往返 + 背压 503 仍有效 + /health
- 变异/反向实证：conformance 里 UNIQUE 冲突映射、读改写合并至少各一个反向用例
