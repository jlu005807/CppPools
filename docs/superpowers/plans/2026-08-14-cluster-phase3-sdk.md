# 期3 实施计划：Python SDK（建环/绕行/黑名单/epoch/307）

**Goal:** 交付 spec §7 的 Python SDK 第一版：启动拉拓扑建环、路由直连 owner、307 跟随、黑名单/attempt 绕行/Fallback 头、epoch 头与不匹配即刷新、错误分类重试、digest 比对告警、Rejoin 头；黄金向量跨语言钉住哈希口径。

**关键决策：**

| # | 决策 | 理由 |
|---|---|---|
| D1 | 零第三方依赖：纯 stdlib（urllib），内置纯 Python murmur3_32（~40 行 reference 实现） | 系统 Python 无 mmh3/requests；黄金向量测试钉住字节一致 |
| D2 | 连接复用偏离 spec 的 `requests.Session`：urllib 无 keep-alive，首版接受每请求新连接 | spec §7.2 该条的性能理由留待引入 requests 的 v1.1；在计划与 SDK 注释留痕 |
| D3 | 目录 `sdk/python/`：`cpppools/`（包）+ `tests/`（unittest，零依赖）| 与 C++ 主体分目录；`python3 -m unittest discover sdk/python/tests` 运行 |
| D4 | 路由键 D1 口径（§3.3 修订）：显式 datasource 或空串；insert 键 = **启动期缓存的 schema**（GET /api/v1/schema）里"列序第一个给了值且非 NULL 的唯一列"——与服务端 handler 逐字同规则 | 两端同键是单 owner 契约的根；schema 启动拉一次 + 拓扑刷新时重拉 |
| D5 | 错误分类矩阵（§5.5/§7.2）：400 不重试抛 ClientError；409 抛 ConflictError 交调用方；307 跟随不计重试；503/连接失败/超时 → 拉黑 + attempt 绕行 + 退避重试；自增 insert（无显式唯一键）失败一律上抛不重试 | 幂等矩阵逐行落地 |
| D6 | 黑名单：`node → 到期时刻`；attempt 查环跳过黑名单；全黑 → 清空重试一轮再抛（§7.2）；过期回切首请求带 `X-CppPools-Rejoin: 1`（§5.2 规则 B）| TTL 取服务端下发（I1） |
| D7 | epoch 头每请求携带；响应头 epoch 不符 → 立即刷新拓扑（不等 30s）重试一次 | §5.4 |
| D8 | digest 比对：拉拓扑时对全节点 GET /api/v1/cluster 的 datasource_digest 比对，不一致打 warning 不阻断（§6.3）—— 首版只对 seed 可达节点比对 | 无强制手段的结构约束给检测 |

---

### Task 1: hash + ring（黄金向量钉死）

**Files:** `sdk/python/cpppools/hashing.py`、`sdk/python/cpppools/ring.py`、`sdk/python/tests/test_hash_ring.py`

- [ ] RED：test_hash_ring.py —— spec §3.2 黄金向量 V1-V7 逐条（V1/V2 vnode、V3/V4/V5 query、V7 tail=3、V8 碰撞对 tie-break）、环归属 V6、无符号排序、回绕
- [ ] GREEN：hashing.py（murmur3_32 seed=0 无符号、lp/le32/vnode_key/query_key）+ ring.py（100 vnodes、升序 lower_bound、碰撞 tie-break (node_id,index) 字典序、owner(attempt) 跳过给定集合）
- [ ] 跑绿 + 提交 `feat(sdk): murmur3 哈希与一致性环（对黄金向量跨语言一致）`

### Task 2: 传输层与错误分类

**Files:** `sdk/python/cpppools/errors.py`、`sdk/python/cpppools/transport.py`、`sdk/python/tests/test_transport.py`

- [ ] RED：可注入的 fake opener 上断言错误分类矩阵（400/409/503/超时/连接失败 → 各自异常类型与可重试性）、307 响应对象带 Location
- [ ] GREEN：errors.py（CppPoolsError/ClientError/ConflictError/ServerError/UnavailableError/AllNodesBlacklistedError）+ transport.py（urllib POST JSON、超时拆分 connect/read、不自动跟随重定向、返回 status/headers/body）
- [ ] 跑绿 + 提交 `feat(sdk): 传输层与错误分类矩阵（400/409/307/503/超时）`

### Task 3: Client（路由/黑名单/epoch/digest/schema）

**Files:** `sdk/python/cpppools/client.py`、`sdk/python/cpppools/__init__.py`、`sdk/python/tests/test_client.py`

- [ ] RED：fake 集群驱动（可编程响应的假节点）断言：
  - 启动强制拉拓扑（全失败抛异常）；刷新失败用旧环
  - 路由直连 owner（请求打到的节点 = 环 owner）
  - owner 超时 → 拉黑 → attempt+1 打替补 + Fallback 头
  - 全黑 → 清空重试一轮 → 仍败抛 AllNodesBlacklistedError
  - 黑名单过期回切带 Rejoin 头
  - 响应 epoch 不符 → 立即刷拓扑 + 重试
  - 409/400 按矩阵；自增 insert 503 不重试、显式键 insert 503 重试
  - digest 不一致产生告警记录
- [ ] GREEN：client.py（ClusterClient：select/insert/update/delete、拓扑缓存+定时刷新、环原子替换、黑名单锁、schema 缓存定 insert 路由键、头注入 X-Client-Id/X-CppPools-Epoch/Fallback/Rejoin）
- [ ] 跑绿 + 提交 `feat(sdk): ClusterClient——路由直连/黑名单绕行/epoch 门控/digest 比对`

### Task 4: 3 节点 e2e + 评审

**Files:** `scripts/sdk_e2e.sh`（起 3 个 cpppools_server + 跑 Python e2e 断言脚本 `sdk/python/tests/e2e_live.py`）

- [ ] 断言：路由分布落对节点（直接打 owner 无 307）；杀 owner → SDK 绕行替补成功（Fallback）；复活 + 黑名单过期 → 回切 owner 且带 Rejoin；滚动重启期（改 epoch 重启单节点）→ SDK 见 epoch 不符立即刷新收敛；数据一致性（写后经 owner 读回）
- [ ] 全量 C++ 回归不破 + SDK 测试全绿
- [ ] 派 code-reviewer 评审期3 → 修正 → 提交

## 自查备忘
- query_key 的 value 串化：int → 十进制文本（与服务端 value_to_routing_string 同口径）；string 原样；REAL/NULL 不路由（SDK 侧同样按 schema 判定）
- urllib 的 HTTPError 带 status；超时是 socket.timeout/TimeoutError；连接拒绝是 URLError(ConnectionRefusedError) —— 三者都算"节点不可达"进黑名单
- 环原子替换：self._ring 直接引用赋值（GIL 下单写多读安全），注释写清
- topology_refresh 用"上次刷新时刻 + 每请求懒检查"，不起后台线程（首版从简，注释留痕）
