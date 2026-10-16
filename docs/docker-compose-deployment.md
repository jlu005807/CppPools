# Docker Compose 部署与远程 SDK/CLI 验收

CppPools 推荐的跨宿主机部署方式是运行一个 Linux 容器。Windows、Linux 和 macOS 只负责提供 Docker 引擎；`cpppools_server` 在容器内始终按 Linux 进程运行，因此本机 `cpl start/stop/status` 的 Windows/macOS 进程适配不属于容器运行路径。

## 职责边界

`docker compose up`、`docker compose stop` 和 `docker compose down` 负责容器生命周期。容器内入口直接执行 `cpppools_server --config /etc/cpppools/config.json`，Docker 的 SIGTERM 会传给服务，Compose 为优雅退出保留 15 秒。

Python SDK 和 `cpl` 只把 Compose 服务当作远程节点：使用 `health`、`select`、`insert`、`update`、`delete` 访问发布端口。不要在容器模式下调用 `cpl start` 管理容器，也不要把容器名当作宿主机进程 PID。

## 文件和持久化

- `docker/Dockerfile` 使用 Ubuntu 24.04 多阶段构建，关闭 MySQL/Redis 可选后端，最终镜像以非 root 的 `cpppools` 用户运行。
- `docker-compose.yml` 将配置只读挂载到 `/etc/cpppools/config.json`，将 SQLite 数据挂载到命名卷 `cpppools-data`。
- 默认端口绑定为 `127.0.0.1:8080`；改用 `CPPPOOLS_PORT` 时，挂载配置的 `cluster.nodes[0].port` 必须填写同一个宿主机端口，否则 SDK 会按服务发布的拓扑访问错误地址。
- 构建上下文使用仓库根目录的 `.dockerignore`；不要只修改 `docker/.dockerignore`。
- 示例镜像预置 `users(id INTEGER PRIMARY KEY, name TEXT NOT NULL)`，便于 smoke 测试。生产部署应使用自己的镜像或初始化数据库，不要把示例数据当成迁移机制。
- 命名卷保存数据库；删除卷会删除数据。普通 `docker compose down` 不删除卷，验收清理使用 `down -v` 时必须确认这是测试数据。

## 启停与远程验证

```bash
docker compose up -d --build
docker compose ps
curl --fail http://127.0.0.1:8080/health

PYTHONPATH=sdk/python python -m cpppools health \
  -u http://127.0.0.1:8080 -o json
PYTHONPATH=sdk/python python -m cpppools insert users \
  -u http://127.0.0.1:8080 -D '{"id":1,"name":"compose"}' -o json
PYTHONPATH=sdk/python python -m cpppools select users \
  -u http://127.0.0.1:8080 \
  -w '{"column":"id","value":1}' -o json

docker compose stop
docker compose down
```

测试或并行实例可使用独立 Compose project 和配置文件：

```bash
CPPPOOLS_PORT=49152 CPPPOOLS_CONFIG_FILE=/path/to/config-with-port-49152.json \
  docker compose --project-name cpppools-test up -d --build --wait
```

Compose 的 `healthcheck` 只表示容器内 HTTP 服务已经响应；SDK 的 health 命令还会验证宿主机到发布端口的可达性。多节点部署应为每个节点提供独立 Compose project、端口和数据卷，并在 `cluster.nodes` 中填写 SDK 实际可达的宿主机地址，不能填写只在 Compose 网络内部可解析的短服务名。

## 边界和安全

容器隔离不会自动解决数据库一致性、集群拓扑或 Redis 共享问题。每个节点仍读取自己的配置和数据库目标；容器重建不会复制 SQLite 数据。S3 已有可选 Redis RowCache 后端，但本 Compose 镜像关闭 Redis 选项且示例不启动 Redis；Redis 自启动/暖恢复和动态发现仍未接入。

本示例验收的是单节点 SQLite。多个独立 SQLite 卷不能充当同一实际数据库；生产集群仍需统一实际数据源，使用 MySQL 时还须另行启用 MySQL 后端构建并提供配置。本批没有将多节点数据一致性计为通过。

地址按使用方解释：数据库连接配置中的 `127.0.0.1` 指容器自身，访问其他容器使用 Compose 网络服务名，访问宿主机使用 Docker 对应的宿主机地址。`cluster.nodes` 则是发布给 SDK 的地址；本例 SDK 在宿主机运行，因此使用宿主机的 `127.0.0.1` 和发布端口。SDK 在其他主机或容器运行时，应配置它实际可达的地址，并相应调整默认仅 loopback 的端口绑定。SQLite 路径使用容器内路径 `/var/lib/cpppools/cpppools.db`。

## 测试和当前环境限制

静态契约测试位于 `sdk/python/tests/test_docker_compose.py`。设置 `CPL_RUN_DOCKER=1` 会执行真实镜像构建、随机宿主端口 `/health`、容器内非 root 检查、SDK/CLI health 与完整 CRUD、`stop` 退出码检查，以及保留命名卷后的重建恢复；测试使用唯一 project name，结束时执行 `down -v`。未设置时只运行不依赖 Docker 的定义测试。没有 Docker 引擎时必须报告为环境未验证，不能把静态 YAML 检查写成容器验收通过。

本轮 WSL Docker Engine 的 Compose 定义/隔离检查通过，但真实镜像 build 因 Docker Hub `ubuntu:24.04` 元数据请求超时阻断，未记为 live smoke 通过；Windows 主机本身没有 Docker Desktop。容器内服务仍统一按 Linux 运行。Redis RowCache 的 S3 实现通过独立 Windows ON/ OFF CTest，Compose 示例本身仍关闭 Redis。
