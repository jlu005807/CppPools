# third_party

存放需要随仓库分发（vendored）的第三方依赖。

## 当前依赖

### nlohmann/json — v3.11.3

- 文件：`nlohmann/json.hpp`（单头文件，约 2.4 万行）
- sha256：`9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6`
- 获取方式：`git clone --depth 1 --branch v3.11.3 https://github.com/nlohmann/json.git`，
  取 `single_include/nlohmann/json.hpp`。升级时重新执行并更新本节指纹。
- 用途：DB 中间件改造的配置文件与请求体解析（**当前代码尚未引用**，
  为下一阶段预留；引用时只在 `.cpp` 中 include，不要进公开头文件，
  否则每个 TU 都要重复编译这 2.4 万行）。
- CMake 接线：`CMakeLists.txt` 的 `nlohmann_json` INTERFACE 库；
  手工构建（`output/t.sh`）已加 `-Ithird_party`。

### 其余能力仍是零依赖

| 能力 | 实现方式 |
|---|---|
| HTTP/1.1 服务 | 直接基于 BSD socket / Winsock 手写，不用第三方 HTTP 库 |
| JSON 输出（现状的 /api 响应） | 手写 `ostringstream` 拼装；解析能力由 nlohmann/json 提供 |
| 单元测试 | 标准库 `assert` + CTest，不引入 GoogleTest / Catch2 |
| 线程与同步 | C++17 标准库（`thread` / `mutex` / `shared_mutex` / `condition_variable`） |

外部**链接型**依赖（不 vendored 进本目录）：

- **MySQL C API**（可选，`-DCPPPOOLS_USE_MYSQL=ON`）：`find_path` / `find_library` 在系统路径查找
- **SQLite3**（DB 改造的第二后端）：系统 SDK 或 brew 提供，`-lsqlite3`

## 何时该往这里放东西

只有当依赖满足以下条件时才考虑 vendoring：

- 无法通过系统包管理器或 CMake `find_package` 可靠获取；
- 需要锁定到特定提交以保证可复现构建；
- 体积小到适合入库。

否则优先用 `find_package` / FetchContent，避免仓库膨胀与升级困难。
