# CppPools 数据库中间件改造设计

- 日期：2026-08-06
- 状态：待评审
- 前置：`2026-08-06-cpp-crud-api-design.md`（本设计取代其中的 score-specific API 部分）

## 1. 定位变更

改造前：单节点三级池化服务，API 是围绕 `user_scores` 表硬编码的 score CRUD。

改造后：**客户端与后端数据库之间的中间件**。

```
HTTP 客户端 / Python SDK
        │  通用行级 CRUD（不含任何数据库方言）
        ▼
┌─────────────────────────────────────────────────────┐
│ CppPools 中间件（单节点）                             │
│                                                     │
│  ThreadPool ──> ObjectPool ──> DbConnectionPool × N  │
│                                      │              │
│                            Database 适配层（抽象基类）│
└─────────────────────────────────────────────────────┘
        │                    │
        ▼                    ▼
   SQLite 数据源         MySQL 数据源
```

**核心性质：类型透明。** 客户端只发"读哪张表哪一列、改哪张表哪一行"，
完全不知道底下是 SQLite 还是 MySQL，也不接触任何 SQL。

**三大池子仍是运行时主干**，且本次改造给连接池增加了一项真实职责：
每条池化连接携带一份预热的 prepared statement 缓存（见 §9）。

## 2. 能力边界（明确不做什么）

| 能做 | 不能做 |
|---|---|
| 单条记录的 select / insert / update / delete | 建表、删表、改表结构（任何 DDL） |
| 按主键或唯一键定位一行 | 按非唯一列批量改删 |
| 读取 schema 自省信息 | 事务、联表、分页、聚合、批量操作 |
| 跨数据源路由（每请求落单一数据源） | 跨数据源的原子操作 |

**关系模型不可变**：中间件只增删改表中的行，不触碰表本身与列定义。
这一点不是靠运行期检查实现的，而是 `Database` 抽象接口里**根本没有 DDL 方法**（§5）。

**为什么单记录 CRUD 让多数据源变得可行**：每个请求只碰一张表，因此只落到一个数据源，
不存在跨库操作，也就不需要分布式事务。若支持联表或事务，多数据源就必须引入两阶段提交。
两个决策互相成就。

## 3. 安全模型

改造前 `user` / `score` 都被收敛成 `int`，没有注入面。改造后**表名、列名、值三者全部来自客户端**。

- **值** 可用 prepared statement 参数化 → 安全
- **表名与列名在 SQL 里无法参数化**（`SELECT ? FROM ?` 不成立）→ 只能拼字符串 → 必须白名单校验

### 3.1 校验管道（唯一通路）

```
JSON body → 请求结构体
  → Database 基类的非虚 select()/insert()/update()/remove()
      ① table 必须在 schema 中
      ② 每个 column 必须属于该 table
      ③ where 列必须是主键或唯一键
      ④ 值类型必须与列类型兼容
  → 通过后调用 protected 纯虚 do_select() 等
  → 子类拼 SQL 时使用的是 schema 里那份 ColumnInfo::name
  → 所有值经 prepared statement 绑定
```

### 3.2 关键论证

通过校验后拼进 SQL 的标识符，**不是"转义过的客户端输入"，而是"从 schema 里查出来的已知字符串"**。
客户端传的字节只用于查表比对，从不进入 SQL 文本。

这把「转义够不够严」这个无法证明的问题，换成了「是否在集合内」这个可证明的问题。

### 3.3 爆炸半径限制

where 列强制为主键或唯一键（自省时可读到该信息），因此四个操作都是「至多影响一行」。

推论：**客户端无法通过本中间件发起批量改删**。
形如 `DELETE FROM orders WHERE status='pending'` 的请求会被拒绝（`status` 非唯一键 → 400），
不会抹掉几千行。对暴露给客户端的中间件而言，爆炸半径被校验锁死比事后审计更有价值。

代价：按非唯一列查询（如"查某部门所有人"）本版不提供。

### 3.4 标识符大小写

一律精确匹配，不做规范化。MySQL 表名在 Linux 区分大小写、macOS 不区分；
规范化会引入跨平台行为漂移。

### 3.5 凭证

配置文件支持 `"password_env": "ENV_NAME"` 从环境变量读取密码。
若同时写了明文 `password`，启动时打印警告。日志与 `/api/v1/get-config` 永不输出密码。

### 3.6 值类型严格校验

INTEGER 列只接受 JSON 整数，REAL 列接受整数或浮点，TEXT 列只接受字符串；
不匹配直接 400，**不做隐式转换**。

**为何必须严格**：SQLite 是动态类型（type affinity，TEXT 列能存数字），
MySQL 则严格。若在校验层宽容并交由后端自行处理，两个后端会对同一请求产生不同结果，
**§12 的双后端一致性测试就失去意义**。在校验层把类型卡死，是跟后端无关的一致性前提。

NULL：仅当列 `nullable` 为真时接受 JSON `null`。

## 4. 数据源与路由

数据源可以**类型相同也可不同** —— 适配器是按数据源实例创建的，不是按类型。
两个 MySQL 分片 = 同一个适配器类的两个实例，各自独立的连接池。

### 4.1 路由规则

启动时自省所有数据源，建立 `table → [datasource]` 索引，然后：

| 情形 | 行为 |
|---|---|
| 请求显式带 `datasource` | 直接定位该数据源（**优先级最高**） |
| 表名只在一个数据源出现 | 自动路由到该数据源 |
| 表名在多个数据源出现，请求未指定 | **409 Conflict**，响应体列出候选数据源名 |
| 表名不在任何数据源 | 400 Bad Request |
| 指定的 `datasource` 不存在 | 400 Bad Request |

**刻意不做"按配置顺序兜底选第一个"。** 理由：

假设 `shard0` 与 `shard1` 都有 `orders`，客户端发
`update orders set amount=0 where id=42`。若按顺序选中 `shard0`，则 `shard1` 里那行未被修改，
**且客户端收到 `affected_rows:1` 认为成功**。这是无报错的静默数据错乱，
且选中谁取决于配置文件书写顺序，客户端无从预料。

歧义必须在**启动时**被发现并告警，而不是在某个生产请求上静默赌一把。

**同理不做"按行匹配消歧"**：insert 的行还不存在，无法按"哪个库有这行"选库；
对 select/update/delete 则需先逐库探测，既放大读又引入竞态（探测后、执行前对方库被改）。

### 4.2 透明性的边界

「透明」指客户端不必知道数据库**类型**，不指不必知道数据**去了哪个逻辑库**。
后者是客户端应当能确定的事实，不该被"透明"抹掉。

## 5. 适配层设计

### 5.1 抽象基类（NVI 惯用法）

```cpp
namespace cpppools::db {

class Database {
public:
    virtual ~Database() = default;

    // 公开接口不是虚函数：校验在这里做，子类无法跳过。
    DbResult<ResultSet>   select(const SelectRequest& req);
    DbResult<InsertResult> insert(const InsertRequest& req);
    DbResult<RowCount>    update(const UpdateRequest& req);
    DbResult<RowCount>    remove(const DeleteRequest& req);

    const Schema& schema() const { return schema_; }
    virtual bool healthy() = 0;

protected:
    // 子类只实现后端特定部分。被调用时参数已保证通过 schema 校验。
    virtual DbResult<ResultSet>   do_select(const SelectRequest&) = 0;
    virtual DbResult<InsertResult> do_insert(const InsertRequest&) = 0;
    virtual DbResult<RowCount>    do_update(const UpdateRequest&) = 0;
    virtual DbResult<RowCount>    do_remove(const DeleteRequest&) = 0;

    Schema schema_;  // 构造时传入（工厂完成“连接+自省+构造”一步）
};

}  // namespace cpppools::db
```

**为什么用 NVI 而不是独立的 validator**：若校验做成独立组件让各后端自己调用，
新增后端时**可能忘记调**。公开接口非虚意味着校验被结构性地强制执行 ——
`select()` 的实现固定为「校验 → 通过则 `do_select()`」，
加第三个后端时校验自动继承，不可能漏掉。这对安全闸门至关重要。

**SQL 生成不抽公共层**：占位符差异（MySQL `?` / PostgreSQL `$1`）与标识符引用符差异
（反引号 / 双引号）由各子类自行消化。2 个后端 × 4 条简单语句，重复量可接受，
比抽一层 dialect + builder 更直观。

### 5.2 数据类型

```cpp
// 中间件只搬运值，不解释业务含义。
using Value = std::variant<std::monostate, std::int64_t, double, std::string>;
//              monostate 表示 SQL NULL

using Row = std::vector<Value>;

struct ResultSet {
    std::vector<std::string> columns;
    std::vector<Row> rows;
};

using RowCount = std::int64_t;

// insert 的结果：受影响行数 + 生成的自增主键（决策③）。
// 表无自增主键时 inserted_id 为 nullopt（由后端判定）。
struct InsertResult {
    RowCount affected_rows{0};
    std::optional<std::int64_t> inserted_id;
};

// 统一错误模型：沿用既有 DbOpStatus 风格，不用异常做控制流。
enum class DbStatus { kOk, kNotFound, kAlreadyExists, kInvalidRequest, kBackendError };

template <typename T>
struct DbResult { DbStatus status; T value; std::string message; };
```

### 5.3 请求结构体

```cpp
struct WhereClause { std::string column; Value value; };   // 单列等值，列须为 PK/UNIQUE

struct SelectRequest { std::string table; std::vector<std::string> columns; WhereClause where; };
struct InsertRequest { std::string table; std::vector<std::pair<std::string, Value>> values; };
struct UpdateRequest { std::string table; std::vector<std::pair<std::string, Value>> set; WhereClause where; };
struct DeleteRequest { std::string table; WhereClause where; };
```

`SelectRequest::columns` 为空表示返回全部列。

### 5.4 工厂与扩展口

```cpp
// type 字符串 → 工厂函数 的注册表。工厂约定不抛异常，失败按值返回。
struct DatabaseCreateResult { std::unique_ptr<Database> db; std::string error; };
using DatabaseFactoryFn = DatabaseCreateResult(*)(const DataSourceConfig&);
bool register_database_type(std::string_view type, DatabaseFactoryFn fn);
DatabaseCreateResult create_database(const DataSourceConfig& cfg);  // 未知 type / 工厂失败均带 error
```

**与原设计的偏差（实现时定稿）**：取消了 `initialize()` / `load_schema()` 两段式，
改为工厂一步完成“连接 + 自省 + 构造”，Schema 经构造函数传入 ——
创建失败的错误上报并入 `DatabaseCreateResult::error`，启动流程可读。

**扩展一个后端的完整成本**：新增一个 `.cpp` 实现 4 个 `do_*`，
在注册表加一行。无需改动基类、路由、HTTP 层或配置解析。

## 6. Schema 自省

```cpp
struct ColumnInfo {
    std::string name;
    ValueKind kind;        // kInteger / kReal / kText / kBlob
    bool nullable;
    bool primary_key;
    bool unique;           // PK 或 UNIQUE 约束覆盖
};
struct TableInfo { std::string name; std::vector<ColumnInfo> columns; };

class Schema {
public:
    bool has_table(std::string_view) const;
    const TableInfo* find_table(std::string_view) const;
    const ColumnInfo* find_column(std::string_view table, std::string_view column) const;
    std::vector<std::string> table_names() const;
};
```

自省语句：

- **SQLite**：`SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'`
  → 逐表 `PRAGMA table_info(<t>)` 取列与 PK；`PRAGMA index_list(<t>)` + `PRAGMA index_info(<i>)` 取 UNIQUE
- **MySQL**：`SELECT TABLE_NAME, COLUMN_NAME, DATA_TYPE, IS_NULLABLE, COLUMN_KEY
  FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = ?`（`COLUMN_KEY` 为 `PRI`/`UNI` 即唯一）

**自省时机**：每条连接建立后执行一次。schema 变更需重启服务 ——
这是"关系模型不变"这一前提的自然结果，写入文档。

## 7. API 契约

### 7.1 端点（9 个）

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/v1/select` | 查一行 |
| POST | `/api/v1/insert` | 插一行 |
| POST | `/api/v1/update` | 改一行 |
| POST | `/api/v1/delete` | 删一行 |
| GET | `/api/v1/schema` | 自省：有哪些数据源、各有哪些表与列 |
| GET | `/api/v1/stats` | 三池运行指标 + 延迟分位 |
| GET | `/api/v1/get-config` | 启动期配置快照（不含密码） |
| GET | `/health` | 存活探针（纯文本） |
| GET | `/metrics` | Prometheus 文本 |

四个 CRUD 统一 **POST + JSON body**：值现在是任意字符串（可含 `&` `=` 空格 中文），
query string 不胜任；insert 还需一次传多列。读写一致也让 SDK 实现最简单。

### 7.2 请求 / 响应

```json
// POST /api/v1/select
{"table":"user_scores","columns":["score"],"where":{"column":"user_id","value":42}}
→ 200 {"datasource":"main","columns":["score"],"rows":[[99]],"row_count":1}

// POST /api/v1/insert
{"table":"user_scores","values":{"user_id":42,"score":99}}
→ 201 {"datasource":"main","affected_rows":1,"inserted_id":42}

// POST /api/v1/update
{"table":"user_scores","set":{"score":88},"where":{"column":"user_id","value":42}}
→ 200 {"datasource":"main","affected_rows":1}

// POST /api/v1/delete
{"table":"user_scores","where":{"column":"user_id","value":42}}
→ 200 {"datasource":"main","affected_rows":1}
```

可选字段 `"datasource":"main"` 用于显式指定数据源。
响应始终回传实际命中的 `datasource`，便于客户端确认写入位置。

### 7.2.1 自增主键

`insert` 响应回传 `inserted_id`：

- SQLite：`sqlite3_last_insert_rowid()`
- MySQL：`mysql_insert_id()`

**为何必需**：where 子句要求主键/唯一键（§3.3），若不回传生成的主键，
客户端插入后就**无法定位刚插的那一行**。两个设计相互依赖。

客户端可不传自增列（由数据库生成），也可显式传入。
若表无自增主键，`inserted_id` 字段缺省为 `null`。

### 7.3 状态码（改为 SQL 语义）

| 情形 | 状态码 | 说明 |
|---|---|---|
| select 无匹配 | **200** + `rows:[]` | 查询成功但无结果，不是错误 |
| update / delete 匹配 0 行 | **200** + `affected_rows:0` | 同上 |
| insert 成功 | 201 | |
| insert 唯一键冲突 | 409 | |
| 表/列不存在、where 列非唯一键、类型不匹配、JSON 格式错 | 400 | 契约违反 |
| 表名跨数据源歧义且未指定 | 409 | 响应体列出候选 |
| 连接池获取超时 | 503 | |
| 后端错误 | 500 | 透传后端消息（经 JSON 转义） |

**这是与改造前的最大行为差异**：原 `get-score` 查不到返回 404，现在返回 200 + 空结果集。
理由：中间件应如实转达数据库语义，不替客户端把"0 行"解释为"资源不存在"。
400 留给真正的契约违反。

### 7.4 `/api/v1/schema` 响应

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

不暴露数据源的 `type`（SQLite / MySQL）—— 那是客户端不该依赖的实现细节。

## 8. 配置文件

`config/cpppools.json`：

```json
{
  "server": { "port": 8080, "worker_threads": 6, "latency_window_size": 1024 },
  "datasources": [
    {
      "name": "main",
      "type": "sqlite",
      "default": true,
      "pool_size": 4,
      "acquire_timeout_ms": 100,
      "sqlite": { "path": "data/main.db", "wal": true }
    },
    {
      "name": "archive",
      "type": "mysql",
      "pool_size": 8,
      "acquire_timeout_ms": 100,
      "mysql": {
        "host": "127.0.0.1", "port": 3306, "user": "root",
        "password_env": "CPPPOOLS_ARCHIVE_PASSWORD", "database": "archive"
      }
    }
  ]
}
```

查找顺序：`--config <path>` → 环境变量 `CPPPOOLS_CONFIG` → `./config/cpppools.json`。
三者都无 → 启动失败并打印示例配置（不再静默回落到某个默认后端，
因为"连哪个库"是必须显式声明的事实）。

`default: true` 的数据源在表名歧义时**不**被自动选用（见 §4.1）；
它只用于未来可能的 `datasource` 缺省场景，本版仅作校验（至多一个数据源可标 default）。

JSON 解析使用 vendored 的 nlohmann/json（`third_party/nlohmann/json.hpp`）。

## 9. 三大池子的位置与变化

| 池 | 变化 |
|---|---|
| **ThreadPool** | 不变。仍是 accept 循环的卸载点 |
| **ObjectPool\<RequestContext\>** | 强化。请求体变大（JSON body），池化 recv 缓冲与响应字符串的收益比之前更大。局限：nlohmann/json 解析时的内部分配无法池化，本池覆盖的是 I/O 缓冲 |
| **DbConnectionPool** | **每数据源一个池**。新增职责：每条池化连接携带 prepared statement 缓存 |

### 9.1 连接池的新职责：statement cache

prepared statement 是**每连接资源**。生产级连接池（HikariCP、pgbouncer）的核心价值之一
就是"池化连接自带预热的 statement cache"。

`Database` 子类内部持 `unordered_map<std::string, Stmt>`（键为 SQL 文本）。
连接被复用时 statement 也被复用 —— 连接池从"省一次握手"升级为
"省一次握手 + 省一次 SQL 编译"。

`/api/v1/stats` 与 `/metrics` 新增 **statement cache 命中率**。
这是三池叙事里一个真实的可观测指标，而非人造演示数字。

缓存上限：每连接 64 条。淘汰策略（实现时定稿）：容量满时**整体清空**而非精确 LRU ——
本中间件约定 do_select/do_insert 始终按 schema 全列生成固定 SQL（每表每操作恰好一条，
列投影在取回后由适配层完成），键空间有界（表数 × 操作数，个位数），
正常运行不会触发淘汰；触发了也只是重编译几条语句，精确 LRU 的复杂度不值得。
另：返回的 statement 引用禁止跨 get_or_create 调用持有（见 statement_cache.h 契约）。

## 10. SQLite 的两个必须处理的陷阱

### 10.1 `:memory:` 与连接池冲突

`sqlite3_open(":memory:")` **每次调用创建独立的内存库**。
连接池里 8 条 `:memory:` 连接 = 8 个互不相干的空库，
用连接 3 写入、连接 7 读不到，且不会有任何报错 —— 表现为随机失败的测试。

处理：

- 配置 `path` 为 `:memory:` 时，**强制 `pool_size = 1`** 并打印警告
- 或使用 `file::memory:?cache=shared` + `SQLITE_OPEN_URI` 共享内存库（测试推荐）

### 10.2 写并发

SQLite 默认同一时刻只允许一个写者，多连接并发写会遇到 `SQLITE_BUSY`。

处理：

- 文件模式默认开启 **WAL**（`PRAGMA journal_mode=WAL`），允许多读者 + 单写者并发
- 设置 `PRAGMA busy_timeout=<acquire_timeout_ms>`，让锁等待与连接池超时语义一致
- 文档明确：**嵌入式后端的连接池主要收益是 statement cache 与并发读，而非写吞吐**

这个差异本身是有价值的工程认知，写入 README 而非藏起来。

## 11. 分阶段实施

每阶段一个提交，阶段结束时项目必须编译通过（零告警）且测试全绿。

**多数据源从第一期就在设计内**（配置格式与路由规则），第 4 期点亮运行时能力。

| 期 | 内容 | 交付物 / 可验证性 |
|---|---|---|
| **1** | vendored nlohmann/json；`config` 模块解析 `datasources` **数组（N 个）**；`--config` 参数与查找顺序；HTTP 请求体读取（Content-Length 循环 recv） | `test_config` 通过；能加载多数据源配置并打印 |
| **2** | `value.h` / `schema.h` / `row_request.h` / `database.h`（NVI 基类含校验管道）/ 工厂注册表 / `statement_cache.h` | `test_schema` + `test_validator`，纯单测 |
| **3** | **SQLite 后端**（自省 + statement cache + WAL / `:memory:` 处理）；`DbConnectionPool` 元素类型切到 `Database` | `test_database_conformance` 本机实测 |
| **4** | **多数据源运行时**：`DataSourceRegistry`（N 个池）+ `table→datasource` 索引 + 歧义检测（409，不兜底） | `test_routing`，用**两个 SQLite 文件数据源**本机实测 |
| **5** | 四个 CRUD 端点 + `/api/v1/schema`；删除 `score_api` / `score_record` / `db_connection` / `mock_score_store` | **完整可运行的多数据源中间件**，端到端实测 |
| **6** | MySQL 后端（prepared statements + 自省） | 静态审查；需外部环境回归 |
| **7** | `docs/api/backend_api.md` 与 README 重写 | — |

第 5 期结束即为可对接 SDK 的里程碑。

第 4 期能本机实测是关键：两个 SQLite 文件数据源（不同文件）足以验证路由、
歧义检测与按源分组指标，不依赖 MySQL。

## 12. 测试策略

| 用例 | 覆盖 |
|---|---|
| `test_thread_pool` | 保留不变（新增 statement cache 无关） |
| `test_object_pool` | 从 `test_pools` 拆出：RAII 归还、`reset()` 保留 capacity、峰值统计 |
| `test_config` | JSON 配置解析：缺字段、类型错、`password_env`、多 default 冲突 |
| `test_schema` | 自省结果的查询与 PK/UNIQUE 判定 |
| `test_validator` | 校验管道全部拒绝分支：表不存在、列不属于表、where 列非唯一、类型不匹配 |
| `test_database_conformance` | **同一批断言跑所有可用后端**（SQLite 必跑，MySQL 条件编译时跑） |
| `test_routing` | 第 4 期：唯一表名自动路由、歧义 409、显式 datasource、未知 datasource |
| `test_http_util` | 保留；新增 Content-Length 请求体读取的边界 |
| `test_crud_api` | 取代 `test_score_api`：9 个端点的状态码矩阵 |

**`test_database_conformance` 是本设计最重要的验证手段** ——
它是唯一能证明抽象层没有泄漏后端细节的办法。同一批 CRUD 断言，
换后端后行为必须完全一致（含 409 唯一键冲突、0 行影响、NULL 处理、类型往返）。

删除 `mock` 后端后，测试跑的是**真实 SQL 生成、真实 prepared statement、真实自省**，
不会再出现改造前那种"mock 语义与 SQL 语义漂移"（原 `/api/v1/process` 与 `get-score`
对同一 user 返回不同分数即是此类症状）。

## 13. 影响面

### 删除

- `include/cpppools/db/score_record.h`（`ScoreRecord` / `ScoreResult`）
- `include/cpppools/db/db_connection.h` + `src/db/db_connection.cpp`
- `include/cpppools/db/mock_score_store.h` + `src/db/mock_score_store.cpp`（mock 后端整体移除）
- `src/server/score_api.{h,cpp}`
- `test/test_score_store.cpp`、`test/test_score_api.cpp`

### 保留（骨架不变）

`thread_pool`、`object_pool`、`db_connection_pool`（元素类型换为 `Database`）、
`http_server`、`http_util`、`route_table`、`api_response`

### 新增

```
third_party/nlohmann/json.hpp

include/cpppools/config/config.h
include/cpppools/db/value.h
include/cpppools/db/schema.h
include/cpppools/db/row_request.h
include/cpppools/db/database.h
include/cpppools/db/database_factory.h
include/cpppools/db/datasource_registry.h        # 第 4 期

src/config/config.cpp
src/db/schema.cpp
src/db/database.cpp                              # 基类校验管道
src/db/database_factory.cpp
src/db/sqlite_database.{h,cpp}
src/db/mysql_database.{h,cpp}
src/db/statement_cache.h                         # 两后端共享
src/db/datasource_registry.cpp                   # 第 4 期
src/server/crud_api.{h,cpp}                      # 取代 score_api
```

### 行数预算（诚实估算）

| 部分 | 行数 |
|---|---|
| 适配层（value / schema / request / database / factory / statement_cache） | ~550 |
| SQLite 后端 | ~280 |
| MySQL 后端 | ~300 |
| 配置加载 | ~150 |
| 多数据源注册表与路由（第 4 期） | ~280 |
| CRUD HTTP handler | ~220 |
| **合计（不含 third_party 与测试）** | **~1780** |

三个池届时约 500 行。**三池仍是运行时主干，但代码量上会被适配层盖过** ——
两个真实数据库的驱动代码各 ~280–300 行是不可压缩的硬成本
（连接、绑参、取结果、类型映射、自省、错误码映射）。

## 14. 已确认的关键决策

以下五条已经用户确认：

1. **where 列强制为主键或唯一键**（§3.3）。收益是爆炸半径锁死；
   代价是按非唯一列查询（如“查某部门所有人”）本版不可用。
2. **路由歧义返回 409，不按顺序兜底**（§4.1）。
3. **状态码采用 SQL 语义**（§7.3）：0 行返回 200 而非 404。
4. **值类型严格校验，不做隐式转换**（§3.6），以保障双后端一致性测试成立。
5. **`insert` 回传 `inserted_id`**（§7.2.1），否则客户端插入后无法定位新行。

## 15. 已预留的扩展位（本版不实现）

以下三条本版不提供，但抽象已留好位置：

- **多条件 where / 比较运算符**：`WhereClause` 目前是单列等值；
  将来可扩为数组 + `op` 字段，不需改动基类接口。
- **PostgreSQL 后端**：新增一个 `.cpp` 实现 4 个 `do_*`，
  在工厂注册表加一行。无需改动基类、路由、HTTP 层与配置解析。
- **数据源级只读标记**：配置加 `"read_only": true` 则拒绝写操作（适用于只读副本）。

## 16. 已知限制

- schema 在启动时自省一次，**运行期变更需重启**。
- 无事务：每个请求是一条独立语句。`insert` 后立即 `select` 在多数据源场景下
  不保证读到（不同数据源无因果序），单数据源下由后端自身保证。
- 无连接失效自动重建：连接断开后 `healthy()` 返回 false，但不会自动重连。
- MySQL 后端在本机无法编译验证（无客户端库），第 6 期仅做静态审查，
  需在具备 MySQL 的环境回归。
- 第 4 期之前运行时只支持单数据源（配置格式已是数组，多元素时启动失败并提示）。
