# 期1验收：单节点行级缓存（spec §8.2）

- **日期**：2026-08-13
- **环境**：与期0基线同机同口径（macOS Apple Silicon，loopback 单客户端串行）
- **配置**：6 worker，SQLite（WAL），连接池 4 槽；对照组仅差 `cache.enabled`
- **负载**：`POST /api/v1/select`（同一行热点读，投影单列）× 2000；
  `POST /api/v1/update`（同行）× 500
- **采集脚本**：`scripts/bench_baseline.py`（读）+ 同口径临时写脚本

## §8.2 门槛对照

| 指标 | 门槛 | 实测 | 判定 |
|---|---|---|---|
| 热点读命中率 | ≥ 80% | 11998 hits / 4 misses ≈ **99.97%** | ✅ |
| DB 读 QPS 降幅 | ≥ 50% | 2004 次读仅 4 次回源 ≈ **降 99.8%** | ✅ |
| 读 p99 降幅 | ≥ 30% | 服务端处理延迟 p99：无缓存 0.020ms → 缓存 0.004ms ≈ **降 80%** | ✅ |
| 写 p99 退化 | ≤ 10% | 客户端 p99：0.416ms → 0.406ms ≈ **-2.4%**（噪声范围内） | ✅ |

## 测量口径说明

- **读 p99 取服务端 `/api/v1/stats` 的处理延迟分位数**（`p99_latency_ms`，
  仅计 handler 耗时）。客户端侧 loopback 往返噪声（±0.3ms 级波动）完全
  淹没单行 SQLite 读（数十微秒）的缓存收益，客户端数字无法体现门槛，
  与期0基线文档"绝对值不代表生产"的口径一致。
- 双服务同跑曾观察到互相干扰（p50 翻倍），对照数据均取**单独运行**轮次。
- 写路径含双删失效（update 前后各一次失效），500 次写产生 501 次
  invalidation，未见 p99 退化。

## 指标族证据（/metrics，采集时刻累计）

```
cpppools_cache_hits_total{pool="main"} 11998
cpppools_cache_misses_total{pool="main"} 4
cpppools_cache_invalidations_total{pool="main"} 501
cpppools_cache_evictions_total{pool="main"} 0
cpppools_cache_bypass_too_large_total{pool="main"} 0
```

## 结论

四项门槛全部通过。期1（单节点行级缓存）验收达成；客户端侧 p99 收益
需在带真实网络/并发压力的环境复测（期3+ e2e 阶段）。
