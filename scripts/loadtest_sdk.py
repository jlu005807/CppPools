#!/usr/bin/env python3
"""SDK 并发压测（cluster-cache spec v1.1 决策证据采集）。

回答两个问题（判据见 spec §5.6 / docs/api/sdk.md §5）：
1. 缓存在并发下的真实收益：cache on/off 对照（命中率、延迟分位、吞吐、
   DB 回源次数）；
2. 归队 miss 风暴是否存在：负载中杀节点→复活，采样 misses 时间线。

附带量化"每请求新建连接"的开销占比（keep-alive 决策的输入）：
服务端 HTTP 模型为 Connection: close，keep-alive 需双端改造，
本脚本只给出当前模型下的基线数字。

用法：
    python3 scripts/loadtest_sdk.py bench --cache on  --workers 8 --total 8000
    python3 scripts/loadtest_sdk.py bench --cache off --workers 8 --total 8000
    python3 scripts/loadtest_sdk.py storm --workers 8 --duration 30

节点由脚本自管理（18401-18403），跑完全自动清理。
"""
import argparse
import json
import os
import sqlite3
import subprocess
import sys
import threading
import time
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "sdk", "python"))

from cpppools.client import ClusterClient  # noqa: E402

PORTS = [18401, 18402, 18403]
NAMES = ["node-a", "node-b", "node-c"]
WORK = os.path.join(os.path.dirname(__file__), "..", "output", "loadtest")

CONFIG_TMPL = """{
  "server": { "port": %(port)d, "worker_threads": 8 },
  "cluster": {
    "node_id": "%(node)s", "epoch": 1,
    "nodes": [
      { "node_id": "node-a", "host": "127.0.0.1", "port": 18401 },
      { "node_id": "node-b", "host": "127.0.0.1", "port": 18402 },
      { "node_id": "node-c", "host": "127.0.0.1", "port": 18403 }
    ]
  },
  "cache": { "enabled": %(cache_enabled)s, "ttl_ms": %(ttl_ms)d,
             "blacklist_ttl_ms": %(blacklist_ttl_ms)d },
  "datasources": [
    { "name": "main", "type": "sqlite", "default": true, "pool_size": 4,
      "sqlite": { "path": "%(db)s" } }
  ]
}"""


def base(port):
    return "http://127.0.0.1:%d" % port


class Cluster:
    def __init__(self, cache_enabled, ttl_ms=600000, blacklist_ttl_ms=600000):
        # I1（blacklist_ttl ≥ cache_ttl）是启动 fail-fast 硬约束：
        # bench 用长 TTL 测稳态命中；storm 只能两者同取 2s（归队回切
        # 依赖短黑名单，代价是 TTL 驱动的周期性 miss 会抬高基线 ——
        # 风暴信号看归队窗口的增量尖峰而非常数项）。
        self.cache_enabled = cache_enabled
        self.ttl_ms = ttl_ms
        self.blacklist_ttl_ms = blacklist_ttl_ms
        self.procs = {}
        os.makedirs(WORK, exist_ok=True)
        for n in NAMES:
            db = self.db(n)
            for suffix in ("", "-wal", "-shm"):
                try:
                    os.remove(db + suffix)
                except OSError:
                    pass
            conn = sqlite3.connect(db)
            conn.execute("CREATE TABLE users("
                         "id INTEGER PRIMARY KEY, name TEXT NOT NULL)")
            conn.close()

    def db(self, node):
        return os.path.join(WORK, "lt_%s.db" % node)

    def cfg(self, node):
        path = os.path.join(WORK, "lt_%s.json" % node)
        with open(path, "w") as f:
            f.write(CONFIG_TMPL % {
                "port": PORTS[NAMES.index(node)], "node": node,
                "cache_enabled": "true" if self.cache_enabled else "false",
                "ttl_ms": self.ttl_ms,
                "blacklist_ttl_ms": self.blacklist_ttl_ms,
                "db": self.db(node)})
        return path

    def start(self, node):
        log = open(os.path.join(WORK, "lt_%s.log" % node), "wb")
        self.procs[node] = subprocess.Popen(
            [EXE, "--config", self.cfg(node)],
            stdout=log, stderr=subprocess.STDOUT)
        for _ in range(50):
            try:
                with urllib.request.urlopen(base(PORTS[NAMES.index(node)])
                                            + "/health", timeout=1) as r:
                    if r.status == 200:
                        return
            except Exception:
                time.sleep(0.1)
        raise RuntimeError("node %s failed to start" % node)

    def start_all(self):
        for n in NAMES:
            self.start(n)

    def kill(self, node):
        p = self.procs.get(node)
        if p:
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
            self.procs[node] = None

    def stop_all(self):
        for n in NAMES:
            self.kill(n)


EXE = None  # 由 main 填充


def cluster_metrics():
    """聚合三节点的缓存指标。"""
    total = {}
    for port in PORTS:
        try:
            with urllib.request.urlopen(base(port) + "/metrics",
                                        timeout=2) as r:
                text = r.read().decode()
        except Exception:
            continue
        for line in text.splitlines():
            for name in ("cpppools_cache_hits_total",
                         "cpppools_cache_misses_total",
                         "cpppools_cache_entries",
                         "cpppools_cache_singleflight_merged_total",
                         "cpppools_cache_backfill_dropped_total",
                         "cpppools_redirects_total"):
                if line.startswith(name + "{") or line.startswith(name + " "):
                    total[name] = total.get(name, 0) + int(line.rsplit(" ", 1)[1])
    return total


def percentile(sorted_vals, p):
    if not sorted_vals:
        return 0.0
    return sorted_vals[min(len(sorted_vals) - 1, int(len(sorted_vals) * p))]


def run_load(client, keys, total, workers, latencies, errors, stop_flag=None):
    """worker 主体：随机热点键 select。"""
    import random
    rng = random.Random()
    local_lat = []
    done = 0
    while True:
        if stop_flag is not None:
            if stop_flag.is_set():
                break
        else:
            if done >= total // workers:
                break
        key = rng.randint(1, keys)
        t0 = time.perf_counter()
        try:
            resp = client.select("users", {"column": "id", "value": key})
            assert resp["row_count"] == 1
            local_lat.append((time.perf_counter() - t0) * 1000.0)
        except Exception as e:  # noqa: BLE001 压测统计所有失败形态
            errors.append(repr(e)[:120])
        done += 1
    latencies.extend(local_lat)


def bench(args):
    cluster = Cluster(cache_enabled=(args.cache == "on"))
    cluster.start_all()
    try:
        client = ClusterClient([base(p) for p in PORTS], backoff_base=0.01)
        # 预热数据：keys 个热点键。
        for i in range(1, args.keys + 1):
            client.insert("users", {"id": i, "name": "u%d" % i})
        # 预热缓存（cache on 时让热点键先入缓存，测稳态命中率）。
        for i in range(1, args.keys + 1):
            client.select("users", {"column": "id", "value": i})
        m0 = cluster_metrics()

        latencies, errors = [], []
        t0 = time.perf_counter()
        threads = [threading.Thread(target=run_load,
                                    args=(client, args.keys, args.total,
                                          args.workers, latencies, errors))
                   for _ in range(args.workers)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        wall = time.perf_counter() - t0

        m1 = cluster_metrics()
        latencies.sort()
        hits = m1.get("cpppools_cache_hits_total", 0) - m0.get("cpppools_cache_hits_total", 0)
        misses = m1.get("cpppools_cache_misses_total", 0) - m0.get("cpppools_cache_misses_total", 0)
        n = len(latencies) + len(errors)
        print("=== bench: cache=%s workers=%d total=%d keys=%d ==="
              % (args.cache, args.workers, args.total, args.keys))
        print("完成 %d 请求 / %.2fs = %.0f req/s（失败 %d）"
              % (n, wall, n / wall, len(errors)))
        if latencies:
            print("延迟 ms: avg=%.2f p50=%.2f p95=%.2f p99=%.2f max=%.2f"
                  % (sum(latencies) / len(latencies),
                     percentile(latencies, 0.50), percentile(latencies, 0.95),
                     percentile(latencies, 0.99), latencies[-1]))
        if hits or misses:
            print("缓存: hits=%d misses=%d 命中率=%.2f%% entries=%d "
                  "singleflight_merged=%d"
                  % (hits, misses, 100.0 * hits / max(1, hits + misses),
                     m1.get("cpppools_cache_entries", 0),
                     m1.get("cpppools_cache_singleflight_merged_total", 0)))
        else:
            print("缓存: 未启用（misses 即 DB 回源读 = %d 次 select 全回库）"
                  % n)
        print("redirects=%d（稳态必须为 0：SDK 直连 owner）"
              % m1.get("cpppools_redirects_total", 0))
        if errors:
            print("失败样例: %s" % errors[:3])
    finally:
        cluster.stop_all()


def storm(args):
    """负载运行中杀 node-c → 复活，采样 misses 时间线观测归队风暴。"""
    cluster = Cluster(cache_enabled=True, ttl_ms=2000, blacklist_ttl_ms=2000)
    cluster.start_all()
    try:
        client = ClusterClient([base(p) for p in PORTS], backoff_base=0.01)
        for i in range(1, args.keys + 1):
            client.insert("users", {"id": i, "name": "u%d" % i})
        for i in range(1, args.keys + 1):
            client.select("users", {"column": "id", "value": i})

        latencies, errors = [], []
        stop_flag = threading.Event()
        samples = []          # (t, 累计 misses, 累计 hits)
        kill_at = args.duration * 0.4
        revive_at = args.duration * 0.6

        def sampler():
            t0 = time.monotonic()
            while not stop_flag.is_set():
                m = cluster_metrics()
                samples.append((time.monotonic() - t0,
                                m.get("cpppools_cache_misses_total", 0),
                                m.get("cpppools_cache_hits_total", 0)))
                time.sleep(0.5)

        workers = [threading.Thread(target=run_load,
                                    args=(client, args.keys, 0, args.workers,
                                          latencies, errors, stop_flag))
                   for _ in range(args.workers)]
        sampler_t = threading.Thread(target=sampler)
        t0 = time.monotonic()
        for t in workers:
            t.start()
        sampler_t.start()
        killed = revived = False
        while time.monotonic() - t0 < args.duration:
            elapsed = time.monotonic() - t0
            if not killed and elapsed >= kill_at:
                cluster.kill("node-c")
                killed = True
                print("[t=%.1fs] 杀掉 node-c" % elapsed)
            if not revived and elapsed >= revive_at:
                cluster.start("node-c")
                revived = True
                print("[t=%.1fs] 复活 node-c（黑名单 2s 后回切 + Rejoin 清缓存）"
                      % elapsed)
            time.sleep(0.2)
        stop_flag.set()
        for t in workers:
            t.join()
        sampler_t.join()

        latencies.sort()
        n = len(latencies) + len(errors)
        print("=== storm: workers=%d duration=%ds ===" % (args.workers,
                                                          args.duration))
        print("完成 %d 请求（失败 %d）；延迟 ms: p50=%.2f p95=%.2f p99=%.2f"
              % (n, len(errors), percentile(latencies, 0.50),
                 percentile(latencies, 0.95), percentile(latencies, 0.99)))
        if errors:
            print("失败样例: %s" % errors[:3])
        print("misses 时间线（t, 累计miss, 窗口内miss增量）：")
        prev = 0
        for t, miss, hits in samples:
            mark = ""
            if kill_at <= t < kill_at + 0.6:
                mark = "  <- kill"
            if revive_at <= t < revive_at + 0.6:
                mark = "  <- revive"
            print("  t=%5.1fs  miss=%6d  Δ=%5d%s" % (t, miss, miss - prev,
                                                      mark))
            prev = miss
    finally:
        cluster.stop_all()


def main():
    global EXE
    root = os.path.join(os.path.dirname(__file__), "..")
    EXE = os.path.join(root, "build", "bin", "cpppools_server")
    if not os.path.exists(EXE):
        print("未找到 %s，先 scripts/build.sh" % EXE, file=sys.stderr)
        sys.exit(1)
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("bench")
    b.add_argument("--cache", choices=["on", "off"], default="on")
    b.add_argument("--workers", type=int, default=8)
    b.add_argument("--total", type=int, default=8000)
    b.add_argument("--keys", type=int, default=100)
    s = sub.add_parser("storm")
    s.add_argument("--workers", type=int, default=8)
    s.add_argument("--duration", type=int, default=30)
    s.add_argument("--keys", type=int, default=100)
    args = ap.parse_args()
    if args.cmd == "bench":
        bench(args)
    else:
        storm(args)


if __name__ == "__main__":
    main()
