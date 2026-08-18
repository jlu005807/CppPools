# CppPools 代码层可靠性优化方案

> 日期：2026-08-17
> 状态：待用户评审
> 评审范围：现有实现与既有 README、API 文档、架构参考和设计 spec 的一致性
> 目标：在不改变架构决策和公开契约的前提下，分阶段修复已证实的代码缺陷

## 1. 结论摘要

项目的整体模块边界、NVI 校验管道、三层池化、单 owner/307、缓存世代 CAS
和 Python SDK 设计方向不需要重做；当前主要问题是实现没有完整兑现已有契约。
优先级最高的是资源生命周期、缓存数据完整性、服务停机安全和 SDK 非幂等重试。

本方案采用“失败测试 -> 最小修复 -> 定向验证 -> 全量回归 -> 评审 -> 单一提交”
的闭环。每个阶段保持可独立回滚，不跨阶段重排公开 API 或改变决策矩阵。

## 2. 已验证基线

| 环境 | 结果 | 说明 |
|---|---|---|
| WSL C++ | CTest 18/18 | 基线通过；有 4 处 hash ring 范围循环告警 |
| WSL SDK | unittest 51/51 | 基线通过 |
| ASan/UBSan | 18/18 | 关闭 LeakSanitizer 后通过 |
| LeakSanitizer | 失败 | database/conformance 等测试复现约 4.3 MB 泄漏 |
| Windows/MinGW | 未通过 | 当前环境缺少 `sqlite3.h`，且 server 使用了不兼容的 Winsock/POSIX 类型 |
| TSan | 未验证 | 当前 WSL 报 `unexpected memory mapping`，属于环境限制 |

工作树中已有的 5 个脚本权限变更不是本方案产生的修改，实施期间必须保留。

## 3. 已确认问题与契约依据

### 3.1 Release blocker

1. `SqliteDatabase` 和 `MysqlDatabase` 的析构体先关闭数据库连接，成员析构随后
   才销毁 statement cache。SQLite 的 `sqlite3_close` 会因 busy 失败并遗留资源；
   MySQL statement 可能在失效的 `MYSQL*` 上关闭。
2. `CachingDatabase` miss 时按请求投影回源并缓存。第一次只查部分列后，后续
   不同投影会由 `make_hit` 用空值补缺列，返回静默错误数据。缓存 spec §4.2
   要求回源保存整行。
3. 自增 insert 的“不重试”只约束 SDK 单节点循环；外层 `NodeUnavailable` 分支
   仍可能换节点重发，响应丢失时可能重复落库。
4. HTTP 停机只等待 `active_requests_`，排队任务仍捕获 `HttpServer*`；对象析构
   顺序可能先销毁 server/request pool，再由 thread pool 执行任务，形成 UAF。

### 3.2 高优先级契约偏差

- `cache.tables` 已解析但 registry 对所有表都安装缓存装饰器；非白名单表不能逃生。
- SQLite/MySQL 将复合主键的每个分量标为独立 unique，违反“每次最多操作一行”。
- HTTP 首部只 `recv` 一次，合法 TCP 分片可能被误判为 malformed 或漏读 body。
- `send_all` 没有发送 deadline，慢读 peer 可无限占用 worker。
- MySQL `do_update` 的 commit 错误、unsigned/截断结果和 metadata 表名转义缺少
  严格处理；SQLite select/delete 也未检查 bind 返回值。
- 普通 307 跟随强制附带 `X-CppPools-Fallback`，破坏提示单调性；目标请求还绕过
  统一 retry/backoff 和故障归因。
- Windows socket 类型/错误码与 POSIX 混用；MSVC Release 下 `assert` 被
  `/DNDEBUG` 消除，测试可能假绿；CMake 未显式发现 SQLite 头和库。

### 3.3 有证据后再处理的 P2

连接池 tried-slot、热连接探活、ObjectPool 异常窗口、ThreadPool worker 自 shutdown、
峰值计数竞争、JSON 转义和配置缺参等问题先各自增加故障注入或回归测试；没有稳定
证据的项目不阻塞前述阶段。

## 4. 硬边界

本轮允许：修复私有实现、补充测试、修正文档与既有契约不一致的实现、补充平台
适配层、增加错误检查和资源收尾。

本轮不允许：

- 修改 `Database` NVI、公开类型/函数签名、池借还/超时语义；
- 改变 hash ring 黄金向量、I0/I1、owner/307/Fallback/epoch 决策；
- 引入 keep-alive、chunked、异步 I/O、第三方 HTTP 库或新的存储架构；
- 把每节点独立 SQLite 改成共享存储，或暗中改变集群故障窗口语义；
- 为精确内存计费、Redis/共享缓存等未来功能提前添加接口。

架构债务 `db -> server/hash_ring.h`、每节点 SQLite 的部署一致性、精确 cache
resident memory 计费和 TSan/Windows 缺失环境单独记录，不在本轮以重构方式解决。

## 5. 分阶段实施

### 阶段 0：测试与构建可信度

目标：让后续失败测试和平台结果可信。

- CMake 使用显式 SQLite3 include/library discovery；保持 Linux/macOS 现有链接。
- MSVC 测试目标显式移除 `NDEBUG`，并保留非 MSVC 的现有行为。
- 为后续生命周期、缓存投影、白名单、SDK 重试和 307 头语义建立最小失败测试。
- 不因本机缺少 SQLite 开发包而宣称 Windows 通过；记录需要的工具链门槛。

验证门槛：基线 CTest/SDK 仍全绿；新增测试在修复前按预期 RED；CMake 配置错误
必须给出可诊断信息。

### 阶段 1：statement 生命周期与数据库资源安全

建议提交：`fix: finalize statement caches before backend close`。

- 为 `StatementCache` 增加私有、异常安全的清空路径，先 finalize statements，
  再关闭 SQLite/MySQL 连接；不改变外部类接口。
- prepare 构造失败不留下孤儿 LRU key；失效 statement 可被替换/重建。
- SQLite select/delete 检查 bind/rebind 返回值，错误按现有 `DbResult` 映射。
- 增加实际使用 statement 后析构的回归测试，并在可用环境运行 ASan/LSan。

验证门槛：相关 CTest、全量 CTest、ASan；LSan 不再报告数据库 statement 泄漏。

### 阶段 2：schema 与缓存数据正确性

建议拆为两个可审查提交：白名单/投影和 cache generation。

- Registry 传递 `cache.tables`：空数组保持“全部表”，非空数组只装饰白名单表。
- miss leader 始终以空 `columns` 回源完整行，再按原请求投影；不同投影的并发
  请求共享同一整行 singleflight。
- 复合主键只标 `primary_key`，只有单列主键/单列 UNIQUE 才标列级 `unique`。
- invalidate/clear 退休旧 generation；写返回后到达的新 reader 不得加入旧 flight。
- miss 计数按实际 leader 回源计一次；TTL 起点按 DB 读开始时刻；保留现有双删和 I0。

验证门槛：config、schema validator、row cache、caching database、cache metrics、
database conformance 定向测试，全量 CTest 和 ASan；覆盖投影顺序交换、白名单空/非空、
复合 PK、clear 期间回填和写后新 waiter。

### 阶段 3：事务与 MySQL 严格性

- SQLite 读改写使用已有契约允许的最小事务边界（优先 `BEGIN IMMEDIATE`），
  保证不同列并发更新不互相覆盖。
- MySQL 锁定读（`FOR UPDATE`）与显式 commit 检查；TxGuard 只负责未完成路径 rollback。
- 识别 unsigned 数值并拒绝无法映射到 `Value` 的范围；所有数值截断/错误都拒绝。
- metadata 查询使用可靠的 literal escape 或 prepared 参数；补特殊表名测试。
- 若能在现有私有契约内传递连接级 broken 状态，则验证持续流量下坏槽位可及时重建；
  每次 release 探活会改变热路径语义，dynamic_cast/backend 分支也会破坏通用池边界，
  因此若无法无公开 API 地实现则只测试、记录并延后，不新增 `poison()` 或改写既有仅空闲
  探活语义。

验证门槛：SQLite 并发 barrier 测试；MySQL 环境存在时 CRUD/introspection/conformance
和故障注入必须失败即失败，未配置环境才允许 skip；ASan/LSan 回归。

### 阶段 4：HTTP 生命周期与传输边界

- 停止接收后在依赖对象仍存活时显式停止/排空线程池；确保 queued、active 任务都不
  能访问已销毁的 server/request pool。
- 有界循环读取到 `CRLFCRLF`，区分不完整、畸形和超长首部；保持既有状态码矩阵。
- 为响应写入设置有限 deadline，慢读客户端不阻塞停机 join。
- 将 Winsock/POSIX socket 类型、错误码、超时参数隔离到现有私有平台适配层；不引入
  新 HTTP 库或 keep-alive。

验证门槛：HTTP unit、queued/stalled shutdown、分片 header/body、慢读 peer、live e2e；
ASan 验证无 UAF。Windows 测试只有在补齐 SQLite/toolchain 后标记通过。

### 阶段 5：Python SDK 协议与重试语义

- 自增 insert 的 no-retry 贯穿 `_execute`、307 跟随和节点切换；首节点传输不确定时
  只报告不确定性，不向另一节点重发。
- 普通 307 不添加 Fallback；已有 Fallback 时保持，不得升级提示；目标请求使用统一
  transport/retry/backoff/blacklist 语义。
- HTTPError 读取截断体统一转换为现有 `NodeUnavailable` 分类。

验证门槛：Python unittest 在现有 51 条基础上新增 commit-then-disconnect、307 多跳
头部、目标失败归因和截断体用例；SDK live e2e 可用时运行。

### 阶段 6：池、配置、观测与收尾

只实现能由稳定测试证明的 P2：tried-slot 重试、异常安全窗口、peak CAS、空 factory、
`--config` 缺参、JSON/Prometheus 转义和 unsigned JSON 转换。每项独立测试、独立提交，
不把猜测性重构混入主线。

最后独立处理文档过时项（skill 中 MySQL “未实现”），并复核脚本/e2e 不引入架构决策。
每节点 SQLite 数据一致性问题只写入已知限制和测试说明，除非获得新的架构授权。

## 6. 风险与回滚

- 阶段 1 先完成是为了消除 LSan 噪声；之后的 sanitizer 结果才有判别力。
- 缓存阶段不改变 key 编码、TTL 数值、分片数或失效策略，只修复回填内容和世代隔离。
- HTTP 停机修复不能通过 detach 或强杀线程绕过生命周期；不能新增假的 timeout 语义。
- MySQL 真实环境不可用时，代码可先由 mock/fault-injection 覆盖，但最终报告必须明确
  哪些 live 断言未执行。
- 任一阶段失败可回滚该阶段单一提交，不依赖后续阶段的未提交变更。

## 7. 完成判据

只有同时满足以下条件才可将本轮标记完成：

1. 所有阶段的目标测试和全量 CTest/SDK 回归通过；
2. ASan/UBSan 通过且数据库 statement 泄漏已消失；
3. 可用平台完成 Windows/MinGW 验证，缺失环境项明确列为未验证；
4. 每个行为修复有对应失败测试、代码审查记录和单独 commit；
5. API docs、spec 留痕、README 和已知限制与实际实现一致；
6. 完成审计逐项说明未处理的架构债务，而不是用窄测试替代证明。
