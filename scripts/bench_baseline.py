#!/usr/bin/env python3
"""性能基线采集（cluster-cache spec §8.2）：缓存上线前的读延迟基线。

用法：先启动服务（scripts/run_server.sh 或 e2e 方式），再运行：
    python3 scripts/bench_baseline.py --port 8080 --table user_scores --n 2000
前置：表内已有一行数据（脚本会先插入一行 user_id=1000001）。
输出：p50/p95/p99/avg（毫秒）+ DB 配置，人工归档进 docs/benchmarks/。
"""
import argparse
import json
import time
import urllib.request

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--table", default="user_scores")
    ap.add_argument("--n", type=int, default=2000)
    args = ap.parse_args()
    base = f"http://127.0.0.1:{args.port}"

    def post(path, payload):
        req = urllib.request.Request(base + path, data=json.dumps(payload).encode(), method="POST")
        with urllib.request.urlopen(req) as r:
            return r.status, r.read()

    # 确保有一行可读。
    post("/api/v1/insert", {"table": args.table,
                            "values": {"user_id": 1000001, "score": 1, "updated_at_ms": 0}})

    payload = json.dumps({"table": args.table, "columns": ["score"],
                          "where": {"column": "user_id", "value": 1000001}}).encode()
    latencies = []
    for _ in range(args.n):
        req = urllib.request.Request(base + "/api/v1/select", data=payload, method="POST")
        t0 = time.perf_counter()
        with urllib.request.urlopen(req) as r:
            assert r.status == 200
            r.read()
        latencies.append((time.perf_counter() - t0) * 1000.0)

    latencies.sort()
    def pct(p):
        return latencies[min(len(latencies) - 1, int(len(latencies) * p))]
    print(f"n={args.n}")
    print(f"avg={sum(latencies)/len(latencies):.3f}ms "
          f"p50={pct(0.50):.3f}ms p95={pct(0.95):.3f}ms p99={pct(0.99):.3f}ms")

if __name__ == "__main__":
    main()
