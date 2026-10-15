# `cpl` 命令行设计方案

**状态**：阶段 0–2 已实现；Linux/WSL 本机生命周期、Windows SDK start/status/stop、ACL/句柄级路径安全、file-level reparse 及 F9–F11 修复通过；发行/兼容性和 Linux 前台就绪、真实终端 Ctrl+C 已补验。Compose 的远程 health、SDK/CLI CRUD、优雅停止和数据卷恢复已在 WSL Docker Engine 验收；macOS 本机生命周期仍待对应环境。当前进度见 [§6.1](#61-当前实施进度2026-10-14)。
**目标版本**：`cpl` v1  
**文档修订**：R5

**修订日期**：2026-10-15
**适用范围**：CppPools Python SDK 的命令行入口，以及本机 CppPools 节点的生命周期管理。
**实施前置**：先完成[统一实施路线 S0 五项高优修复](implementation-roadmap.md#21-s0-内部执行顺序五个优先问题)；CLI 的真实 HTTP 验收依赖 HTTP 慢速发送边界已经有测试证据。

**实现顺序**：见 [统一实施路线](implementation-roadmap.md)。本文阶段0–2对应S1，阶段3–5对应S2；Redis发现命令在S7扩展现有parser/包入口，不重做本地生命周期。启用缓存/发现后的资源收尾完成，才允许main发布stopped终态；本地stop不关闭独立Redis进程。

## 1. 目标与边界

`cpl` 是 SDK 的命令行外壳。它的 CRUD、拓扑发现、唯一键路由、307 跟随、epoch 处理、故障绕行和重试都复用现有 `ClusterClient`；它不重新实现一套 HTTP 客户端，也不改变服务端 API 契约。

CLI 有两类职责：

1. **远程数据操作**：`health`、`select`、`insert`、`update`、`delete` 可以访问任意可达 CppPools 节点。`--url`/`-u` 指定一个或多个 seed，行为与 SDK 一致，不要求目标节点由当前 CLI 启动。
2. **本地节点管理**：`start`、`stop`、`status` 只管理本机、且由 `cpl` 状态目录登记的服务进程。它们不把 `cluster.nodes` 误认为本机进程清单，也不自动启动节点来满足 CRUD 请求。

首版不增加 DDL、任意 SQL、批量删除、全表扫描、分页、排序、复合 `where`、事务控制或新的 HTTP 端点。现有服务的单行 CRUD 范围保持不变：`where` 必须是一个非空的主键/唯一键等值条件；查询无结果和更新/删除影响 0 行仍是成功。

## 2. 现状约束与设计决策

### 2.1 已复用的现有能力

| 现有能力 | CLI 复用方式 |
|---|---|
| `ClusterClient.select/insert/update/delete` | CRUD 命令唯一的数据操作入口 |
| `transport.request` | 独立的 `health` 探针；v1 不增加 stats/schema 命令 |
| SDK 的 Murmur3 环、一致性哈希和 schema 路由 | CLI 不复制路由逻辑 |
| SDK 的 307、epoch、Fallback、黑名单、重试矩阵 | CLI 不自行重发请求 |
| 服务端 `/health` | 只作为进程可达/存活探针，不声称数据库深度健康 |
| `cpppools_server --config <path>` | `start` 的进程启动入口 |
| `SIGTERM/SIGINT` 停机流程 | POSIX 下的优雅停止基础 |

`ClusterClient` 构造时会立即拉取 `/api/v1/cluster` 和 `/api/v1/schema`，所以 `start`、`stop`、`status` 和基础 `health` 不能先构造 SDK；它们必须使用独立的本地进程管理器或一次 HTTP 探针。

### 2.2 必须守住的边界

- CLI 只做参数解析、输入读取、调用编排、输出和本地进程管理；数据库校验仍在服务端 `Database` NVI 管道中完成。
- 远程操作失败时沿用 SDK 异常分类：客户端错误不重试，409 交由调用者判断，503/5xx/节点不可达遵循 SDK 的既有策略；无显式可路由唯一键的 insert 不自动重试。
- 服务端代码当前的 HTTP 请求体上限为 64 KiB（`kMaxBodyBytes = 65536`）。CLI 可以在发送前按同一序列化口径拒绝超限输入，但不能把旧文档中的 1 MB 当作实现事实。
- `/health` 返回纯文本 `cpppools worker is active`，只证明 HTTP 服务能响应，不证明每个数据源、连接池或 CRUD 都正常。
- 服务端已有的 5 秒 drain 是停止接入后的排空窗口，不是整个进程退出的硬上限。CLI 的 `stop --timeout` 是独立等待期限；超时不得假报成功。
- Windows 的 `Popen.terminate()` 等价于强杀，不会触发 C++ 的 POSIX 信号处理器。Windows 优雅停止必须使用专用的本地控制通道，或明确报告只执行了强制停止。

## 3. 总体架构

```text
                         ┌──────────────────────────────┐
                         │            cpl CLI             │
                         │ argparse / 输入 / 输出 / 错误 │
                         └─────────────┬────────────────┘
                                       │
             ┌─────────────────────────┴─────────────────────────┐
             │                                                   │
   远程操作路径                                          本地生命周期路径
             │                                                   │
  ClusterClient / transport                            LocalNodeManager
             │                                                   │
  任意 seed 或节点 URL                                      本机 server 进程
             │                                                   │
  /cluster /schema /CRUD /health                       状态文件 + 控制通道
             │                                                   │
       CppPools 集群                                       cpppools_server
```

### 3.1 CLI 层

负责命令与参数契约、JSON 输入、输出及退出码。使用 argparse，在根 parser 和每个子命令 parser 都显式设置 allow_abbrev=False，不能假设根配置会继承。只接受声明的长短选项。入口仅导入标准库和轻量 CLI 模块，ClusterClient 延迟到 CRUD 执行时导入；help/version 不访问网络，冷启动性能需按阶段 0 实测。

### 3.2 远程客户端层

`cpl` 直接调用 `sdk/python/cpppools/client.py` 的 `ClusterClient`。CLI 只负责把命令行参数组装成 SDK 需要的 `table`、`where`、`values`、`set_values`、`columns` 和 `datasource`。所有节点选择、拓扑刷新、307 跟随和错误重试由 SDK 完成。

`health` 是例外：它使用一次独立的 GET `/health`，不创建 `ClusterClient`，以便目标节点尚未启动、拓扑暂时不可读时仍能判断 HTTP 存活。

### 3.3 本地节点层

`LocalNodeManager` 只负责本机进程：解析路径、按实例加锁、启动子进程、等待就绪、保存状态、发送停止请求、核验身份与处理超时；终态后清理含 token 的文件，日志保留。

默认状态目录使用平台用户态位置：Windows 为 `%LOCALAPPDATA%/CppPools/cpl/instances`，Linux/macOS 优先 `$XDG_STATE_HOME/cpl/instances`，否则 `~/.local/state/cpl/instances`；`--state-dir/-R` 显式覆盖。实例名限定为 `[A-Za-z0-9][A-Za-z0-9_-]{0,63}`，拒绝路径分隔符、点段及 Windows 保留设备名；Windows 按不区分大小写识别实例。目录拼接后必须仍位于状态根目录内。状态记录必须至少包含：

```json
{
  "protocol_version": 1,
  "state_revision": 1,
  "name": "default",
  "launch_id": "uuid",
  "pid": 1234,
  "process_start_time": "platform-specific",
  "binary": "absolute/path/cpppools_server",
  "config": "absolute/path/config.json",
  "config_sha256": "digest-of-config-bytes-observed-by-cli",
  "workdir": "absolute/path",
  "endpoint": "http://127.0.0.1:8080",
  "log_file": "absolute/path/cpl-default.log",
  "state": "starting|running|stopping|stopped|crashed",
  "stop_mode": null,
  "force_delivery": null,
  "exit_kind": null,
  "last_exit_code": null,
  "started_at": "RFC3339",
  "updated_at": "RFC3339"
}
```

实例布局固定为 `<state-root>/<name>/state.json`、同目录 `instance.lock`、`launches/<launch_id>/` 控制目录；默认日志为本代目录的 server.log。instance.lock 是稳定文件，运行期间不删除或原子替换；状态文件原子替换不影响锁。每次新 launch 都有独立目录，不复用旧控制文件。

创建、更新和删除状态都在实例锁内完成。锁使用 POSIX fcntl.flock、Windows msvcrt.locking 的非阻塞尝试配合有限轮询，CLI 崩溃后由 OS 释放，不能仅以 lock 文件存在判断占用。锁等待最多 3 秒，且不超过 start/stop 的剩余期限；到期返回 10、不得 spawn 或发送控制请求。锁覆盖登记、spawn 与身份补写、控制请求发布；网络探针与退出等待在锁外执行，前台不会一直持锁。

每次状态写入增加 state_revision。锁外工作持有 `(launch_id, state_revision, expected_state)` 快照，重新加锁后读取当前记录；任一项已改变则重新核验并按下表归并，不能直接覆盖。已停止的记录不得写回 running；stopping 不得被迟到 ready/health 写回 running；force 已确认发送时不得被旧的 graceful 等待结果覆盖。launch_id 不同则旧操作只报告本代被替换，不修改新代记录。

停止前必须核验 PID、进程创建时间和可执行文件身份；无法确认归属时不得发送停止或强杀信号。平台进程适配器只使用标准库及 `ctypes`：Linux 读取 `/proc` 启动时间和 exe，macOS 通过 libproc 获取进程启动信息/路径，Windows 持有进程句柄核对创建时间和映像路径。优先通过带 token/launch_id 的控制请求停止，目标服务自己确认归属。POSIX 阶段的信号路径发送前重新核验；仍有核验与发送之间的 OS 竞态，不能声称仅靠 PID 消除了竞态；可用时采用 Linux pidfd，否则验收必须证明遇到已识别的 PID 复用时拒绝发送。

spawn 前 PID/启动时间为 null，spawn 后立即在锁内补写。process_start_time 必须是可精确比较的 OS 创建标识，不用四舍五入的时间戳：Linux 为 boot_id + /proc start_ticks，Windows 为原始 FILETIME，macOS 为原始启动秒/微秒。server 在 ready 中写入自己的同口径标识，CLI 对照 OS 信息验证，不能拿当前 PID 的新创建时间倒推旧进程身份。进程已退出或成为 zombie 不算 process_alive，kill(pid,0) 成功不能单独证明存活。

后台父 CLI 退出后，另一 POSIX CLI 通常不能 wait 任意非子进程；last_exit_code 只在实际观察到退出码或可信终态提供时填写，其余为 null。exit_kind 为 null、graceful、unexpected 或 forced；强杀后 state=crashed、exit_kind=forced。

若 CLI 在 spawn 与身份补写之间崩溃，status 只能用同代 starting.json 或 ready 的 PID/process_start_time 与 OS 身份和登记 binary 对照后恢复；同一 binary 的 PID 复用也必须拒绝。没有任何可信启动握手且 PID 为空时保留 starting，返回 8，不猜测进程或覆盖后重启。确认退出后删除 control.token、含 token 的 stop.request 和密钥临时文件；仅保留无 secret 的元数据、终态与日志。

归属/权限的临时查询失败仅在本次输出显示 unmanaged，保留 last_known_state，不把可信生命周期状态永久改写为 unmanaged。权限恢复后可重新核验、恢复查询和续停；损坏记录返回 10，不写一个猜测的新记录。

状态迁移固定为：

| 当前状态 | 事件 | 新状态 | 约束 |
|---|---|---|---|
| 无记录 / `stopped` / `crashed` | start 获得实例锁 | `starting` | 分配新 launch_id；spawn 前写入 |
| `starting` | matching ready + identity + health 200 | `running` | 快照仍为 starting；已有停止请求时禁止提交 |
| `starting` | child 已确认退出 | `crashed` | 保留已知退出码和日志，未知退出码为 null |
| `starting` | 启动期限到达且 child 仍存活 | `stopping` | 请求优雅停止；未确认退出不得标记 crashed |
| `starting/running` | 已核验身份后用户 stop/force 发出 | `stopping` | 与启动超时清理使用同一停止规则 |
| `stopping` | matching stopped 终态且确认进程退出 | `stopped` | exit_kind=graceful；stop 返回 0 |
| `stopping` | 确认强杀后进程退出 | `crashed` | exit_kind=forced；stop 返回 9 |
| `running/starting/stopping` | 已退出、有效 graceful 终态、force_delivery 为空或 rejected | `stopped` | 避免父 CLI 关闭后被误记 crashed |
| `stopping` | 已退出、force_delivery=pending 且无法补证 | `crashed` | exit_kind=null，返回 10，不猜测 graceful/forced |
| `running/starting/stopping` | 旧进程确认死亡或 PID 被复用，缺少有效终态 | `crashed` | 不给复用 PID 的进程发信号；据 force_delivery 判定已知 exit_kind |
| `stopping` | CLI deadline 到达 | `stopping` | stop 返回 7，可稍后查询或续停 |
| 任一登记状态 | 活进程归属无法确认 / 状态损坏 | 原可信状态不变 | 本次输出 unmanaged；拒绝控制，返回 8/10 |
| `running` | 进程意外退出 | `crashed` | 更新可获得的退出信息，不编造退出码 |

start 只有在确认旧进程已经死亡或 PID 已被复用时，才能将旧记录转为 `crashed` 后创建新 launch。权限不足等导致无法核验存活进程时返回 8，状态损坏返回 10，均不覆盖旧记录；不得仅凭端口开放复用旧记录。

### 3.4 本地控制通道

方案为后台节点定义可选的本机参数 `--control-dir`，由 CLI 在启动时生成实例目录和随机 token。Linux 与 Windows C++ 服务消费同一 JSON v1 控制协议；Linux 与 Windows SDK start/status/stop 已有真实进程证据。每次 launch 使用独立子目录。Windows 当前用户 + SYSTEM 受保护 DACL、父目录句柄固定、文件级 reparse 检查和中文/emoji 状态目录已实测；macOS 生命周期、Windows/macOS 前台真实终端仍有门槛，不能据此称三平台生命周期均已完成。

服务完成 bind/listen 后原子写入 ready，包含 launch_id、实际端口、node_id、PID、精确 process_start_time 和状态；CLI 同时核验 ready、OS 身份和 HTTP health。配置摘要仅由 CLI 用 hashlib 记录，不要求 C++ 新增 SHA-256 实现/依赖，也不是加载证明。启动期间配置应保持不变；spawn 前/ready 后复检发现变化时，按启动失败规则请求停止并给 10 秒清理期限，已退出返回 6、仍活返回 7。v1 不做配置快照/热加载。

当前 src/main.cpp 的第二次 SIGTERM/SIGINT 会直接 _exit。阶段 3 必须调整**启用 control-dir 的托管进程**：普通停止信号重复到达时只幂等调用 request_stop，不升级为强制退出；文件 I/O 仍在主流程完成，不放进信号 handler。未启用 control-dir 的直接启动方式保留现有信号行为。阶段 4 的控制请求重复到达也幂等；强杀只能通过显式 stop --force 发出，前台 CLI 在等待收尾期间的再次 Ctrl+C 不自动强杀。

托管信号桥在控制元数据验证后、耗时配置/数据库初始化前安装，覆盖初始化到完整资源收尾和终态发布。HttpServer 尚未存在或已经解除引用时，handler 只记录原子 pending_stop；主流程在安全检查点消费它，取消后续初始化或调用 request_stop。server 销毁前解除 handler 引用，不在池/数据库析构前恢复默认信号处置。阶段 4 同样在初始化检查点读取认证 stop 文件；阻塞中的后端初始化不新增取消接口，CLI 期限到达仍返回 7并保留记录，不能假报已退出。

为覆盖 exec 到 handler 安装之间的窗口，main 安装托管桥后立即写 starting.json（同代 PID/创建身份），再做耗时初始化。所有普通 SIGTERM，包括阶段 4 的信号后备，都须等待 matching starting/ready 并重新核验身份；未收到握手且已到期限返回 7、保留 stopping，不向默认 handler 发信号。--force 可在身份核验后直接强杀。starting 不是 HTTP 就绪；文件停止可在早期发布，由服务在安全检查点读取。

控制目录的约束：

- 目录位于 CLI 状态目录下。POSIX 启动时强制目录 0700、控制文件 0600；Windows 设置并验证当前用户 + SYSTEM 的受保护 DACL。读取、写入、清理期间逐级打开目录且不共享删除权限，防止父目录重命名；创建缺失目录前先固定父目录，文件读取检查已打开句柄的 reparse 属性。测试覆盖真实 junction、file-level GUID reparse 和打开时替换；这不是对同一账户其他程序的隔离保证。
- `control.token` 和 `stop.request` 只存在受限目录；stop 请求只包含协议版本、`launch_id`、请求时间和 token，不接受任意 shell 命令。token 不出现在命令行、环境变量、stdout/stderr 或普通 status 输出中。
- `control.meta` 在 spawn 前创建且不包含 secret；缺失、协议版本不支持或 launch_id 无效时，server fail-fast 且不写 ready。
- starting/ready/state/stopping/stopped 文件均采用临时写入加原子替换，带协议版本与 launch_id；starting 写失败则不继续耗时初始化，fail-fast。
- 服务只有在 listen 成功后写 ready；写入失败必须中止启动并关闭 listener，不能留下未登记的可用服务。接受停止后写 stopping；即使该文件写入失败也继续停机。stopped 在 main 完成 HTTP/线程池/数据库资源收尾后写入，失败时输出无 secret 诊断并以非零退出，CLI 返回 10。graceful 指沿现有 request_stop 路径有序退出，不保证 drain 到期前所有排队请求都已提交；当前实现会中断活动 socket/丢弃待执行任务，CLI 不改变该策略。
- POSIX listener 使用 `poll(..., 200ms)`；Windows listener 使用 `wait_socket_readable(..., 200ms)` 后再以阻塞 `accept` 保持客户端 socket 的阻塞接收语义。Windows 原生 live 测试已验证 authenticated control request 可在没有新 HTTP 连接时停止服务；Linux 和 Windows C++ 服务行为已测，macOS 生命周期仍待 S2 验收。

控制文件的最小协议如下；JSON 协议版本为整数 1，含 launch_id 的文件必须匹配当前代，忽略旧代文件。CLI 是 state.json 唯一写入者，server 不直接修改它。

| 文件 | 写入者 | 字段 / 生效条件 |
|---|---|---|
| `control.meta` | CLI，spawn 前 | protocol_version、launch_id；缺失或不支持则启动失败 |
| `control.token` | CLI，spawn 前 | `secrets` 生成的至少 256 bit 随机 token；只读入内存、不输出 |
| `starting.json` | main，信号桥安装后 | protocol_version、launch_id、pid、process_start_time、state=starting；不代表 HTTP 就绪 |
| `ready.json` | server，listen 成功后 | protocol_version、launch_id、pid、process_start_time、port、node_id、state=ready |
| `stop.request` | CLI，原子发布 | protocol_version、launch_id、requested_at、token；重复同代请求幂等 |
| `stopping.json` | server，接受停止时 | protocol_version、launch_id、pid、process_start_time、state=stopping |
| `stopped.json` | main，资源收尾后 | protocol_version、launch_id、pid、process_start_time、state=stopped、exit_kind=graceful、exit_code |

starting/ready/stopping/stopped 含同代 process_start_time，恢复时与已登记身份对照；spawn 前 control.meta 尚无身份。旧 server 缺少握手时，CRUD/health 仍可使用，start 提示升级；child 已退出返回 6，仍活且不能安全清理返回 7、保留记录并提示显式 --force。不能用普通 SIGTERM 冒充已知安全的托管停止；v1 不自动接管直接启动的进程。

控制目录和文件采用 POSIX 0700/0600；Windows 用标准库 ctypes 调用安全 API 设置仅当前用户、SYSTEM 可访问的 ACL，不直接继承宽泛权限。权限无法验证时失败，不降级到开放目录。坏 token、旧 launch 或格式错误的请求不停止服务，只写不含 token 的诊断；控制文件限长 4 KiB，拒绝符号链接/reparse point 指向的控制文件。该协议不提供远程停机能力，也不声称防御同一用户的其他程序。

这是一个本地控制面，不是新的远程 HTTP API，也不改变集群路由和数据库层依赖方向。

### 3.5 实现边界与兼容性

CLI 模块按 parser/输入/输出/远程适配/本地管理分工；本地管理器内部通过少量私有平台函数封装身份查询、OS 锁和终止，不把平台细节放进 CRUD 处理器。现有 SDK 的 client.py、transport.py 和错误分类保持业务语义；health 可通过 transport 的 opener 注入不自动重定向的标准库 handler，不另建 HTTP 栈。

服务端本机控制文件读写放在 `src/server/` 私有组件中，仅由 main 和 HTTP 生命周期调用。`main.cpp` 解析可选参数并负责完整退出终态，`http_server.cpp` 在 listen 成功后发布 ready、在循环中轮询请求；如需在 HttpServerConfig 增加可选控制路径，默认空值且不暴露 token/JSON 实现类型。未传 control-dir 的服务保持既有启动、HTTP、集群和数据库行为。实施阶段 3/4 同步相关 spec 的增量修订，现有不变量不变。

## 4. 命令契约

命令名固定为 `cpl`。长命令和别名均显式登记；不依赖 argparse 的任意前缀匹配。

### 4.1 命令总表

| 正式命令 | 别名 | 作用 | 是否需要 SDK |
|---|---|---|---|
| `start` | — | 启动本机服务进程 | 否 |
| `stop` | — | 停止 CLI 管理的本机进程 | 否 |
| `status` | `st`（独立命令别名） | 查看本机进程和 HTTP 状态 | 否；可选探针 |
| `health` | `h`（独立命令别名） | 对任意 URL 执行一次 `/health` | 否 |
| `select` | `get` | 查询单行 | 是 |
| `insert` | `add` | 插入一行 | 是 |
| `update` | `set` | 更新一行 | 是 |
| `delete` | `del` | 删除一行 | 是 |

### 4.2 通用选项

网络、数据源、输出和超时选项在各自命令 parser 中显式登记，固定写在命令名之后；首版不支持把这些选项放在 `cpl` 根命令之前。这样每个 `-u/-d/-o/-t` 都有明确命令作用域，避免 argparse 根 parser 与子 parser 默认值覆盖，也使多 seed 顺序可预测。版本选项是例外，只在根 parser 提供。

| 选项 | 短写 | 默认值 | 说明 |
|---|---|---|---|
| `--url URL` | `-u URL` | `CPL_URL` 或 `http://127.0.0.1:8080` | 远程 seed；CRUD/health 可重复，按命令行顺序尝试；环境变量只接受单 URL |
| `--datasource NAME` | `-d NAME` | 空 | CRUD 的显式数据源 |
| `--output {table,json}` | `-o` | `table`（可选 `json`） | 输出格式；支持该选项的命令显式登记 |
| `--timeout SEC` | `-t SEC` | health/CRUD/status 探针 3 秒；start 15 秒；stop 10 秒 | 网络命令为每次请求的 SDK/transport timeout；start/stop 为生命周期等待期限 |
| `--help` | `-h` | — | 打印帮助并退出 0 |
| `--version` | `-V` | — | 仅根命令：打印版本并退出 0 |

`-d` 仅表示数据源，`-u` 仅表示 URL，`-t` 仅表示超时；短选项不能在不同语义间复用。对 seed 列表，显式 `-u` 全部覆盖 `CPL_URL`；不提供时才使用环境变量，再回退默认 URL。诊断写 stderr，业务结果写 stdout。非 TTY 默认不使用颜色。

URL 为包含主机、可选端口的 HTTP/HTTPS origin，去除尾部 `/`；拒绝用户信息、业务路径、query 和 fragment。非法 URL、空表名/数据源名及非正数、NaN/Infinity timeout 均在网络请求前返回 2。单值选项重复给出返回 2；只有 CRUD/health 的 `-u` 与 select 的 `-C` 允许重复。

CRUD 的 `-t` 原样传给 `ClusterClient(read_timeout=...)`，包括拓扑/schema 请求；SDK 的 connect_timeout 首版没有独立效果。该值是 urllib 的连接及 socket 读取等待参数，不是整条命令 deadline；初始化、多节点尝试、307 和退避可能让总耗时超过它。health 多 URL 逐个执行，每个探针使用同样 timeout。start/stop 的等待用 monotonic 时钟，探针以剩余时间限制；不通过 CLI 中断/重发写请求制造一个新的 SDK 总超时模型。

### 4.3 JSON 输入约定

`where`、`values`、`set` 和其他 JSON 参数统一支持三种形式：

- 直接 JSON 字面量，例如 `'{"column":"id","value":42}'`；
- `@path/to/file.json`，从文件读取；
- `-`，从 stdin 读取。

一次命令最多有一个 stdin 输入，且不能与同一字段的文件/字面量重复。文件/stdin 按 UTF-8 读取，允许移除一个 UTF-8 BOM，拒绝无效编码。解析器拒绝重复键、NaN、Infinity、解析后成为非有限值的浮点数、超过 int64 的整数和非 JSON 数据；JSON 对象/数组结构嵌套最多 256 层，超限统一报告输入错误。CLI 不把所有值强行转换为字符串；最终列类型交给服务端严格校验。

结构校验要求 where 恰好含非空字符串 column 和标量 value；values/set 必须是非空对象，值只接受 SDK 支持的 JSON 标量，不接受嵌套数组/对象；布尔值是否合法由服务端决定。输入文件读取设 64 KiB 上限；组装完整 payload（含 table、datasource、where、columns、values/set）后，按现有 `json.dumps(payload).encode('utf-8')` 计算真实请求体大小，恰好 65536 字节可发送，65537 字节及以上返回 2。不能只检查 `-D` 原文长度；中文默认转义带来的膨胀也需计入。预检在 SDK 构造前完成，避免错误输入先访问远端。

### 4.4 `start`

```text
cpl start -c CONFIG [-n NAME] [-b BINARY] [-W WORKDIR]
          [-R STATE_DIR] [-l LOG_FILE] [-t SEC] [-F] [-o table|json]
```

| 参数 | 短写 | 必需 | 说明 |
|---|---|---:|---|
| `--name NAME` | `-n` | 否 | 实例名，默认 `default`；用于状态隔离 |
| `--config PATH` | `-c` | 是 | 服务 JSON 配置；转为绝对路径 |
| `--binary PATH` | `-b` | 否 | 顺序：显式参数 → `CPL_BINARY` → PATH 中的 `cpppools_server[.exe]`；找不到返回 6，不猜测构建目录 |
| `--workdir PATH` | `-W` | 否 | 默认启动命令的当前目录；SQLite 相对路径按该目录解析。若从其他目录启动项目配置，必须显式指定项目工作目录 |
| `--state-dir PATH` | `-R` | 否 | 第 3.3 节的用户状态目录；转为绝对路径 |
| `--log-file PATH` | `-l` | 否 | 默认实例目录下日志文件 |
| `--timeout SEC` | `-t` | 否 | 等待子进程存活、ready 文件和 health 的总期限，默认 15 秒 |
| `--foreground` | `-F` | 否 | 默认后台；附着等待时 CLI 捕获 Ctrl+C，经本阶段可用停止机制收尾 |
| `--output {table,json}` | `-o` | 否 | 同通用输出选项；前台模式也保持 stdout 可解析 |

行为契约：

1. 将参数中的相对路径按 CLI 调用时的 cwd 解析，固定子进程 cwd；SQLite 相对路径仍按 workdir 解析，不重写配置。同名 running 且 binary/config/workdir 和配置字节摘要一致时，返回 already_running=true、退出 0；同路径配置变更返回 6，提示显式 stop 后 start。starting/stopping 不重复 spawn，返回 6并提示 status/stop；归属不明返回 8。
2. 生成 launch_id/token，锁内写 starting 和 CLI 观察到的 config_sha256，再写 control.meta/token，spawn 并补齐身份后释放锁。当前 CLI 若能确认控制文件创建/Popen 失败且未创建 child，就记录 crashed、清理本代 secret，按原因返回 6（启动）或 10（本地 I/O）；不把已知未 spawn 当成崩溃后的未知结果。状态写入本身失败时返回 10并报告实际保留路径，不假报清理完成。服务读取控制元数据；token 不进入命令行/环境变量。
3. 同时观察子进程退出、ready 文件和 `/health`。只收到旧进程或其他服务的 200 不能判定启动成功。
4. 成功后按状态提交规则写 running，再输出 endpoint/PID/日志；endpoint 使用 ready 的实际 port 与本机 loopback。若并发 stop 已让本代进入 stopping/stopped/crashed，start 不得写回 running，返回 6、输出最新状态。启动超时先请求优雅停止，另给固定 10 秒清理期限；退出后返回 6，仍活返回 7 并保留控制信息，不自动强杀。
5. 前后台都登记状态。后台 POSIX 用独立 session，Windows 用脱离调用终端的进程启动标志并隐藏窗口；stdin 为 DEVNULL、stdout/stderr 写日志，以参数数组启动且 shell=false。`-F` 由 CLI 保持附着，server 使用独立进程组防止 Ctrl+C 被双方重复处理；server stdout/stderr 写日志或转发到 CLI stderr，CLI stdout 只输出结果。另一 CLI 可 stop/status；CLI 被单独关闭时保留可恢复登记。
6. 已完成就绪登记的前台附着阶段，正常 graceful 退出返回 0，异常/无法确认退出返回 10，显式 force 且确认退出返回 9；就绪前失败按前述 start 规则。Ctrl+C 使用默认 stop 的 10 秒等待：确认 graceful 退出返回 130，超时返回 7，身份/状态失败返回 8/10，不自动强杀。阶段 3 SIGTERM、阶段 4 控制文件。前台就绪登记完成后立即在 stderr 输出并 flush 一份 running 状态，再附着等待；stdout 只在结束后输出最终结果。

### 4.5 `stop`

```text
cpl stop [-n NAME] [-R STATE_DIR] [-t SEC] [-f] [-o table|json]
```

`--name/-n`、`--state-dir/-R` 默认值同 start；`--force/-f` 默认 false，`--timeout/-t` 默认 10 秒，`--output/-o` 默认 table。缺失登记返回 10；已确认 stopped 幂等返回 0。旧进程已死亡时先按 force_delivery 与终态规则归并：accepted 返回 9，pending 无法补证返回 10，有效 graceful 返回 0，其余 crashed 返回 10；不向复用 PID 的进程发信号。

默认流程：读取状态、核验身份、发布带 token 的 stop.request，再在锁外等待 matching stopped 且进程退出。阶段 3 POSIX 使用 SIGTERM，阶段 4 后控制文件为跨平台默认通道；POSIX 在控制请求无法发布但仍能核验身份时允许信号后备，并记录诊断。状态为 stopping 时允许同代续停并幂等补发认证请求；已经 accepted 的 force 不重复投递或降级为普通请求。Windows 不把 terminate 称为优雅停止。

`--force` 立即向已核验目标发送强杀（POSIX SIGKILL、Windows TerminateProcess），随后用 -t 等待，不先优雅等待。锁内先登记 state=stopping、stop_mode=force、force_delivery=pending；OS 接受后登记 accepted 及发送时间。后续 status 以 accepted 和确认退出恢复 crashed/forced；旧 graceful 等待结果不能覆盖它。

同代 accepted 是单调事实：后续 stop/force/状态更新都不得降回 pending/rejected 或清空发送时间。再次 force 看到 accepted 时只重新核验并等待退出，不重复初始化发送记录；只有没有 accepted 历史才走 pending→accepted/rejected。pending 在崩溃后可由新的显式 force 操作重新核验并发送，不能自动当作已送达。

OS 明确拒绝强杀时登记 rejected，返回 8（权限/身份）或 10（其他控制失败）；发送前目标已退出则按既有终态返回，不谎报强杀。若 CLI 在发送与落盘之间崩溃，pending 不证明已发送，也不能排除已发送；退出后的 exit_kind=null、返回 10，除非实际父进程退出信息能补证。accepted 且确认退出返回 9，仍活返回 7、保留 stopping 与发送证据。force_delivery 的值固定为空、pending、accepted、rejected。SIGTERM/控制文件重复停止必须幂等。

终态归并顺序固定为：accepted + 退出 → forced；pending + 退出且无法补证 → unknown；为空/rejected + 有效 graceful 终态 + 退出 → graceful；其余已退出 → unexpected。在发送前已确认退出时取消本次 pending，按此前记录和终态归并。stop 用 0/9/10 报告结果；status 只要成功核验并读出状态仍返回 0，用 exit_kind/last_exit_code 报告分类。

### 4.6 `status`

```text
cpl status [-n NAME] [-R STATE_DIR] [-u URL] [-t SEC] [-o table|json]
cpl st    [-n NAME] [-R STATE_DIR] [-u URL] [-t SEC] [-o table|json]
```

`-n/-R` 默认值同 start；`-u` 最多一次，只覆盖该本机实例的 HTTP 探针 URL，不改变登记 endpoint、进程归属或控制目标，不读取 CPL_URL。缺省检查登记 endpoint；停止态不探测端口，避免把其他服务误算进来。缺失记录返回 10。

输出区分 stopped、starting、running、stopping、crashed、unmanaged，并分别展示进程存在、身份匹配、ready、HTTP 响应、exit_kind 和可为空的退出码。探针失败不把仍活进程变成 crashed。状态可读取且归属可判断时返回 0，即使 stopped/crashed；存活归属无法核验返回 8，损坏/无法读取状态返回 10。探针失败作为字段展示，不影响 status 的查询退出码；脚本检查 HTTP 存活应使用 health。

### 4.7 `health`

```text
cpl health [-u URL] [-t SEC] [-o table|json]
cpl h      [-u URL] [-t SEC] [-o table|json]
```

对每个 URL 请求 GET /health，不创建 SDK、不拉拓扑、不跟随 307；通过 transport 的 opener 参数关闭 urllib 默认 GET 重定向，以免检查到另一节点。成功条件是 HTTP 200 且可读取响应。按输入顺序打印每个 URL、HTTP 状态、响应正文和耗时，单个失败也继续其他探针。不增加重试；复用 transport 对一次请求的分类。只宣称 HTTP 存活。

全部成功返回 0；混合错误按 3（网络不可达）→5（5xx）→4（4xx/其余非 200）的固定优先级取退出码，与 URL 顺序无关；每项保留各自错误。输入校验失败返回 2 且不发任何请求。

### 4.8 `select/get`

```text
cpl select TABLE -w WHERE [-C COLUMN]... [-u URL]... [-d DATASOURCE]
           [-t SEC] [-o table|json]
```

- `TABLE` 为表名。
- `-w/--where` 必须解析为 `{"column": string, "value": JSON_VALUE}`。
- `-C/--column` 每次一个列名，可重复，如 `-C id -C score`；缺省为全列，顺序和重复列保持服务端语义。
- 返回 SDK 的解析后 JSON；无匹配是成功，结果为 `rows: []`。

### 4.9 `insert/add`

```text
cpl insert TABLE -D VALUES [-u URL]... [-d DATASOURCE] [-t SEC] [-o table|json]
```

`-D/--data` 必须是 JSON object。显式唯一键 insert 遵循 SDK 的可重试规则；无显式可路由唯一键的自增 insert 不自动重发。成功对应 HTTP 201，输出 `datasource`、`affected_rows` 和 `inserted_id`。

### 4.10 `update/set`

```text
cpl update TABLE -w WHERE -D PATCH [-u URL]... [-d DATASOURCE]
           [-t SEC] [-o table|json]
```

`-D/--data` 必须是 JSON object，`-w` 仍是单列唯一键等值条件。影响 0 行返回成功并输出 `affected_rows: 0`。

### 4.11 `delete/del`

```text
cpl delete TABLE -w WHERE [-u URL]... [-d DATASOURCE] [-t SEC] [-o table|json]
```

只允许单行唯一键等值删除；不提供全表删除开关。影响 0 行仍返回成功。

### 4.12 典型调用

```powershell
# 直接把任意远端节点作为 seed；本机无需启动服务
cpl get user_scores -u http://node-a:8080 -w '{"column":"user_id","value":12345}' -C score

# 多个 seed，复杂 JSON 用文件避免终端转义
cpl select user_scores -u http://node-a:8080 -u http://node-b:8080 `
  -w '@where.json' -d main -o json
cpl insert user_scores -u http://node-a:8080 -D '@row.json'
cpl update user_scores -u http://node-a:8080 -w '@where.json' -D '{"score":88}'
cpl delete user_scores -u http://node-a:8080 -w '@where.json'

# 本地节点默认后台运行；指定 -F 时当前终端附着
cpl start -n dev-a -c config/cpppools.example.json -b build/bin/cpppools_server
cpl status -n dev-a
cpl stop -n dev-a -t 10
```

## 5. 输出与退出码

业务数据写 stdout，错误/诊断/重试说明写 stderr。CRUD 的 JSON 模式原样输出 SDK 返回对象，不另包一层；select 的 table 输出列头和行，写操作输出 datasource/affected_rows/inserted_id。所有模式都保留 null、列顺序和空结果，不把零行当错误。

非 CRUD JSON 输出如下；stdout 至多一个完整 UTF-8 JSON 文档加换行，未获得的信息为 null，token 永不输出。health 的部分失败仍输出完整数组，stop 超时/强杀仍输出实例结果，诊断另写 stderr。参数解析失败沿用 argparse 用法文字；运行期 JSON 错误在 stderr 输出 error 对象，不打印 traceback。

`start -F` 的就绪提示属于诊断，写 stderr；JSON 模式为单行 running 状态，table 模式为状态摘要。前台失败或中断时，stderr 后续还会出现单行错误对象，因此不能把整个前台 stderr 当作一份 JSON 解析；stdout 仍只有最终实例对象。就绪提示只在 running 状态提交后发出，提示输出失败保留已登记进程，不自动杀进程。

| 命令 | JSON 字段 |
|---|---|
| start | name、launch_id、pid、endpoint、state、log_file、already_running |
| stop | name、launch_id、pid、state、exit_kind、last_exit_code |
| status | 实例字段加 last_known_state、process_alive、identity_verified、ready、health、updated_at |
| health | results 数组；每项 url、ok、http_status、body、elapsed_ms、error |

类型冻结：name/launch_id/endpoint/state 等为 string 或 null，pid/last_exit_code/http_status 为 integer 或 null；process_alive、identity_verified、ready 为 boolean 或 null。status.health 为 health.results 的一个结果对象或 null（未探测）；last_known_state 记录可信登记状态，正常时等于 state，临时 unmanaged 时保持原值。探针 body 为 UTF-8 文本，elapsed_ms 为非负 number，ok 为 boolean；成功 error=null。

error 固定为 `{kind, message, exit_code, http_status}`，无 HTTP 状态时 http_status=null。kind 取 usage_error、input_error、node_unavailable、topology_error、client_error、server_error、protocol_error、start_error、stop_timeout、identity_error、forced_stop、local_state_error 或 interrupted；message 供人阅读，不作为脚本判断依据。

CRUD 在成功 HTTP 响应中遇到无效 JSON/结果结构时属于 protocol_error，返回 5，不重新发送；写操作可能已经生效，诊断明确“结果无法确认”。只捕获响应解析/结构校验的错误，不能把任意 ValueError 都误判成远端错误。初始化畸形拓扑仍由 SDK 的 TopologyError 返回 3。JSON 模式保留 SDK 对象的数据字段，结构校验不把返回数组/标量伪装成有效结果。

退出码固定如下：

| 退出码 | 含义 |
|---:|---|
| 0 | 成功；包括 CRUD 无匹配、update/delete 影响 0 行 |
| 2 | 参数、JSON、文件或命令用法错误 |
| 3 | `NodeUnavailable`、`AllNodesBlacklistedError`、`TopologyError`：远程节点或拓扑不可用 |
| 4 | `ClientError`、`ConflictError`：服务端 4xx/409，请求无效或唯一键冲突 |
| 5 | UnavailableError、ServerError 或 CRUD 成功响应无法解析；error.kind 区分 server_error/protocol_error，不新增重发 |
| 6 | 本地节点启动失败、配置/可执行文件不存在，或启动被同代停止请求取消 |
| 7 | 停止超时，实例仍可能处于 stopping |
| 8 | 进程身份无法核验，拒绝执行停止/强杀 |
| 9 | 已核验目标被强杀且确认退出，exit_kind=forced；与 graceful success 区分 |
| 10 | 本地状态缺失/损坏、实例锁等待到期、控制 I/O 失败、异常/无法确认退出，或旧操作被新 launch 替换 |
| 130 | 用户 Ctrl+C 中断；前台节点已确认 graceful 退出后返回，未退出用 7 |

`--help`、`--version` 不访问网络、不读取节点状态，也不创建 SDK。

## 6. 分阶段实现计划

每个阶段遵循 TDD：先写一个能表达契约的失败测试，运行并确认因功能缺失而失败；实现最小行为使其通过；再重构并运行本阶段回归。阶段完成后必须通过评审门，不把未完成的后续能力伪装成可用功能。

每阶段记录 RED 的失败原因、GREEN 的通过输出、回归结果、评审问题及关闭理由。下表保留原阶段目标；当前落地范围见 §6.1，详细历史证据见验收报告和修复计划。

实施前先确认可用 C++ 构建环境和真实 Python 解释器，记录解释器绝对路径与版本；WindowsApps 的 python 别名或命令名存在不算已安装。测试子进程使用 sys.executable。阶段 0–4 用受控 PYTHONPATH 指向 sdk/python，再运行 `python -m cpppools`；本节中的 cpl 表示目标命令契约，不要求阶段 0 已安装 console script。阶段 5 才验收安装后的 cpl，避免依赖倒置。

| 阶段 | 前置条件 | 可 review 的交付 | 进入下一阶段的门槛 |
|---|---|---|---|
| 0 | 本方案定稿 | parser、命令帮助、错误映射、模块骨架 | 无网络 help/version、命令/别名/参数位置测试通过 |
| 1 | 0 | 严格 JSON、table/json 输出、远程 health | 三种输入与多 URL 混合错误验收通过 |
| 2 | 1 | 4 个 SDK CRUD 适配与真实请求测试 | SDK 回归、单节点 CRUD、多节点路由与重试边界通过 |
| 3 | 2 | POSIX 节点管理、ready 和完整 stopped 协议 | 真进程前后台/并发/超时/停机通过；Windows 本地管理标记未就绪 |
| 4 | 3 | 认证 stop 协议、Windows 有限等待、平台身份/权限适配 | 三平台真实生命周期和无新连接的 Windows 停机通过 |
| 5 | 4 | 可安装包、cpl 入口、静态补全、使用文档 | 第 7.4 节发布门槛全部通过 |

### 6.1 当前实施进度（2026-10-14）

原 F9–F11 修复已包含在 `50bc1ff`；2026-10-15 的发行补验见验收报告 §9。以下记录已实现和已测范围，不表示已发布 v1。

| CLI 阶段 / 全局阶段 | 当前实现与已测范围 | 剩余交付或验收 |
|---|---|---|
| 0 / S1 | parser、别名、帮助、模块入口已实现；离线 help/version 及参数契约已测 | 无本轮新增阻断 |
| 1 / S1 | 严格 JSON、输出及远程 health 已实现；F6 补齐超长整数、深嵌套和运行期错误 JSON | 无本轮新增阻断 |
| 2 / S1 | 四个 CRUD 复用 SDK；真实 HTTP 往返及既有路由/重试回归通过 | 发布时继续保留 SDK 组合回归 |
| 3 / S2 | Linux/WSL 与 Windows start/status/stop、身份核验、配置一致性、退出归并已实现；Windows 中文/emoji 状态目录真实启停通过；Linux 前台 running 登记后即时 stderr 提示、真实 PTY Ctrl+C + 慢收尾和输出失败保留进程均已验证 | macOS 身份/生命周期；Windows/macOS 前台真实终端验收 |
| 4 / S2 | Linux 与 Windows C++ 服务使用 JSON v1 控制协议；Windows DACL、句柄级路径固定、目录创建/清理 junction、file-level reparse 和真实停止竞态回归已通过 | macOS 控制生命周期；Windows 自定义日志路径不属于控制目录防护范围 |
| 5 / S2 | 安装版 metadata 版本、三类静态补全和 README 加载说明已交付；Windows 3.12.3/3.8.10 六项独立构建/安装/真实 CRUD 矩阵通过；3.8 源码单测实跑通过 | 平台生命周期仍需按第 7.4 节单独验收；macOS 发行与生命周期不能从 Windows/Linux 结果外推 |

F9–F11 的实现、测试和独立代码复审已完成：前台 Ctrl+C 使用普通认证停止，graceful 后返回 `interrupted`/130；同代 stopping 可补发请求；被取消的 start 返回 6，被新代替换的旧 stop 返回 10。无需新增状态机。详见 [整体评审](completed-stages-overall-review-2026-10-14.md) 和 [修复计划第 7 节](completed-stages-fix-plan-2026-10-14.md#7-整体复审补充任务与执行结果)。

前台命令在 running 登记后即时将状态写 stderr，在附着结束后向 stdout 输出一个最终实例结果；携带结果的失败另写单行结构化诊断。2026-10-15 在 WSL 真实 PTY 中输入 `0x03`，验证内核将 SIGINT 发给 CLI 前台进程组，server 独立 session 不重复接收；在唯一 worker 被半包请求阻塞时，CLI 等待 graceful 后返回 130。就绪输出异常不杀 running 进程的真实测试也通过，详见验收报告 §10；这些证据仅覆盖 Linux/WSL。

F9–F11 的历史测试计数保留在验收报告 §7；本轮发行/兼容性测试见 §9。S2 整体与 v1 发布仍按第 7.4 节门槛验收；A01 已有真实 MySQL SQL mode/CRUD 证据，A02 Windows 总期限的负对照门槛继续保留。

本表为当前状态；后文 F4–F7 按日期保留历史实现限制，已关闭项以验收报告 §11–12 为准。Compose 部署验收见 [部署说明](docker-compose-deployment.md)，不会自动补齐 macOS 本机生命周期、Windows/macOS 前台真实终端或原生完整 CTest 的未决门槛。Unicode 状态目录通过不代表配置、数据库和二进制的所有 Unicode 路径均已验证。

### 阶段 0：契约冻结与工程骨架

**目标**：冻结命令表、短选项、退出码、JSON 输入形态和模块边界。

**设计单元**：只创建实际使用的 cli.py、__main__.py 和 parser 测试；输入/输出模块随阶段 1 的行为测试新增，不提前建空壳。开发入口提供帮助、0+dev 版本和未实现命令错误。

**TDD 与验收**：

- RED：测试 `cpl --help`、`cpl --version` 不构造 SDK、不访问网络；测试每个正式命令和独立别名（`status/st`、`health/h`）能解析。
- GREEN：实现 argparse parser、固定 aliases、`allow_abbrev=False` 和稳定退出码映射。
- 验收：帮助列出全部命令和短选项；把 -u/-d/-o/-t 放在命令名前返回 2；根与每个子 parser 的未声明缩写（如 get --time）都返回 2；单值重复、缺必需参数返回 2。未实现命令不访问网络/启动进程。记录 warm-cache help/version 的耗时和环境，50 ms 为优化目标，不能用未测性能声明完成。

**阶段评审**：检查命令是否覆盖用户目标，`-u/-d/-w/-C/-D/-t` 是否语义唯一，远程与本地职责是否没有交叉。

### 阶段 1：输入、输出与纯远程 health

**目标**：在不创建 SDK 的情况下可靠执行 health，并完成 JSON 输入/输出基础。

**TDD 与验收**：

- RED：测试 literal、`@file`、stdin 三种 JSON 来源；重复 stdin、重复键、NaN/Infinity、int64 越界和不存在文件失败。
- RED：测试 HTTP 200、非 200、307 不跟随、超时、无效 URL；多 URL 中混合网络/4xx/5xx 时仍输出所有结果且聚合退出码顺序无关。
- GREEN：实现输入解析、明确错误、TTY/非 TTY 输出和 `GET /health`。
- 验收：`cpl h -u <url>` 可访问远端节点；不创建 SDK；stdout JSON 可被解析。完整请求体 65536/65537 字节、中文转义、UTF-8 BOM、无效编码及浮点溢出有边界测试；阶段 2 将预检接入实际 CRUD。

**阶段评审**：用旧 API 文档与源码交叉检查，避免把 1 MB 旧描述写入 CLI 契约；检查 stdout/stderr 可管道化。

### 阶段 2：CRUD SDK 适配

**目标**：把四个 CRUD 命令完整映射到现有 `ClusterClient`。

**TDD 与验收**：

- RED：用 fake HTTP 层验证 `select/get`、`insert/add`、`update/set`、`delete/del` 生成的 payload 与现有 SDK API 一致。
- RED：验证多 seed、显式 datasource、307、epoch、503、节点不可达和无键 insert 的重试边界由 SDK 决定，CLI 不二次重试。
- RED：验证 select 无匹配、update/delete 0 行退出 0；409、400、503、拓扑错误映射到固定退出码。
- RED：验证 `-t` 只传给 SDK read_timeout；模拟初始化/重试/退避后的总耗时可超过该值，CLI 不再包一层 deadline 或自动重发。
- RED：模拟写操作已执行但成功响应为畸形 JSON；CLI 返回 5/protocol_error、不打印 traceback、不重发。同类错误在拓扑初始化期间仍返回 3。
- GREEN：实现命令适配、参数校验和输出格式。
- 验收：SDK 回归和 CLI parser/adapter 测试通过；单节点 CRUD 往返、至少两节点的 owner/307 验证、无键 insert 结果不确定时不重发。此阶段尚无 cpl start，夹具用标准库 Popen 直接启动 server；阶段 3/4 再切换为 CLI 启动验证自身生命周期。

**阶段评审**：检查 CLI 没有直接调用 transport 绕过 ClusterClient 的 CRUD 路径，没有全表/任意 SQL 入口，没有对非幂等 insert 额外重发。

### 阶段 3：本地启动、前台运行与 POSIX 停止

**目标**：实现 POSIX 前后台托管、状态记录、与本次启动匹配的 ready、完整 stopped 终态、优雅停止和 status；该阶段用信号停止，Windows 本地生命周期命令暂返回 6 并说明需阶段 4，远程命令不受影响。

**TDD 与验收**：

- RED：测试配置/二进制不存在、端口占用（缺少本次 launch_id 的 ready 时必须失败）、子进程启动后立即退出、health 超时和同名实例重复启动。
- RED：测试状态文件原子替换、实例锁、过期 PID/启动时间不匹配、陈旧状态恢复，以及并发 start/stop 的实例级串行化。
- RED：启动超时但 child 仍活、清理也超时、前台运行中另一 CLI stop、启动 CLI 退出后查询未知退出码、CLI 在 spawn 前后崩溃、非法实例名、强杀后仍未退出。
- RED：同代 start 的迟到 ready 与 stop 竞争，状态不得倒退；ready 的原创建身份与复用 PID 不符必须拒绝；并发 stop、超时后续停与 Ctrl+C 不能触发第二信号强退。force 接受后超时、临时身份查询失败恢复、同路径配置变更也有测试。
- RED：accepted 后再次 force 只等待，不能重写 pending/rejected；pending 的崩溃结果不得猜测 forced，终态与发送证据按统一顺序归并。
- RED：ready 前慢初始化期间及资源析构期间收到普通停止信号均不走默认终止；初始化阻塞超时保留记录，正常初始化取消后也发布可信终态。
- RED：暂停在 exec 后、starting 握手前，普通 stop 等待或超时、不提前发信号；握手出现后重新核验身份才可发送，force 仍需显式选项。
- GREEN：实现 LocalNodeManager、身份/锁/revision、cwd/日志与 POSIX stop。托管桥覆盖初始化至收尾，普通信号幂等；starting 握手确认 handler 已安装，ready 确认监听，main 收尾后写 stopped。阶段 3 不读 stop.request；未托管信号语义不变。
- 验收：真实 POSIX server 前后台启动、status、SIGTERM 排空和终态通过；Ctrl+C 确认优雅退出返回 130，超时返回 7；前台 stdout 不混入 server 日志。模拟旧端口 200 不能冒充新 launch；旧 CLI 的等待结果不能覆盖新 launch。后台父 CLI 已退出时 last_exit_code 可为空；停止超时保留可继续管理的记录。

**阶段评审**：重点检查“端口上旧服务返回 200”不能冒充新实例 ready，检查 SQLite 相对路径没有因 cwd 改变而漂移；用唯一端口和独立临时工作目录的真实进程 fixture 可重复验证。

### 阶段 4：跨平台本地控制通道

**目标**：在服务端与 POSIX SDK 同批切换到认证 JSON v1 stop 请求，保持阶段 3 的 ready/终态语义，并让 C++ 服务在 Linux 与 Windows 都能通过本机控制文件优雅停止。Windows 的 `LocalNodeManager.start` 仍明确不支持，Windows SDK 生命周期启动留待独立 S2 门禁；macOS 本机身份与生命周期也不在本阶段实现。

**TDD 与验收**：

- RED：测试 token 错误、launch_id 不匹配、半写状态文件、并发 stop/start、控制目录权限错误，以及 ready/终态文件写入失败。
- RED：Windows 实际 C++ 服务进程测试验证 stop 请求触发 `request_stop()`，而不是直接 `TerminateProcess`；POSIX 验证相同协议。
- RED：Windows 无新连接时仍响应 stop；前台 Ctrl+C 只处理一次；未退出仍保留 stopping 并返回 7；权限无法设置或验证时不 spawn。强杀归并与 force wire delivery 属于阶段 5/F5。
- RED：ready/stopping/stopped 写入失败的行为分别符合协议；退出后的 token、stop.request、密钥临时文件都被清理，日志/终态不含 secret。
- RED：POSIX 控制文件不可发布时的 SIGTERM 后备仍等待可信 starting/ready，不回到默认信号窗口。
- GREEN：服务端在监听检查点验证 token/launch_id 后调用 `request_stop`；POSIX 使用 poll，Windows 使用 listener 的有限等待；保持同步 accept 与线程池。终态由 main 在所有服务资源析构后发布。POSIX SDK 同批写入 JSON v1；Windows 只验证直接启动的 C++ 服务能消费协议文件，不实现 Windows SDK 后台启动或进程句柄生命周期。
- 验收：Linux 真实 C++ server 与 POSIX SDK 完成 start→ready→stop→stopped；Windows 原生 C++ server 接受已认证文件请求后，无 HTTP 新连接仍退出并发布匹配 stopped。macOS 和 Windows SDK `start` 不计入本阶段通过；fake 协议测试不能替代已承诺平台的真实进程门禁。

**阶段评审状态**：本批代码与测试确认没有新增远程 shutdown HTTP 端点；坏 token 请求不会停止服务，诊断不含 secret 且进程内只输出一次；Linux 进程身份检查有真实生命周期覆盖。规格与质量双评审已通过 F4 本批实现范围。Windows 原生 directory junction live 用例已验证拒绝 reparse `control-dir`；`test_local_control.exe` 中基于文件 symbolic link 的子用例仍因系统权限 skip，因此 file-level reparse 分支未独立实测。Windows 当前用户 + SYSTEM DACL、基于已打开句柄的抗 TOCTOU 防护尚未实现；macOS 身份和 Windows SDK 启动门禁也未执行。以上不代表原设计阶段 4 已整体验收。

**F4 实现记录（2026-10-14）**：按 fix plan 的本批范围，Linux 与 Windows C++ 服务使用同一 JSON v1 控制文件，POSIX SDK 完成真实生命周期，Windows 原生服务通过直接启动和 authenticated control file 验证了无新 HTTP 连接的 idle stop；启动前提供坏 token 后服务仍运行，main 与 HTTP 两个检查路径共用进程级 atomic，stderr 只输出一次不含 secret 的诊断。WSL C++ 专项 3/3、`test_local_node.py` 28/28；`test_server_socket_live.py` 7 项通过、Windows idle-stop 和 directory-reparse 两项按平台跳过。Windows C++ `test_local_control` 1/1；原生 socket live 9 项中 8 项通过、慢读期限 1 项因无法形成可验证回压而跳过，覆盖无新连接 idle stop、启动前坏 token 一次告警和 directory-junction reparse 拒绝；Windows SDK `test_local_node.py` 9 项通过、19 项因 Linux `/proc` 生命周期门控跳过。Windows `LocalNodeManager.start` 保持显式拒绝，macOS `/proc` 身份与生命周期尚未验收；CMake 不在 Apple 上注册依赖 `/proc` 的 `test_local_control`，SDK 本机生命周期测试也仅在 Linux 执行。Windows C++ file-symbolic-link 子用例因当前账户缺少创建链接权限而跳过，directory junction live 用例已实际验证目录 reparse 拒绝，file-level reparse 分支仍未独立实测。`ReplaceFileW` 对已有目标以 flags=0 原子替换；`REPLACEFILE_WRITE_THROUGH` 在 [Microsoft ReplaceFileW 文档](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew) 中明确标记为不支持。Windows control-dir 仍依赖父目录继承的 ACL，未主动读取或强制当前用户 + SYSTEM DACL，也未实现基于已打开句柄的抗 TOCTOU 路径防护；共享可写路径下 reparse 检查不是无竞态保证。以上结果只说明 fix plan 定义的 Linux/Windows C++ 服务和 POSIX SDK 批次已实现，不代表原设计的整个阶段 4 已验收；DACL/句柄安全、file-level reparse、macOS 生命周期和 Windows SDK 启动仍是后续门禁。

**修复计划 F5 实施记录（2026-10-14）**：本批统一本机进程退出归并与 CLI 退出结果。外部进程死亡不会单独成为 graceful 证据；force 送达状态可恢复，stop 的实例结果先写 stdout、结构化诊断写 stderr。启动失败时，即使状态文件不可读，也通过本次 `start()` 持有的 Popen 回收自有子进程；确认回收后只清理本次 launch 的 token 与 stop 请求。

WSL `test_local_node` 51/51、使用真实服务的 `test_cli_commands` 22/22 通过；Windows 原生定向测试通过，但本地 `start` 明确仍不支持。规格和质量复审通过本批 Linux/WSL 范围。Windows/macOS SDK 生命周期未据此验收，原方案仍需完成其余阶段和跨平台门禁。

**修复计划 F6 实施记录（2026-10-14）**：CLI 在 `int()` 前拒绝超长整数词法输入，并保留 signed int64 精确上下界；JSON 解码中的 `ValueError`/`RecursionError` 映射为 `InputError`，`MemoryError` 等异常不被吞掉。结构嵌套最多 256 层，限制在引号外扫描，不受字符串内括号影响。health 与 CRUD 的 post-argparse 输入错误输出单个 JSON 到 stderr、退出码 2，且发生在网络/SDK 调用前。WSL command/parser 测试 37/37 通过；独立质量评审通过，边界覆盖建议已补齐。Windows 原生解释器当前不可用；F6 的 Windows 测试结果不扩展到本机 start 生命周期，原方案其他阶段仍未验收。

**修复计划 F7 实施记录（2026-10-14）**：本机 `start` 将配置摘要按原始字节流式计算，并把 binary、config、workdir 统一为绝对真实路径。仅当已有进程存活、OS 身份可信且四项配置完全一致时返回 `already_running=true`；配置不匹配/旧状态无摘要返回 6，身份不明返回 8。spawn 前与 ready 后复核配置变化，变化后安全请求停止；进程按时退出返回 6，10 秒仍存活返回 7 并保留 stopping。真实 Linux server 和 CLI 覆盖同配置重复启动与配置路径变更，mock 覆盖内容、workdir、binary、身份和启动期间变更；WSL 的本机生命周期与 CLI/parser 组合测试 95/95 通过，独立质量评审通过。Windows SDK start 仍不支持，macOS 生命周期未验收；原方案尚有其他阶段与平台门槛。

### 阶段 5：打包、文档和兼容性

**目标**：让用户可以稳定执行 `cpl` 或 `python -m cpppools`。

**TDD 与验收**：

- RED：在干净 Python 环境验证入口不存在或版本错误时给出明确消息。
- GREEN：根 pyproject.toml 声明包目录 sdk/python、包名 cpppools、requires-python >=3.8，console script 为 `cpl = cpppools.cli:main`；-m 调用同一 main。Python 包版本只在 pyproject 元数据定义，安装版通过 importlib.metadata 获取，源码入口 0+dev，不借用 C++ 版本。不引入运行时依赖；PEP 517 构建可在较新 Python 执行，记录并冻结实际构建工具版本。
- GREEN：Bash/zsh/PowerShell 补全从 parser 契约导出命令、别名及静态选项；作为 wheel 包数据和 sdist 附件发布，在 cpppools/completions 下安装。提供资源定位与 shell 加载示例，不增加动态补全服务。
- 验收：分别构建 wheel、sdist，并从 sdist 重建 wheel；各在干净 venv 安装，清除 PYTHONPATH、切到源码树外，核验包加载位置、cpl/-m/version 一致、离线 help、真实 CRUD 和补全资源。既有 SDK 测试会主动插入源码路径，不能拿它们代替包验收。
- 验收：Python 3.8 至少有单元测试和安装包 smoke 的实际证据；三平台用明确记录的支持版本做真实生命周期验收，不要求全版本×全平台组合。pidfd 等可选 API 先检测能力，3.8/macOS 的后备路径同样测试。阶段 4 真 Windows 停机未过不得发布 v1。

**阶段评审**：检查 SDK 文档与 CLI 契约一致，版本号来源明确，发布包不携带本机状态或数据库文件。

## 7. 测试矩阵与验收清单

### 7.1 单元测试

放在 `sdk/python/tests`，覆盖：

- parser 正式命令/别名、长短选项、`allow_abbrev=False`、全局选项位置；
- JSON literal/file/stdin、严格类型、重复键、大小限制；
- 输出格式、stderr/stdout 分离、退出码映射；
- CRUD payload 适配、datasource/seed 透传和异常映射；
- 状态 schema、原子写入、锁、PID/启动时间核验；
- control request token、launch_id 和状态迁移。

### 7.2 集成测试

- 启动真实 SQLite CppPools server，验证 `cpl start -F` 与后台 start；
- 通过 `cpl h` 检查本机和远端节点；
- 通过 `cpl add/get/set/del` 完成真实 CRUD 往返；
- 多数据源显式 `-d`、歧义表 409、无匹配 0 退出码；
- 端口占用、配置错误、服务崩溃、旧状态和 PID 复用；
- stop 正常排空、stop 超时、force 停止和重复 stop。

### 7.3 跨平台验收

| 平台 | 必测项 |
|---|---|
| Linux | signal fallback、控制目录、前后台、相对 SQLite 路径 |
| macOS | 与 Linux 相同，验证 socket/进程时间字段差异 |
| Windows | 控制通道 graceful stop、强杀显式标记、PowerShell JSON 文件输入、ACL、多配置构建路径 |

另设 Python 3.8 兼容性验收（单测与安装包 smoke），以及源码树外的 wheel/sdist 包验收；平台生命周期测试和版本兼容测试分别留证，不用一个环境结果代表全部组合。

所有真实进程测试都必须记录构建目录、Python 版本、退出码和跳过原因；不能用 fake 单测代替 Windows 后台停止验收。

### 7.4 发布验收标准

只有同时满足以下条件才允许标记 v1 可用：

1. `cpl --help`、`cpl --version` 在无服务、无网络时成功。
2. 四个 CRUD 命令与 SDK 的请求和重试语义一致。
3. CLI 可以操作远端 seed，不要求本机启动节点。
4. 本地 start/stop/status 能识别自己的实例，不能误停其他进程。
5. POSIX 和 Windows 的优雅停止行为都有真实进程证据；强杀与 graceful 的输出和退出码不同。
6. 现有 C++ CTest 与 SDK 单测保持通过；新增 CLI 测试通过。
7. 文档、帮助输出和示例中的短选项、退出码、64 KiB 限制和 CRUD 边界一致。
8. wheel、sdist 重建安装及源码树外 cpl/-m 验证通过，补全资源可取得；Python 3.8 有实跑证据。

### 7.5 实施时的执行入口与证据

复用 unittest 和 CTest。每个目标模块必须存在、发现的用例数必须大于 0，再核对退出码和必测用例无 skip；零测试的 OK 不算 GREEN。以下命令对应当前已有测试模块。python 表示已核验的真实解释器，可替换为其绝对路径；本次仅同步文档，不把历史结果记作新运行。

```bash
# 单配置生成器
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
PYTHONPATH=sdk/python python -m unittest discover -s sdk/python/tests
PYTHONPATH=sdk/python python -m unittest discover -s sdk/python/tests -p test_cli_parser.py
PYTHONPATH=sdk/python python -m unittest discover -s sdk/python/tests -p test_cli_commands.py
PYTHONPATH=sdk/python python -m unittest discover -s sdk/python/tests -p test_local_node.py
```

Windows Visual Studio 等多配置生成器使用匹配配置，不能漏掉 --config/-C：

```text
cmake -S . -B build
cmake --build build --config RelWithDebInfo --parallel
ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

当前真实 CLI CRUD 与 Linux 本机生命周期用例分别位于 `test_cli_commands.py` 和 `test_local_node.py`，使用 `CPL_RUN_E2E=1` 启用、`CPL_TEST_SERVER` 指定构建产物的绝对路径；原规划的 `test_cli_e2e.py` 尚不存在。Linux/WSL 当前组合回归入口为：

```bash
PYTHONPATH=sdk/python CPL_RUN_E2E=1 \
  CPL_TEST_SERVER="$PWD/build-accept-s0-s2/bin/cpppools_server" \
  python3 -W error -m unittest discover -s sdk/python/tests
```

Windows C++ 服务 socket/control live 用例位于 `test_server_socket_live.py`，可通过已构建的原生服务执行下例；它不代替尚未支持的 Windows CLI start 验收。开发环境可明确跳过；发布 gate 开启 e2e 后缺少必测二进制/平台能力必须失败，不能静默 skip。

```powershell
$env:PYTHONPATH = (Resolve-Path 'sdk/python').Path
$env:CPL_RUN_E2E = '1'
$env:CPL_TEST_SERVER = (Resolve-Path 'build/bin/RelWithDebInfo/cpppools_server.exe').Path
python -m unittest discover -s sdk/python/tests -p test_server_socket_live.py
```

上例为多配置 Windows；单配置 Windows 使用 build/bin/cpppools_server.exe，POSIX 单配置为 build/bin/cpppools_server。fixture 在独立临时目录建立表、配置、状态根/日志和端口，不用示例数据库或已有实例。多节点共享实际数据库文件和拓扑/schema，分别验证稳态直达 owner、拓扑滞后 307，不能用一次成功代替重定向证据。

fixture 的 finally 清理只终止自己已核验的子进程，确认退出后删除自己的临时目录。Linux/macOS/Windows 每个平台保存测试计数、跳过项、exit code、binary/Python 版本和终态摘要。慢请求验证 5 秒 drain 后进程未退出时 stop 仍不得成功；无新连接的 Windows stop、前台另一个 CLI stop、Ctrl+C、force 都必须有真进程证据。

包验收使用独立 test_cli_packaging.py，从临时 venv 的 site-packages 加载安装包，不采用既有测试的 sys.path 注入。核验 cpppools.__file__ 位于该环境、console script 使用该解释器、wheel 内没有测试/状态/数据库。构建工具和 pip 可属测试环境依赖，不计入运行时依赖。

补全文件只维护 sdk/python/cpppools/completions 中一份，包含可被 Python 3.8 资源 API 访问的包标记。安装后用 importlib.resources 定位 cpppools.completions；加载方式为 Bash 的 `source <path>/cpl.bash`、zsh 将目录加入 fpath 后执行 compinit、PowerShell 的 `. <path>/cpl.ps1`。README 提供可复制的定位/加载命令，各 shell 独立 smoke，环境限制记录在结果中。

## 8. 逐阶段评审、修订与优化记录

本节是方案落地前的强制评审记录，不是实现完成声明。

### 评审一：范围与职责

**发现**：若把 CLI 设计成只操作本机，会遗漏“命令行 SDK 可以读取其他节点”的核心需求；若把 CRUD 放进本地进程管理器，会重复 SDK 路由。

**修订**：明确 `-u/--url` 可指向任意 seed；CRUD 全部进入 `ClusterClient`；只有 start/stop/status 使用 `LocalNodeManager`；普通 CRUD 永不隐式启动服务。

### 评审二：协议与数据语义

**发现**：旧后端 API 文档记载的 1 MB body 与源码 64 KiB 常量不一致；CLI 若自行把 where 值转字符串会破坏整数/文本键语义。

**修订**：本方案以 64 KiB 实现为准；JSON 输入保留原生类型；CLI 只做结构和大小预检，最终类型/唯一键校验仍由服务端负责。

### 评审三：健康检查与 SDK 初始化

**发现**：用 `ClusterClient` 实现 health 会在服务尚未启动时先失败于拓扑拉取；`/health` 也不能代表数据库深度健康。

**修订**：health 使用独立 GET；输出“HTTP 存活”而非“数据库健康”；status 将进程身份与 HTTP 探针分栏展示。

### 评审四：生命周期与进程安全

**发现**：仅凭 PID 或端口停止有 PID 复用/旧服务误判风险；仅等待服务端 5 秒 drain 会错误宣称整体退出有硬上限。

**修订**：状态含 launch_id、PID、进程创建时间、可执行文件和配置路径；实例锁和原子状态文件成为强制条件；stop timeout 超时保留 stopping；force 单独返回强杀结果。

### 评审五：Windows 落地性

**发现**：Windows `Popen.terminate()` 不触发 C++ 信号处理；此外必须确认现有 Windows accept 是否已有有限等待，避免基于错误现状设计新机制。

**修订**：增加本机 control/token/ready/stopped 协议。当时复核发现 Windows listener 仍阻塞 accept，之前“已有有限等待”的结论不成立；因此将 listener 的 200 ms select 等待和无新连接停止列为阶段 4 门槛。F4 已完成该等待并有 Windows 原生 idle-stop live 证据；DACL/句柄、macOS 与 Windows SDK 启动门槛仍未验收。

### 评审六：可实现性与范围优化

**发现**：一次性 CLI 若同时新增 shell、stats、schema、日志跟随和所有补全，会把首版变成多个独立项目；复杂第三方 CLI 框架也不符合现有零依赖 SDK。

**修订**：v1 只实现 8 个核心命令、标准库 argparse、JSON 文件/stdin 输入、状态管理和跨平台停止；`restart`、`logs`、交互 shell、stats/schema 命令列为后续增量，不进入本方案的首版验收。

### 评审七：阶段依赖与退出契约

**发现**：原阶段 3 只写 ready，却要求 stop 读 stopped 才成功；CRUD 将 -t 写成总 deadline 与现有 SDK 不符；前台运行若一直持锁，另一 CLI 无法 stop；后台 POSIX 也无法保证获取非子进程退出码。

**修订**：完整终态前移到阶段 3 并在 main 资源析构后发布；网络 -t 明确对应逐次 timeout；锁外等待、更新时比较 launch_id；未知退出码用 null；补齐 start/stop 输出、错误码 10/130，以及强杀的执行时点与退出证据。

### 评审八：超时、参数与真实验收

**发现**：启动超时可能留下仍活进程，直接标 crashed 或删除控制目录会失去管理能力；PowerShell 的未引用 @file 示例存在解析歧义；多平台“测试通过”不能只依赖 fake。

**修订**：启动超时请求优雅停止并有限等待，仍活则返回 7、保留 stopping/PID/控制信息；限制实例名，修正 '@file' 示例；请求体按完整 SDK payload 计算；补齐健康探针混合错误和每阶段真实验收入口。

### 第一轮逐阶段复验记录（2026-10-07，已由 R2 补充）

第一轮按当时的检查范围形成下表。第二轮进一步发现同代并发写入、崩溃恢复身份与重复信号等缺口，已补充修订；本表不作为当前版本“无问题”的证明。两轮均属于设计审查，不代表实现或功能测试已完成。

| 阶段 | 复验重点 | 设计结论 |
|---|---|---|
| 0 | 命令覆盖、长短参数、作用域、错误码 | 通过；以第 4/5 节冻结契约 |
| 1 | 严格 JSON、无 SDK health、stdout/stderr | 通过；明确 64 KiB 和多 URL 聚合 |
| 2 | SDK payload、逐次 timeout、非幂等重试 | 通过；CLI 不新增路由或重发策略 |
| 3 | ready/终态前置、锁范围、PID/退出码、超时保留 | 通过；完整资源收尾后才允许优雅成功 |
| 4 | Windows listener 唤醒、认证请求、ACL/句柄 | F4 fix-plan 本批实现与双评审通过，Windows directory-junction reparse 拒绝已有 live 证据；file-level reparse、原设计 DACL/句柄安全、macOS 生命周期与 Windows SDK 启动仍未验收 |
| 5 | 包目录/入口、版本来源、补全、发布证据 | 通过；须完成第 7.4 节后才发布 v1 |

方案交付自查覆盖了用户要求的架构、命令契约、分阶段实现、TDD、测试、验收和评审完善。本次仅编写这份 Markdown；所有实现项与功能测试证据留待相应实施阶段。

### 第二轮分项评审与优化（R2）

本轮采用三个独立只读视角：生命周期一致性、命令与 SDK 契约、分阶段交付。P1 表示按原文实施会导致错误控制或状态错误，必须先修；P2 为契约/验收缺口；P3 为工程复杂度或交付体验优化。下表记录已采纳修订及其实施验收，不把文档修订冒充功能测试通过。

| 编号 | 级别 | 问题与修订 | 对应实施证据 |
|---|---|---|---|
| R2-01 | P1 | 同代迟到 start 覆盖 stop；增加 revision/expected_state 和终态优先提交 | 阶段 3 同代 start/stop/force 的交错执行测试 |
| R2-02 | P1 | spawn 后崩溃恢复仅靠 PID；ready 带原 OS 创建身份，Linux 含 boot_id | 阶段 3 同 binary 的 PID 复用仍被拒绝 |
| R2-03 | P1 | 第二信号强退及初始化/析构窗口；starting 确认桥已安装，托管信号覆盖全程并幂等 | 阶段 3 握手前、ready 前、析构期、并发/续停；阶段 4 后备 |
| R2-04 | P2 | 终态恢复、force 超时证据、临时 unmanaged 不明；固定归并优先级与 pending/accepted/rejected | 阶段 3 崩溃窗口、权限恢复、退出后再 status |
| R2-05 | P2 | token 清理遗漏 stop.request；明确所有 secret 文件清理和写失败行为 | 阶段 4 终态/权限/I/O 失败与残留文件核查 |
| R2-06 | P2 | 成功响应坏 JSON 无错误码；返回 5/protocol_error且不重发 | 阶段 2 写已生效但响应畸形的单次执行证据 |
| R2-07 | P2 | 子 parser 缩写、status JSON 类型未冻结；逐 parser 配置与固定字段类型 | 阶段 0 误缩写；阶段 1/3 JSON 类型验收 |
| R2-08 | P2 | 阶段 0 依赖阶段 5 的安装入口；0–4 用源码 -m，5 验收 cpl | 各阶段用 sys.executable；包验收不带源码路径 |
| R2-09 | P2 | 源码测试代替包/3.8 验收；独立 wheel/sdist 安装、解释器兼容门槛 | 阶段 5 源码树外安装与 Python 3.8 smoke |
| R2-10 | P2 | Windows 多配置命令、零用例 OK 漏检；补 --config/-C 并核对模块和测试计数 | 各阶段测试摘要；实际 binary 配置路径 |
| R2-11 | P3 | 阶段 2 依赖未实现 start；用 Popen fixture，后续验收 CLI 自启动 | 阶段 2/3 的夹具职责分别验证 |
| R2-12 | P3 | 补全无法随包取得、阶段 0 空壳文件；资源只维护一份，模块按行为新增 | 包内容、shell smoke 和每文件实际调用方 |

优化保留原定的 8 个命令与 SDK 复用边界。状态 revision 是本地文件提交规则；配置摘要仅用 Python hashlib，不给 C++ 加密码学依赖；跨平台控制仍是服务端私有组件，不发展成通用服务管理框架。

**R2 最终复验结论**：三个分项评审均已复核修订文本，原 P1/P2 已闭环，未发现阻碍进入实施的未关闭项；P3 优化已纳入计划。该结论仅针对方案。进入实施时仍须按第 6/7 节取得 RED/GREEN、真进程及安装包证据。

## 9. 推荐落地顺序

按以下顺序实施，每阶段完成评审后再进入下一阶段：

1. 阶段 0：冻结命令和错误契约。
2. 阶段 1：输入/输出与远程 health。
3. 阶段 2：SDK CRUD 适配。
4. 阶段 3：本地 start/status 和 POSIX stop。
5. 阶段 4：Windows/跨平台控制通道。
6. 阶段 5：打包、补全、文档和发布验收。

阶段 0–2 和 Linux 已实现的阶段 3 路径可用于远程 CLI 与 Linux 本地开发；macOS 生命周期不能从 Linux 结果外推。发布 v1 前仍须满足第 7.4 节的全部跨平台验收条件。

## 10. 当前实现落点与待交付文件

下表按当前源码标明职责落点和未交付范围；JSON 输入与输出已在 cli.py 内实现，无需为原规划额外创建空壳模块。

| 文件 | 职责 |
|---|---|
| `sdk/python/cpppools/__main__.py` | `python -m cpppools` 入口 |
| `sdk/python/cpppools/cli.py` | 已实现 parser、命令分发、严格 JSON 输入、输出和退出码 |
| `sdk/python/cpppools/local_node.py` | 本地进程、状态文件、锁、控制请求 |
| `src/main.cpp` | control-dir 参数、完整资源收尾后的 stopped 终态 |
| `src/server/local_control.h/.cpp` | 私有控制文件协议、权限与原子读写；加入 CMake 构建 |
| `src/server/http_server.cpp`、`include/cpppools/server/http_server.h` | 可选控制配置、ready、停止检查、Windows listener 有限等待 |
| `sdk/python/tests/test_cli_parser.py` | parser/帮助/别名测试 |
| `sdk/python/tests/test_cli_commands.py` | health/CRUD 适配、真实 HTTP CRUD、输出与退出码测试 |
| `sdk/python/tests/test_local_node.py` | 状态、身份、锁、确定性交错与真实 Linux 生命周期测试 |
| `sdk/python/tests/test_server_socket_live.py` | 当前原生服务 socket/control live 入口 |
| `sdk/python/tests/test_foreground_live.py` | Linux 真实 PTY Ctrl+C、慢收尾与就绪通知失败边界 |
| `sdk/python/tests/test_cli_packaging.py` | 基础安装及可选独立 wheel/sdist 重建、双解释器六项源码树外安装/真实 CRUD 矩阵 |
| `sdk/python/tests/test_cli_completions.py` | parser 资源完整性、实际 Bash/PowerShell 候选项与光标范围 |
| `test/test_local_control.cpp`、`CMakeLists.txt` | 本机控制协议/边界 CTest 注册及现有 C++ 回归 |
| `pyproject.toml` | `cpl` console script 和包元数据 |
| `sdk/python/cpppools/completions/` | 已交付 Bash/zsh/PowerShell 静态资源，wheel/sdist 均包含 |
| `docs/api/sdk.md` | 已有 SDK 使用说明；CLI 用户用法说明仍待补齐 |

上表不要求另建 `test_cli_e2e.py`：现有模块已承载 live 测试，沿用其入口即可。新增文件和平台能力只按实际交付更新状态。

## 11. 本方案的事实依据

本次以当前工作区源码为准；旧说明与实现有差异时已在本方案说明，实施时同步相关文档。

- [SDK 客户端](../sdk/python/cpppools/client.py)：CRUD 签名、初始化、路由和重试。
- [传输层](../sdk/python/cpppools/transport.py)与[异常分类](../sdk/python/cpppools/errors.py)：JSON 序列化、逐次 timeout、HTTP 错误映射。
- [服务入口](../src/main.cpp)：配置启动、信号桥接、资源生命周期。
- [HTTP 服务实现](../src/server/http_server.cpp)与[HTTP 服务声明](../include/cpppools/server/http_server.h)：监听循环、health、64 KiB 常量及 drain。
- [SDK 文档](api/sdk.md)、[后端 API 文档](api/backend_api.md)与[项目约束](../AGENT.md)：现有契约、单行 CRUD 与模块边界。
