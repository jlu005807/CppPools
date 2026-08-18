"""ClusterClient 行为测试（cluster-cache spec §7.2 行为规范）。

用可编程 fake 集群钉住：启动强制拉拓扑、路由直连 owner、黑名单/attempt
绕行 + Fallback 头、全黑清空重试、Rejoin 头、epoch 不符即刷新、
错误分类重试矩阵、digest 比对告警、307 跟随。
"""
import json
import os
import sys
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from cpppools import errors, transport  # noqa: E402
from cpppools.client import ClusterClient  # noqa: E402
from cpppools.hashing import query_key  # noqa: E402
from cpppools.ring import HashRing  # noqa: E402

SCHEMA = {
    "datasources": [{
        "name": "main",
        "tables": [{
            "name": "users",
            "columns": [
                {"name": "id", "type": "integer", "nullable": False,
                 "primary_key": True, "unique": True},
                {"name": "name", "type": "text", "nullable": False,
                 "primary_key": False, "unique": False},
                {"name": "rating", "type": "real", "nullable": True,
                 "primary_key": False, "unique": False},
            ],
        }],
    }],
}


class FakeCluster:
    """可编程假集群：node_id 作主机名，harness 分发请求。"""

    def __init__(self, node_ids, epoch=1, blacklist_ttl_ms=60000):
        self.epoch = epoch
        self.blacklist_ttl_ms = blacklist_ttl_ms
        self.nodes = {nid: {"down": False, "responders": {},
                            "digest": [{"name": "main", "type": "sqlite",
                                         "digest": "d-%s" % nid}]}
                      for nid in node_ids}
        self.node_ids = list(node_ids)
        self.log = []               # {node, method, path, payload, headers}
        self.topology_gets = 0
        self.schema = SCHEMA
        self.force_topology_failure = False

    # ---- 测试侧控制 ----
    def set_down(self, node_id, down=True):
        self.nodes[node_id]["down"] = down

    def set_responder(self, node_id, path, fn):
        """fn(payload, headers) -> (status, extra_headers, body_bytes)"""
        self.nodes[node_id]["responders"][path] = fn

    def cluster_doc(self, node_id):
        return {
            "self": node_id,
            "epoch": self.epoch,
            "hash": {"algorithm": "murmur3_32", "seed": 0, "unsigned": True,
                     "vnodes_per_node": 100, "attempt_base": 0},
            "ttl": {"cache_ttl_ms": 60000,
                    "blacklist_ttl_ms": self.blacklist_ttl_ms},
            "nodes": [{"node_id": nid, "host": nid, "port": 80}
                      for nid in self.node_ids],
            "datasource_digest": self.nodes[node_id]["digest"],
        }

    # ---- SDK 注入的 http 入口（契约与 transport 相同）----
    def http(self, method, url, payload=None, headers=None):
        host = url.split("//", 1)[1].split("/", 1)[0].split(":")[0]
        path = "/" + url.split("//", 1)[1].split("/", 1)[1]
        node = self.nodes.get(host)
        if node is None:
            raise errors.NodeUnavailable("unknown node %s" % host)
        if node["down"]:
            raise errors.NodeUnavailable("connection refused: %s" % host)
        self.log.append({"node": host, "method": method, "path": path,
                         "payload": payload, "headers": dict(headers or {})})
        responder = node["responders"].get(path)
        if responder is not None:
            status, extra, body = responder(payload, headers or {})
        elif path == "/api/v1/cluster":
            if self.force_topology_failure:
                raise errors.NodeUnavailable("topology endpoint down")
            self.topology_gets += 1
            status, extra, body = 200, [], json.dumps(
                self.cluster_doc(host)).encode()
        elif path == "/api/v1/schema":
            status, extra, body = 200, [], json.dumps(self.schema).encode()
        else:
            status, extra, body = 200, [], b'{"rows":[],"row_count":0}'
        return self._result(status, extra, body)

    def _result(self, status, headers, body):
        if status == 307 or 200 <= status < 300:
            return transport.Response(status, headers, body)
        message = transport._extract_message(body)
        if status == 409:
            raise errors.ConflictError(message, status, body)
        if status == 503:
            raise errors.UnavailableError(message, status, body)
        if 500 <= status < 600:
            raise errors.ServerError(message, status, body)
        raise errors.ClientError(message, status, body)

    def requests_to(self, node_id, path=None):
        return [r for r in self.log
                if r["node"] == node_id and (path is None or r["path"] == path)]

    def make_client(self, **kw):
        defaults = dict(topology_refresh=30.0, backoff_base=0.001,
                        retry_max=3, http=self.http)
        defaults.update(kw)
        return ClusterClient(["http://%s:80" % self.node_ids[0]], **defaults)

    def independent_owner(self, key_value, attempt=0):
        """归属真值：独立建环（不借道被测 client）。"""
        ring = HashRing()
        for nid in self.node_ids:
            ring.add_node(nid, 100)
        return ring.owner(query_key("", "users", "id", key_value, attempt))


class TestStartup(unittest.TestCase):
    def test_topology_required_at_startup(self):
        fc = FakeCluster(["node-a", "node-b"])
        fc.set_down("node-a")
        fc.set_down("node-b")
        with self.assertRaises(errors.TopologyError):
            fc.make_client()

    def test_refresh_failure_keeps_old_ring(self):
        fc = FakeCluster(["node-a", "node-b"])
        client = fc.make_client(topology_refresh=0.0)
        fc.force_topology_failure = True
        # 刷新失败不报错：继续用旧环（spec §7.2）。
        resp = client.select("users", {"column": "id", "value": 42})
        self.assertEqual(resp["row_count"], 0)
        self.assertGreaterEqual(client.topology_refresh_failures, 1)

    def test_malformed_topology_doc_is_not_raw_exception(self):
        # W3：畸形 200（非法 JSON/缺字段）不得以 JSONDecodeError/KeyError
        # 炸穿 —— 启动期归为 TopologyError，运行期视同刷新失败用旧环。
        fc = FakeCluster(["node-a"])

        def garbage(payload, headers):
            return 200, [], b"this is not json"
        fc.set_responder("node-a", "/api/v1/cluster", garbage)
        with self.assertRaises(errors.TopologyError):
            fc.make_client()

        # 运行期：拓扑端点变畸形 → 不报错，旧环继续服务。
        fc2 = FakeCluster(["node-a"])
        client = fc2.make_client()
        fc2.set_responder("node-a", "/api/v1/cluster", garbage)
        client._topology_refresh = 0.0  # 每请求强刷
        resp = client.select("users", {"column": "id", "value": 1})
        self.assertEqual(resp["row_count"], 0)
        self.assertGreaterEqual(client.topology_refresh_failures, 1)

    def test_wrong_shape_json_topology_not_raw_exception(self):
        # 评审 W1：合法 JSON 但形状错（[]/标量）也不得裸穿透
        #（AttributeError 必须在捕获面内）。
        fc = FakeCluster(["node-a"])

        def wrong_shape(payload, headers):
            return 200, [], b"[]"
        fc.set_responder("node-a", "/api/v1/cluster", wrong_shape)
        with self.assertRaises(errors.TopologyError):
            fc.make_client()

        fc2 = FakeCluster(["node-a"])
        client = fc2.make_client()
        fc2.set_responder("node-a", "/api/v1/cluster", wrong_shape)
        client._topology_refresh = 0.0
        resp = client.select("users", {"column": "id", "value": 1})
        self.assertEqual(resp["row_count"], 0)
        self.assertGreaterEqual(client.topology_refresh_failures, 1)

    def test_digest_mismatch_recorded(self):
        fc = FakeCluster(["node-a", "node-b"])
        # node-b 的库指向另一个目标（digest 不同）—— §6.3 检测。
        fc.nodes["node-b"]["digest"] = [{"name": "main", "type": "sqlite",
                                         "digest": "WRONG"}]
        client = fc.make_client()
        self.assertTrue(client.digest_warnings, "digest 不一致必须留痕")


class TestRouting(unittest.TestCase):
    def test_select_goes_straight_to_owner(self):
        fc = FakeCluster(["node-a", "node-b", "node-c"])
        client = fc.make_client()
        hits = {}
        for key in range(40):
            client.select("users", {"column": "id", "value": key})
            owner = fc.independent_owner(str(key))
            last = fc.log[-1]
            self.assertEqual(last["node"], owner,
                             "键 %d 应直连 owner %s，实际打到 %s"
                             % (key, owner, last["node"]))
            hits[owner] = hits.get(owner, 0) + 1
        self.assertGreaterEqual(len(hits), 2, "40 个键全落一个节点？")
        # 直连：没有任何 307 发生（每键恰一个 select 请求）。
        selects = [r for r in fc.log if r["path"] == "/api/v1/select"]
        self.assertEqual(len(selects), 40)

    def test_redirect_followed_and_not_counted_as_retry(self):
        fc = FakeCluster(["node-a", "node-b"])
        target = None

        def redirect(payload, headers):
            return 307, [("Location", "http://node-b:80/api/v1/select")], \
                b'{"code":"not_owner"}'
        # 找出一个归属 node-a 的键，把 node-a 的 select 改成 307。
        for key in range(100):
            if fc.independent_owner(str(key)) == "node-a":
                target = key
                break
        fc.set_responder("node-a", "/api/v1/select", redirect)
        client = fc.make_client()
        resp = client.select("users", {"column": "id", "value": target})
        self.assertEqual(resp["row_count"], 0)
        # 跟随落在 node-b：一读两个请求（307 + 跟随），不算重试绕行。
        self.assertEqual(len(fc.requests_to("node-a", "/api/v1/select")), 1)
        self.assertEqual(len(fc.requests_to("node-b", "/api/v1/select")), 1)

    def test_no_routing_for_real_or_nonunique_where(self):
        fc = FakeCluster(["node-a", "node-b"])
        client = fc.make_client()
        # REAL 键与非唯一列不参与环路由（spec 决策⑫ / §3.6 补充）：
        # 请求仍能成功（打到任一节点），不产生黑名单/绕行语义。
        resp = client.select("users", {"column": "rating", "value": 4.5})
        self.assertEqual(resp["row_count"], 0)
        resp = client.select("users", {"column": "name", "value": "x"})
        self.assertEqual(resp["row_count"], 0)


class TestFailover(unittest.TestCase):
    def _owned_key(self, fc, node):
        for key in range(200):
            if fc.independent_owner(str(key)) == node:
                return key
        raise AssertionError("找不到归属 %s 的键" % node)

    def test_owner_down_blacklists_and_fallbacks(self):
        fc = FakeCluster(["node-a", "node-b", "node-c"])
        client = fc.make_client()
        key = self._owned_key(fc, "node-a")
        fc.set_down("node-a")
        resp = client.select("users", {"column": "id", "value": key})
        self.assertEqual(resp["row_count"], 0)
        # 绕行请求带 Fallback 头（§3.5），且不在 node-a。
        fallback_reqs = [r for r in fc.log if r["path"] == "/api/v1/select"
                         and r["headers"].get("X-CppPools-Fallback")]
        self.assertEqual(len(fallback_reqs), 1)
        self.assertNotEqual(fallback_reqs[0]["node"], "node-a")
        self.assertIn("node-a", client.blacklisted_nodes())

    def test_all_blacklisted_clears_once_then_raises(self):
        fc = FakeCluster(["node-a", "node-b"])
        client = fc.make_client()
        fc.set_down("node-a")
        fc.set_down("node-b")
        with self.assertRaises(errors.AllNodesBlacklistedError):
            client.select("users", {"column": "id", "value": 1})
        # 清空重试一轮已发生：恢复一个节点后立即恢复服务（不等 TTL）。
        fc.set_down("node-b", False)
        resp = client.select("users", {"column": "id", "value": 1})
        self.assertEqual(resp["row_count"], 0)

    def test_pre_blacklisted_owner_routes_with_fallback_header(self):
        # attempt-0 owner 已在黑名单（上一轮拉黑未过期）：直接路由到替补，
        # 且替补请求必带 Fallback 头（e2e phase2 撞红的场景，评审修正）。
        fc = FakeCluster(["node-a", "node-b", "node-c"])
        client = fc.make_client()
        key = self._owned_key(fc, "node-a")
        fc.set_down("node-a")
        client.select("users", {"column": "id", "value": key})  # 拉黑 node-a
        fc.set_down("node-a", False)  # 复活但黑名单未过期
        client.select("users", {"column": "id", "value": key})
        # 第二轮请求不得打到 node-a，且替补必带 Fallback 头。
        second_round = [r for r in fc.log if r["path"] == "/api/v1/select"]
        last = second_round[-1]
        self.assertNotEqual(last["node"], "node-a")
        self.assertTrue(last["headers"].get("X-CppPools-Fallback"),
                        "预拉黑 owner 后的绕行必须带 Fallback 头")

    def test_redirect_follow_failure_does_not_blacklist_responder(self):
        # 307 跟随目标不可达：只重新绕行，不得拉黑活着且正确响应的节点
        #（e2e phase2 的第二只 bug：误拉黑导致全黑）。
        fc = FakeCluster(["node-a", "node-b"])
        target = None
        for key in range(100):
            if fc.independent_owner(str(key)) == "node-a":
                target = key
                break

        def redirect(payload, headers):
            return 307, [("Location", "http://node-b:80/api/v1/select")], b"{}"
        fc.set_responder("node-a", "/api/v1/select", redirect)
        fc.set_down("node-b")
        client = fc.make_client()
        with self.assertRaises(errors.CppPoolsError):
            client.select("users", {"column": "id", "value": target})
        self.assertNotIn("node-a", client.blacklisted_nodes(),
                         "响应 307 的活节点不得被拉黑")

    def test_scan_covers_all_nodes_beyond_small_attempt_window(self):
        # 评审 W2 同源缺陷的确定性钉桩：存在键的前若干个 attempt 全落
        # 被拉黑节点，固定小窗口会"绕行用尽"报错，而活节点就在窗口外。
        # 扫描必须覆盖全部节点（去重）而非只看前几个 attempt。
        fc = FakeCluster(["node-a", "node-b", "node-c"])
        # 找一个键：attempt 0..5 全落 {a,b}，且 0..14 内出现 c。
        target = None
        for k in range(3000):
            owners = [fc.independent_owner(str(k), attempt=a)
                      for a in range(15)]
            if (all(o in ("node-a", "node-b") for o in owners[:6])
                    and "node-c" in owners):
                target = k
                break
        self.assertIsNotNone(target, "找不到窗口外才有活节点的键")
        client = fc.make_client()  # 先建客户端（seed node-a 此时还活着）
        fc.set_down("node-a")
        fc.set_down("node-b")
        # a/b 都 down：旧实现会"attempt limit exceeded"；
        # 新实现扫到 node-c 并经 Fallback 就地处理。
        resp = client.select("users", {"column": "id", "value": target})
        self.assertEqual(resp["row_count"], 0)
        served = [r for r in fc.log if r["path"] == "/api/v1/select"][-1]
        self.assertEqual(served["node"], "node-c")
        self.assertTrue(served["headers"].get("X-CppPools-Fallback"))

    def test_rejoin_header_on_blacklist_expiry(self):
        fc = FakeCluster(["node-a", "node-b"], blacklist_ttl_ms=40)
        client = fc.make_client()
        key = self._owned_key(fc, "node-a")
        fc.set_down("node-a")
        client.select("users", {"column": "id", "value": key})  # 拉黑 node-a
        fc.set_down("node-a", False)
        time.sleep(0.06)  # 等黑名单过期（40ms）
        client.select("users", {"column": "id", "value": key})
        rejoin_reqs = [r for r in fc.requests_to("node-a", "/api/v1/select")
                       if r["headers"].get("X-CppPools-Rejoin")]
        self.assertEqual(len(rejoin_reqs), 1, "回切首请求必须带 Rejoin 头")
        # 第二次回切请求不再带（一次性）。
        client.select("users", {"column": "id", "value": key})
        rejoin_reqs = [r for r in fc.requests_to("node-a", "/api/v1/select")
                       if r["headers"].get("X-CppPools-Rejoin")]
        self.assertEqual(len(rejoin_reqs), 1)


class TestEpochGate(unittest.TestCase):
    def test_epoch_mismatch_refreshes_without_resending(self):
        fc = FakeCluster(["node-a", "node-b"], epoch=1)
        client = fc.make_client()
        # node-a 的 select 响应回显 epoch=2（模拟节点已滚动重启）。
        def stale(payload, headers):
            return 200, [("X-CppPools-Epoch", "2")], b'{"rows":[],"row_count":0}'
        fc.set_responder("node-a", "/api/v1/select", stale)
        gets0 = fc.topology_gets
        fc.epoch = 2  # 拓扑端点此刻起返回 epoch=2（模拟运维已递增）
        resp = client.select("users", {"column": "id", "value": 7})
        self.assertEqual(resp["row_count"], 0)
        self.assertGreater(fc.topology_gets, gets0, "epoch 不符必须立即刷新拓扑")
        self.assertEqual(client.current_epoch, 2)
        # 不重发：本次请求只有首发的那一个 select（重发会让写撞 409）。
        if fc.independent_owner("7") == "node-a":
            self.assertEqual(len(fc.requests_to("node-a", "/api/v1/select")), 1)
        # 刷新后的下一个请求带新 epoch 头。
        client.select("users", {"column": "id", "value": 8})
        last = [r for r in fc.log if r["path"] == "/api/v1/select"][-1]
        self.assertEqual(last["headers"].get("X-CppPools-Epoch"), "2")

    def test_every_request_carries_epoch_header(self):
        fc = FakeCluster(["node-a"])
        client = fc.make_client()
        client.select("users", {"column": "id", "value": 1})
        self.assertEqual(fc.log[-1]["headers"].get("X-CppPools-Epoch"), "1")


class TestRetryMatrix(unittest.TestCase):
    def test_400_raises_without_retry(self):
        fc = FakeCluster(["node-a"])
        def bad(payload, headers):
            return 400, [], b'{"error":"unknown column"}'
        fc.set_responder("node-a", "/api/v1/select", bad)
        client = fc.make_client()
        with self.assertRaises(errors.ClientError):
            client.select("users", {"column": "id", "value": 1})
        self.assertEqual(len(fc.requests_to("node-a", "/api/v1/select")), 1)

    def test_409_handed_to_caller_without_retry(self):
        fc = FakeCluster(["node-a"])
        def conflict(payload, headers):
            return 409, [], b'{"error":"duplicate"}'
        fc.set_responder("node-a", "/api/v1/insert", conflict)
        client = fc.make_client()
        with self.assertRaises(errors.ConflictError):
            client.insert("users", {"id": 1, "name": "a"})
        self.assertEqual(len(fc.requests_to("node-a", "/api/v1/insert")), 1)

    def test_autoincrement_insert_never_auto_retried(self):
        fc = FakeCluster(["node-a"])
        def busy(payload, headers):
            return 503, [], b'{"error":"db timeout"}'
        fc.set_responder("node-a", "/api/v1/insert", busy)
        client = fc.make_client()
        # values 无唯一列 → 自增 insert：非幂等，失败直接上抛（spec §5.5）。
        with self.assertRaises(errors.UnavailableError):
            client.insert("users", {"name": "a"})
        self.assertEqual(len(fc.requests_to("node-a", "/api/v1/insert")), 1)

    def test_explicit_key_insert_retries_on_503(self):
        fc = FakeCluster(["node-a"])
        state = {"n": 0}
        def flaky(payload, headers):
            state["n"] += 1
            if state["n"] == 1:
                return 503, [], b'{"error":"db timeout"}'
            return 201, [], b'{"affected_rows":1,"inserted_id":5}'
        fc.set_responder("node-a", "/api/v1/insert", flaky)
        client = fc.make_client()
        resp = client.insert("users", {"id": 5, "name": "a"})
        self.assertEqual(resp["inserted_id"], 5)
        self.assertEqual(state["n"], 2)

    def test_select_retries_on_503_with_backoff(self):
        fc = FakeCluster(["node-a"])
        state = {"n": 0}
        def flaky(payload, headers):
            state["n"] += 1
            if state["n"] <= 2:
                return 503, [], b'{"error":"db timeout"}'
            return 200, [], b'{"rows":[],"row_count":0}'
        fc.set_responder("node-a", "/api/v1/select", flaky)
        client = fc.make_client()
        resp = client.select("users", {"column": "id", "value": 1})
        self.assertEqual(resp["row_count"], 0)
        self.assertEqual(state["n"], 3)

    def test_retry_exhaustion_raises(self):
        fc = FakeCluster(["node-a"])
        def busy(payload, headers):
            return 503, [], b'{"error":"db timeout"}'
        fc.set_responder("node-a", "/api/v1/select", busy)
        client = fc.make_client(retry_max=2)
        with self.assertRaises(errors.UnavailableError):
            client.select("users", {"column": "id", "value": 1})
        # retry_max=2 → 首次 + 2 次重试 = 3 个请求。
        self.assertEqual(len(fc.requests_to("node-a", "/api/v1/select")), 3)


class TestAutoIncrementNoFailover(unittest.TestCase):
    """Phase 5 Task 1: auto-increment insert must not fail over after commit."""

    def test_commit_then_disconnect_does_not_failover(self):
        fc = FakeCluster(["node-a", "node-b"])
        committed = []
        calls = []

        def fake_select(payload, headers):
            return 200, [], b'{"rows":[],"row_count":0}'

        def fake_insert_a(payload, headers):
            committed.append(payload)
            raise errors.NodeUnavailable("response lost after commit")

        def fake_insert_b(payload, headers):
            return 201, [], b'{"affected_rows":1,"inserted_id":9}'

        fc.set_responder("node-a", "/api/v1/select", fake_select)
        fc.set_responder("node-a", "/api/v1/insert", fake_insert_a)
        fc.set_responder("node-b", "/api/v1/select", fake_select)
        fc.set_responder("node-b", "/api/v1/insert", fake_insert_b)
        client = fc.make_client()

        with self.assertRaises(errors.NodeUnavailable):
            client.insert("users", {"name": "x"})

        # Exactly one insert was sent, and it went to node-a only.
        inserts = [r for r in fc.log if r["path"] == "/api/v1/insert"]
        self.assertEqual(len(inserts), 1, "auto-increment must not be retried")
        self.assertEqual(inserts[0]["node"], "node-a")
        self.assertEqual(len(committed), 1, "payload was committed once")
        # No request reached node B.
        b_inserts = [r for r in fc.log if r["node"] == "node-b" and r["path"] == "/api/v1/insert"]
        self.assertEqual(len(b_inserts), 0, "must not fail over non-idempotent insert")

    def test_explicit_key_insert_still_retries_on_failover(self):
        fc = FakeCluster(["node-a", "node-b"])
        state = {"n": 0}

        def flaky(payload, headers):
            state["n"] += 1
            if state["n"] == 1:
                raise errors.NodeUnavailable("node-a down")
            return 201, [], b'{"affected_rows":1,"inserted_id":5}'

        # Find a key owned by node-a
        key = None
        for k in range(100):
            if fc.independent_owner(str(k)) == "node-a":
                key = k
                break
        self.assertIsNotNone(key)

        fc.set_responder("node-a", "/api/v1/insert", flaky)
        fc.set_responder("node-b", "/api/v1/insert",
                         lambda p, h: (201, [], b'{"affected_rows":1,"inserted_id":5}'))
        client = fc.make_client()
        resp = client.insert("users", {"id": key, "name": "a"})
        self.assertEqual(resp["inserted_id"], 5)
        # Explicit key = idempotent, so failover is allowed.
        self.assertGreater(state["n"], 0)


class TestRedirectFallbackMonotonicity(unittest.TestCase):
    """Phase 5 Task 2: fallback hint must be preserved, not created, by 307."""

    def _owned_key(self, fc, node):
        for k in range(100):
            if fc.independent_owner(str(k)) == node:
                return k
        raise AssertionError("no key found for %s" % node)

    def test_normal_request_redirect_has_no_fallback(self):
        fc = FakeCluster(["node-a", "node-b"])
        follow_headers = {}

        def redirect(payload, headers):
            return 307, [("Location", "http://node-b:80/api/v1/select")], b'{}'

        def capture_follow(payload, headers):
            follow_headers.clear(); follow_headers.update(headers)
            return 200, [], b'{"rows":[],"row_count":0}'

        key = self._owned_key(fc, "node-a")
        fc.set_responder("node-a", "/api/v1/select", redirect)
        fc.set_responder("node-b", "/api/v1/select", capture_follow)
        client = fc.make_client()
        resp = client.select("users", {"column": "id", "value": key})
        self.assertEqual(resp["row_count"], 0)
        self.assertNotIn("X-CppPools-Fallback", follow_headers,
                         "normal redirect must not gain fallback hint")

    def test_fallback_request_redirect_preserves_fallback(self):
        fc = FakeCluster(["node-a", "node-b", "node-c"])
        follow_headers = {}

        def redirect(payload, headers):
            return 307, [("Location", "http://node-c:80/api/v1/select")], b'{}'

        def capture_follow(payload, headers):
            follow_headers.clear(); follow_headers.update(headers)
            return 200, [], b'{"rows":[],"row_count":0}'

        key = self._owned_key(fc, "node-a")
        fc.set_responder("node-a", "/api/v1/select", redirect)
        fc.set_responder("node-b", "/api/v1/select", redirect)
        fc.set_responder("node-c", "/api/v1/select", capture_follow)
        client = fc.make_client()
        # Make node-a unavailable so we go through fallback to node-b first.
        fc.set_down("node-a")
        resp = client.select("users", {"column": "id", "value": key})
        self.assertEqual(resp["row_count"], 0)
        # The redirect follow should have preserved the fallback hint.
        self.assertEqual(follow_headers.get("X-CppPools-Fallback"), "1",
                         "fallback hint must be preserved across redirect")


class TestRedirectTargetRetry(unittest.TestCase):
    """Phase 5 Task 3: redirect targets get retry/backoff; target attribution."""

    def _owned_key(self, fc, node):
        for k in range(100):
            if fc.independent_owner(str(k)) == node:
                return k
        raise AssertionError("no key found for %s" % node)

    def test_redirect_target_503_then_success(self):
        fc = FakeCluster(["node-a", "node-b"])
        state = {"n": 0}

        def redirect(payload, headers):
            return 307, [("Location", "http://node-b:80/api/v1/select")], b'{}'

        def flaky(payload, headers):
            state["n"] += 1
            if state["n"] == 1:
                return 503, [], b'{"error":"busy"}'
            return 200, [], b'{"rows":[],"row_count":0}'

        key = self._owned_key(fc, "node-a")
        fc.set_responder("node-a", "/api/v1/select", redirect)
        fc.set_responder("node-b", "/api/v1/select", flaky)
        client = fc.make_client()
        resp = client.select("users", {"column": "id", "value": key})
        self.assertEqual(resp["row_count"], 0)
        self.assertGreaterEqual(state["n"], 2, "target should retry on 503")
        # node-a (the 307 responder) must not be blacklisted.
        self.assertNotIn("node-a", client.blacklisted_nodes())

    def test_redirect_target_unreachable_blacklists_target_only(self):
        fc = FakeCluster(["node-a", "node-b", "node-c"])

        def redirect(payload, headers):
            return 307, [("Location", "http://node-b:80/api/v1/select")], b'{}'

        key = self._owned_key(fc, "node-a")
        fc.set_responder("node-a", "/api/v1/select", redirect)
        fc.set_down("node-b")
        client = fc.make_client()
        with self.assertRaises(errors.CppPoolsError):
            client.select("users", {"column": "id", "value": key})
        # node-b should be blacklisted (it's the unreachable target).
        self.assertIn("node-b", client.blacklisted_nodes())
        # node-a must not be blacklisted (it returned a valid 307).
        self.assertNotIn("node-a", client.blacklisted_nodes())

    def test_autoincrement_insert_redirected_to_unreachable_does_not_retry(self):
        fc = FakeCluster(["node-a", "node-b"])

        def redirect(payload, headers):
            return 307, [("Location", "http://node-b:80/api/v1/insert")], b'{}'

        fc.set_responder("node-a", "/api/v1/insert", redirect)
        fc.set_down("node-b")
        client = fc.make_client()
        with self.assertRaises(errors.CppPoolsError):
            client.insert("users", {"name": "auto"})
        # Only one insert attempt to node-a (the 307).
        inserts_a = [r for r in fc.log if r["node"] == "node-a" and r["path"] == "/api/v1/insert"]
        self.assertEqual(len(inserts_a), 1)
        # node-b should be blacklisted (unreachable target), node-a should not.
        self.assertIn("node-b", client.blacklisted_nodes())
        self.assertNotIn("node-a", client.blacklisted_nodes())




if __name__ == "__main__":
    unittest.main()
