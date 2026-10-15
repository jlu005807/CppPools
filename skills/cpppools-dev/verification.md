# 构建与运行验证

## Docker Compose 验收入口

容器部署使用仓库根的 `docker-compose.yml`。先运行 `docker compose config --quiet`，再用 `CPL_RUN_DOCKER=1 PYTHONPATH=sdk/python python3 -W error -m unittest sdk.python.tests.test_docker_compose -v` 执行真实构建、容器 health、非 root 用户、SDK/CLI CRUD、优雅 stop 和命名卷恢复。测试用唯一 project、随机宿主端口与匹配的 `cluster.nodes` 临时配置，结束时只清理自己的卷。无引擎时运行静态测试并明确 live skip，不能用宿主机进程代替容器证据。

2026-10-15，`98dd98e` 上的未提交修改在 WSL Ubuntu 实测 Docker Engine 29.1.3 / Compose 2.40.3：Compose 定义/隔离/真实容器验收共 5/5；WSL C++ `build-accept-s0-s2` 构建与 CTest 20/20。Windows Anaconda Python 3.12.3（`D:/Anaconda/python.exe`）`-W error` 全量 202 项中 159 通过、43 跳过，包含真实生命周期 7 项、Windows 控制文件 5 项；WSL Python live 全量 202 项中 179 通过、23 跳过，均退出 0。中文/emoji 状态目录由真实失败修复后通过。当前 macOS 本机生命周期、A02 总期限负对照仍开放；Windows 全量原生 CTest 受 `test_config.cpp` 的 POSIX API 阻断，服务/control 专项构建不代表全部原生目标通过。详细证据与范围见 [验收报告 §12](../../docs/completed-stages-acceptance-2026-10-14.md#12-docker-compose-远程验收与回归修复2026-10-15)。

所有命令从仓库根运行；Bash 命令用于 Linux/WSL。需要 C++17 编译器、CMake
3.16+、SQLite 开发库和 Python 3.8+。不要混用 Windows 与 WSL 的 CMake 构建目录。

## 当前真实回归命令

```bash
cmake --build build-accept-s0-s2 --parallel 2
ctest --test-dir build-accept-s0-s2 --output-on-failure
PYTHONPATH=sdk/python CPL_RUN_E2E=1 \
  CPL_TEST_SERVER="$PWD/build-accept-s0-s2/bin/cpppools_server" \
  CPL_PACKAGING_WHEELHOUSE="$PWD/output/release-wheelhouse" \
  python3 -W error -m unittest discover -s sdk/python/tests
PYTHONPATH=sdk/python CPL_RUN_DOCKER=1 \
  python3 -W error -m unittest sdk.python.tests.test_docker_compose -v
```

本机 Windows 定向/组合验证使用以下入口；构建目录和 PATH 指向本机实存工具，其他机器按实际安装位置替换：

```powershell
$env:PATH='D:/Anaconda/envs/equipment_tool/Library/bin;D:/winlibs/mingw64/bin;'+$env:PATH
cmake --build build-redis-verify-off2 --target cpppools_server test_local_control --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'native build failed' }
build-redis-verify-off2/bin/test_local_control.exe
if ($LASTEXITCODE -ne 0) { throw 'control test failed' }
$env:PYTHONUTF8='1'
$env:PYTHONPATH='sdk/python'
$env:CPL_RUN_E2E='1'
$env:CPL_TEST_SERVER=(Resolve-Path build-redis-verify-off2/bin/cpppools_server.exe).Path
D:/Anaconda/python.exe -W error -m unittest discover -s sdk/python/tests
```

Windows 主机未安装 Docker Desktop；本轮 Docker 引擎位于 WSL Ubuntu，真实入口为 `wsl.exe -u root -e sh -c 'cd /mnt/d/Internt_of_Thing/CppPools && PYTHONPATH=sdk/python CPL_RUN_DOCKER=1 python3 -W error -m unittest sdk.python.tests.test_docker_compose -v'`。这不等于 Windows PATH 已有 `docker`。本批最终修改已再次独立构建 wheel/sdist/重建 wheel，Windows Python 3.12.3/3.8.10 六项源码树外安装与真实 CRUD 矩阵通过（132.883 秒，退出 0）；入口见验收报告 §9、§12，不能只运行源码 help 代替。

真实 Windows Python 3.8.10 在同样设置 `CPL_RUN_E2E=1`、原生 `CPL_TEST_SERVER` 和离线 wheelhouse 后也运行了最终源码全量：202 项中 159 通过、43 跳过（47.758 秒），退出 0，包含 Windows 控制文件及本机生命周期。仅安装包远程 CRUD 通过不足以证明新增 Win32 路径代码兼容最低版本。

下面保留 2026-10-06 的基础验证及当时环境限制；其中测试数、未运行平台和依赖缺失不代表当前状态。当前构建仍存在既有编译告警，WSL 挂载目录有 clock-skew 告警，不声明零告警构建。

## 历史基础验证（2026-10-06）

2026-10-06，代码 HEAD `6c554dd`，WSL Ubuntu，GCC 13.3.0、CMake 3.28.3、
Python 3.12.3、SQLite 开发库 3.45.1：

```bash
cmake -S . -B build-skill-verify -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCPPPOOLS_USE_MYSQL=OFF
cmake --build build-skill-verify -j2
ctest --test-dir build-skill-verify --output-on-failure
python3 -m unittest discover -s sdk/python/tests
```

全部命令退出码 0：CTest 18/18，SDK 60/60。构建有 GCC 的
`-Wmaybe-uninitialized`（variant/缓存投影）、HTTP `-Wsign-conversion`
和未使用函数告警，因此不能描述为零告警。

## 历史真实服务验证（2026-10-06）

现有 `sdk/python/examples/e2e_crud.py` 自建临时数据库和配置、启动真实
服务并执行 5 项 CRUD/错误映射断言；但 SERVER 硬编码到 build/bin。
现有 `sdk/python/tests/e2e_live.py` 的 run_flow 支持传入可执行文件和
独立工作目录，不依赖 sqlite3 CLI。自定义构建目录可在 Python 中加载
模块，分别设置 SERVER 与调用 run_flow；勿把二进制覆盖到已有 build。

本次通过一次临时 Python 驱动加载这两个现有模块，使用
build-skill-verify/bin/cpppools_server：单节点 5/5；三节点端口
18511–18513，稳态 30 键写读、杀节点绕行、归队清缓存 flush=1、滚动
epoch 收敛到 3，窗口内 42 次读写零失败，退出码 0。驱动与临时数据已清理。
这些流程使用独立数据库，验证的是路由和缓存流程，不证明数据库复制。

## 脚本前提与历史环境限制

- build.sh、run_server.sh、cluster_e2e.sh、sdk_e2e.sh 均假设 Bash，
  后三者还需要 build/bin 产物；建演示库及 shell 集群 e2e 需要 sqlite3 CLI，
  集群 e2e 还需 curl。WSL 本次没有 sqlite3 CLI；开发库可用不代表 CLI 已装。
- 当前 Windows 工作区 .sh 为 CRLF，实测 bash scripts/build.sh --help
  报 `set: pipefail\r: invalid option name`。使用 LF 工作区后才能直接运行；
  未修改项目脚本，本次没有把 shell e2e 记为通过。
- run_server.sh 会新建不存在的演示数据库，并固定生成 SQLite 配置。要启用
  集群/缓存，直接用服务二进制 --config 指定完整 JSON。该脚本 exec 服务后
  EXIT trap 不能保证临时配置清理；不要假定运行后毫无残留。
- 2026-10-06 MySQL ON 因缺开发库未运行；2026-10-15 已用本机 MySQL
  8.0.11 完成 ON 构建、默认/空/NBE SESSION 自省和 CRUD，见验收报告 §8。
  其他环境配置 MYSQL_INCLUDE_DIR、MYSQL_LIBRARY；专项测试需
  CPPPOOLS_TEST_MYSQL_USER 及 HOST/PORT/PASSWORD，账号须能建删测试库。
  缺 USER 时测试内返回 0 跳过，必须查看输出，不能只看 CTest 全绿。
- loadtest_sdk.py 使用固定端口 18401–18403，覆盖 output/loadtest 的测试库；
  运行前确认目录与端口可用。本次未执行压测、Windows 原生构建、ASan/TSan。

测试计数只对该提交与环境有效。新改动应重新执行相应验证并保留真实退出码；
通过 tail 等管道记录日志时使用 pipefail，避免掩盖失败。
