---
name: cpppools-dev
description: CppPools 项目开发指南——C++17 多数据源数据库中间件（三级池化 + NVI 校验管道 + SQLite 后端），支持一致性哈希分布式集群（单 owner 路由/307 重定向/epoch 门控）与行级写穿透缓存，配套零依赖 Python SDK。涵盖项目背景、架构设计、构建部署、测试流程与开发约定。在本仓库中做任何开发、调试、测试、架构问答或文档工作时使用。
---

# CppPools 开发指南

## 1. 项目背景与定位

CppPools 是一个**学习驱动的 C++17 数据库中间件**，从"三级池化"练手项目演进而来，
当前形态：

- **多数据源中间件**：客户端发通用 JSON CRUD（`POST /api/v1/select|insert|update|delete`），
  服务端按表名路由到配置的 SQLite（MySQL 后端保留未做）数据源。
- **分布式集群（可选，`cluster` 配置段）**：按路由键
  `(datasource, table, 唯一键列, 键值)` 做一致性哈希（murmur3_32 seed=0 无符号、
  100 vnodes），每个键归唯一 owner 节点 —— 单 owner ⇒ 无需失效广播/分布式锁；
  非 owner 回 **307 重定向**（不做代理转发）；拓扑变更用 **epoch 门控**。
- **行级写穿透缓存（可选，`cache` 配置段）**：每数据源一份共享 RowCache
  （分片锁/LRU 双阈值/TTL/世代号 CAS 回填/Singleflight），写直达 DB + 双删失效。
- **零依赖 Python SDK**（`sdk/python/`）：建环、直连 owner、307 跟随、黑名单/attempt
  绕行、Rejoin、错误分类重试。

**一致性契约**：稳态**同键**强一致；跨键/绕行期/拓扑未收敛/外部写入者 →
最终一致（旧读 ≤ TTL）。

权威文档（改动前先读对应的）：

| 文档 | 内容 |
|---|---|
| `README.md` | 项目全貌、模块结构、快速开始 |
| `docs/api/backend_api.md` | HTTP API 契约（端点/状态码矩阵/集群头/指标） |
| `docs/api/sdk.md` | SDK 参数/语义/错误矩阵 |
| `docs/superpowers/specs/2026-08-11-cluster-cache-design.md` | 集群+缓存设计 spec（规范性，含黄金向量 §3.2、决策矩阵 §3.6、不变量 I0/I1） |
| `docs/superpowers/specs/2026-08-06-db-middleware-design.md` | 中间件改造 spec |
| `docs/benchmarks/` | 性能基线与验收归档 |

## 2. 架构速览

```
SDK / curl
  │  建环（GET /api/v1/cluster）→ 按路由键直连 owner
  ▼
┌──────────── CppPools 节点 × N（单节点即退化形态）────────────┐
│ ThreadPool → ObjectPool → NVI 校验管道 → 表路由 → 归属门控    │
│   → RowCache（写穿透）→ DbConnectionPool × 数据源数           │
└──────────────────────────────────────────────────────────────┘
  ▼
SQLite（各节点各自的库 = 真理之源）
```

两个不变量（必须守住）：
- **I0 失效永不 bypass**：任何旁路形态（kill-switch、非 owner 就地、epoch 不符）
  只关读命中与回填，写的失效照常执行。
- **I1 blacklist_ttl ≥ cache_ttl**：配置解析期 fail-fast；归队安全性的承担者。

模块边界与关键决策细节见 [architecture.md](architecture.md)。

## 3. 快速开始与部署

环境：C++17 编译器 + CMake 3.16+ + SQLite3 开发库；SDK 需 Python 3.8+（零依赖）。

```bash
# 构建 + 全部测试（一条命令）
scripts/build.sh                  # 可选 --clean / --no-test

# 手动
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure     # 预期 18/18

# 单节点启动（自动种子演示库 data/cpppools_demo.db）
scripts/run_server.sh
curl -X POST http://127.0.0.1:8080/api/v1/insert \
  -d '{"table":"user_scores","values":{"user_id":42,"score":99,"updated_at_ms":0}}'

# 集群：每节点一份配置（cluster.node_id 不同、nodes 列表相同、epoch 一致）
# 格式见 config/cpppools.example.json 与 docs/api/backend_api.md §6
# 拓扑变更流程：递增全部节点 epoch → 逐节点重启 → 客户端 ≤30s 收敛

# SDK 使用
python3 -c "import sys; sys.path.insert(0,'sdk/python'); \
from cpppools.client import ClusterClient; \
c=ClusterClient(['http://127.0.0.1:8080']); print(c.select('user_scores',{'column':'user_id','value':42}))"

# 优雅停机：SIGTERM/SIGINT（停 accept → 排空在途，上限 5s）
```

配置路径优先级：`--config` > `CPPPOOLS_CONFIG` > `config/cpppools.json`。
剩余环境变量：`CPPPOOLS_MAX_QUEUE_SIZE` / `CPPPOOLS_DB_ACQUIRE_TIMEOUT_MS` /
`CPPPOOLS_LATENCY_WINDOW_SIZE`。

## 4. 测试流程与步骤

测试金字塔（改动后**自下而上**验证）：

```bash
# ① C++ 单测（18 项 CTest；assert 基，CMake 已对测试加 -UNDEBUG 防假绿）
ctest --test-dir build --output-on-failure

# ② SDK 单测（51 项，fake 集群驱动；含 spec §3.2 黄金向量跨语言对拍）
python3 -m unittest discover -s sdk/python/tests

# ③ live e2e（起真实多进程集群）
bash scripts/cluster_e2e.sh    # 双节点：路由分布/307/Fallback/epoch
bash scripts/sdk_e2e.sh        # 三节点：杀节点绕行/归队清缓存/滚动重启

# ④ 压测（v1.1 决策证据采集；bench 对照 / storm 归队观测）
python3 scripts/loadtest_sdk.py bench --cache on --workers 8 --total 8000
python3 scripts/loadtest_sdk.py storm --workers 8 --duration 30
```

开发流程约定（项目惯例，逐期验证有效）：

1. **TDD**：先写失败测试（RED，必须亲眼看到失败原因正确）→ 最小实现（GREEN）
   → 全量回归。新端点/新后端必须进 conformance 或决策矩阵测试。
2. **变异验证**：关键校验逻辑（如校验管道、世代 CAS）用"删掉某检查测试必红"
   证明测试真的钉住了行为。
3. **每期/每功能评审闭环**：code-reviewer 子代理评审 → 分级修复（阻塞/重要/建议）
   → 复验 → 提交。评审曾抓到：忙等待、世代 CAS 非原子、Fallback 头判定、
   attempt 扫描窗口漏节点等真实缺陷 —— 不要跳过评审。
4. **零告警**：`-Wall -Wextra ... -Werror=switch -Werror=return-type`，
   新增路由忘接 handler 会被编译拦截。
5. **黄金向量不可动**：spec §3.2 冻结值两端（C++ test_hash_ring / SDK
   test_hash_ring）共同引用，改哈希口径必须两端同改 + spec 留痕。

## 5. 开发约定

- **提交信息**：单行标题，Conventional 前缀（feat/fix/test/docs/chore），
  不写多段正文；不带"期N"字样，只描述改动本身。
- **错误处理**：C++ 侧错误按值返回（`DbResult` + `DbStatus`），不用异常做控制流。
- **文档同步**：改 API/指标/配置 → 同步 `docs/api/backend_api.md`；
  改 SDK 语义 → 同步 `docs/api/sdk.md`；设计级变更 → spec 修订记录留痕。
- **接口安全边界**：`Database` 基类禁止 DDL 方法（类型级保证）；
  where 必须 PK/UNIQUE；表/列名走 schema 白名单（防注入靠集合成员判断，不靠转义）。
- **MySQL 后端**：保留未做（工厂注册表已留位 + conformance 矩阵可复用；
  待做唯一性自省口径专项）。
