# 构建与运行验证

所有命令从仓库根运行；Bash 命令用于 Linux/WSL。需要 C++17 编译器、CMake
3.16+、SQLite 开发库和 Python 3.8+。不要混用 Windows 与 WSL 的 CMake 构建目录。

## 实际执行过的基础验证

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

## 真实服务验证入口

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

## 脚本和可选环境的前提

- build.sh、run_server.sh、cluster_e2e.sh、sdk_e2e.sh 均假设 Bash，
  后三者还需要 build/bin 产物；建演示库及 shell 集群 e2e 需要 sqlite3 CLI，
  集群 e2e 还需 curl。WSL 本次没有 sqlite3 CLI；开发库可用不代表 CLI 已装。
- 当前 Windows 工作区 .sh 为 CRLF，实测 bash scripts/build.sh --help
  报 `set: pipefail\r: invalid option name`。使用 LF 工作区后才能直接运行；
  未修改项目脚本，本次没有把 shell e2e 记为通过。
- run_server.sh 会新建不存在的演示数据库，并固定生成 SQLite 配置。要启用
  集群/缓存，直接用服务二进制 --config 指定完整 JSON。该脚本 exec 服务后
  EXIT trap 不能保证临时配置清理；不要假定运行后毫无残留。
- MySQL ON 配置实测失败：缺少客户端头文件/库。没有执行 MySQL 编译或连接
  测试。可配置 MYSQL_INCLUDE_DIR、MYSQL_LIBRARY 后重试；专项测试需
  CPPPOOLS_TEST_MYSQL_USER 及 HOST/PORT/PASSWORD，账号须能建删测试库。
  缺 USER 时测试内返回 0 跳过，必须查看输出，不能只看 CTest 全绿。
- loadtest_sdk.py 使用固定端口 18401–18403，覆盖 output/loadtest 的测试库；
  运行前确认目录与端口可用。本次未执行压测、Windows 原生构建、ASan/TSan。

测试计数只对该提交与环境有效。新改动应重新执行相应验证并保留真实退出码；
通过 tail 等管道记录日志时使用 pipefail，避免掩盖失败。
