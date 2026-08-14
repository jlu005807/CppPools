#!/usr/bin/env bash
# 期3 三节点 live e2e（cluster-cache spec §11 期3 交付物）。
#
# 拓扑：node-a/node-b/node-c（18301-18303），同名数据源 main、同 schema、
# 各自独立库。四个阶段（杀节点/复活/epoch 滚动）由本脚本编排，
# 断言在 sdk/python/tests/e2e_live.py。
#
# 用法：bash scripts/sdk_e2e.sh   （需先 scripts/build.sh）
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

EXE="build/bin/cpppools_server"
[[ -x "$EXE" ]] || { echo "错误: 未找到 $EXE，请先 scripts/build.sh" >&2; exit 1; }

PORT_A=18301
PORT_B=18302
PORT_C=18303
WORK="$(mktemp -d "${TMPDIR:-/tmp}/cpppools_sdk_e2e.XXXXXX")"
PID_A="" PID_B="" PID_C=""

cleanup() {
    for pid in "$PID_A" "$PID_B" "$PID_C"; do
        [[ -n "$pid" ]] && kill "$pid" 2>/dev/null || true
    done
    sleep 0.3
    rm -rf "$WORK"
}
trap cleanup EXIT

for n in a b c; do
    sqlite3 "$WORK/$n.db" \
        "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL);"
done

make_cfg() {  # $1=node_id $2=port $3=db $4=epoch
    cat > "$WORK/$1.json" <<CFGEOF
{
  "server": { "port": $2, "worker_threads": 4 },
  "cluster": {
    "node_id": "$1",
    "epoch": $4,
    "nodes": [
      { "node_id": "node-a", "host": "127.0.0.1", "port": $PORT_A },
      { "node_id": "node-b", "host": "127.0.0.1", "port": $PORT_B },
      { "node_id": "node-c", "host": "127.0.0.1", "port": $PORT_C }
    ]
  },
  "cache": { "enabled": true, "ttl_ms": 2000, "blacklist_ttl_ms": 2000 },
  "datasources": [
    { "name": "main", "type": "sqlite", "default": true, "pool_size": 4,
      "sqlite": { "path": "$3" } }
  ]
}
CFGEOF
}

start_node() {  # $1=node_id -> 设置 PID_<X>
    local node=$1
    "$EXE" --config "$WORK/$node.json" > "$WORK/$node.log" 2>&1 &
    case "$node" in
        node-a) PID_A=$! ;;
        node-b) PID_B=$! ;;
        node-c) PID_C=$! ;;
    esac
}

wait_health() {
    local port=$1
    for _ in $(seq 1 50); do
        curl -s "http://127.0.0.1:$port/health" >/dev/null 2>&1 && return 0
        sleep 0.1
    done
    echo "FAIL: 节点 $port 未就绪" >&2
    return 1
}

make_cfg node-a "$PORT_A" "$WORK/a.db" 1
make_cfg node-b "$PORT_B" "$WORK/b.db" 1
make_cfg node-c "$PORT_C" "$WORK/c.db" 1
start_node node-a; start_node node-b; start_node node-c
for port in $PORT_A $PORT_B $PORT_C; do wait_health "$port"; done
echo "-> 三节点就绪（epoch 1）"

E2E="sdk/python/tests/e2e_live.py"

python3 "$E2E" 1 "$PORT_A" "$PORT_B" "$PORT_C"

# ---- 阶段2：杀 node-c → Fallback 绕行 ----
kill "$PID_C"; sleep 0.3
python3 "$E2E" 2 "$PORT_A" "$PORT_B" "$PORT_C"

# ---- 阶段3：复活 node-c（epoch 不变）→ 回切 ----
start_node node-c
wait_health "$PORT_C"
python3 "$E2E" 3 "$PORT_A" "$PORT_B" "$PORT_C"

# ---- 阶段4：node-c 带 epoch 2 重启 → epoch 门控优雅降级 ----
kill "$PID_C"; sleep 0.3
make_cfg node-c "$PORT_C" "$WORK/c.db" 2
start_node node-c
wait_health "$PORT_C"
python3 "$E2E" 4 "$PORT_A" "$PORT_B" "$PORT_C"

# ---- 期4 flow：Python 自管理三节点的归队/滚动重启全链路（独立端口）----
python3 "$E2E" flow "$EXE" "$WORK" 18311 18312 18313

echo "PASS: 三节点 SDK e2e 全部阶段通过"
