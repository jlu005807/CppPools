# S0–S2 已实现阶段审查与验收

> 日期：2026-10-14；代码 HEAD：`a8acc5b09c99f7ec617b0b996747efc7d46e2681`。
> 范围：S0 五项修复、S1 远程 CLI、S2 已实现的本机管理与基础打包。S3 仅保留进度记录，S4–S7 未进入本次验收。
> 方法：源码与方案逐项比对、独立代码审查、构建与回归、隔离真实服务及边界复现。第 1–3 节保留修复前发现；第 6 节记录 F1–F8 复验；最新整体复审结论见第 7 节。
> 当前进度：A03–A08 与 R01–R03 在已测 Linux/WSL 范围修复复验通过；A01/A02 缺少真实环境证据，S2 平台/前台输出/发行门槛仍开放。完整阶段状态见 [CLI §6.1](cpl-cli-design.md#61-当前实施进度2026-10-14)。

## 1. 修复前验收基线

正常操作的回归结果成立，但阶段整体完成状态需要收窄。S0 有一个 MySQL 兼容回归；S1 正常 CLI 流程通过，输入错误契约仍有缺口；S2 Linux 顺序启停和安装入口通过，并发状态、停机证据和 Windows 服务收包存在实际问题。

| 阶段/子项 | 本轮结论 | 通过范围与剩余门槛 |
|---|---|---|
| S0.1 MySQL/MariaDB 构建 | 构建及默认模式专项通过；存在回归 | 两种客户端均编译成功，Windows 本机 MySQL 自省/CRUD 通过；须修复 A01 的 SQL mode 回归 |
| S0.2 Singleflight | 通过 | generation/flight_id 身份核对、同分片其他 key 失效、旧 flight 与 replacement 的现有确定性测试通过 |
| S0.3 bypass 异常复位 | 通过 | 四种 CRUD 使用作用域 guard；异常回归和连接归还相关测试通过 |
| S0.4 Prometheus 声明 | 通过 | HELP/TYPE 只声明一次，缓存指标相关测试通过 |
| S0.5 HTTP 发送总期限 | Linux 通过；Windows 同语义专项未验收 | 现有慢读/非读端和过载 header/body 共享 deadline 测试通过；Windows HTTP 收包另外发现 S2 引入的 A02 |
| S1 CLI 阶段 0–2 | 正常流程通过；错误输入待修正 | parser、health、CRUD、SDK 重试边界通过；A06/A07 未符合冻结契约 |
| S2 CLI 阶段 3–5 | 部分通过，不能标记整体完成 | Linux 顺序启停、控制 token 和源码树外安装通过；A02–A05/A08 及跨平台、发布门槛尚未关闭 |

本表和下文问题描述是 F1–F7 实施前的验收基线。修复后的当前状态以第 6–7 节为准，原始问题复现继续作为 RED 证据保留，不据此重开已关闭问题。

P1 表示影响有效部署、请求或停机正确性，应在继续交付前处理；P2 表示稳定错误处理或操作契约的缺口。不将代码风格、历史警告或未承诺的新功能列为修复问题。

## 2. 本轮实际运行证据

环境：WSL Ubuntu、GCC 13.3.0、Python 3.12.3；Windows MinGW、Anaconda Python 3.12.3、本机 MySQL Server 8.0.11。Windows Python 位于 `D:\Anaconda\python.exe`，尽管 `python` 命令是别名、`py -0p` 没有登记解释器，仍存在可用解释器。

| 验证 | 实际结果 | 说明 |
|---|---|---|
| 新建 Debug 构建，MySQL OFF、Redis OFF | 配置、全量编译退出 0 | `build-accept-s0-s2`，来自当前工作区源码 |
| OFF CTest | 18/18 通过，退出 0，34.07s | 含慢速 HTTP、Singleflight、bypass、metrics |
| MariaDB ON 重新构建 | 编译、链接退出 0 | 使用现有隔离客户端头/库；有时间戳偏移告警，实际测试随后执行 |
| MariaDB ON 非 MySQL 集成 CTest | 18/18 通过，退出 0，34.12s | 明确排除两个真实连接用例，不把无账号 SKIP 计为通过 |
| Windows MySQL 专项重新构建与运行 | 自省、CRUD 两项均 `[ok]`，退出 0 | 查询未发现本次测试前缀的数据库残留；密码仅用于进程环境，不写入报告 |
| Python 默认回归 | 共 100 项：93 通过、7 跳过，退出 0 | 跳过项由真实服务环境变量控制；不是“100 通过加 7 跳过” |
| 启用真实服务的 Python 全量回归 | 100/100 通过、0 跳过，退出 0，10.916s | `python3 -W error`；含真实 health、四个 CRUD、start/status/stop、错误 token、force 清理、包安装 smoke |
| Windows 原生服务隔离网络探测 | 即时 health 200；两次延迟发送均断开 | 确认 A02，属于失败证据，不计为验收通过 |
| 独立边界探测 | 复现 A01、A03–A07 | 与现有回归分列；边界探测脚本正常退出只表示成功观察到问题 |

主要命令从仓库根执行：

```bash
cmake -S . -B build-accept-s0-s2 -DCMAKE_BUILD_TYPE=Debug \
  -DCPPPOOLS_USE_MYSQL=OFF -DCPPPOOLS_USE_REDIS=OFF
cmake --build build-accept-s0-s2 -j2
ctest --test-dir build-accept-s0-s2 --output-on-failure
cmake --build build-s0-mysql-wsl -j2
ctest --test-dir build-s0-mysql-wsl --output-on-failure -E test_mysql
PYTHONPATH=sdk/python CPL_RUN_E2E=1 \
  CPL_TEST_SERVER="$PWD/build-accept-s0-s2/bin/cpppools_server" \
  python3 -W error -m unittest discover -s sdk/python/tests -v
```

Windows 使用 `cmake --build build-s0-mysql-win2 --target test_mysql_introspection test_mysql_crud cpppools_server -j2`，配置测试连接环境后直接运行两个测试二进制。HTTP 探测使用测试自行创建的 SQLite 库、动态端口和服务进程；复现后关闭该进程并清理临时目录。MySQL 模式探测只修改自身连接的 SESSION，不更改本机服务的 GLOBAL 配置。

## 3. 需要处理的实际问题

### A01 · P1 · MySQL 转义调用不支持原先支持的 SQL mode

位置：[mysql_database.cpp](../src/db/mysql_database.cpp)，第 157、232 行；引入提交 `b4e893f`。

为兼容 MariaDB，原 `mysql_real_escape_string_quote` 被替换为 `mysql_real_escape_string`。Oracle MySQL 在 `NO_BACKSLASH_ESCAPES` 下使后者返回错误；当前源码将返回值直接用作 `std::string` 长度，未检查失败。

本机 MySQL 8.0.11 的独立连接实测，仅设置 SESSION 模式、转义普通表名 `users`：

```text
mysql_real_escape_string_return=4294967295 mysql_errno=2062
previous_quote_api_return=5
```

这证明当前自省代码会使用错误长度；实际数据源初始化后果由源码调用链推导，本轮没有改变全局模式来启动完整数据源。默认模式的真实 CRUD 通过不能覆盖此分支。[MySQL 官方 C API 说明](https://dev.mysql.com/doc/c-api/8.4/en/mysql-real-escape-string.html)明确该模式下失败及 unsigned -1 返回语义。

修复门槛：保留 Oracle 客户端的正确转义能力，同时提供 MariaDB 兼容路径或参数绑定，检查失败返回；分别验证默认模式与 `NO_BACKSLASH_ESCAPES` 的自省/CRUD。

### A02 · P1 · S2 的非阻塞监听使 Windows 服务提前关闭合法请求

位置：[http_server.cpp](../src/server/http_server.cpp)，第 488、541、690–708、764 行；引入提交 `2172177`。

Windows listener 被设为非阻塞，accept 出来的 socket 继承该属性。收包代码只设置 `SO_RCVTIMEO`，没有恢复阻塞或等待可读；`WSAEWOULDBLOCK` 被当作读取结束。客户端连上后稍晚发送、或请求分段到达时，会提前断开或解析不完整请求。

当前 Windows 原生服务实测：

```text
WINDOWS_IMMEDIATE_HEALTH=200
WINDOWS_DELAYED_HEALTH 0 ConnectionAbortedError winerror=10053
WINDOWS_DELAYED_HEALTH 1 ConnectionAbortedError winerror=10053
```

延迟仅 250ms，远小于已有接收超时。此问题影响直接启动的 Windows 服务，不能归为“Windows 本机 CLI 尚未实施”。[Microsoft accept 文档](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-accept)说明属性继承；[recv 文档](https://learn.microsoft.com/en-us/windows/win32/api/winsock/nf-winsock-recv)说明非阻塞无数据返回。

修复门槛：正确处理 accepted socket 的接收模式；Windows 真实 delayed-header、分片 body、慢读响应通过，保持 S0 响应发送总 deadline。

### A03 · P1 · 本机状态提交未核对同代 revision

位置：[local_node.py](../sdk/python/cpppools/local_node.py)，第 281、333–335、350–399 行；方案 §3 已要求 `(launch_id, state_revision, expected_state)` 核对。

spawn 前释放锁时状态 PID 仍为 null，另一 start 可重复启动；锁外等待后的提交也不重新读取记录，迟到 health 可覆盖 stop 已写入的 stopping，旧代操作可覆盖新代记录。文件写入是原子的，但操作间的状态归并不具备条件核对。

本轮使用 mock 在 `_health` 返回前写入同代 stopping，随后 start 仍写回 running，得到 `concurrent_stop_state_after_start=running`。这是确定性交错复现，不是靠调度碰撞的概率测试。

修复门槛：spawn 登记和回锁提交遵守既定锁/代/revision 规则；两次 start、迟到 ready 对 stop、旧代等待对新代启动、force 对 graceful 交错均保持状态单调。

### A04 · P1 · stop 将异常退出当作优雅成功

位置：[local_node.py](../sdk/python/cpppools/local_node.py)，第 389–399 行；[cli.py](../sdk/python/cpppools/cli.py)，第 508 行。

普通 stop 只要观察到 PID 不再存活，就写 stopped/graceful，未核验 matching stopped 终态。对本轮测试自行启动的服务，使用错误 token 使请求不被接受，再在等待中强杀该测试进程，实际得到：

```text
REAL_UNEXPECTED_EXIT {"state":"stopped","exit_kind":"graceful","last_exit_code":137}
stopped_marker=False
```

CLI 因此返回成功，而服务没有正常收尾。显式 force 确认退出也返回 0，方案要求返回 9并保留 force_delivery；当前没有该恢复证据。

修复门槛：按同代终态、退出证据和 force_delivery 归并；异常退出不能输出 graceful；强制退出返回契约正确，重复 status/stop 可恢复一致结论。

### A05 · P1 · 托管信号与终态不覆盖完整收尾

位置：[main.cpp](../src/main.cpp)，第 43–44、227、294–296 行。

信号桥在数据库初始化后安装；托管进程收到第二个普通 SIGTERM/SIGINT 仍直接 `_exit`。`stopped` 在 registry、线程池等局部资源析构之前发布，桥的析构也先于这些资源。与方案已有的“托管信号幂等、初始化至析构全程覆盖、完整收尾后终态”不一致。

真实 Linux 托管服务保持一个未发送完的请求，再依次发送两次 SIGTERM：退出码 143，未生成 stopped marker。这证明普通重复停止能跳过收尾；未要求新增数据库取消接口。

修复门槛：托管路径使用 pending-stop 与幂等停止语义，在安全检查点消费，完成全部资源析构后发布同代终态；初始化、排空、析构窗口及重复信号均复验。

### A06 · P2 · 严格 JSON 的部分解析异常逃逸

位置：[cli.py](../sdk/python/cpppools/cli.py)，第 200–202、223–237、546–548 行。

真实 subprocess 执行 `python -m cpppools insert users -D SOURCE -o json`，在访问 SDK 前出现：

| SOURCE | 字节数 | 实际结果 | 应有结果 |
|---|---|---|---|
| `{"id":` + 4301 位数字 + `}` | 4308 | 退出 1，ValueError traceback | 输入错误 2 |
| `{"id":` + 5000 层数组嵌套 + `0` + 闭合 | 10008 | 退出 1，RecursionError traceback | 输入错误 2 |

两者都小于 64KiB。修复只需归一化解析失败，不放宽 Python 的整数/递归保护。补齐边界回归，确保无 SDK 构造、无 traceback。

### A07 · P2 · JSON 模式的运行期输入错误仍为纯文本

位置：[cli.py](../sdk/python/cpppools/cli.py)，第 540、546 行；方案 §5 已冻结结构化错误对象。

`python -m cpppools insert users -D '{}' -o json` 返回 2，但 stderr 是纯文本，无法 `json.loads()`；health 参数校验走相同路径。此处已完成 argparse 解析，应为 input_error 对象；argparse 自身的 usage 文字可以保留。

修复门槛：运行期错误带 kind/message/exit_code/http_status，stdout 保持空，退出 2；同样覆盖 health 和 CRUD。

### A08 · P2 · 已运行实例不检查配置一致性

位置：[local_node.py](../sdk/python/cpppools/local_node.py)，第 260–262 行；方案 §4.4 要求比较 binary/config/workdir 和配置摘要。

身份核验通过后，start 对 starting/running/stopping 一律返回 already_running。修改同路径配置或要求另一配置文件时，也会返回成功而继续旧进程；stopping 不按约定拒绝。本项来自源码与方案逐项核对，本轮未增加真实重复启动用例。

修复门槛：仅 running 且启动身份及配置摘要匹配时幂等成功；starting/stopping 或配置变化返回既定错误码，不能重复 spawn。

## 4. 已知的平台与发布范围

- 本机管理当前仅核验 Linux `/proc` 身份；macOS 无此接口，不能把 Linux 测试写成所有 POSIX 平台通过。Windows start 仍显式拒绝执行。
- F4 已将 Linux/WSL 生命周期的 ready/stopped marker 改为带 launch、PID 和进程启动身份的结构化证据，并覆盖原子发布、写失败和重复信号收尾；Windows 服务/control 的已测路径也已通过。macOS 生命周期、Windows SDK 本机 start 及 Windows ACL/句柄级 TOCTOU 仍未验收。
- 基础 `pyproject` 和 `cpl` 入口安装通过；CLI 固定显示 `0+dev`，发行元数据为 `0.1.0`，完整发行版版本来源仍需对齐。静态补全资源未交付，独立 wheel/sdist 重建和 Python 3.8 smoke 未验收。
- 前台当前在附着结束后输出实例结果；原规划的就绪即时输出未实现。本轮 Ctrl+C 路径有确定性模拟测试，真实终端 Ctrl+C 专项尚未执行；这些原方案门槛与 R01 修复关闭分别记录。
- Windows 打包测试把 Scripts 目录重复拼接为 `Scripts/Scripts/cpl.exe`；属于待补的平台测试矩阵，不能由 Linux 安装 smoke 推断 Windows 发布通过。
- 当前构建存在已知编译告警；未运行 sanitizer、性能压力或长期稳定性测试，未给出这些范围的通过承诺。

## 5. 原计划与当前剩余关闭顺序

针对 A01–A08 的原始任务、依赖、修改文件、回归命令和关闭条件见 [验收问题修复实施计划](completed-stages-fix-plan-2026-10-14.md)；F1–F8 的完成与未完成证据汇总在第 6 节。平台与发行残项单独保留。

F1–F8 原范围关闭 A03–A08 的证据保留；整体复审新增的 R01–R03 已按第 7 节修复并通过 Linux/WSL 回归。A01 的真实 MySQL SQL mode 流程和 A02 的 Windows 慢读发送期限继续开放。S2 的 macOS/Windows 本机管理与发行矩阵另按 [修复计划](completed-stages-fix-plan-2026-10-14.md) 验收。

正常功能已有通过证据应保留；S0/S1/S2 的阶段状态仍需按实际平台和发行承诺收窄，不把 Linux/WSL 通过外推为跨平台全部完成。实施顺序与当前进度见 [统一路线](implementation-roadmap.md)。

## 6. 修复后复验（2026-10-14）

本节保留 F1–F8 原范围的实现验收结果和历史通过证据；最新整体复审补充见第 7 节，不能依据本节将生命周期整体标记为通过。工作在当前未提交工作区完成，未创建提交；下列结论只覆盖实际运行的平台与环境。

| 问题 | 修复后结论 | 证据与边界 |
|---|---|---|
| A01 MySQL SQL mode | **未关闭** | F1 的兼容实现和 MariaDB/Oracle MySQL 客户端构建通过；MySQL ON 基础 CTest 通过。真实自省与 CRUD 专项均因未设置 `CPPPOOLS_TEST_MYSQL_USER` 而 skip，未验证默认/NBE SESSION 下生产自省和 CRUD 文本往返。 |
| A02 Windows HTTP | **未关闭** | Windows 原生 delayed-header、split-header/body 和空闲认证停止 live 用例通过；慢读 deadline 用例因当前 loopback/WSL 缓冲行为无法证明远端背压而 skip，不能据此验收期限。 |
| A03 状态条件提交 | **关闭（F1–F8 已测 Linux/WSL 范围）** | 双 start、stop/ready 交错、旧 launch 与新 launch 竞争、force 与 graceful 交错均有确定性测试；后续命令结果归属补充见第 7 节 R03。 |
| A04 退出证据归并 | **关闭（F1–F8 已测 Linux/WSL 范围）** | 外部强杀、accepted/pending force、超时最后检查点、状态恢复与 CLI 稳定退出码的定向和全量测试通过；后续旧 stop 结果语义见第 7 节 R03。 |
| A05 托管信号与完整终态 | **关闭（F1–F8 已测范围）** | Linux/WSL 重复 SIGTERM、初始化检查点停止、完整收尾后的 matching marker、marker 写失败通过；Windows 原生控制与空闲认证停止测试通过。前台中断补充见第 7 节 R01；macOS 生命周期和完整跨平台 SDK 管理仍未验收。 |
| A06 输入解析异常 | **关闭** | 超长整数、深 JSON、解码异常范围及 literal/file/stdin 来源的测试通过；输入错误统一为退出码 2，无 traceback 或 SDK/网络调用。 |
| A07 结构化输入错误 | **关闭** | health 与 CRUD 的 post-argparse 输入错误均验证 stderr 单个 JSON、stdout 为空、退出码 2；测试通过。 |
| A08 配置一致性启动 | **关闭（Linux/WSL 范围）** | 相同配置幂等；配置路径、原始内容、binary、workdir、starting/stopping 和未知身份场景拒绝且不重复 spawn；ready 后漂移会清理并留下可核验终态，超时保留 stopping。 |

Linux/WSL 组合构建与回归结果：

| 检查 | 结果 | 说明 |
|---|---|---|
| Debug，MySQL OFF、Redis OFF：配置、构建、全量 CTest | 20/20 通过 | `build-accept-s0-s2`；命令为 `cmake -S . -B build-accept-s0-s2 -DCMAKE_BUILD_TYPE=Debug -DCPPPOOLS_USE_MYSQL=OFF -DCPPPOOLS_USE_REDIS=OFF`、`cmake --build build-accept-s0-s2 -j2`、`ctest --test-dir build-accept-s0-s2 --output-on-failure`。 |
| MariaDB/MySQL ON 构建及非 MySQL CTest | 构建通过；20/20 通过 | `build-s0-mysql-wsl`；通过 `ctest --test-dir build-s0-mysql-wsl --output-on-failure -E test_mysql` 排除真实连接专项。 |
| MySQL introspection / CRUD CTest | 两项 skip | `CPPPOOLS_TEST_MYSQL_USER` 未设置；按验收门槛不算通过，且不推断 SQL mode 兼容。 |
| F8 Python 全量与真实服务 live 测试 | 167 项中 165 通过、2 项按平台 skip | `PYTHONPATH=sdk/python CPL_RUN_E2E=1 CPL_TEST_SERVER="$PWD/build-accept-s0-s2/bin/cpppools_server" python3 -W error -m unittest discover -s sdk/python/tests`；两个 skip 是 Windows 专属用例。F9–F11 后的 171/169/2 最新结果另记于第 7 节。 |
| 配置漂移清理与重复 SIGTERM 联测 | 全量套件 1 次通过；门闩修正后定向复跑 3/3 通过 | 在服务 ready 后保持一个未完成 HTTP 请求，改变配置字节，触发 startup cleanup，再发送两次 SIGTERM。验证进程不因第二次信号提前中止；最终 state 与 `stopped.json` 的 launch ID、PID、process start identity、退出类型和退出码一致，state revision 递增，控制 token 与 stop request 清理。独立复核指出 5 秒健康门闩与请求准备窗口过近，已延长为 20 秒并重复验证。 |

Windows 原生复验结果：

- MySQL ON 的 `cpppools_server`、`test_mysql_introspection`、`test_mysql_crud`、`test_local_control` 目标构建成功；`test_local_control` CTest 1/1 通过。两个 MySQL 真实连接专项因缺少 `CPPPOOLS_TEST_MYSQL_USER` 均 skip。
- 原生 `test_server_socket_live.py` 共 9 项，8 项通过、慢读 deadline 1 项因无法形成可验证远端背压而 skip。通过项覆盖延迟头、分片 header/body、空闲认证停止、control token 告警和 Windows 目录 reparse 拒绝。测试进程使用本次构建目录、MinGW 与 Python 环境的临时 PATH，未修改系统环境。
- 全量原生 CMake build 仍在既有 `test/test_http_server.cpp:18` 因包含 POSIX `<arpa/inet.h>` 失败；所需服务、MySQL 专项和 control 专项目标已独立构建。此构建阻断不改写为通过，也不影响上述已实际运行的 Windows 专项证据。

上述为 F1–F8 原范围关闭 A03–A08 的证据；F9–F11 已补齐生命周期衔接并通过本轮复验。A01、A02 保留开放，直到真实 SQL mode 服务和可验证 Windows 慢读背压环境可用。S2 整体仍未完成：macOS 生命周期、Windows 本机 start、Windows ACL/句柄边界、发行版本来源、Windows 安装 smoke、独立 wheel/sdist 与最低 Python 版本矩阵均保留在 [修复计划的平台与发行门槛](completed-stages-fix-plan-2026-10-14.md) 中。未运行 sanitizer、性能压力或长期稳定性测试，不对这些范围作通过承诺。

## 7. 整体复审补充（2026-10-14）

对当前 F1–F8 修复整体复审后，确认三个需要补齐的设计问题，已按 [整体评审与设计补充](completed-stages-overall-review-2026-10-14.md) 实施。既有状态保护、服务端信号及终态测试通过证据保留；下表记录新增边界的修复结果。

| 编号 | 问题与影响 | 结论 |
|---|---|---|
| R01 · P1 | 前台 Ctrl+C 原来会在 1 秒后自动 kill。 | 已修复：已就绪进程走 10 秒认证停止，graceful 时返回 130；启动前自有子进程回收保留原路径。 |
| R02 · P1 | stopping 已落盘但普通请求未送达，后续 stop 不补发。 | 已修复：同代普通 stop 幂等补发认证请求；accepted force 不重复投递。 |
| R03 · P2 | 并发 stop 取消 start 或新 launch 替换旧 stop 时，旧命令可能返回 0。 | 已修复：取消 start 返回 6；旧 stop 返回 10 并携带新代状态，不归并或改写新代。 |

定向重跑 `test_local_node` 与 `test_cli_commands` 共 92 项，79 项通过、13 项按平台/live 条件跳过；Python 全量 live 为 171 项中 169 项通过、2 项 Windows 专属用例跳过。未重跑 C++、MySQL 或 Windows live。R01–R03 在已测 Linux/WSL 范围关闭；A06/A07/A08 原结论保留，A01/A02 和 S2 平台/发行门槛继续开放。

独立代码复审已完成，未发现需要阻止本批验收的问题。以上为修复批次的测试结果，本次进度文档更新没有重跑代码测试。后续剩余工作按 [统一路线 §2.6](implementation-roadmap.md#26-修复完成后的剩余工作顺序) 推进；本报告不将平台 skip、未交付输出/补全或未完成发行矩阵记为通过。

## 8. 本机真实环境补验（2026-10-15）

基线 `50bc1ff`；Windows 原生 MinGW 14.2、Anaconda Python 3.12；Linux 对照为 WSL Ubuntu。以下结果更新前文的环境缺口，历史运行记录保留。

| 项目 | 实际结果与验收结论 |
|---|---|
| A01 真实 MySQL | 本机 MySQL 服务及 Oracle 客户端均为 8.0.11。ON 构建两专项目标成功，CTest 2/2 实际通过、无 skip；自省和 CRUD 均打印 SESSION `''` 与 `NO_BACKSLASH_ESCAPES`，原 factory 默认模式路径保留。特殊名称、唯一列和含引号/反斜杠/中文/emoji 文本往返断言通过。A01 关闭。 |
| 数据库隔离与清理 | 仅使用测试自己的 `cpppools_intro_test_*` / `cpppools_crud_test_*` 临时库；运行前后 `SHOW DATABASES LIKE 'cpppools%'` 均仅有原 `cpppools`。GLOBAL mode 前后均为 `STRICT_TRANS_TABLES,NO_ENGINE_SUBSTITUTION`，未修改全局设置。 |
| Windows HTTP 原生专项 | 修复测试专用 socket/timeout 类型与 POSIX 头依赖，SQLite 夹具改用系统临时目录。`test_http_server` 编译并实际通过，约 38 秒。1 MiB 真实 CRUD 响应、服务端 `SO_SNDBUF=0` 下，不读端约 10023 ms、慢读端约 10049 ms 结束；慢读累计 163840 字节，结束后 health 可用。 |
| A02 证据边界 | **仍开放。** 独立评审在隔离测试副本中把 deadline 改为每次成功发送后重置，错误实现仍约 10 秒通过。该 Winsock provider 在零发送缓冲时是一次 send 等到 socket timeout，没有连续成功的 partial send；AF_UNIX 探测也相同。因此本用例只证明本机有界退出，不能关闭 Windows 多次发送共享总期限的门槛。未为适配测试修改生产发送逻辑。 |
| Windows TCP/control live | `test_server_socket_live` 共 9 项，8 通过、外部慢读夹具 1 skip；延迟/分片收包、只插入一次、空闲认证停止和目录 reparse 均通过。外部夹具无法限制服务端发送队列，skip 原因明确指向原生专项及上述边界。 |
| Linux 回归 | `build-accept-s0-s2` 从当前源码完整构建成功，CTest 20/20 通过，约 44 秒；不读/慢读专项均实际执行。 |
| Windows 全量构建 | HTTP 编译阻断已解除；继续全量构建时在既有 `test_config.cpp` 的双参数 `mkdir` 和 `setenv/unsetenv` 失败。全量原生 CTest 仍不能计为通过。 |

实际命令（凭据只通过进程环境注入，不写入文档）：

```powershell
cmake --build build-s0-mysql-win2 --target test_mysql_introspection test_mysql_crud -j2
# CPPPOOLS_TEST_MYSQL_HOST/USER/PASSWORD 已在本次进程设置；PATH 包含客户端 DLL。
ctest --test-dir build-s0-mysql-win2 -V -R '^test_mysql_(introspection|crud)$'
cmake --build build-redis-verify-off2 --target test_http_server -j2
ctest --test-dir build-redis-verify-off2 -R '^test_http_server$' -V
# PYTHONPATH=sdk/python，CPL_TEST_SERVER=上述构建的 cpppools_server.exe
D:/Anaconda/python.exe -W error -m unittest sdk.python.tests.test_server_socket_live -v
```

```bash
cmake --build build-accept-s0-s2 -j2
ctest --test-dir build-accept-s0-s2 --output-on-failure
```

独立评审完成：测试可移植性和资源回收未发现额外阻断；要求保留 A02 的负对照限制，已改名并补注释。S2 平台/发行及 S3–S7 功能不因本节测试通过而自动验收。

## 9. S2 发行与兼容性补验（2026-10-15）

本批基于 `44a7d03`，交付 CLI 阶段 5 的版本来源、静态补全与独立构建/安装验证；保留平台生命周期发布门槛。

| 项目 | 修改与真实验证 |
|---|---|
| 版本与包范围 | 安装版读取 `importlib.metadata.version("cpppools")`，缺 metadata 回退 `0+dev`；RED 复现旧入口始终输出 `0+dev`。包发现限制为 `cpppools*` 的普通包；安装检查先复现误带 `tests/`、`examples/`，修正后两者不再进入 wheel。运行时仍零依赖。 |
| 干净发行矩阵 | 临时复制无 egg-info/缓存的源码，分别构建 wheel、sdist，再从 sdist 重建 wheel。每次使用唯一产物目录。Windows Python 3.12.3 / 3.8.10 × 原 wheel / 直接 sdist / 重建 wheel 共六项安装，均清除 PYTHONPATH、切到源码树外，核验 site-packages 加载位置、metadata/cpl/-m 版本、离线 help 和补全资源；每项真实 insert/select/update/select/delete 通过。矩阵测试退出 0，用时 144.710 秒。 |
| 补全资源 | wheel 和重建 wheel 均含 `cpl.bash`、`_cpl`、`cpl.ps1`；直接 sdist 安装也检查三类资源。实际 Bash 候选和 PowerShell 5.1 `CompleteInput` 通过；评审发现 PowerShell 忽略中间光标位置，新增 RED 后修正，两个中间光标回归通过。WSL 提取 Ubuntu zsh 5.9 包到忽略目录，`compinit` 注册和 `autoload +X _cpl` 加载实际成功；未把加载 smoke 表述为完整交互候选测试。README 提供三种 shell 的资源定位与加载命令。 |
| Python 3.8 源码兼容 | 真实 Python 3.8.10，`-W error` 全量 178 项：129 通过、49 按 Windows/POSIX/live 条件跳过，退出 0；没有设置 live 环境。修正仅测试夹具的 Windows 可执行文件后缀，避免 `shutil.which` 的 PATHEXT 差异。 |
| Windows 存活检查 | 全量 suite 两次收到非预期 KeyboardInterrupt，单独 socket suite 正常。定位 `_pid_alive` 在 Windows 使用 `os.kill(pid, 0)`，该平台的 0 是 CTRL_C_EVENT。改用 SYNCHRONIZE 进程句柄与零超时等待，不发送事件；真实当前/活子进程/退出子进程验证通过。评审补齐 DWORD PID 上界与未知 OpenProcess 错误的 RED→GREEN，未知错误不得当作进程已退出。独立评审连续 1000 次真实探测句柄数 163→163，等待各分支正确关闭句柄。 |
| Windows 组合回归 | 最终句柄修正后，Python 3.12.3 全量 live 配置 178 项：141 通过、37 跳过，退出 0，用时 35.073 秒；句柄定向 5/5 通过。Windows start/macOS 仍不可由这些结果宣称可用。 |
| Linux 组合回归 | WSL Python 3.12.3、真实 server、`CPL_RUN_E2E=1`，`-W error` 全量 178 项：172 通过、6 跳过，退出 0，用时 47.823 秒；包括既有生命周期和 Bash 实际候选项。 |

构建器：Python 3.12.3，`build 1.3.0`、`setuptools 80.9.0`、`wheel 0.45.1`；安装环境冻结 `pip 24.3.1`、`setuptools 68.2.2`、`wheel 0.45.1`，通过本地 wheelhouse 离线获取，避免 Python 3.8 随附旧 pip 的 TLS/proxy 故障。Python 3.8.10 使用官方 NuGet runtime，未修改系统 Python 注册或 PATH；下载包 SHA256 为 `C63A2FC8FC62612B5ABD391FDA99AE1D90BAD42ED8A9E99B1BD81C7FFDB4FEFA`。

实际入口（环境变量路径均指向本机实存资源）：

```powershell
# PYTHONPATH=sdk/python
# CPL_BUILD_PYTHON=output/release-build-env/Scripts/python.exe 的绝对路径
# CPL_TEST_PYTHON38=官方 NuGet 3.8.10 tools/python.exe 的绝对路径
# CPL_TEST_SERVER=build-redis-verify-off2/bin/cpppools_server.exe 的绝对路径
# CPL_PACKAGING_WHEELHOUSE=output/release-wheelhouse 的绝对路径
# CPL_PACKAGE_OUTPUT=output/release-s2-20261015 的绝对路径
$env:CPL_RUN_PACKAGING='1'
D:/Anaconda/python.exe -W error -m unittest sdk.python.tests.test_cli_packaging.TestDistributionAcceptance -v
```

产物本地保留在 `output/release-s2-20261015/cpppools_distribution_qm0vsl_p/`，不提交二进制构建产物。句柄错误分类修正由后续源码定向/组合回归单独覆盖，不用前面的包安装 smoke 冒充错误注入测试。

独立评审已完成，所提补全光标、产物目录和句柄检查问题均修正复验。S2 的 Windows start/ACL/句柄级路径防护、file-level reparse、macOS 生命周期、前台就绪即时提示与真实终端 Ctrl+C 仍待完成；A02 总期限负对照仍开放。未接入 Redis RowCache 或动态发现。

## 10. S2 前台就绪与真实终端补验（2026-10-15）

基于 `6c3696e` 补齐 CLI 阶段 3 的 Linux 前台交付。此前 §9 的前台残项在本节所测范围关闭，Windows/macOS 平台状态不外推。

- **RED**：CLI 就绪即时诊断测试返回 6（缺少 `on_ready`）；真实进程回调测试因接口不存在失败，真实 PTY 用例等待 10 秒没有就绪输出而失败。
- **修改**：`LocalNodeManager.start` 在 running 状态提交、进程保留标记设置后调用可选 `on_ready`，再等待前台进程；CLI 将该状态立即 flush 到 stderr。stdout 仍只输出最终结果；已登记后的提示输出异常保留进程，不走就绪前清理强杀。
- **真实 PTY GREEN**：Linux/WSL、Python 3.12.3、`build-accept-s0-s2/bin/cpppools_server`。实际控制终端的前台进程组为 CLI，server 使用独立 session。唯一 worker 被不完整 HTTP body 阻塞且 health 探针超时时，向 PTY 写 `0x03`，由终端 ISIG 产生 SIGINT；CLI 等待真实 graceful 收尾后返回 130，server 退出码 0、launch 不变，stdout 恰好一份最终 JSON，stderr 有就绪和中断诊断，token/stop.request 清除且 token 未输出。
- **异常边界 GREEN**：就绪通知抛 RuntimeError 的真实服务测试确认 running 登记和进程仍在、身份可核验，随后可正常 stop。新增两个 live 用例最终修正夹具后 2/2 通过，6.017 秒。
- **组合回归**：生产代码最终版本的 `-W error` 全量 live 套件，Linux/WSL 181 项中 175 通过、6 跳过（56.007 秒）；Windows 181 项中 142 通过、39 跳过（37.945 秒），均退出 0。Python 3.8.10 的 CLI 本机分发/parser 定向 14/14 通过（0.414 秒）；其全量与发行证据另见 §9。
- **最终发行回归**：包含本批前台生产修改的干净源码再次独立构建 wheel/sdist/重建 wheel，Windows 3.12.3/3.8.10 六项安装与真实 CRUD 全部通过，矩阵退出 0，用时 132.530 秒。产物保留于 `output/release-s2-20261015/cpppools_distribution_e2xndjyl/`。
- **评审与回归修正**：独立评审确认生产回调/状态/信号/输出契约无新阻断，复现了夹具停止失败后遗留 server 的问题。夹具改为仅在确认退出后删除目录：本次自有 Popen 有界回收；CLI 创建的服务须核对本次配置、binary、workdir、launch/PID/创建身份，再走已验证信号路径；无法确认则保留登记目录。实际启动两个 native server 并注入 stop 超时，拥有 Popen 与分离 manager 两条清理后备均验证进程退出和目录删除。CLI 子进程自身也补齐超时 kill/reap。早期 starting/PID 未知时明确保留目录，独立隔离 probe 验证不会删除；最终复审无剩余 Critical/Important。此后备只属于测试失败清理，不改变生产自动强杀禁令。

真实入口：

```bash
PYTHONPATH=sdk/python \
  CPL_TEST_SERVER="$PWD/build-accept-s0-s2/bin/cpppools_server" \
  python3 -W error -m unittest sdk.python.tests.test_foreground_live -v

PYTHONPATH=sdk/python CPL_RUN_E2E=1 \
  CPL_TEST_SERVER="$PWD/build-accept-s0-s2/bin/cpppools_server" \
  CPL_PACKAGING_WHEELHOUSE="$PWD/output/release-wheelhouse" \
  python3 -W error -m unittest discover -s sdk/python/tests
```

本节的终端证据覆盖**单次** Ctrl+C。停止等待期间再次 Ctrl+C 仍可能传播 KeyboardInterrupt 并保留进程，这是既有行为，本批没有把它记为已通过的重复中断验收。Windows start/ACL/句柄级路径防护与 file-level reparse、macOS 生命周期、A02 总期限负对照继续开放；S2 整体和 v1 尚未验收，Redis RowCache/动态发现仍待后续阶段。
