# 本地 Redis 安装与验证记录（2026-10-15）

## 实例

- 安装根目录：`D:\CppPools\redis`
- Redis 版本：`8.10.2`（`taizod1024/redis-windows-fork` portable zip）
- 发布包 SHA-256：`98b5bff78933511ad5866423e8176251f157ef96c950c60ead968bb1a90ac429`
- 可执行文件：`D:\CppPools\redis\Redis-8.10.2-Windows-x64-msys2\redis-server.exe`
- 配置文件：`D:\CppPools\redis\redis.conf`
- 数据目录：`D:\CppPools\redis\data`
- 日志文件：`D:\CppPools\redis\logs\redis.log`
- 监听地址：`127.0.0.1:6379`
- 认证：已配置 `requirepass`（密码按用户要求设置为 `123456`）
- 持久化：AOF `appendfsync everysec`，同时保留 RDB 快照

该实例是独立前台子进程，不是 Windows 服务，也不是 CppPools 自动启动功能的验收；当前进程在验证后保持运行，停止前应按 PID/可执行路径核对，不要按进程名批量终止。

## 实测证据

| 验证 | 结果 |
|---|---|
| 未认证 `PING` | `NOAUTH Authentication required.` |
| `AUTH 123456` 后 `PING` | `PONG` |
| `SET` / `GET` | `ok` 原值读回 |
| `SET ... PX 1200` | 约 1182 ms 后 `EXISTS` 返回 `0` |
| AOF 状态 | `aof_enabled:1` |
| 受控停止 | `SHUTDOWN SAVE`，端口释放，日志记录正常退出 |
| 重启恢复 | 重启后 `PING=PONG`，持久化键仍读回 `ok` |
| 启动日志 | `DB loaded from append only file`，`Ready to accept connections tcp` |
| 错误密码 | `WRONGPASS`，随后仍要求认证 |

## 与 12 项遗留项的关系

本次只证明了本地 Redis 实例本身可用，不改变项目阶段状态：

1. A01：仍缺真实 MySQL SQL mode 自省和 CRUD 的完整账号/运行证据。
2. A02：仍缺 Windows 慢读客户端总发送 deadline 的可靠远端背压证据。
3. Windows Python `LocalNodeManager.start`：源码仍明确不支持。
4. macOS 本机生命周期：当前环境未验收。
5. Windows ACL 和句柄级 TOCTOU：仍未完成。
6. Windows file-level reparse：仍缺独立实测；已有 directory junction 证据不能替代。
7. 前台真实终端 Ctrl+C：仍缺独立真实终端验收。
8. wheel/sdist 独立重建和安装矩阵：仍未完成。
9. Python 3.8 smoke：仍未完成。
10. 静态 shell completion：仍未交付。
11. Redis RowCache：仍未接入；当前 CppPools 默认仍使用内存缓存。
12. 动态发现和 discovery CLI：仍未实现。

## CppPools 构建边界

仓库当前 `CPPPOOLS_USE_REDIS` 仍是可选基础 transport 开关，默认关闭；源码尚未实现 Redis RowCache、故障回退、自动启动、恢复命名空间或动态发现。因此本地 Redis 的可用性不能写成 S3/S4 已完成，也不能把 `CPPPOOLS_USE_REDIS=ON` 在缺少 hiredis 时的构建状态写成已验证。

## 后续复验（同日）

- Redis 进程仍由 `D:\CppPools\redis\...\redis-server.exe` 提供，Windows 监听 PID 为 `56508`，认证 `PING` 返回 `PONG`，AOF 状态为 `aof_enabled:1`。
- 新建 Windows CMake 目录 `build-redis-verify-off2`，显式使用本机 SQLite 依赖后，核心库、`cpppools_server` 和 `test_local_control` 均成功编译；`test_local_control` CTest 为 `1/1` 通过。
- Windows 完整测试构建仍被既有 `test/test_http_server.cpp` 对 POSIX `<arpa/inet.h>` 的依赖阻断；这不是 Redis 失败证据。
- `CPPPOOLS_USE_REDIS=ON` 配置按预期在 `find_path(hiredis/hiredis.h)` 处失败，说明当前环境没有 hiredis，不能声称 ON 构建或真实 CppPools Redis transport 已验收。
- 直接启动新构建的 `cpppools_server.exe` 返回 Windows 缺少运行时 DLL 的错误码 `-1073741515`；服务端二进制本身已链接生成，但该独立运行时 smoke 仍未关闭 Windows 发布矩阵问题。
