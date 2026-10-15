# Python SDK 文档（`sdk/python/`）

零第三方依赖（纯 stdlib）的 CppPools 集群客户端。负责：建环、直连 owner、
307 跟随、黑名单/attempt 绕行、epoch 门控、错误分类重试、配置摘要比对。

设计依据：`docs/superpowers/specs/2026-08-11-cluster-cache-design.md` §7。
服务端契约见 `docs/api/backend_api.md`。

---

## 1. 快速开始

```python
import sys
sys.path.insert(0, "sdk/python")
from cpppools.client import ClusterClient
from cpppools import errors

client = ClusterClient(["http://10.0.0.1:8080", "http://10.0.0.2:8080"])

client.insert("users", {"id": 42, "name": "alice"})
print(client.select("users", {"column": "id", "value": 42}))
client.update("users", {"name": "bob"}, {"column": "id", "value": 42})
client.delete("users", {"column": "id", "value": 42})
```

- 启动时**强制**从 seed 拉拓扑（逐 seed 重试，全失败抛 `TopologyError`，
  无默认兜底）；同时拉 schema（定 insert 路由键）并比对各节点
  `datasource_digest`（不一致记入 `client.digest_warnings`，不阻断）。
- 运行期每 `topology_refresh`（默认 30s）懒刷新；**刷新失败继续用旧环**
  + `topology_refresh_failures` 计数（不报错）。

Docker Compose 部署时，SDK 仍按上述 HTTP 拓扑工作：seed 使用可达的宿主发布端口，
`cluster.nodes` 必须发布 SDK 同样可达的地址/端口。Compose 管容器启停，远程
SDK/CLI 管 health 和 CRUD；具体操作见 [Compose 部署说明](../docker-compose-deployment.md)。

## 2. 构造参数（默认值规范性）

| 参数 | 默认 | 说明 |
|---|---|---|
| `seeds` | —（必需） | 任一可达节点即可 bootstrap 全拓扑 |
| `http` | 内置 urllib | 可注入自定义传输（测试/替换 requests） |
| `retry_max` | 3 | 仅对可重试操作（§4 矩阵） |
| `attempt_max` | 5 | 环上绕行次数上限 |
| `backoff_base` | 100ms | 指数退避基数 |
| `backoff_jitter` | ±25% | **必须有抖动**：无抖动的同步退避在恢复时造成归队风暴 |
| `topology_refresh` | 30s | 懒刷新间隔；epoch 不符时立即刷新（不等） |
| `connect_timeout` | 1s | v1 保留签名；urllib 单一超时，实际以 read_timeout 为准 |
| `read_timeout` | **3s** | 必须 > 服务端 `db_acquire_timeout`（100ms），否则会把"服务端正常返回 503"误判为超时并拉黑健康节点 |
| `client_id` | 环境变量 `CPPPOOLS_CLIENT_ID` → 生成 | `X-Client-Id` 头，排障对账用 |

黑名单 TTL 不由客户端配置：取自服务端 `/api/v1/cluster` 下发
（不变量 I1：`blacklist_ttl ≥ cache_ttl`）。

## 3. 路由与故障语义

- **建环**：本地建环，参数（哈希算法/vnodes/TTL）全部取自
  `/api/v1/cluster` —— 与服务端逐字同口径，由 spec §3.2 黄金向量钉死。
- **路由键**：`(datasource, table, 唯一键列, 键值)`。`datasource` 取
  请求显式值或空串（与服务端同口径）。insert 的路由键取 schema 列序
  第一个"给了值且非 NULL"的唯一列；无显式唯一键的 insert 不路由。
  REAL 键列不参与路由。
- **直连 owner**：稳态下请求直达 owner，无 307；收到 307（环视图滞后
  窗口）自动跟随 `Location`，不计入重试。
- **绕行**：节点不可达（拒连/超时/响应半断）→ 拉黑（服务端下发的 TTL）
  → attempt+1 查环（跳过黑名单，扫描覆盖全部节点）→ 携带
  `X-CppPools-Fallback: 1` 就地处理。
- **全黑降级**：全部节点在黑名单时清空重试一轮再抛
  `AllNodesBlacklistedError`（避免永久拒服务）。
- **回切**：黑名单过期后回到原 owner 的首个请求带
  `X-CppPools-Rejoin: 1`（服务端清缓存加速收敛，best-effort）。
- **epoch 门控**：每请求携带 `X-CppPools-Epoch`；响应回显不符 → 立即
  刷新拓扑。本次操作**不重发**（响应即结果；写重发会撞 409/重复写入）。
- **线程安全**：拓扑状态原子替换（读者无锁）；黑名单/Rejoin 状态加锁。

## 4. 错误分类矩阵

| 异常 | 触发 | SDK 行为 |
|---|---|---|
| `ClientError` | 400 / 其余 4xx | 不重试，直接上抛 |
| `ConflictError` | 409 唯一冲突 | 不重试，交调用方判定（显式键 insert 重试撞 409 = 已生效） |
| `UnavailableError` | 503 池超时 | 同一节点退避重试（限 `retry_max`），仍败上抛 |
| `ServerError` | 其余 5xx | 同上 |
| `NodeUnavailable` | 拒连/超时/DNS/响应半断（IncompleteRead） | 拉黑该节点 + attempt 绕行 |
| `AllNodesBlacklistedError` | 全黑且清空重试一轮仍败 | 上抛 |
| `TopologyError` | 启动期拓扑全 seed 失败 / 文档畸形 | 构造失败 |

**自增 insert（无显式唯一键）非幂等：任何失败一律上抛，不自动重试。**

## 5. 已知限制（v1）

- 无连接复用（urllib 每请求新建 TCP）：keep-alive 留待引入 requests 的
  v1.1（可经 `http` 参数提前注入自定义传输）。
- 故障窗口内写到替补节点的数据，owner 归队后读 owner 库读不到
  （spec §5.1 例外②，最终一致 ≤ TTL）——这是单 owner 模型的已记录契约，
  不是 SDK 缺陷。
- `connect_timeout` 与 `read_timeout` 暂共用 urllib 单一超时。

## 6. 测试

```bash
python3 -m unittest discover -s sdk/python/tests        # 单测（fake 集群）
bash scripts/sdk_e2e.sh                                 # 三节点 live e2e
python3 sdk/python/examples/e2e_crud.py                 # 单节点 CRUD 往返（自管 server）
```

- `test_hash_ring.py`：spec §3.2 黄金向量（与 C++ 端跨语言一致）、环归属、
  tie-break、回绕。
- `test_transport.py`：错误分类矩阵、307-as-Response、网络故障归类。
- `test_client.py`：fake 集群驱动的路由/绕行/全黑/Rejoin/epoch/畸形拓扑。
- `e2e_live.py`：三节点稳态（0 个 307 = 两端环一致的硬证据）、杀节点
  Fallback、复活回切、epoch 优雅降级、归队清缓存、滚动重启窗口内
  持续流量零失败。
- `examples/e2e_crud.py`：单节点自管 server 子进程，跑 insert/select/update/
  delete 往返 + 唯一冲突(409)/缺必填(400)/0 行(200) 映射断言（需先构建 server）。

## 7. 示例与依赖

- 最简 CRUD 用法见 `sdk/python/examples/crud_demo.py`：
  ```bash
  scripts/run_server.sh                                   # 启动单节点
  python3 sdk/python/examples/crud_demo.py                # 跑一遍增删改查
  ```
- 依赖见 `sdk/python/requirements.txt`：运行时**零第三方依赖**（纯 stdlib），
  与服务端「自研 Socket HTTP、不引入第三方 HTTP 库」的口径一致；
  仅当显式注入基于 `requests` 的传输（v1.1 keep-alive 路线）时才需 `requests`。
