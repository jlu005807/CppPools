# S0–S2 验收问题修复实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 关闭验收报告 A01–A08 的实际问题，恢复已有部署、请求和本机启停的正确性，再重新验收已实现范围。

**Architecture:** 沿用 Database NVI、固定线程池与阻塞请求模型。服务端提供同代就绪和完整收尾证据，CLI 在实例锁内核对 launch/revision 后提交状态，锁外执行探测和退出等待。仅补齐已有 CLI 方案中与这些缺陷直接相关的控制协议，不扩展 Redis、路由或数据库取消接口。

**Tech Stack:** C++17、CMake/CTest、Oracle MySQL/MariaDB C API、Winsock/POSIX、Python 标准库 unittest、现有 JSON 实现。

---

## 1. 基线、范围与执行顺序

2026-10-15 补验更新：A01 已用本机 Oracle MySQL 8.0.11 实际通过并关闭；A02 已验证 Windows 有界退出，但重置期限负对照仍通过，因此总期限门槛保持开放。Windows HTTP 测试已可原生编译，全量构建的下一个阻断为 `test_config.cpp`。命令、结果和评审边界见 [验收报告第 8 节](completed-stages-acceptance-2026-10-14.md#8-本机真实环境补验2026-10-15)。下文任务记录保留历史状态，以本更新为准。

同日后续验收：Windows start/status/stop、DACL/句柄级路径防护、file-level reparse 和 Unicode 状态目录已通过；Linux/WSL 前台真实终端、Windows 双解释器六项发行矩阵已通过。Compose 真实远程部署验收见 [验收报告 §12](completed-stages-acceptance-2026-10-14.md#12-docker-compose-远程验收与回归修复2026-10-15)，它不替代 macOS 本机管理和 A02 负对照门槛。

日期：2026-10-14；审查基线：`a8acc5b09c99f7ec617b0b996747efc7d46e2681`。问题事实和复现证据见 [验收报告](completed-stages-acceptance-2026-10-14.md)，阶段进度见 [统一实施路线](implementation-roadmap.md)，协议和退出码以 [CLI 方案 §3–§5](cpl-cli-design.md) 为准。

本文件已进入执行记录：F1–F7 的实现、定向回归和阶段评审已完成；F8 的 Linux/WSL 组合验收已完成，完整环境验收仍部分完成。整体复审新增 F9–F11 的实现、测试和独立代码复审已完成；Windows 慢读期限及真实 MySQL SQL mode 仍缺环境证据。A01、A02 保持开放，不把 skip 记为通过。CLI 分阶段进度见 [CLI §6.1](cpl-cli-design.md#61-当前实施进度2026-10-14)。

| 顺序 | 任务 | 对应问题 | 优先级 | 前置条件 | 本任务完成门槛 |
|---|---|---|---|---|---|
| 0 | F0 固定基线和测试环境 | 全部 | 准备 | 无 | 记录真实 HEAD、改动边界和验证环境 |
| 1 | F1 MySQL 自省 SQL mode 兼容 | A01 | P1 | F0 | 两类客户端构建；默认/NBE 模式的真实自省和 CRUD |
| 2 | F2 Windows HTTP 接收与监听等待 | A02 | P1 | F0 | 原生延迟头、分片 body、发送期限；空闲控制停止随 F4 复验 |
| 3 | F3 本机状态条件提交 | A03 | P1 | F0 | 重复 spawn、同代和跨代覆盖的确定性测试通过 |
| 4 | F4 托管信号与终态生产 | A05；A04 所需证据 | P1 | F3 | 启动握手、重复信号、析构后终态；协议两端同步迁移 |
| 5 | F5 退出证据归并和 CLI 退出码 | A04 | P1 | F3、F4 | 不误报 graceful；force=9；超时/恢复结果一致 |
| 6 | F6 输入异常和错误 JSON | A06、A07 | P2 | F0 | 输入错误=2、stderr 可解析、无 traceback/网络 |
| 7 | F7 重复启动的配置一致性 | A08 | P2 | F3、F5 | 仅同配置 running 幂等成功；不重复 spawn |
| 8 | F8 组合回归与重新验收 | 全部 | 验收 | F1–F7 | 每个问题有针对性通过证据；更新阶段状态 |

推荐按表执行。F1、F2、F6 没有运行时依赖，可单独交付；F3–F5 共用状态/控制协议，应连续完成。F4 必须先提供可信的完整收尾证据，F5 才能据此关闭误报成功问题。F4 修改控制文件格式时，服务与 SDK 必须在同一批变更中切换，避免混用新旧协议。

本轮不要求重做已通过的 Singleflight、bypass、metrics，也不顺带重构 CLI parser/SDK 重试。macOS/Windows 本机启动、发行包完整矩阵另见第 5 节；它们影响 S2 整体验收，但不应包装为本轮新增的八个缺陷。

## 2. 修改文件地图

| 文件 | 操作 | 责任/归属 |
|---|---|---|
| `src/db/mysql_database.cpp` | 修改 | F1：两处元数据查询、安全字面量、共享自省入口 |
| `src/db/mysql_database.h` | 修改 | F1：仅私有测试/实现入口；不改 public factory |
| `test/test_mysql_introspection.cpp` | 修改 | F1：SESSION 模式、特殊表名及原有 schema 断言 |
| `test/test_mysql_crud.cpp` | 修改 | F1：同模式连接上的真实参数化 CRUD |
| `src/server/http_server.cpp` | 修改 | F2 监听等待；F4 就绪/停止协议和 pending-stop 消费 |
| `include/cpppools/server/http_server.h` | 修改 | F4：保留可选控制路径；移除配置中的 token，隐藏私有控制实现 |
| `src/server/local_control.h`、`src/server/local_control.cpp` | 新增 | F4：本机控制文件、OS 身份、原子 marker、托管 pending-stop |
| `src/main.cpp` | 修改 | F4：早期信号桥、初始化检查点、完整析构后终态 |
| `test/test_local_control.cpp` | 新增 | F4：协议、写失败、信号幂等与收尾顺序 |
| `CMakeLists.txt` | 修改 | F4：注册私有组件和专项测试 |
| `sdk/python/cpppools/local_node.py` | 修改 | F3/F4/F5/F7：锁、协议消费、退出归并、配置摘要 |
| `sdk/python/cpppools/cli.py` | 修改 | F5/F6：本机返回码、输入归一化和 JSON 错误 |
| `sdk/python/tests/test_local_node.py` | 修改 | F3/F4/F5/F7：状态交错、身份、真实启停与恢复 |
| `sdk/python/tests/test_server_socket_live.py` | 新增 | F2/F4：可跨平台运行的原生服务 socket/control 测试 |
| `sdk/python/tests/test_cli_commands.py` | 修改 | F5/F6：CLI subprocess 退出码、输出、无 SDK/网络 |
| `docs/completed-stages-acceptance-2026-10-14.md`、`docs/implementation-roadmap.md` | 修改 | F8：追加修复证据与限定范围验收结论 |
| `docs/cpl-cli-design.md` | 修改 | F4/F5/F7：只同步实现记录和已冻结协议；不另造退出码 |

私有组件置于 `src/server/`，第三方 JSON 头只在 `.cpp` 引入。MySQL 测试入口只放 `src/db/` 私有头，沿用测试目标已有 `-Isrc`；不增加生产 datasource 的 `sql_mode` 配置或全局测试开关。

## 3. 分项修复任务

### F0：固定基线与验证条件

**文件：** 读取验收报告、相关源码与上述测试；执行期间更新本文件任务复选框和验收报告，不修改无关工作区内容。

- [ ] 运行以下命令，记录当次 HEAD 和脏文件。保留当前已有的文档、技能与临时产物，不将其混入修复提交。

```powershell
git rev-parse HEAD
git status --short
git log -5 --oneline
```

- [ ] 确认 WSL Ubuntu 的编译器和 Python，以及 Windows 原生 `D:\Anaconda\python.exe` 和 MinGW；分别使用现有 `build-accept-s0-s2`、`build-s0-mysql-wsl`、`build-s0-mysql-win2`，不混用构建缓存。
- [ ] MySQL 连接凭据仅经 `CPPPOOLS_TEST_MYSQL_*` 环境变量注入；测试账号需能创建/删除独立测试库。缺账号返回 skip 不能计为通过，不把密码写到命令、日志或文档。
- [ ] 每个修复先新增针对性失败测试，再修改实现；保存 RED/GREEN 的命令、退出码、关键断言与平台。原始报告中的 18/100 是历史基线，不作为未来固定测试计数。

### F1：移除 MySQL 自省对反斜杠转义的依赖（A01）

**修改：** `src/db/mysql_database.cpp`、`src/db/mysql_database.h`。
**测试：** `test/test_mysql_introspection.cpp`、`test/test_mysql_crud.cpp`。

两处 `table_name` 都是 SQL 值，不是标识符。统一编码成 `CONVERT(X'十六进制字节' USING utf8mb4)`，不再调用有 SQL mode 限制的转义 API，也不新增 Oracle/MariaDB 的分支。十六进制字符集固定，避免引号、反斜杠进入字面量；显式转换回当前连接使用的 utf8mb4，保留文本比较语义。`X'...'` 的基础语法分别见 [MySQL 文档](https://dev.mysql.com/doc/refman/8.4/en/hexadecimal-literals.html) 和 [MariaDB 文档](https://mariadb.com/docs/server/reference/sql-structure/sql-language-structure/hexadecimal-literals)。本修复选择仍须以下真实模式测试验证。

- [x] 在私有头声明 `bool mysql_introspect_schema(MYSQL*, Schema*, std::string*)`，在 `.cpp` 匿名命名空间外定义薄封装，调用现有 `introspect`；factory 调用该入口。该入口仅在 `src/db` 私有头声明，公开 API 未变。

```cpp
bool mysql_introspect_schema(MYSQL* conn, Schema* out, std::string* error) {
    return introspect(conn, out, error);
}
```

- [x] 在测试中加入已建临时库、utf8mb4 连接下的空模式和 NBE SESSION 自省，以及原 schema 断言和含引号/反斜杠/中文表名。由于测试账号缺失，尚未在真实服务执行这些断言，见下方未完成项。

```cpp
assert(mysql_query(root, "SET SESSION sql_mode='NO_BACKSLASH_ESCAPES'") == 0);
Schema schema;
std::string error;
assert(mysql_introspect_schema(root, &schema, &error));
assert(error.empty());
test_introspection(schema);
```

预期 Oracle 客户端 RED：现有普通表 `users` 即触发错误长度/自省失败；MariaDB 客户端另记实际表现。仅验证转义 API 返回 -1 不足以作为修复后的 GREEN。

- [x] 在 `.cpp` 添加私有 helper，用它替换两处 `escaped_table/escaped_tbl` 及包在外面的单引号。保留 `MysqlRows` 对 query/store_result 失败的现有返回；自省路径中已不再调用这两处 `mysql_real_escape_string`。

```cpp
std::string mysql_text_literal(const std::string& value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result = "CONVERT(X'";
    for (unsigned char byte : value) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    result += "' USING utf8mb4)";
    return result;
}
// 查询列时的条件；索引查询以 table->name 调用同一 helper。
// "WHERE table_schema = DATABASE() AND table_name = " +
// mysql_text_literal(table) + " ORDER BY ordinal_position"
```

- [x] CRUD 测试新增两种 SESSION 模式的自省和现有 prepared CRUD 流程，并把含单引号、反斜杠、中文、emoji 的文本往返加入测试；默认 factory 路径仍保留。未修改 GLOBAL `sql_mode`。
- [x] Windows Oracle MySQL 客户端和 WSL MariaDB 客户端构建均成功；无凭据时两个 CTest 用例现以退出码 77 明确显示为 `Skipped`，不再计为通过。
- [ ] 配置有效测试账号后，在真实 MySQL/MariaDB 服务上运行两个专项；记录客户端、服务版本和 SESSION mode。当前 Windows 本机 MySQL 服务可连接但没有可用测试凭据，WSL 无 MariaDB 服务，因此真实 RED/GREEN 尚未验证。

- [x] 规格审查与代码质量审查均通过；`git diff --check` 通过。未提交代码，按本次任务授权范围保留为工作区改动。

当前结论：F1 实现、双客户端构建和静态审查完成；A01 仍待真实数据库 SESSION 模式与 CRUD GREEN 证据，不能据此标记关闭。

- [ ] 检查本任务 diff 与证据，只提交上述四个文件，建议提交名 `fix: make mysql introspection independent of escape mode`。

### F2：恢复 Windows 阻塞接收，保留有界监听等待（A02）

**修改：** `src/server/http_server.cpp`。
**新增测试：** `sdk/python/tests/test_server_socket_live.py`；已有发送期限回归仍在 `test/test_http_server.cpp`。

- [x] 新测试类 `TestServerSocketLive` 仅在 `CPL_TEST_SERVER` 指定可执行文件时启用。每例自行创建临时 SQLite 库/配置，直接 `Popen([binary, '--config', config])`，不经过 Windows 尚未实现的 `LocalNodeManager.start`。使用动态空闲端口，等待 health；finally 关闭 socket、回收自己启动的进程并清理临时目录。
- [x] 实现 delayed-header 测试：TCP 建连后等待 250 ms 再发送以下请求；读完整 HTTP 响应，必须得到 200。补一例将头拆成两段间隔 250 ms、一例 CRUD body 拆成两段间隔 250 ms，断言成功及只插入一行。等待均小于已有收包超时。

```python
with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
    sock.settimeout(3)
    time.sleep(0.25)
    sock.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
    response = bytearray()
    while True:
        part = sock.recv(4096)
        if not part:
            break
        response.extend(part)
    self.assertTrue(bytes(response).startswith(b"HTTP/1.1 200 "))
```

预期 Windows RED：断连/10053 或无完整 200；Linux 对照应通过。

- [x] 删除 listener 的 `FIONBIO=1` 设置和循环中的固定 20 ms sleep。在 Windows 分支、accept 前使用已有 helper；listener/accepted socket 均保持阻塞。每次超时回到循环头检查 stop；POSIX poll 和现有 recv timeout 保留。

```cpp
#ifdef _WIN32
if (!wait_socket_readable(server_fd, std::chrono::milliseconds(200))) {
    continue;
}
#endif
```

`send_all_until` 仍可为发送阶段切换非阻塞，不恢复逐段发送期限。一个 accept 线程消费 listener，不引入第二个 accept 消费者。

- [x] 在同一新测试文件增加慢读/不读响应测试：使用一个 worker，在临时 SQLite 库创建 512 张表、每张 1024 个 TEXT 列（列名填充到 48 字节），基准 GET `/api/v1/schema` 确认响应超过 1 MiB。测试客户端在 connect 前设 `SO_RCVBUF=1024`，每 50 ms 读 1 字节或完全不读；以现有发送总期限 10 秒加 3 秒调度余量为上限，确认 worker 释放后下一次 health 成功。POSIX 已实际验证该 deadline/recovery；Windows 子测试因本机无法形成可验证背压而显式 skip，故 Windows 发送期限仍未验证。必须先观察到发送受阻，若响应提前完整进入 OS 缓冲则测试不具备验证力，应增大同一 fixture 的表数重新执行，不能算期限通过。
- [x] F4 完成后补认证控制文件下的“无新连接停止”测试：Windows 原生 C++ 服务接收认证 stop.request 后，在没有新 HTTP 连接时退出并发布匹配 stopped marker；命令与结果见下方 F2 补充记录。
- [x] 重新构建 Windows `cpppools_server`，用原生 Python 执行新测试；Linux 执行既有慢读/overload 总 deadline 回归。命令从仓库根运行。

```powershell
cmake --build build-s0-mysql-win2 --target cpppools_server -j2
$env:PYTHONPATH = 'sdk/python'
$env:CPL_TEST_SERVER = (Resolve-Path 'build-s0-mysql-win2/bin/cpppools_server.exe').Path
$env:PATH = 'D:\Anaconda\envs\equipment_tool\Library\bin;' + $env:PATH
& D:\Anaconda\python.exe -W error -m unittest discover -s sdk/python/tests -p test_server_socket_live.py -v
```

```bash
cmake --build build-accept-s0-s2 -j2
ctest --test-dir build-accept-s0-s2 --output-on-failure -R '^test_http_server$'
```

预期：原生延迟/分片测试全部实际执行并通过；Linux 总期限测试通过。Windows 现有 POSIX-only C++ 测试不能原生构建的限制继续注明。

- [ ] 审查并仅提交 HTTP 实现和新测试文件，建议提交名 `fix: preserve blocking windows request receive`。

F2 执行记录（2026-10-14）：Windows 原生构建成功，live socket 测试 3 项通过、慢读子测试 1 项明确跳过。Windows loopback 对 65,552,520 字节响应、WSL 对 131,105,836 字节响应，在客户端不读取 body 且接收缓冲区为 2,304 字节时，紧随其后的 `/health` 仍立即返回 200；该环境无法用此黑盒场景证明发送期限触发，因此不将 Windows 慢读期限记为通过。Linux/POSIX live socket 测试 4/4 通过（含慢读探针超时和期限后 worker 恢复）；既有 `test_http_server` CTest 1/1 通过。Linux 构建报告 `wait_socket_readable` unused 警告（helper 仅由 Windows 分支调用），保留现状以控制本阶段范围。独立需求与代码质量评审均未发现阻塞问题；评审指出的诊断文案和 deadline 窗口不一致已修正，并重跑 Windows/POSIX live socket 测试通过。F4 后补证（早期快照，最终复验见第 403 行）：当时 Windows 原生 `test_server_socket_live.py` 为 8 项中 7 项通过、慢读回压 1 项因环境无法形成可验证背压而跳过；`test_windows_idle_control_request_stops_without_http_connection` 实际通过，确认无新 HTTP 连接时认证停止生效，F2 对应复选项已完成。A02 暂不关闭：Windows 慢读期限仍未验证。

```powershell
cmake --build build-s0-mysql-win2 --target cpppools_server test_local_control --parallel 2
ctest --test-dir build-s0-mysql-win2 -R test_local_control --output-on-failure
$env:PYTHONPATH = 'sdk/python'
$env:CPL_TEST_SERVER = (Resolve-Path 'build-s0-mysql-win2/bin/cpppools_server.exe').Path
$env:PATH = 'D:\Anaconda\envs\equipment_tool\Library\bin;C:\Program Files\MySQL\MySQL Server 8.0\bin;D:\winlibs\mingw64\bin;' + $env:PATH
D:\Anaconda\python.exe -W error -m unittest discover -s sdk/python/tests -p test_server_socket_live.py -v
```

验证命令和结果：

```powershell
cmake --build build-s0-mysql-win2 --target cpppools_server -j2
$env:PATH = 'D:\Anaconda\envs\equipment_tool\Library\bin;' + $env:PATH
& D:\Anaconda\python.exe -W error -m unittest discover -s sdk/python/tests -p test_server_socket_live.py -v
# Ran 4 tests; OK (skipped=1)
```

```bash
cmake --build build-accept-s0-s2 --target cpppools_server test_http_server -j2
PYTHONPATH=sdk/python CPL_TEST_SERVER=build-accept-s0-s2/bin/cpppools_server python3 -W error -m unittest discover -s sdk/python/tests -p test_server_socket_live.py -v
ctest --test-dir build-accept-s0-s2 --output-on-failure -R '^test_http_server$'
# Live socket: 4/4 passed. CTest: 1/1 passed.
```

### F3：锁内登记进程，按 launch/revision 条件提交（A03）

**修改：** `sdk/python/cpppools/local_node.py`。
**测试：** `sdk/python/tests/test_local_node.py`。

- [x] 在 `TestLocalNodeState` 添加确定性测试。用 `threading.Event`/mock 屏障固定交错顺序，不依靠反复 sleep 碰撞：①第一个 start 尚未登记 PID 时第二个 start；②health 返回前写入同代 stopping；③旧 launch 等待后新 launch 已登记；④force 接受后旧 graceful 等待结束。断言 spawn 次数、launch、revision、state 和 force_delivery。
- [x] 先增加私有 `_commit_state(expected, changes)` 测试入口，expected 为三元组 `(launch_id, state_revision, state)`。建立完整的测试状态后验证以下关键交错：

```python
before = manager.read_state()
expected = (before["launch_id"], before["state_revision"], before["state"])
stopping = dict(before, state="stopping", state_revision=before["state_revision"] + 1)
manager.write_state(stopping)
self.assertIsNone(manager._commit_state(expected, {"state": "running"}))
self.assertEqual(manager.read_state()["state"], "stopping")
```

RED 必须体现旧操作能够覆盖最新记录；若只因新 helper 未定义失败，继续把 start/stop 的实际交错测试跑出原始错误，再修复。

- [x] 将首次 `starting` 写入、控制文件创建、Popen 及 PID/创建身份登记放在一次短的实例独占锁范围内。网络、退出 wait、drain 都在锁外；spawn 失败在同一代下写 crashed。`starting + pid=null` 是尚未完成的启动意图，不允许另一个 start 重建实例。
- [x] 条件提交采用以下核心实现；所有 start 就绪、超时、foreground 退出、stop 完成和后续 status 归并路径都改用它。调用失败后重读并重新判定本次操作结果，不用旧快照再次盲写；新 launch 已出现时不删除其文件、不向其 PID 发信号。

```python
def _commit_state(self, expected, changes):
    with self._exclusive():
        current = self.read_state()
        actual = (current["launch_id"], current["state_revision"], current["state"])
        if actual != expected:
            return None
        updated = dict(current, **changes)
        updated["state_revision"] = current["state_revision"] + 1
        updated["updated_at"] = _now()
        self.write_state(updated)
        return updated
```

同代允许 `starting→running/stopping/crashed`、`running→stopping/stopped/crashed`、`stopping→stopped/crashed`；终态不可回到 starting/running。同代 force_delivery 从 pending 到 accepted/rejected，普通 stop 不得清空 pending，accepted 不可清空或降级。先核对预期，再核对转换；不能靠原子 replace 代替并发归并。条件冲突时，ready 操作只能重新判定/结束；force 接受证据必须重读同代 revision 后重试提交，不能因普通状态更新丢失证据。跨代时停止提交并报告原代结果。

- [x] 运行本机单测，确认两个 start 最多一个 Popen，旧 ready 无法复活 stopping/终态，旧 launch 无法覆盖新 launch；实例锁失败保持既有退出码 10，starting/stopping 的启动拒绝为 6。

```bash
PYTHONPATH=sdk/python python3 -W error -m unittest discover -s sdk/python/tests -p test_local_node.py -v
```

**执行记录（2026-10-14，独立规格与质量评审通过）：**

- 实现了三元组条件提交、同代状态转换校验、force_delivery 单调性校验；start 的状态/控制文件/Popen/PID 身份登记现在位于同一实例锁范围，等待和 health 检查留在锁外。`starting`（无论 PID 是否登记）和 `stopping` 都以 code 6 拒绝新 start；ready、超时、foreground 退出及 stop 完成都采用条件提交，遇到冲突会重读并保留新记录。
- RED：修复前运行 `PYTHONPATH=sdk/python python3 -W error -m unittest sdk.python.tests.test_local_node.TestLocalNodeState.test_concurrent_start_does_not_spawn_twice_before_pid_registration -v`，失败为两次 Popen（期望一次）。
- WSL live 命令：`cd /mnt/d/Internt_of_Thing/CppPools && PYTHONPATH=sdk/python CPL_TEST_SERVER=build-accept-s0-s2/bin/cpppools_server python3 -W error -m unittest discover -s sdk/python/tests -p test_local_node.py -v`，22/22 通过、0 skip；覆盖真实 start/status/stop、CLI 合约和无效控制 token 后的 force 清理。未设置 `CPL_TEST_SERVER` 的同一命令也通过，22 项中 19 项通过、3 个 live 用例按预期 skip。
- Windows 原生 Python 命令：`$env:PYTHONPATH='sdk/python'; D:\Anaconda\python.exe -W error -m unittest discover -s sdk/python/tests -p test_local_node.py -v`，22 项中 6 项通过、16 项因 POSIX-only 或未设置 `CPL_TEST_SERVER` 跳过。Windows 的 `start` 当前明确不支持；POSIX live 生命周期与交错测试以 WSL 结果为准。
- force 接受交错现在由真实 graceful `stop()` 线程执行：在进程退出检查后的 `_reap_child` checkpoint 暂停，另一线程条件提交 pending→accepted，再放行旧 graceful 提交；断言旧提交无法覆盖 accepted。force_delivery 的实际发送/接受 wire protocol 由 F4/F5 接入后再做端到端验证，F3 不提前实现协议。
- 全工作树 `git diff --check` 通过（只有现存脚本的 CRLF 行尾提示）；计划文档无尾随空格。测试创建的服务进程均已退出；未清理 build/docs/tmp 或其他既有文件；未提交。

代码实现、测试、独立规格评审与质量评审均已完成，F3 通过。评审发现的启动异常清理缺口和 readiness timeout 二次清理问题均已通过定向 RED/GREEN 测试修复；readiness timeout 仍返回 code 7 并保留 stopping 与进程身份。最终 WSL live 测试 22/22 通过，Windows 原生测试 6 项通过、16 项按平台/环境跳过；`git diff --check` 通过。未提交。F4/F5/F8 后续仍补齐受管信号、退出证据归并和最终 CLI 行为。

F3 cleanup follow-up (2026-10-14):

- RED: deterministic setup-error, PID-registration-failure, unreaped-child, and post-spawn health-exception tests exposed stale `starting`/`running` state, missing kill escalation, and an unmanaged child. The health-exception RED also exposed an unclosed log handle.
- GREEN: pre-ready startup failures close the log, clean up the Popen-owned child, and conditionally publish `crashed` only after reap; an unreaped child remains registered as `stopping` with the manager retaining its process handle. Once `running` is committed, later exceptions preserve the registration instead of entering failed-start kill cleanup. State cleanup checks launch identity/revision through `_commit_state`.
- Latest WSL live run: 22/22 passed. WSL without `CPL_TEST_SERVER`: 22 tests, 19 passed and 3 live cases skipped. Native Windows Anaconda run: 22 tests, 6 passed and 16 POSIX/live cases skipped. These latest totals supersede the earlier F3 counts above.
- Code 7 regression correction: readiness-timeout coverage confirms that `StopTimeout` preserves `stopping`, PID/start identity, and revision after the 10-second graceful wait. Pre-ready failures still use startup cleanup; post-ready failures preserve their managed state.
- RED/GREEN tests: `test_unexpected_setup_error_clears_starting_intent`, `test_registration_write_failure_reaps_spawned_process_before_crashed`, `test_unreaped_startup_child_remains_registered_as_stopping`, `test_health_exception_reaps_registered_startup_child`, `test_readiness_stop_timeout_preserves_stopping_child`, and `test_log_close_error_does_not_override_readiness_stop_timeout`.


- Log-close RED: `test_log_close_error_does_not_override_readiness_stop_timeout` raised the close `OSError` before `preserve_stopping_timeout` was set, causing the code-7 contract to be lost. The flag is now set first and log-close failure is suppressed, so the original `StopTimeout` is retained.

- Independent review: F3 is SPEC COMPLIANT and quality review PASS. Both reviewers confirmed startup cleanup remains within F3, timeout state is preserved, and no force-delivery wire protocol was added early.

整体复审补充：以上保留原执行与阶段评审记录。R01/F9 将就绪后的前台中断和异常与启动失败回收分开；Ctrl+C 走 10 秒认证停止，graceful 时返回 130。就绪前失败仍由管理器回收其自有 Popen 子进程。见 [整体评审与设计补充](completed-stages-overall-review-2026-10-14.md)。
### F4：托管信号全程有效，完整收尾后发布终态（A05）

**新增：** `src/server/local_control.h/.cpp`、`test/test_local_control.cpp`。
**修改：** `src/main.cpp`、`src/server/http_server.cpp`、`include/cpppools/server/http_server.h`、`CMakeLists.txt`、`sdk/python/cpppools/local_node.py`。
**测试/同步：** `sdk/python/tests/test_local_node.py`、`sdk/python/tests/test_server_socket_live.py`、`docs/cpl-cli-design.md`。

- [x] 为私有控制组件建立以下接口；JSON 在 `.cpp` 内部处理。`LocalControl` 保存已验证的路径、同代 OS 身份及 token，token 只在私有实现内存中使用。HTTP 配置仅保留可选 control_dir，删除 public 配置的 control_token/control_launch_id；HTTP 私有成员持有控制对象，不向公共 API 暴露 JSON 类型。

```cpp
// src/server/local_control.h，namespace cpppools::server
struct LocalIdentity {
    std::string launch_id;
    std::int64_t pid;
    std::string process_start_time;
};
class LocalControl {
public:
    static std::unique_ptr<LocalControl> open(const std::string& dir,
                                             std::string* error);
    const LocalIdentity& identity() const noexcept;
    bool publish_starting(std::string* error) const;
    bool publish_ready(int port, const std::string& node_id, std::string* error) const;
    bool publish_stopping(std::string* error) const;
    bool publish_stopped(int exit_code, std::string* error) const;
    bool stop_requested(std::string* error) const;
};
void request_managed_stop() noexcept;
bool managed_stop_requested() noexcept;
```

上述为本任务新增的私有接口，不是现存 API。实现可增加 private 字段，但不能用全局 token 或让 marker 接口打印 secret。

将 `src/server/local_control.cpp` 加入现有 cpppools 源文件清单，使用 `cpppools_add_test(test_local_control)` 注册测试。若 HttpServer 新增私有 `unique_ptr<LocalControl>`，析构定义放在包含完整私有类型的 `.cpp`，避免公共头对不完整类型析构导致编译失败。

- [x] C++ 专项覆盖 protocol/version/launch 不匹配、损坏/超长文件、符号链接、重复 stop、临时文件替换失败。托管 stop 的进程内原子标志采用 lock-free atomic bool，并有 `static_assert(std::atomic<bool>::is_always_lock_free)`；`request_managed_stop` 仅 store true，读取函数仅 load。测试两次调用仍只得到 true、不产生退出/I/O。

- [x] Windows 原生目录 reparse-point 拒绝已实际执行：Windows live 用例通过 PowerShell 创建无特权目录 junction，将服务 `control-dir` 指向该 reparse point，并确认服务在读取配置前以 `control path is not a regular directory` 拒绝；原生用例 1/1 通过。`test_local_control.exe` 中基于文件 symbolic link 的子用例仍因系统权限限制而 `SKIP`，因此 file-level reparse 分支未独立实测。

```cpp
request_managed_stop();
request_managed_stop();
assert(managed_stop_requested());
```

真实 Linux 测试保持半个未发送完的请求，在 matching starting/ready 后连续两次 SIGTERM，预期退出 0 且存在 matching stopped.json。RED 是当前退出 143 且无终态。

- [x] 服务与 CLI 同时切换到以下 JSON v1 文件。所有身份标记与 CLI 已登记的 `pid/process_start_time/launch_id` 相等；Linux 使用同口径 `/proc/<pid>/stat` 创建身份，Windows 原生服务用精确 OS 创建时间，不以当前时间代替。

| 文件 | 必须字段 | 写入时机 |
|---|---|---|
| control.meta | protocol_version=1、launch_id | CLI spawn 前 |
| starting.json | 上述字段、pid、process_start_time、state=starting | 信号桥安装后，初始化前 |
| ready.json | 上述身份、port、node_id、state=ready | bind/listen 成功后 |
| stop.request | protocol_version=1、launch_id、requested_at、token | CLI 请求停止 |
| stopping.json | 上述身份、state=stopping | 服务接受停止时 |
| stopped.json | 上述身份、state=stopped、exit_kind=graceful、exit_code | HTTP/线程池/数据库完整收尾后 |

原始 `control.token` 继续使用至少 256 bit 随机值，独立文件，不改成公开 JSON 字段。POSIX 目录 0700、文件 0600。控制文件最多 4 KiB，严格验证类型/版本/launch；拒绝链接，Windows 原生文件控制检查 reparse point。临时文件同目录、唯一名称，写入/flush/原子替换每步检查失败。Windows 用可替换已有文件的系统原子操作，不能假设普通 rename 与 POSIX 等价。

- [x] main 先解析/验证 control-dir，安装托管桥并写 starting，再读耗时配置和初始化数据库。托管信号 handler 仅 `request_managed_stop()`，不访问可能析构的 HttpServer；HTTP accept 循环在 200 ms 检查点消费标志并 `request_stop()`。未托管路径保留现有直接启动信号行为。主流程检查点同样消费已认证 stop.request，不新增阻塞数据库初始化的取消能力。
- [x] 调整 main 资源作用域，保证信号桥在初始化和全部析构期间有效。代码排列必须满足下列顺序；正常停止不在 handler 内写文件。

```text
验证控制元数据 → 安装托管信号桥 → 原子写 starting.json
  资源作用域开始
    读取配置 / 初始化 registry 与三类池；各安全检查点检查 pending-stop
    server listen → ready.json → accept/drain → server 析构
    请求池 / 工作线程池 / registry 与数据库析构完成
  资源作用域结束
原子写 stopped.json → 恢复信号处置 → main 返回
```

starting/ready 写失败：中止启动且回收已建资源。stopping 写失败：继续停机并记录无 secret 诊断。stopped 写失败：服务非零退出，CLI 不判 graceful。初始化失败不生成成功终态；提前接受普通停止则有序释放已建资源后记录实际退出结果。

- [x] CLI 的 ready 读取必须核验完整 JSON 身份、实际 port、OS 身份和 health；旧 ready 文本或旧 launch marker 不算就绪。普通 SIGTERM 后备须先收到 matching starting/ready，并再次核验身份；未握手时仅文件请求/等待，到期限返回 7，保留 stopping。启动超时清理也遵守该规则，不再无条件 send_signal。

已有旧协议活实例保持原记录，不现场重写它的 control.meta/token 或声称自动升级。新 CLI 对缺握手旧服务按冻结兼容规则返回启动失败/清理超时，普通 stop 不盲发信号；显式 force 仍须先核验 OS 身份。新启动生成 JSON 协议，新旧文件混合不降级猜测。
- [x] 完成回归：①初始握手前禁止普通信号；②初始化检查点 stop；③drain 重复信号；④析构窗口重复信号；⑤终态写失败；⑥旧端口的 health=200 加旧 marker 不能使新 launch ready；⑦Windows 原生服务认证 stop 且无新 HTTP 连接时退出。初始化/析构窗口用测试 fixture 的 Event/条件变量固定顺序，不给生产进程增加测试环境开关。直接测试私有控制组件的收尾安排，并保留 main 源码作用域审查和真实进程证明。

```bash
cmake --build build-accept-s0-s2 -j2
ctest --test-dir build-accept-s0-s2 --output-on-failure -R '^(test_local_control|test_http_server)$'
PYTHONPATH=sdk/python CPL_TEST_SERVER="$PWD/build-accept-s0-s2/bin/cpppools_server" \
  python3 -W error -m unittest discover -s sdk/python/tests -p test_local_node.py -v
```

Windows 重建服务后重跑 F2 原生命令，新加入的空闲控制停止用例必须执行。建议提交名 `fix: publish managed lifecycle evidence after complete teardown`；协议两端与测试一起提交，不能只发布一端。

**执行记录（2026-10-14）：**

- WSL/POSIX：构建 `cpppools_server test_local_control test_managed_signal_bridge` 成功；`test_local_control`、`test_managed_signal_bridge`、`test_http_server` 分别以 CTest 1/1 通过（合计 3/3），`test_http_server` 包含 drain/send deadline 回归。`PYTHONPATH=sdk/python CPL_TEST_SERVER="$PWD/build-accept-s0-s2/bin/cpppools_server" python3 -W error -m unittest discover -s sdk/python/tests -p test_local_node.py -v` 28/28 通过；同参数运行 `test_server_socket_live.py` 为 7 项通过、Windows 专属 idle-stop 用例 1 项跳过。真实 Linux duplicate SIGTERM、init checkpoint stop、stopped 写失败均通过。旧 marker 用例强制 mock 进程身份匹配，确保拒绝原因是旧 launch marker，而非虚构 PID 身份失败。
- RED/GREEN 证据：`test_local_control` 的超大整数版本用例先因 `4294967297` 被窄化接受而失败，修正为精确有符号/无符号比较后通过；control-dir 私有权限用例先因安全创建 helper 缺失失败，加入目录拒绝链接/非目录并设 POSIX 0700 后通过。无 secret 诊断 RED 中，旧 launch/token mismatch 没有 error，live 请求 warning 次数为 0；加入无 secret 错误原因和 main/HTTP 共用的一次性 warning slot 后，协议断言及“坏 token 后合法 token 停止”真实进程用例通过，warning exactly once 且不含 token。
- Windows 原生（早期快照，最终复验见第 403 行）：`cmake --build build-s0-mysql-win2 --target cpppools_server test_local_control --parallel 2` 成功；`ctest --test-dir build-s0-mysql-win2 -R test_local_control --output-on-failure` 1/1 通过。原生 live `test_server_socket_live.py` 当时为 8 项中 7 项通过、慢读期限用例 1 项因当前 Windows/WSL loopback 无法证明远端背压而跳过；已通过用例包括 idle authenticated stop（无新 HTTP 连接）、invalid-token 单次无 secret warning、init checkpoint stop 和 stopped marker 写失败。Windows SDK `test_local_node.py` 28 项中 9 项通过、19 项因 Linux `/proc` 生命周期门控跳过；测试设置了 `CPL_TEST_SERVER`，`TestLiveLocalNode` 整类按平台门控，不会尝试 Windows `LocalNodeManager.start`。Windows 本地 start 继续明确不支持。`test_managed_signal_bridge` 是 POSIX SIGTERM 专项，Linux CTest 通过，Windows 不注册；Windows 的 `cpppools_server` 和 `test_local_control` 已实际编译。
- `test_local_control.exe` 在 Windows 输出 `SKIP: Windows symlink/reparse test needs Developer Mode or privilege`，该 C++ 子用例只覆盖文件 symbolic link。新增 Windows live `test_windows_reparse_control_directory_is_rejected` 通过无特权 directory junction 实际验证服务拒绝目录 reparse point；file-level reparse 分支仍未独立实测。全量 Windows C++ build 仍受既有 `test/test_http_server.cpp:18` 使用 `<arpa/inet.h>` 阻断；本阶段服务与控制专项目标构建成功。
- macOS 本机身份仍未实现，本批不外推 Linux `/proc` 方案。CMake 已明确不在 Apple 平台注册 `test_local_control`；macOS 生命周期与 reparse 验证留待 S2 平台门禁。Windows control-dir 依赖父目录继承的 ACL，没有主动验证/强制当前用户 + SYSTEM DACL；基于已打开句柄的抗 TOCTOU 路径防护也未实现，共享可写目录不在本批安全保证内。本阶段没有实现 F5 force wire delivery。独立规格与质量评审现已通过 F4 本批实现范围；Windows file-level reparse、ACL/句柄 TOCTOU、macOS 生命周期和 Windows SDK `start` 仍为未验收项，不将整个 CLI 设计阶段 4 标成完成。

最终验证命令（WSL 内从仓库根执行）：

```bash
cmake --build build-accept-s0-s2 --target cpppools_server test_local_control test_managed_signal_bridge -j2
ctest --test-dir build-accept-s0-s2 --output-on-failure -R test_local_control
ctest --test-dir build-accept-s0-s2 --output-on-failure -R test_managed_signal_bridge
ctest --test-dir build-accept-s0-s2 --output-on-failure -R test_http_server
PYTHONPATH=sdk/python CPL_TEST_SERVER="$PWD/build-accept-s0-s2/bin/cpppools_server" \
  python3 -W error -m unittest discover -s sdk/python/tests -p test_local_node.py -v
PYTHONPATH=sdk/python CPL_TEST_SERVER="$PWD/build-accept-s0-s2/bin/cpppools_server" \
  python3 -W error -m unittest discover -s sdk/python/tests -p test_server_socket_live.py -v
```

```powershell
cmake --build build-s0-mysql-win2 --target cpppools_server test_local_control --parallel 2
ctest --test-dir build-s0-mysql-win2 -R test_local_control --output-on-failure
$env:PYTHONPATH = 'sdk/python'
$env:CPL_TEST_SERVER = (Resolve-Path 'build-s0-mysql-win2/bin/cpppools_server.exe').Path
$env:PATH = 'D:\Anaconda\envs\equipment_tool\Library\bin;C:\Program Files\MySQL\MySQL Server 8.0\bin;D:\winlibs\mingw64\bin;' + $env:PATH
D:\Anaconda\python.exe -W error -m unittest discover -s sdk/python/tests -p test_server_socket_live.py -v
cmd.exe /v:on /d /c 'D:\Anaconda\python.exe -W error -m unittest discover -s sdk/python/tests -p test_local_node.py -v & echo PYEXIT=!ERRORLEVEL!'
```

最终复验（进程级诊断修复与 Windows junction 用例加入后）：WSL 三项 CTest 分别 1/1 通过；`test_local_node.py` 28/28 通过；`test_server_socket_live.py` 7 项通过、Windows idle-stop 与 reparse 两项各按平台跳过。Windows `test_local_control` CTest 1/1 通过；socket live 9 项中 8 项通过、慢读回压 1 项因环境无法形成可验证背压而跳过，包含启动前坏 token 恰有一条无 secret 告警、无新连接 idle stop 和目录 junction reparse 拒绝；`test_local_node.py` 28 项中 9 项通过、19 项按 Linux `/proc` 生命周期门控跳过。坏 token 在服务启动前写入可复现旧代码双告警（RED）；进程级 atomic 与双 `LocalControl` C++ 用例修复后，WSL/Windows live 与 CTest 通过。独立规格与质量复审通过 F4 本批实现范围；Windows file-level symbolic-link reparse 分支、ACL/句柄 TOCTOU、macOS 生命周期及 Windows SDK `start` 仍是明确未验收项，不将整个原设计阶段 4 标成完成。

### F5：统一退出归并，拒绝把进程死亡当作 graceful（A04）

**修改：** `sdk/python/cpppools/local_node.py`、`sdk/python/cpppools/cli.py`。
**测试：** `sdk/python/tests/test_local_node.py`、`sdk/python/tests/test_cli_commands.py`；同步 `docs/cpl-cli-design.md` 实施记录。

- [x] 写确定性状态归并测试及真实复现：坏 token 的普通 stop 等待中，对测试自己启动的进程 SIGKILL；必须 `crashed/unexpected`、CLI=10，不再 `stopped/graceful`。增加 accepted/pending/rejected force 与有效/缺失/旧代/损坏 stopped marker 的组合，验证恢复后的 status/stop 一致。
- [x] 添加私有 `_exit_transition(state, graceful_proof, observed_exit_code)` 统一判定已确认退出的实例，供 stop/status/foreground/启动清理使用；进程仍存活时不得调用。核心判定与输出如下。

```python
def _exit_transition(state, graceful_proof, observed_exit_code):
    delivery = state.get("force_delivery")
    if delivery == "accepted":
        return {"state": "crashed", "exit_kind": "forced",
                "last_exit_code": observed_exit_code}, 9
    if delivery == "pending":
        return {"state": "crashed", "exit_kind": None,
                "last_exit_code": observed_exit_code}, 10
    if graceful_proof and observed_exit_code in (None, 0):
        return {"state": "stopped", "exit_kind": "graceful",
                "last_exit_code": 0}, 0
    return {"state": "crashed", "exit_kind": "unexpected",
            "last_exit_code": observed_exit_code}, 10
```

graceful_proof 为完整 schema/同代身份匹配、exit_kind=graceful、exit_code=0 的 stopped.json，且已确认 OS 进程死亡；若观察到退出码且非零，proof=false。无法直接 reap 的恢复场景保留未知退出码 null；可信终态记录可提供成功退出证据，不能编造 137 或 0。

- [x] force 发送前以 F3 规则写 `stop_mode=force`、`force_delivery=pending`、force_requested_at；再次核验 OS 身份后发送 SIGKILL。系统接受后条件提交 accepted/force_accepted_at，明确拒绝则写 rejected。发送接受与登记之间 CLI 崩溃时保留 pending，恢复不猜测。accepted 只能继续等待，不改成普通 stop、不重新发送；不从外部 SIGKILL/退出码 137 推断本 CLI 曾接受 force。
- [x] status 在锁外取证、锁内条件归并，成功读取/归并状态仍返回 0；stop 的退出码按归并结果为 0/9/10。仍活超时返回 7，保存 stopping 和 force 证据；身份无法核验的活进程为 8，损坏状态/控制 I/O 为 10。重复 stop 在已退出终态上返回同一判定。

同步修正现有 `test_atomic_state_round_trip_and_status_detects_dead_process` 对已死亡进程仍保持 running 的断言，改验归并后的 crashed 与 last_known_state 语义。终态只能补充证据/已知退出信息，不能复活进程；pending 后补证 accepted 应稳定收敛为 crashed/forced。
- [x] stop 超时、forced 和缺终态时，CLI 先输出实例结果到 stdout，再向 stderr 输出既有 `{kind,message,exit_code,http_status}` 诊断。建议私有本机异常增加可选 `result` 属性，`_run_local` 捕获时打印此结果后返回异常 code；不得改变 CRUD 输出。缺证据保留记录和日志，确认终态后只清理该 launch 的 token/stop.request。

关键 CLI 验收断言：

```python
self.assertEqual(completed.returncode, 9)
result = json.loads(completed.stdout)
error = json.loads(completed.stderr)
self.assertEqual(result["exit_kind"], "forced")
self.assertEqual(error["kind"], "forced_stop")
self.assertEqual(error["exit_code"], 9)
```

- [x] 运行真实本机测试和 CLI 退出码测试；确认日志、token、临时进程无跨代误删/泄露。

```bash
PYTHONPATH=sdk/python CPL_TEST_SERVER="$PWD/build-accept-s0-s2/bin/cpppools_server" \
  python3 -W error -m unittest discover -s sdk/python/tests -p test_local_node.py -v
CPL_RUN_E2E=1 PYTHONPATH=sdk/python CPL_TEST_SERVER="$PWD/build-accept-s0-s2/bin/cpppools_server" \
  python3 -W error -m unittest discover -s sdk/python/tests -p test_cli_commands.py -v
```

建议提交名 `fix: reconcile local exit evidence and forced stop codes`。



### F5 实施记录（2026-10-14）

F5 已按本修复计划的 Linux/WSL 范围实现并通过规格与质量复审。`_exit_transition` 统一 stop/status/foreground/失败启动的退出归并；外部 SIGKILL 不再被当作 graceful 证据，force 的 pending/accepted/rejected 可持久化恢复，CLI 对异常结果先输出实例 JSON，再向 stderr 输出结构化诊断。进程身份信号优先使用 pidfd；系统不支持时保留重验身份后的数字 PID fallback。

复审期间补齐三处实际缺口：deadline 最后一轮退出仍会重新归并；状态不可读时通过当前 `start()` 持有的 Popen 子进程执行失败启动清理；确认子进程已回收后，按本次 launch ID 清理该代 `control.token` 与 `stop.request`，不删除其他 launch 或本代 marker。另修复 POSIX 缺失可选 `ready.json` 时错误抛出异常的问题。

验证结果：WSL `test_local_node` 51/51、带真实服务的 `test_cli_commands` 22/22 通过；Windows 原生 F5 定向测试 17/17 通过，CLI 测试 22 项通过、4 项按平台跳过；Python 编译检查及 `git diff --check` 通过。deadline 测试在最后一次 sleep 后才切换为退出，确实覆盖 deadline 最终归并分支。规格与质量复审确认通过本批实现范围。

范围仍限 Linux/WSL 生命周期：Windows `LocalNodeManager.start` 按设计明确不支持，macOS 身份与生命周期未验收；这些结果不代表原 CLI 设计的跨平台本机生命周期已整体完成。

### F6：输入解析异常归一化，输出结构化错误（A06/A07）

**修改：** `sdk/python/cpppools/cli.py`。
**测试：** `sdk/python/tests/test_cli_commands.py`。

- [x] 在 `TestStrictJsonInputs` 添加以下测试；分别从 literal、@file、stdin 进入。数据均小于 64 KiB，不应逃逸为 ValueError/RecursionError。

```python
def test_huge_integer_and_deep_json_are_input_errors(self):
    values = [
        '{"id":' + '1' * 4301 + '}',
        '{"id":' + '[' * 5000 + '0' + ']' * 5000 + '}',
    ]
    for value in values:
        with self.subTest(size=len(value)), self.assertRaises(cli.InputError):
            cli.parse_json_source(value)
```

- [x] `_parse_int` 在 int 转换前限制词法长度，保留精确 signed int64 边界；`_load_json_bytes` 仅在 JSON 解码调用周围捕获 ValueError/RecursionError 并转成 InputError。已有 InputError 保留；不捕获 MemoryError 或任意 Exception，不放宽解释器保护。

```python
def _parse_int(value):
    if len(value.lstrip("-")) > 19:
        raise InputError("integer is outside signed int64 range")
    number = int(value)
    if number < -(1 << 63) or number > (1 << 63) - 1:
        raise InputError("integer is outside signed int64 range")
    return number

# _load_json_bytes 的 json.loads 异常分支：
# except InputError: raise
# except (ValueError, RecursionError) as exc:
#     raise InputError("invalid JSON: %s" % exc) from None
```

- [x] health 和四种 CRUD 的 post-argparse InputError 分支复用已有 `_write_runtime_error`：

```python
except InputError as exc:
    _write_runtime_error("input_error", exc, 2)
    return 2
```

argparse 缺必需参数/未知参数保持原有 usage；不要将合法 health 的多节点结果或网络错误汇总改成输入错误。

- [x] 添加下列调用级测试，mock `_get_cluster_client` 和 `transport.request`，确认错误发生在 SDK 构造/网络前；覆盖空 data、重复 key、无效 URL、非正/非有限 timeout、空/非法 where、超大 payload。子进程再测超长整数和深嵌套，断言无 traceback。

```python
out, err = io.StringIO(), io.StringIO()
with mock.patch.object(cli, "_get_cluster_client") as client, \
     contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
    code = cli.main(["insert", "users", "-D", "{}", "-o", "json"])
self.assertEqual(code, 2)
self.assertEqual(out.getvalue(), "")
self.assertEqual(json.loads(err.getvalue()), {
    "kind": "input_error", "message": json.loads(err.getvalue())["message"],
    "exit_code": 2, "http_status": None,
})
client.assert_not_called()
```

- [x] 跑 CLI command/parser 测试；保留 int64 上下界、有限浮点、BOM、重复 key、64 KiB 以及畸形成功响应=5且不重发的既有断言。

```bash
PYTHONPATH=sdk/python python3 -W error -m unittest discover -s sdk/python/tests -p 'test_cli_*.py' -v
```

预期新增输入边界全部执行通过，stderr 单个 JSON 对象、stdout 空、退出 2。建议提交名 `fix: normalize cli input errors and json diagnostics`。

**F6 实施记录（2026-10-14）**：已将超长整数、过深 JSON 与 post-argparse 输入异常统一为 `InputError` 和结构化错误输出。JSON 结构深度上限为 256，避免不同平台解码器对深嵌套输入表现不一致；括号扫描忽略字符串内容。测试覆盖 literal、`@file`、stdin、精确 signed int64 上下界及双向溢出、解码异常范围、`MemoryError` 透传、health/CRUD 输入错误无网络/SDK，以及子进程无 traceback。WSL CLI command/parser 37/37 通过；Windows 原生解释器在当前环境不可用，之前记录的 Windows 32 项通过、4 项平台/服务用例跳过是在本次补充边界断言之前完成。独立质量评审通过，且其提出的边界覆盖建议已补齐。未提交。

### F7：运行实例幂等启动必须配置一致（A08）

**修改：** `sdk/python/cpppools/local_node.py`。
**测试：** `sdk/python/tests/test_local_node.py`；同步 `docs/cpl-cli-design.md` 实施记录。

- [x] mock 与真实测试分别覆盖：完全一致、另一配置路径、同路径内容改变、不同 workdir、不同 binary、starting、stopping、身份无法核验。比较 PID/launch 前后不变、Popen 总次数与 CLI 返回码；不能只断言 already_running 字段。
- [x] 配置摘要基于原始文件字节，不用 Python 对象 hash 或 JSON 重新序列化。`binary/config/workdir` 均规范为绝对真实路径；新增状态字段 `config_sha256`，CLI 按以下方式计算。

```python
import hashlib

def _config_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(65536), b""):
            digest.update(block)
    return digest.hexdigest()
```

- [x] 锁内核验旧状态/OS 身份。仅 `running` 且 `(binary,config,workdir,config_sha256)` 全匹配才返回 already_running=true/0。starting/stopping 或配置不同返回 start_error/6；活进程归属无法确认返回 identity_error/8；状态损坏返回 10。旧状态缺配置摘要时不推断相等、不自动接管，保留实例并说明无法确认。
- [x] 在 spawn 前与 ready 后复检配置字节摘要；变化按已有启动失败规则请求停止，给 10 秒清理期：已退出返回 6，仍活返回 7并保留 stopping。复用 F3/F5 条件提交和安全停止，不实现快照/热加载，不新增 C++ SHA-256 依赖。规范路径失败/文件读取失败按本机启动错误处理，不吞掉异常继续启动。
- [x] 运行本机测试，确认仅完全一致的 running 重复 start 成功，其余情况不生成第二个服务、不触碰不明身份进程。

```bash
PYTHONPATH=sdk/python CPL_TEST_SERVER="$PWD/build-accept-s0-s2/bin/cpppools_server" \
  python3 -W error -m unittest discover -s sdk/python/tests -p test_local_node.py -v
```

建议提交名 `fix: verify launch configuration before idempotent start`。

**F7 实施记录（2026-10-14）**：配置摘要按原始文件字节流式 SHA-256 计算，`config`、`binary`、`workdir` 统一规范为绝对真实路径。锁内只允许 OS 身份已核验且 `(binary, config, workdir, config_sha256)` 全匹配的 running 实例幂等复用；starting/stopping 或配置不匹配返回 6，活动进程身份不明返回 8，损坏摘要返回 10，旧 running 记录缺摘要时不接管并返回 6。spawn 前、锁内和 ready 后复核摘要；配置在启动期间变化时发布 stop 请求，进程退出后返回 6，10 秒仍未退出则返回 7 并保留 stopping 与请求证据。mock 与真实 C++ server/CLI 测试覆盖相同配置、其他路径、内容变化（包括非法 JSON）、workdir、binary、starting/stopping、身份未验证、旧摘要缺失、PID/launch 保持和不重复 Popen；还覆盖启动前阻止 spawn、ready 后成功清理及超时保持 stopping。WSL 组合回归 `test_local_node`、`test_cli_commands`、`test_cli_parser` 共 95/95 通过；`py_compile` 与 `git diff --check` 通过。独立质量评审通过。Windows SDK start 仍明确不支持，macOS 生命周期未验收，本记录仅覆盖 Linux/WSL 本批范围；未提交。

### F8：组合验收与阶段状态更新

**修改：** 验收报告、统一路线、本计划任务状态；必要的实现记录写回 CLI 方案。

- [x] Linux/WSL 从当前源码构建 OFF/OFF 并跑全量 CTest；重建 MySQL ON 并运行非 MySQL 基础回归。OFF/OFF 与 ON 基础 CTest 均 20/20 通过。
- [ ] MySQL ON 的真实自省和 CRUD 专项均已启动，但因未设置 `CPPPOOLS_TEST_MYSQL_USER` 明确 skip；缺少默认/NBE SESSION 下端到端证据，A01 不关闭。

```bash
cmake -S . -B build-accept-s0-s2 -DCMAKE_BUILD_TYPE=Debug \
  -DCPPPOOLS_USE_MYSQL=OFF -DCPPPOOLS_USE_REDIS=OFF
cmake --build build-accept-s0-s2 -j2
ctest --test-dir build-accept-s0-s2 --output-on-failure
cmake --build build-s0-mysql-wsl -j2
ctest --test-dir build-s0-mysql-wsl --output-on-failure -E test_mysql
ctest --test-dir build-s0-mysql-wsl --output-on-failure -R '^test_mysql_(introspection|crud)$'
```

- [x] 跑 Python 全量与全部可用 live：本机启停、真实 health/CRUD、控制协议、并发交错、退出恢复、重复启动和发行入口 smoke。WSL 本次 167 项中 165 项通过、2 项 Windows 专属用例按平台跳过；所需 Linux live 用例均实际执行。

```bash
PYTHONPATH=sdk/python CPL_RUN_E2E=1 \
  CPL_TEST_SERVER="$PWD/build-accept-s0-s2/bin/cpppools_server" \
  python3 -W error -m unittest discover -s sdk/python/tests -v
```

- [ ] Windows 原生服务与 MySQL 专项目标构建成功；`test_local_control` 1/1、socket live 8 项通过。MySQL 两专项缺账号 skip，慢读 deadline 因无法形成可证明的远端背压而 skip；全量原生 CMake 构建仍被既有 `test_http_server.cpp` 的 POSIX `<arpa/inet.h>` 阻断。因此 A01、A02 不关闭。
- [x] 检查 ready/stop 交错、旧 launch 覆盖、新旧 force/普通 stop、CLI 等待中退出再恢复，以及配置漂移清理中的重复 SIGTERM。WSL 全量套件覆盖前四类；新增真实进程联测验证最后一类。独立复核发现健康门闩与准备请求的窗口同为 5 秒，已将门闩延长到 20 秒；修正后该联测连续运行 3 次、3/3 通过。匹配终态、PID/launch/进程启动标识、revision 递增、退出码 0 和敏感控制文件清理均通过。
- [x] 在原验收报告追加“修复后复验”章节，保留原 RED 证据；逐项填写平台、通过/未验证，路线同步更新已验收范围。A01/A02 继续开放，S2 平台/发行残项继续保留。
- [x] 运行 `git diff --check`、文档相对链接和 Markdown fence 检查；保留原有未提交工作，不清理或合并无关文件。

## 4. 关闭问题的最小证据清单

| 问题 | 必需的新增通过证据 | 不足以关闭问题的证据 |
|---|---|---|
| A01 | 生产自省入口在默认/NBE SESSION 下完整执行，模式下 CRUD 文本往返；两类客户端构建 | 默认模式 CRUD、只探测转义 API、缺账号 skip |
| A02 | Windows 原生 delayed header/分片 body、慢读期限、空闲停止 | 即时 health=200、Linux-only HTTP 单测 |
| A03 | 双 start/迟到 ready/旧 launch/force 交错，最多一个 spawn、无状态回退 | 顺序 start/status/stop、文件原子替换 |
| A04 | 外部强杀不 graceful；accepted force=9；pending=10；超时=7；恢复一致 | PID 死亡、退出码 137、CLI 命令正常返回 |
| A05 | 早期握手、managed 重复信号、完整资源作用域之后的 matching 终态、写失败非成功 | 仅 HTTP drain 完成、marker 文件存在 |
| A06 | 三种输入来源超长整数/深嵌套都为 InputError/2，无 traceback/网络 | 普通 int64 越界测试 |
| A07 | health/CRUD post-argparse 输入错误有固定 stderr JSON、stdout 空、退出 2 | 仅退出码正确、文本报错 |
| A08 | 同配置 running 才幂等；路径/内容/状态不匹配拒绝且不重复 spawn | 单次真实启动、仅 PID 存活 |

恢复性原则：写不出证据或身份无法核验时保留可信记录，返回对应失败码；不能为了让测试全绿而降低归属、终态或发送期限要求。通过修复点的定向验证后做一次组合回归，不另加与这些缺陷无关的压测/风格整改。

## 5. S2 仍需单独完成的平台与发行门槛

以下是原方案尚未完成的范围，不纳入 A01–A08 修复关闭条件，也不能随本计划一起标记完成。

| 门槛 | 当前状态 | 后续顺序与通过要求 |
|---|---|---|
| macOS 本机生命周期 | Linux /proc 实现不能外推 | 修复批次验收后补平台身份查询/锁/启停，再跑 macOS 实机矩阵 |
| Windows 本机 start | 已完成：真实 start/status/stop、重复 start、显式 force、独立 CLI 进程、中文/emoji 状态目录通过 | 见验收报告 §11–12；Windows 前台真实终端结果不从 Linux PTY 外推 |
| Windows 控制目录安全 | 已完成本批：protected DACL、祖先目录句柄固定、创建/清理 junction 拒绝和 file-level reparse 实测 | 见验收报告 §12；自定义日志路径不属于控制目录防护范围 |
| 前台输出与终端验收 | Linux/WSL 已完成：running 后即时 stderr 提示，stdout 一份最终结果；真实 PTY Ctrl+C 与慢收尾通过 | 见验收报告 §10；Windows/macOS 前台验收依赖各平台本机启动，不外推为通过 |
| 发行版本来源 | 已完成：安装版读取 metadata，缺 metadata 才回退 0+dev | 两类入口与 metadata 在六项安装矩阵中一致，见验收报告 §9 |
| Windows console script smoke | 已完成：修正路径且源码树外实跑通过 | 六项安装矩阵实际调用 cpl.exe 与 -m |
| wheel/sdist、Python 3.8、补全资源 | 已完成本轮 Windows 发行与兼容性补验；三类补全随包交付 | 独立构建/重建和 3.12.3/3.8.10 六项安装/真实 CRUD 通过；Bash/PowerShell 候选、zsh 加载实跑。macOS 平台结果单独保留 |

F8 通过后可以确认 S0/S1 和 Linux 已实现本机生命周期的修复验收；只有上述承诺的平台/发行矩阵也关闭，才允许将 S2 标记为整体完成。后续 Redis/发现继续沿统一路线推进，不在此次修复计划中重复设计。

## 6. 计划自审与执行记录

- A01–A08 均有任务、文件、失败测试方向、修复规则和通过门槛；A04/A05 的生产者/消费者依赖已对齐。
- F1–F7 实施记录见各阶段章节；F8 受真实 MySQL 账号与 Windows 慢读环境限制而部分完成。本文记录的是未提交工作区状态，不代表已创建提交。
- 未新增远程停机 API、热加载、数据库取消能力或 Redis 功能；保留现有 SDK 路由/重试不变量。就绪前自有 Popen 失败仍使用原清理路径；就绪后的普通异常和前台中断不再触发该自动强杀路径。
- 优先级遵循实测影响：先有效部署/HTTP/停机正确性，后输入和幂等契约；平台残项如实列示，不重开已通过功能。

执行时为每个 F 任务记录：`实际 HEAD/提交 → RED 命令与断言 → 修改文件 → GREEN 命令/退出码/skip → 复验结论`。缺平台或真实服务时写“未验证”及具体缺失条件，不改成通过。

## 7. 整体复审补充任务与执行结果

问题与修复策略见 [F1–F11 整体评审与进度](completed-stages-overall-review-2026-10-14.md)。本节记录 F9–F11 的实现结论和验证结果。

| 顺序 | 问题 | 修复策略 | 当前状态 |
|---|---|---|---|
| F9 · P1 | R01 就绪后的异常清理强杀前台进程 | 前台 Ctrl+C 使用 10 秒认证停止；running 后异常保留登记；就绪前自有 Popen 回收沿用原路径。 | 实现、测试与评审完成（Linux/WSL） |
| F10 · P1 | R02 stopping 落盘但请求未送达后，普通续停无法补发 | 同代停止重试幂等补发认证请求；accepted force 保持单调且不改发普通请求。 | 实现、测试与评审完成（Linux/WSL） |
| F11 · P2 | R03 启动被取消或旧 stop 被新代替换，仍可返回成功 | start 取消返回 6 和当前同代状态；旧 stop 替换返回 10，不归并或改写新代。 | 实现、测试与评审完成（Linux/WSL） |

F9 → F10 → F11 及组合回归均已完成。`test_local_node` 与 `test_cli_commands` 共 92 项，79 项通过、13 项按平台/live 条件跳过；Python 全量 live 套件 171 项中 169 项通过、2 项 Windows 专属用例跳过。A01/A02 的环境验证与 S2 平台/发行残项继续保留。

上述测试计数来自修复批次，独立代码复审未发现验收阻断；本次仅同步文档进度。后续执行入口已转到 [统一路线 §2.6](implementation-roadmap.md#26-修复完成后的剩余工作顺序)，无需重新执行 F9–F11 开发。
