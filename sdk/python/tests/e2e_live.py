"""期3 三节点 live e2e（cluster-cache spec §11 期3 交付物）。

由 scripts/sdk_e2e.sh 编排调用：
    python3 e2e_live.py <phase> <port1> <port2> <port3>
phase：
    1 三节点稳态：SDK 直连 owner 写读一致、无 307（两端环一致）、分布 ≥2 节点
    2 node-c 被杀：其归属键经 Fallback 就地处理（替补 reason=fallback 计数）
    3 node-c 复活 + 黑名单过期：键回到 node-c（直连恢复）
    4 node-c epoch+1 重启：epoch 门控优雅降级（操作仍成功 + 服务端计数）

期4 增补（Python 自管理进程，避免跨进程编排竞态）：
    python3 e2e_live.py flow <exe> <workdir> <port1> <port2> <port3>
    flow：杀 node-c → 绕行写 → 复活 → 黑名单过期回切（Rejoin 清缓存
    取证）→ 逐节点滚动重启 epoch 收敛 → 全程读写不断。
"""
import json
import os
import subprocess
import sys
import threading
import time
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from cpppools.client import ClusterClient  # noqa: E402
from cpppools.hashing import query_key  # noqa: E402


def base(port):
    return "http://127.0.0.1:%d" % port


def get_metrics(port):
    with urllib.request.urlopen(base(port) + "/metrics", timeout=3) as r:
        return r.read().decode()


def metric(metrics_text, line_prefix):
    for line in metrics_text.splitlines():
        if line.startswith(line_prefix + " "):
            return int(line.split(" ", 1)[1])
    raise AssertionError("metrics 缺 %s" % line_prefix)


def owners_of(client, ids):
    return {client._topology.ring.owner(query_key("", "users", "id", str(i), 0))
            for i in ids}


def run_phase1(ports):
    client = ClusterClient([base(p) for p in ports],
                           topology_refresh=30.0, backoff_base=0.01)
    ids = range(1, 31)
    for i in ids:
        resp = client.insert("users", {"id": i, "name": "u%d" % i})
        assert resp.get("affected_rows") == 1, resp
    # 读回一致（同键强一致，稳态）。
    for i in ids:
        resp = client.select("users", {"column": "id", "value": i})
        assert resp["rows"] == [[i, "u%d" % i]], resp
    # 分布：30 个键必须落在 ≥2 个节点（环真的在分流）。
    dist = owners_of(client, ids)
    assert len(dist) >= 2, "30 键全落一个节点：%s" % dist
    # 两端环一致的硬证据：全程无 307（SDK 直连命中 owner）。
    for port in ports:
        redirects = metric(get_metrics(port), "cpppools_redirects_total")
        assert redirects == 0, "节点 %d 出现 307：%d（两端环不一致！）" % (port, redirects)
    print("phase1 OK: 30 键写读一致，分布 %d 节点，0 次 307" % len(dist))


def run_phase2(ports):
    alive = [ports[0], ports[1]]
    client = ClusterClient([base(p) for p in alive], backoff_base=0.01)
    # 找归属 node-c 的键（用 client 自己的环 —— 与三节点时同拓扑）。
    dead_owned = [i for i in range(1001, 1201)
                  if client._topology.ring.owner(
                      query_key("", "users", "id", str(i), 0)) == "node-c"]
    assert dead_owned, "找不到归属 node-c 的键"
    sample = dead_owned[:5]
    for i in sample:
        resp = client.insert("users", {"id": i, "name": "f%d" % i})
        assert resp.get("affected_rows") == 1, resp
        resp = client.select("users", {"column": "id", "value": i})
        assert resp["row_count"] == 1, resp
    # 替补节点的 reason=fallback 计数必须增长（规则 A 的可观测证据）。
    fallback_total = 0
    for port in alive:
        fallback_total += metric(
            get_metrics(port),
            'cpppools_cache_bypass_total{pool="main",reason="fallback"}')
    assert fallback_total >= len(sample), \
        "替补 fallback 计数不足：%d < %d" % (fallback_total, len(sample))
    print("phase2 OK: %d 个 node-c 键经 Fallback 就地处理（计数 %d）"
          % (len(sample), fallback_total))


def run_phase3(ports):
    client = ClusterClient([base(p) for p in ports], backoff_base=0.01)
    c_owned = [i for i in range(2001, 2201)
               if client._topology.ring.owner(
                   query_key("", "users", "id", str(i), 0)) == "node-c"]
    assert c_owned, "找不到归属 node-c 的键"
    sample = c_owned[:5]

    def c_requests():
        import json
        with urllib.request.urlopen(base(ports[2]) + "/api/v1/stats",
                                    timeout=3) as r:
            return json.load(r)["total_requests"]

    before = c_requests()
    for i in sample:
        resp = client.insert("users", {"id": i, "name": "r%d" % i})
        assert resp.get("affected_rows") == 1, resp
    after = c_requests()
    # 复活 + 黑名单过期（脚本已等过 TTL）→ 键直连回 node-c。
    assert after - before >= len(sample), \
        "node-c 未收到回切请求（%d -> %d）" % (before, after)
    print("phase3 OK: node-c 复活后 %d 个键直连回切" % len(sample))


def run_phase4(ports):
    client = ClusterClient([base(p) for p in ports], backoff_base=0.01)
    c_owned = [i for i in range(3001, 3201)
               if client._topology.ring.owner(
                   query_key("", "users", "id", str(i), 0)) == "node-c"]
    assert c_owned, "找不到归属 node-c 的键"
    # node-c 已带新 epoch 重启：epoch 门控应优雅降级（bypass + 回显），
    # 操作仍然成功 —— 正确性优先于加速（§5.4）。
    for i in c_owned[:3]:
        resp = client.insert("users", {"id": i, "name": "e%d" % i})
        assert resp.get("affected_rows") == 1, resp
    epoch_bypass = metric(
        get_metrics(ports[2]),
        'cpppools_cache_bypass_total{pool="main",reason="epoch_mismatch"}')
    assert epoch_bypass >= 1, "node-c 未记录 epoch_mismatch bypass"
    print("phase4 OK: epoch 不匹配优雅降级（服务端 bypass 计数 %d）"
          % epoch_bypass)


def main():
    phase = sys.argv[1]
    if phase == "flow":
        run_flow(sys.argv[2], sys.argv[3], [int(p) for p in sys.argv[4:7]])
        return
    ports = [int(p) for p in sys.argv[2:5]]
    {"1": run_phase1, "2": run_phase2,
     "3": run_phase3, "4": run_phase4}[phase](ports)


# ===== 期4 flow：Python 自管理三节点的归队/滚动重启全链路 =====

CONFIG_TMPL = """{
  "server": { "port": %(port)d, "worker_threads": 4 },
  "cluster": {
    "node_id": "%(node)s",
    "epoch": %(epoch)d,
    "nodes": [
      { "node_id": "node-a", "host": "127.0.0.1", "port": %(pa)d },
      { "node_id": "node-b", "host": "127.0.0.1", "port": %(pb)d },
      { "node_id": "node-c", "host": "127.0.0.1", "port": %(pc)d }
    ]
  },
  "cache": { "enabled": true, "ttl_ms": 2000, "blacklist_ttl_ms": 2000 },
  "datasources": [
    { "name": "main", "type": "sqlite", "default": true, "pool_size": 4,
      "sqlite": { "path": "%(db)s" } }
  ]
}"""


class MiniCluster:
    """Python 自管理的三节点：可随时 kill/复活/改 epoch 重启。"""

    def __init__(self, exe, workdir, ports):
        self.exe = exe
        self.workdir = workdir
        self.ports = ports
        self.names = ["node-a", "node-b", "node-c"]
        self.procs = {}
        self.epochs = {n: 1 for n in self.names}
        import sqlite3
        for n in self.names:
            db = self.db_path(n)
            if os.path.exists(db):
                os.remove(db)
            conn = sqlite3.connect(db)
            conn.execute("CREATE TABLE users("
                         "id INTEGER PRIMARY KEY, name TEXT NOT NULL)")
            conn.close()

    def db_path(self, node):
        return os.path.join(self.workdir, "flow_%s.db" % node)

    def cfg_path(self, node):
        return os.path.join(self.workdir, "flow_%s.json" % node)

    def port_of(self, node):
        return self.ports[self.names.index(node)]

    def write_cfg(self, node):
        with open(self.cfg_path(node), "w") as f:
            f.write(CONFIG_TMPL % {
                "port": self.port_of(node), "node": node,
                "epoch": self.epochs[node],
                "pa": self.ports[0], "pb": self.ports[1],
                "pc": self.ports[2], "db": self.db_path(node)})

    def start(self, node):
        self.write_cfg(node)
        log = open(os.path.join(self.workdir, "flow_%s.log" % node), "wb")
        self.procs[node] = subprocess.Popen(
            [self.exe, "--config", self.cfg_path(node)],
            stdout=log, stderr=subprocess.STDOUT)
        self.wait_health(node)

    def start_all(self):
        for n in self.names:
            self.write_cfg(n)
        for n in self.names:
            self.start(n)

    def kill(self, node):
        proc = self.procs.get(node)
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()  # SIGTERM 失效兜底，防 e2e 永久挂起
                proc.wait()
            self.procs[node] = None

    def restart(self, node, epoch=None):
        self.kill(node)
        if epoch is not None:
            self.epochs[node] = epoch
        self.start(node)

    def wait_health(self, node):
        url = base(self.port_of(node)) + "/health"
        for _ in range(50):
            try:
                with urllib.request.urlopen(url, timeout=1) as r:
                    if r.status == 200:
                        return
            except Exception:
                pass
            time.sleep(0.1)
        raise AssertionError("节点 %s 未就绪" % node)

    def stop_all(self):
        for n in self.names:
            self.kill(n)


def run_flow(exe, workdir, ports):
    cluster = MiniCluster(exe, workdir, ports)
    cluster.start_all()
    try:
        client = ClusterClient([base(p) for p in ports], backoff_base=0.01)

        # 稳态：写读 30 键（部分会被缓存，为 Rejoin 清理准备旧条目）。
        for i in range(1, 31):
            client.insert("users", {"id": i, "name": "u%d" % i})
            resp = client.select("users", {"column": "id", "value": i})
            assert resp["rows"] == [[i, "u%d" % i]], resp
            client.select("users", {"column": "id", "value": i})  # 回填进缓存
        print("flow: 稳态 30 键写读一致")

        ring = client._topology.ring
        owned_by = lambda i: ring.owner(
            query_key("", "users", "id", str(i), 0))
        c_owned_pre = [i for i in range(1, 31) if owned_by(i) == "node-c"]
        c_owned_new = [i for i in range(5001, 5401) if owned_by(i) == "node-c"]
        assert c_owned_pre and c_owned_new, "找不到归属 node-c 的键"

        # ---- 阶段5：杀 node-c → 绕行写 → 复活 → 回切（Rejoin 清缓存）----
        cluster.kill("node-c")
        # 新键经 fallback 写入替补节点的库（c 被拉黑）—— 故障窗口的写入。
        for i in c_owned_new[:3]:
            resp = client.insert("users", {"id": i, "name": "f%d" % i})
            assert resp.get("affected_rows") == 1, resp
        assert "node-c" in client.blacklisted_nodes()
        cluster.start("node-c")
        # 等过黑名单 TTL：用客户端实际拿到的 TTL 派生，避免与配置双写。
        time.sleep(client._topology.blacklist_ttl + 0.3)
        # 回切首请求自动带 Rejoin 头 → node-c 清空缓存（§5.2 规则 B）。
        # 读的是 kill 之前已写入 c 库的键（同键强一致：owner 库是真理之源）。
        for i in c_owned_pre:
            resp = client.select("users", {"column": "id", "value": i})
            assert resp["rows"] == [[i, "u%d" % i]], resp
        flush = metric(get_metrics(cluster.port_of("node-c")),
                       'cpppools_cache_flush_total{pool="main",reason="rejoin"}')
        assert flush >= 1, "归队 Rejoin 未触发清缓存（flush=%d）" % flush
        print("flow: 杀节点→绕行→复活回切，Rejoin 清缓存生效（flush=%d）" % flush)

        # ---- 阶段6：滚动重启（epoch 1→3），停机窗口内流量不断 ----
        # 后台线程在整个滚动阶段持续读写（评审 W2：旧版只在重启完成后
        # 发请求，停机窗口可用性未被真实验证）。窗口内新键的读可能落
        # 替补（owner 重启中，行在 owner 库）⇒ 空行属故障窗口的已记录
        # 语义（§5.1 例外）；断言目标是**可用性**：全程零未捕获异常。
        stop_traffic = threading.Event()
        traffic_errors = []
        traffic_counter = [7000]

        def traffic():
            while not stop_traffic.is_set():
                i = traffic_counter[0]
                traffic_counter[0] += 1
                try:
                    client.insert("users", {"id": i, "name": "t%d" % i})
                    resp = client.select("users", {"column": "id", "value": i})
                    assert resp["row_count"] in (0, 1)
                except Exception as e:  # noqa: BLE001 窗口内一切异常都算失败
                    traffic_errors.append(repr(e))
                    return

        worker = threading.Thread(target=traffic)
        worker.start()
        try:
            for node in cluster.names:
                cluster.restart(node, epoch=3)
                time.sleep(0.4)  # 让流量在停机窗口前后持续跑一段
        finally:
            stop_traffic.set()
            worker.join()
        assert not traffic_errors, \
            "滚动窗口内出现失败：%s" % traffic_errors[:3]
        assert traffic_counter[0] > 7000, "滚动窗口内流量未实际运行"

        # 收敛：全部节点 epoch=3，客户端视图同步。
        for node in cluster.names:
            with urllib.request.urlopen(
                    base(cluster.port_of(node)) + "/api/v1/cluster",
                    timeout=3) as r:
                assert json.load(r)["epoch"] == 3, "%s epoch 未收敛" % node
        assert client.current_epoch == 3, "客户端 epoch 未收敛"
        # 收敛后稳态：新键写读一致（同键强一致恢复）。
        for i in range(6001, 6006):
            resp = client.insert("users", {"id": i, "name": "r%d" % i})
            assert resp.get("affected_rows") == 1, resp
            resp = client.select("users", {"column": "id", "value": i})
            assert resp["rows"] == [[i, "r%d" % i]], resp
        print("flow: 滚动重启 epoch 收敛为 3，窗口内 %d 次读写零失败"
              % (traffic_counter[0] - 7000))
    finally:
        cluster.stop_all()


if __name__ == "__main__":
    main()
