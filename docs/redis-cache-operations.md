# 节点本地 Redis 缓存运维说明

本文是[节点本地 Redis 行缓存设计](redis-cache-design.md)的操作摘要。S3 已实现可选 Redis 行后端的最小 `GET/SET PX/DEL` 与一次性冷内存回退；本文中关于自启动、持久化恢复、暖复用和真实服务端运维的内容仍是 S4 后续目标，不代表当前程序已经支持。

## 最小配置

```json
{
  "cache": {
    "enabled": true,
    "backend": "auto",
    "ttl_ms": 60000
  }
}
```

`backend` 缺省时保持现有内存缓存。只有显式写入 `auto` 才尝试连接或启动本节点专用 Redis。Redis 不可用时，服务退回空的内存缓存；本次进程不会反复重连 Redis。

跨 CppPools 重启默认冷启动。只有能证明同一实际数据库由单个 CppPools 写入进程按顺序接力时，才可以显式设置：

```json
{
  "cache": {
    "enabled": true,
    "backend": "auto",
    "ttl_ms": 60000,
    "warm_reuse": true
  }
}
```

`warm_reuse` 不适用于多节点、多个重叠写入进程、外部写入者或无法确认写入者的部署。`source journal` 只能发现部分本机启动介入，不能证明所有数据库写入已经被观察。

## Redis 实例归属

自动启动只使用本节点登记的回环地址、端口和数据目录。程序不会扫描其他端口，不会接管未知 Redis，不会对外部实例执行 `CONFIG SET`、`FLUSHDB`、`SHUTDOWN` 或强制终止。

CppPools 正常停止后，本节点专用 Redis 默认继续运行，以便后续显式暖复用或下一次冷启动使用。停止 Redis 时，必须通过操作系统服务管理工具或实例身份记录核对后操作；不能按端口、进程名或 PID 文件直接杀进程。

同一台机器运行多个 CppPools 节点时，每个节点必须使用独立端口和独立数据目录。只使用不同 Redis database 编号不能替代实例隔离。

## 失败处理

| 场景 | 行为 |
|---|---|
| Redis transport 未编译（`CPPPOOLS_USE_REDIS=OFF`） | 使用现有内存缓存；OFF 构建不包含 Redis 依赖 |
| `redis-server` 未安装 | 记录 `not_installed`，使用内存 |
| 端口被未知进程占用 | 记录 `foreign_instance`，不修改外部进程，使用内存 |
| Redis 启动超时 | 记录 `startup_timeout`，只清理能证明由本次启动的失败进程 |
| Redis 运行期间断连、超时、OOM 或权限变化 | 记录一次故障，切换为空内存缓存；后续请求不继续尝试 Redis |
| 状态清单无法安全废弃或持久化 | 启动失败，不能退回并留下可复用的旧恢复资格 |
| SQL 已提交但缓存失效失败 | 返回原 SQL 结果；缓存命名空间进入冷状态，不能把请求重试为第二次写入 |

Redis AOF/RDB 恢复后，即使键仍然存在，首版也不直接复用。Redis `run_id` 变化、清单损坏、异常退出、停机排空超时、数据库目标或 schema 变化都会使下一次启动冷缓存。

## 发布前检查

实施 Redis 版本前，按以下顺序验证：

1. 使用旧配置构建 OFF 版本，确认原内存缓存和 SDK 回归通过。
2. 使用显式 `backend: auto`、`warm_reuse` 缺省运行，确认 Redis 不可用时服务仍能启动并回退内存。
3. 使用 `auto_start: false` 连接隔离的外部 Redis，验证命中、TTL、写后失效和 Redis 故障切换。
4. 在 Linux/WSL 验证本地 Redis 自启；Windows 原生先验证 OFF 构建和显式连接，不把 WSL 自启结果当作 Windows 原生证据。
5. 只有所有阶段通过后，才在同一真实数据库上进行集群 owner/Fallback/重启验收。

测试实例必须使用临时数据目录、动态端口和独立 key 前缀。清理时先核对实例身份，不使用 `kill-all`、进程名匹配或全库清空。
