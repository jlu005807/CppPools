---
name: cpppools-dev
description: Use when 在 CppPools 仓库理解、开发、调试、评审或验证 C++17 数据库中间件、SQLite/MySQL 后端、三级池化、行缓存、集群路由及 Python SDK，或对齐相关文档。
---

# CppPools 开发指南

CppPools 是数据库前面的 HTTP 中间件：JSON 行级 CRUD → 表路由 → 连接池 → SQLite/MySQL。可选集群把同一个路由键交给唯一 owner 处理和缓存；Python SDK 负责发现拓扑、直连 owner、跟随 307 与故障绕行。跨宿主机交付优先使用 Linux Docker 容器：Compose 管部署和启停，SDK/CLI 只通过远程 HTTP 执行 health 与 CRUD。

## 先建立正确的项目模型

- SQLite 已实现且默认可用；MySQL 已实现，但必须用 CPPPOOLS_USE_MYSQL=ON 编译并提供客户端开发库。CMake 中 OFF 模式的 mock 摘要是遗留描述，不表示服务使用模拟数据库。
- **拓扑来自静态配置**：每节点启动时读本地 JSON 的 cluster.nodes，通过 GET /api/v1/cluster 发布完整视图。SDK 给一个或多个 seed URL，初始化时逐个尝试获取视图并建环，再从 /api/v1/schema 获取唯一列信息。节点没有心跳、自动注册或配置传播。
- **缓存在服务进程内存**：每个节点、每个数据源一份 RowCache，该源所有池连接共享；不是每连接一份，也不是 SDK 缓存或 Redis。分片哈希表存整行，LRU/TTL 管淘汰，Singleflight 合并回源，世代 CAS 阻止旧回填。
- **数据库目标由部署配置保证**：集群设计要求所有节点连同一组实际数据源。节点各读各的 datasources 并建立连接池，不从 SDK 或邻居取得账号密码。datasource_digest 仅核对目标标识，SDK 初始化时比对并记录 digest_warnings，不能分发配置或证明数据一致。
- 多进程 e2e 使用独立 SQLite 库验证路由与故障流程；实现没有复制、迁移或补写机制。不要由这些测试推断独立库故障后数据会收敛。相同路径在不同主机上也不代表同一 SQLite 数据库；TTL 只让缓存过期，不同步数据库。
- **容器部署边界**：`docker-compose.yml`、根目录 `.dockerignore`、`docker/Dockerfile` 和 `docs/docker-compose-deployment.md` 是当前可审查的 Compose 入口。容器内服务按 Linux 运行；Compose 负责容器生命周期和数据卷，`cpl start/stop/status` 不管理容器。更改发布端口时必须同步 `cluster.nodes` 的可达端口；测试用唯一 project、随机端口和临时配置。Docker Compose 真实 smoke 只有在 `CPL_RUN_DOCKER=1` 且本机 Docker 引擎可用时才运行；静态文件检查不能替代真实容器证据。2026-10-15 WSL Docker Engine 的真实 health/SDK/CLI CRUD/stop/卷恢复已通过。
- **本机管理边界**：Linux/WSL 与 Windows 的 `LocalNodeManager.start/status/stop` 已实测；Windows 控制目录使用当前用户+SYSTEM 的 protected DACL、祖先句柄固定和打开后 reparse 检查。中文/emoji 状态目录已通过真实启停，但配置、数据库、二进制的全部 Unicode 路径及自定义日志路径安全没有因此获得验收。macOS 本机管理、Windows/macOS 前台真实终端仍需各自平台证据，Compose 的 Linux 容器结果不能代替。

## 按任务读取

从仓库根执行命令。先读 AGENT.md、git status --short 和相关最近提交；旧文档与源码冲突时指出冲突，不把历史规划当现状。AGENT.md 中 MySQL 未做的例子已经过时，但架构与范围约束仍适用。

| 任务 | 入口 |
|---|---|
| 模块边界、请求路径、缓存与数据库区别 | [architecture.md](architecture.md) |
| 构建、运行、测试前提及实测限制 | [verification.md](verification.md) |
| HTTP 契约、配置、状态码、指标 | docs/api/backend_api.md、config/cpppools.example.json |
| SDK 拓扑、重试、错误分类 | docs/api/sdk.md、sdk/python/cpppools/client.py、transport.py |
| 集群不变量、哈希黄金向量 | docs/superpowers/specs/2026-08-11-cluster-cache-design.md |
| MySQL 自省、事务、类型限制 | docs/superpowers/specs/2026-08-15-mysql-backend-design.md、src/db/mysql_database.cpp |
| 最近加固的目的与验收要求 | docs/superpowers/plans/2026-08-17-code-hardening-index.md、相应源码与测试 |
| 节点本地 Redis 缓存设计、恢复边界及分阶段 TDD | [redis-cache-design.md](../../docs/redis-cache-design.md) |
| Redis 设计的运行配置、故障行为和发布检查摘要 | [redis-cache-operations.md](../../docs/redis-cache-operations.md) |
| 缓存、Redis transport 与动态拓扑的统一实施顺序 | [implementation-roadmap.md](../../docs/implementation-roadmap.md) |
| Redis 动态拓扑发现的独立设计 | [redis-discovery-design.md](../../docs/redis-discovery-design.md) |
| Docker Compose 部署、持久化和远程验收 | [docker-compose-deployment.md](../../docs/docker-compose-deployment.md) |

**实现状态边界**：当前行缓存仍是每个服务进程、每个数据源一份的内存 `RowCache`；Redis 缓存和 Redis 拓扑发现均尚未实现。上述 Redis 文档是设计/实施计划，不是当前配置能力、运行保证或已通过测试的证明。回答“现在怎么运行”时以源码、当前配置示例和实际测试为准；回答“将来如何实现”时才引用这些方案，并标明尚未落地。

**实施顺序入口**：按 [统一实施路线](../../docs/implementation-roadmap.md) 区分 S0 修复、S1 远程 CLI、S2 本机 CLI/打包、S3–S4 本地 Redis 缓存和 S5–S7 动态发现。S0 五项修复已有实现；A02 Windows 慢读总期限的负对照仍开放，不能因本机有界退出就关闭。继续任务前先查最新状态及证据，不重做已关闭修复，也不把 S3 的配置/transport 基础计为 RowCache 后端完成。

## 改动时守住的契约

- Database public 非虚 CRUD 做校验，后端只实现 do_*；无 DDL。where 必须单列真正唯一，联合主键的单个成员不能因此标为 unique。公开头与私有实现边界以实际文件为准。
- **I0**：关闭缓存、Fallback 或 epoch bypass 只停读命中和回填，缓存表的写失效仍执行。**I1**：blacklist_ttl_ms >= ttl_ms，配置期拒绝违反者。
- 路由哈希中的 datasource 是请求显式值或空串；本地缓存键使用解析后的数据源名。两者不可混用。哈希黄金向量和单 owner/307/epoch 决策不能随意更改。
- 缓存允许表由 cache.tables 控制（空列表代表全部）。非空行 miss 读取整行再按请求投影；投影顺序和重复列保留。测试缓存行为须覆盖跨连接共享和写后旧回填。
- SQLite update 用 BEGIN IMMEDIATE 包围读改写；MySQL update 使用事务内 SELECT ... FOR UPDATE。语句必须在连接关闭前释放，prepare 失败需可恢复。
- SDK 默认每 30 秒在请求路径懒刷新，无后台发现线程；运行期刷新失败继续用旧环。epoch 响应不符刷新供后续请求使用，**不重发已经完成的操作**。
- select/update/delete 可按错误类别限次重试；insert 仅在能得到显式可路由唯一键时允许重试。无键 insert 及其重定向目标失败不得自动重发；409 交调用方判断。

改 API/指标/配置时同步 docs/api/backend_api.md，改 SDK 语义同步 docs/api/sdk.md。验证选与改动相关的单测和真实运行入口；报告命令、环境、退出码及跳过原因，不沿用历史测试数。普通文档修改不授权新增功能、重写 Git 历史或推送。
