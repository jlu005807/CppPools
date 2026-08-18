"""ClusterClient：集群路由与故障绕行（cluster-cache spec §7）。

行为契约（§7.2）：
- 拓扑：启动强制拉取（逐 seed 重试，全失败抛 TopologyError，无默认兜底）；
  运行期 lazy 刷新，失败继续用旧环 + 计数；epoch 变化即重建环。
- 建环：本地建环，参数取自 /api/v1/cluster（黄金向量同口径，§3.2）。
- 绕行：节点不可达 → 拉黑 → attempt+1 查环 → 携带 X-CppPools-Fallback；
  全黑 → 清空黑名单重试一轮再抛（避免永久拒服务）。
- epoch 门控（§5.4）：每请求携带 X-CppPools-Epoch；响应回显不符 →
  立即刷新拓扑并重试一次。
- 黑名单联动：拓扑刷新后清理已下线节点条目；过期回切首请求带
  X-CppPools-Rejoin: 1（best-effort，§5.2 规则 B）。

线程模型：环/拓扑为"构建完成后原子替换引用"（读者无锁）；黑名单与
计时用一把锁保护。首版 topology 刷新由请求路径懒触发，不起后台线程。

已知限制（计划 D2）：传输层无连接复用（urllib），keep-alive 留待 v1.1。
"""
import collections
import os
import random
import threading
import time
import uuid

from . import errors, transport
from .hashing import query_key
from .ring import HashRing

_KNOWN_RETRYABLE = (errors.UnavailableError, errors.ServerError)

# 拓扑状态聚合为单个不可变对象：一次属性赋值整体替换（ring/nodes/epoch/ttl
# 四元组若非原子替换，并发刷新时读者可能拿到新环配旧节点表）。
_TopologyState = collections.namedtuple(
    "_TopologyState", ["ring", "nodes", "epoch", "blacklist_ttl"])


class ClusterClient:
    """CppPools 集群客户端。入口方法：select/insert/update/delete。"""

    def __init__(self, seeds, http=None, retry_max=3, attempt_max=5,
                 backoff_base=0.1, backoff_jitter=0.25, topology_refresh=30.0,
                 connect_timeout=1.0, read_timeout=3.0, client_id=None):
        if not seeds:
            raise ValueError("seeds must not be empty")
        self._seeds = list(seeds)
        self._retry_max = retry_max
        self._attempt_max = attempt_max
        self._backoff_base = backoff_base
        self._backoff_jitter = backoff_jitter
        self._topology_refresh = topology_refresh
        # 超时：urllib 单一 timeout 参数同时作用于 connect 与每次 recv，
        # 首版以 read_timeout 为准；connect_timeout 保留签名、v1 不单独生效
        #（spec §7.1 的两段式超时留待 v1.1）。read_timeout 必须 > 服务端
        # db_acquire_timeout，否则会把正常 503 误判为超时并拉黑健康节点。
        self._connect_timeout = connect_timeout
        if http is None:
            def http(method, url, payload=None, headers=None):
                return transport.request(method, url, payload, headers,
                                         timeout=read_timeout)
        self._http = http
        # X-Client-Id 兜底链（§7.2）：显式参数 → 环境变量 → 生成。
        self._client_id = (client_id or os.environ.get("CPPPOOLS_CLIENT_ID")
                           or "py-sdk-%d-%s" % (os.getpid(),
                                                uuid.uuid4().hex[:8]))
        self._lock = threading.Lock()
        self._blacklist = {}          # node_id -> 到期时刻（monotonic）
        self._rejoin = set()          # 过期回切、待带 Rejoin 头的节点
        # 拓扑状态：原子替换（读者先取引用再用，永不阻塞；并发刷新竞态
        # 最多造成瞬态绕行，无正确性影响）。
        self._topology = _TopologyState(ring=None, nodes={}, epoch=0,
                                        blacklist_ttl=60.0)
        self._schema = {}             # (datasource, table) -> columns 列表
        self._last_refresh = 0.0
        self.topology_refresh_failures = 0
        self.digest_warnings = []

        self._refresh_topology(startup=True)   # 失败抛 TopologyError；成功时内部已拉 schema
        self._compare_digests()

    # ===== 公开 API =====

    def select(self, table, where, columns=None, datasource=None):
        payload = {"table": table, "where": where}
        if columns is not None:
            payload["columns"] = columns
        if datasource:
            payload["datasource"] = datasource
        key = self._route_key(table, where=where, datasource=datasource)
        return self._execute("/api/v1/select", payload, key)

    def insert(self, table, values, datasource=None):
        payload = {"table": table, "values": values}
        if datasource:
            payload["datasource"] = datasource
        key = self._route_key(table, values=values, datasource=datasource)
        # §5.5：自增 insert（无显式唯一键）非幂等，失败不自动重试。
        return self._execute("/api/v1/insert", payload, key,
                             retry_allowed=key is not None)

    def update(self, table, set_values, where, datasource=None):
        payload = {"table": table, "set": set_values, "where": where}
        if datasource:
            payload["datasource"] = datasource
        key = self._route_key(table, where=where, datasource=datasource)
        return self._execute("/api/v1/update", payload, key)

    def delete(self, table, where, datasource=None):
        payload = {"table": table, "where": where}
        if datasource:
            payload["datasource"] = datasource
        key = self._route_key(table, where=where, datasource=datasource)
        return self._execute("/api/v1/delete", payload, key)

    @property
    def current_epoch(self):
        return self._topology.epoch

    def blacklisted_nodes(self):
        now = time.monotonic()
        with self._lock:
            return [nid for nid, expiry in self._blacklist.items()
                    if expiry > now]

    # ===== 路由键（与服务端 handler 逐字同规则）=====

    def _route_key(self, table, where=None, values=None, datasource=None):
        """返回 (key_datasource, table, column, value_str)；不可路由 → None。

        D1 口径（§3.3 修订）：key_datasource = 显式值或空串。
        REAL 键 / NULL / 非唯一列 / 无键 insert 不参与环路由（§3.6 补充）。
        """
        columns = self._table_columns(table, datasource)
        if columns is None:
            return None
        if where is not None:
            col_name, value = where.get("column"), where.get("value")
        else:
            # insert：schema 列序第一个"给了值且非 NULL"的唯一列。
            col_name = value = None
            for col in columns:
                if not col.get("unique"):
                    continue
                given = values.get(col["name"]) if values else None
                if given is not None:
                    col_name, value = col["name"], given
                    break
            if col_name is None:
                return None
        info = next((c for c in columns if c["name"] == col_name), None)
        if info is None or not info.get("unique"):
            return None
        if info.get("type") == "real":
            return None
        if isinstance(value, int) and not isinstance(value, bool):
            value_str = str(value)
        elif isinstance(value, str):
            value_str = value
        else:
            return None  # NULL/浮点等：与服务端同口径，不路由
        return (datasource or "", table, col_name, value_str)

    def _table_columns(self, table, datasource):
        if datasource:
            return self._schema.get((datasource, table))
        matches = [ds for (ds, t) in self._schema if t == table]
        if len(matches) == 1:
            return self._schema[(matches[0], table)]
        return None  # 歧义/未知：留给服务端校验管道回 400/409

    # ===== 执行主循环 =====

    def _execute(self, path, payload, key, retry_allowed=True):
        self._maybe_refresh_topology()
        cleared_once = False
        next_attempt = 0        # 下一轮候选扫描的起点（失败推进）
        sent = 0                # 总发送次数上限 = attempt_max（§7.1）
        while True:
            node_id, used_attempt = self._pick_node(key, next_attempt)
            if node_id is None:
                # 无可选节点：区分"全黑"与"绕行用尽"。
                if self._all_nodes_blacklisted() and not cleared_once:
                    # §7.2：全黑清空重试一轮，避免永久拒服务。
                    with self._lock:
                        self._blacklist.clear()
                    cleared_once = True
                    next_attempt = 0
                    sent = 0
                    continue
                if self._all_nodes_blacklisted():
                    raise errors.AllNodesBlacklistedError(
                        "all nodes blacklisted and retry round failed")
                raise errors.NodeUnavailable(
                    "attempt limit (%d) exceeded" % self._attempt_max)
            if sent >= self._attempt_max:
                raise errors.NodeUnavailable(
                    "attempt limit (%d) exceeded" % self._attempt_max)
            sent += 1
            # attempt>0 = attempt-0 owner 不可达/已拉黑 ⇒ 绕行（§3.5）。
            is_fallback = used_attempt > 0
            try:
                resp = self._send_to_node(node_id, path, payload,
                                          is_fallback, retry_allowed)
            except errors.NodeUnavailable:
                self._blacklist_node(node_id)
                if not retry_allowed:
                    raise
                next_attempt = used_attempt + 1
                continue
            try:
                resp = self._follow_redirects(resp, payload, is_fallback,
                                             retry_allowed)
            except errors.NodeUnavailable:
                # 重定向目标不可达：响应 307 的节点是活的，不得拉黑它；
                # 只推进绕行重新选点（307 本身 = 环视图滞后，刷拓扑收敛）。
                # 非幂等操作不得在 redirect 失败后换节点重发。
                if not retry_allowed:
                    raise
                self._refresh_topology(startup=False)
                next_attempt = used_attempt + 1
                continue
            # epoch 门控（§5.4）：响应回显不符 = 本端视图滞后。操作已被
            # 服务端正确处理（响应即结果），只需刷拓扑供后续请求使用 ——
            # 绝不重发：写操作重发会撞 409/重复写入（e2e phase4 实证）。
            if (resp.server_epoch is not None
                    and resp.server_epoch != str(self._topology.epoch)):
                self._refresh_topology(startup=False)
            return resp.json()

    def _pick_node(self, key, start_attempt):
        """返回 (node_id, used_attempt)；无候选 → (None, _)。

        有键时从 start_attempt 起扫描环归属（§3.5：attempt 进哈希输入 ⇒
        不同替补），跳过黑名单。扫描到"全部节点都试过"为止 —— 固定小
        窗口会概率性漏掉活节点（连续 N 个 attempt 全落被拉黑节点，
        滚动重启窗口内实测撞红）；同一 owner 不重复尝试。
        used_attempt 直接编码绕行语义：>0 即 Fallback。
        """
        topo = self._topology
        now = time.monotonic()
        if key is None:
            node_id, index = self._first_available(list(topo.nodes), now)
            if node_id is None:
                return None, start_attempt
            # 拓扑序首个是无键请求的"主节点"；跳过它视同绕行。
            return node_id, 1 if index > 0 else 0
        checked = set()
        scan_limit = start_attempt + max(10 * len(topo.nodes),
                                         self._attempt_max)
        for a in range(start_attempt, scan_limit):
            candidate = topo.ring.owner(query_key(key[0], key[1], key[2],
                                                  key[3], a))
            if candidate in checked:
                continue  # 同一 owner 已在更早 attempt 试过（被拉黑）
            checked.add(candidate)
            node_id, _ = self._first_available([candidate], now)
            if node_id is not None:
                return node_id, a
            if len(checked) >= len(topo.nodes):
                break  # 全部节点都试过且不可用
        return None, scan_limit

    def _follow_redirects(self, resp, payload, is_fallback, retry_allowed):
        """307 跟随（§5.5：不计重试）：服务端权威纠偏环视图滞后。
        跟随目标不可达时抛 NodeUnavailable，由主循环只绕行不拉黑。

        is_fallback 由发起请求保守性决定，不得因 307 升级为 True
        （Fallback 单调性：只能保持或降级，不能变激进）。
        retry_allowed 透传给目标重试策略：非幂等操作的重定向目标同样不得重试。"""
        follows = 0
        while resp.status == 307:
            follows += 1
            if follows > 3 or not resp.location:
                raise errors.ServerError("redirect loop or bad Location")
            resp = self._send_url_with_retry(resp.location, payload,
                                             is_fallback, retry_allowed)
        return resp

    def _node_for_url(self, url):
        """从 URL 反查 node_id（redirect target 在已知拓扑内时返回）。"""
        for node_id, base in self._topology.nodes.items():
            if url == base or url.startswith(base + "/"):
                return node_id
        return None

    def _send_url_with_retry(self, url, payload, is_fallback, retry_allowed):
        """对 redirect target 发送：已知节点用 retry/backoff；不可达时
        只拉黑目标（不拉黑 307 响应者），然后上抛由主循环绕行。"""
        target_id = self._node_for_url(url)
        retries = 0
        while True:
            headers = self._build_headers(is_fallback=is_fallback)
            try:
                return self._http("POST", url, payload, headers)
            except (errors.NodeUnavailable,) + _KNOWN_RETRYABLE:
                if target_id is not None:
                    self._blacklist_node(target_id)
                if not retry_allowed or retries >= self._retry_max:
                    raise
                retries += 1
                time.sleep(self._backoff_delay(retries))

    def _first_available(self, candidates, now):
        with self._lock:
            for index, node_id in enumerate(candidates):
                expiry = self._blacklist.get(node_id)
                if expiry is not None:
                    if expiry > now:
                        continue
                    # 过期：清条目并标记回切 Rejoin（§5.2 规则 B）。
                    del self._blacklist[node_id]
                    self._rejoin.add(node_id)
                return node_id, index
        return None, -1

    def _build_headers(self, is_fallback):
        headers = {
            "X-Client-Id": self._client_id,
            "X-CppPools-Epoch": str(self._topology.epoch),
        }
        if is_fallback:
            headers["X-CppPools-Fallback"] = "1"
        return headers

    def _send_to_node(self, node_id, path, payload, is_fallback,
                      retry_allowed):
        """对单个节点发送：503/5xx 限次指数退避重试（§7.2）；307 原样返回
        由主循环跟随；节点不可达（NodeUnavailable）上抛由主循环拉黑绕行。"""
        url = self._topology.nodes.get(node_id)
        if url is None:
            # 环刷新与选点之间的竞态：节点已下线，按不可达处理。
            raise errors.NodeUnavailable("node %s left topology" % node_id)
        url += path
        retries = 0
        while True:
            headers = self._build_headers(is_fallback)
            with self._lock:
                if node_id in self._rejoin:
                    headers["X-CppPools-Rejoin"] = "1"
                    self._rejoin.discard(node_id)
            try:
                return self._http("POST", url, payload, headers)
            except _KNOWN_RETRYABLE:
                # 幂等矩阵（§5.5）：非幂等操作（自增 insert）不自动重试。
                if not retry_allowed or retries >= self._retry_max:
                    raise
                retries += 1
                # 退避必须带抖动：无抖动的同步退避在恢复时造成归队风暴。
                time.sleep(self._backoff_delay(retries))

    def _backoff_delay(self, n):
        delay = self._backoff_base * (2 ** (n - 1))
        return delay * (1.0 + random.uniform(-self._backoff_jitter,
                                             self._backoff_jitter))

    # ===== 黑名单 =====

    def _blacklist_node(self, node_id):
        with self._lock:
            self._blacklist[node_id] = (
                time.monotonic() + self._topology.blacklist_ttl)

    def _all_nodes_blacklisted(self):
        now = time.monotonic()
        with self._lock:
            return all(self._blacklist.get(nid, 0) > now
                       for nid in self._topology.nodes)

    # ===== 拓扑 / schema / digest =====

    def _maybe_refresh_topology(self):
        if time.monotonic() - self._last_refresh >= self._topology_refresh:
            self._refresh_topology(startup=False)

    def _refresh_topology(self, startup):
        urls = self._seeds if startup else (
            list(self._topology.nodes.values()) + self._seeds)
        doc = None
        last_err = None
        for base in urls:
            try:
                resp = self._http("GET", base + "/api/v1/cluster")
                doc = resp.json()
                break
            # 单个 seed 失败（含畸形 200：解析/字段缺失/形状错误）继续试下一个：
            # 拓扑拉取在任何路径上都不得以非 TopologyError 的异常炸穿
            # 请求链路（尤其 epoch 触发的运行期刷新，此时本次操作已成功）。
            # AttributeError 覆盖"合法 JSON 但类型错"（如 body 是 []/标量）。
            except (errors.CppPoolsError, ValueError, KeyError,
                    TypeError, AttributeError) as e:
                last_err = e
                doc = None
        if doc is None:
            if startup:
                raise errors.TopologyError(
                    "failed to fetch topology from all seeds: %s" % last_err)
            # §7.2：刷新失败继续用旧环 + 打点（不报错）。
            self.topology_refresh_failures += 1
            self._last_refresh = time.monotonic()
            return
        # 构建新状态后单次赋值整体替换（ring/nodes/epoch/ttl 四元组原子）。
        try:
            ring = HashRing()
            nodes = {}
            vnodes = int(doc.get("hash", {}).get("vnodes_per_node", 100))
            for node in doc.get("nodes", []):
                ring.add_node(node["node_id"], vnodes)
                nodes[node["node_id"]] = "http://%s:%d" % (node["host"],
                                                             node["port"])
            epoch = int(doc.get("epoch", 0))
            blacklist_ttl = (doc.get("ttl", {}).get("blacklist_ttl_ms",
                                                     60000)) / 1000.0
        except (ValueError, KeyError, TypeError, AttributeError) as e:
            # 200 但内容畸形（含合法 JSON 错误形状）：视同拉取失败
            #（同上，不得炸穿请求链路）。
            if startup:
                raise errors.TopologyError(
                    "malformed topology document: %s" % e)
            self.topology_refresh_failures += 1
            self._last_refresh = time.monotonic()
            return
        old = self._topology
        self._topology = _TopologyState(ring, nodes, epoch, blacklist_ttl)
        self._last_refresh = time.monotonic()
        # 拓扑变化（节点集/epoch）时重拉 schema（D4）：服务端加表后
        # SDK 需重新获得新表的路由键判定能力。
        if old.nodes != nodes or old.epoch != epoch:
            self._fetch_schema()
        # 黑名单联动：清理已下线节点条目（§7.2）。
        with self._lock:
            for nid in list(self._blacklist):
                if nid not in nodes:
                    del self._blacklist[nid]
            self._rejoin = {nid for nid in self._rejoin if nid in nodes}

    def _fetch_schema(self):
        for base in list(self._topology.nodes.values()) + self._seeds:
            try:
                resp = self._http("GET", base + "/api/v1/schema")
                doc = resp.json()
                schema = {}
                for ds in doc.get("datasources", []):
                    for table in ds.get("tables", []):
                        schema[(ds["name"], table["name"])] = table["columns"]
                self._schema = schema
                return
            except (errors.CppPoolsError, ValueError, KeyError,
                    TypeError, AttributeError):
                continue
        # schema 拉不到不致命：路由键无法确定时退化为不门控（服务端兜底）。

    def _compare_digests(self):
        """§6.3：全节点 datasource_digest 比对，不一致记告警不阻断。"""
        seen = {}
        for node_id, base in self._topology.nodes.items():
            try:
                resp = self._http("GET", base + "/api/v1/cluster")
                digest = resp.json().get("datasource_digest")
                seen[node_id] = sorted(
                    (d.get("name"), d.get("type"), d.get("digest"))
                    for d in (digest or []))
            except (errors.CppPoolsError, ValueError, KeyError,
                    TypeError, AttributeError):
                continue
        values = list(seen.values())
        if len(values) >= 2 and any(v != values[0] for v in values[1:]):
            self.digest_warnings.append(
                "datasource_digest mismatch across nodes: %s" % seen)
