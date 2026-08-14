#!/usr/bin/env bash
# 期2 双节点 e2e：归属自判 + 307 重定向 + Fallback 就地 + epoch 门控
#（cluster-cache spec §3.6 / §11 期2 交付物）。
#
# 拓扑：node-a(18201) / node-b(18202)，同一 epoch 的两节点互指视图，
# 各自独立 SQLite 库（同名数据源 main、同 schema —— 环路由的前提）。
#
# 用法：bash scripts/cluster_e2e.sh   （需先 scripts/build.sh）
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

EXE="build/bin/cpppools_server"
[[ -x "$EXE" ]] || { echo "错误: 未找到 $EXE，请先 scripts/build.sh" >&2; exit 1; }

A_PORT=18201
B_PORT=18202
WORK="$(mktemp -d "${TMPDIR:-/tmp}/cpppools_cluster_e2e.XXXXXX")"
PIDS=()

cleanup() {
    for pid in "${PIDS[@]:-}"; do kill "$pid" 2>/dev/null || true; done
    sleep 0.3
    rm -rf "$WORK"
}
trap cleanup EXIT

make_db() {
    sqlite3 "$1" "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL);"
}
make_db "$WORK/a.db"
make_db "$WORK/b.db"

make_cfg() {  # $1=node_id $2=port $3=db
    cat > "$WORK/$1.json" <<CFGEOF
{
  "server": { "port": $2, "worker_threads": 4 },
  "cluster": {
    "node_id": "$1",
    "epoch": 1,
    "nodes": [
      { "node_id": "node-a", "host": "127.0.0.1", "port": $A_PORT },
      { "node_id": "node-b", "host": "127.0.0.1", "port": $B_PORT }
    ]
  },
  "cache": { "enabled": true, "ttl_ms": 60000, "blacklist_ttl_ms": 60000 },
  "datasources": [
    { "name": "main", "type": "sqlite", "default": true, "pool_size": 4,
      "sqlite": { "path": "$3" } }
  ]
}
CFGEOF
}
make_cfg node-a "$A_PORT" "$WORK/a.db"
make_cfg node-b "$B_PORT" "$WORK/b.db"

"$EXE" --config "$WORK/node-a.json" > "$WORK/a.log" 2>&1 & PIDS+=($!)
"$EXE" --config "$WORK/node-b.json" > "$WORK/b.log" 2>&1 & PIDS+=($!)

for port in $A_PORT $B_PORT; do
    for _ in $(seq 1 50); do
        curl -s "http://127.0.0.1:$port/health" >/dev/null 2>&1 && break
        sleep 0.1
    done
    curl -sf "http://127.0.0.1:$port/health" >/dev/null \
        || { echo "FAIL: 节点 $port 未就绪" >&2; exit 1; }
done
echo "-> 两节点就绪"

insert_body() { echo "{\"table\":\"users\",\"values\":{\"id\":$1,\"name\":\"u$1\"}}"; }
select_body() { echo "{\"table\":\"users\",\"where\":{\"column\":\"id\",\"value\":$1}}"; }

# ① 路由分布：60 个键打 node-a，必有本地 201 与 307 两类。
owned=0; redirected=0; foreign_id=""; owned_id=""
for id in $(seq 1 60); do
    out=$(curl -s -o /dev/null -w '%{http_code} %{redirect_url}' \
          -X POST "http://127.0.0.1:$A_PORT/api/v1/insert" \
          -d "$(insert_body "$id")")
    code=${out%% *}; rurl=${out#* }
    case "$code" in
        201) owned=$((owned+1)); owned_id=$id ;;
        307) redirected=$((redirected+1)); foreign_id=$id
             [[ "$rurl" == http://127.0.0.1:$B_PORT/api/v1/insert ]] \
                 || { echo "FAIL: 307 的 Location 错误: $rurl" >&2; exit 1; } ;;
        *) echo "FAIL: 意外状态码 $code (id=$id)" >&2; exit 1 ;;
    esac
done
[[ $owned -gt 0 && $redirected -gt 0 ]] \
    || { echo "FAIL: 路由分布异常 owned=$owned redirected=$redirected" >&2; exit 1; }
echo "-> 路由分布: node-a 本地 ${owned} / 307 重定向 ${redirected}（Location 全部指向 node-b）"

# ①b 四端点门控一致性：update/delete 打非 owner 键同样回 307 + 正确 Location。
for op in update delete; do
    if [[ "$op" == "update" ]]; then
        body="{\"table\":\"users\",\"set\":{\"name\":\"z\"},\"where\":{\"column\":\"id\",\"value\":$foreign_id}}"
    else
        body="{\"table\":\"users\",\"where\":{\"column\":\"id\",\"value\":$foreign_id}}"
    fi
    out=$(curl -s -o /dev/null -w '%{http_code} %{redirect_url}' -X POST \
          "http://127.0.0.1:$A_PORT/api/v1/$op" -d "$body")
    code=${out%% *}; rurl=${out#* }
    [[ "$code" == "307" && "$rurl" == "http://127.0.0.1:$B_PORT/api/v1/$op" ]] \
        || { echo "FAIL: $op 非 owner 未正确重定向: $out" >&2; exit 1; }
done
echo "-> update/delete 同样门控重定向"

# ② curl -L 跟随 307：insert + select 端到端成功。
code=$(curl -s -o /dev/null -w '%{http_code}' -L -X POST \
       "http://127.0.0.1:$A_PORT/api/v1/insert" -d "$(insert_body 9001)")
[[ "$code" == "201" ]] || { echo "FAIL: -L insert 得 $code" >&2; exit 1; }
body=$(curl -s -L -X POST "http://127.0.0.1:$A_PORT/api/v1/select" -d "$(select_body 9001)")
echo "$body" | grep -q '"u9001"' || { echo "FAIL: -L select 未读回行: $body" >&2; exit 1; }
echo "-> curl -L 跟随 307 读写成功"

# ③ Fallback 就地：属 node-b 的键带 X-CppPools-Fallback 打 node-a，
#    就地 200 且 node-a 缓存条目不增长（规则 A）。
entries_before=$(curl -s "http://127.0.0.1:$A_PORT/api/v1/stats" \
                 | grep -o '"cache_entries":[0-9]*' | cut -d: -f2)
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
       -H "X-CppPools-Fallback: 1" \
       "http://127.0.0.1:$A_PORT/api/v1/select" -d "$(select_body "$foreign_id")")
[[ "$code" == "200" ]] || { echo "FAIL: Fallback 就地读得 $code" >&2; exit 1; }
entries_after=$(curl -s "http://127.0.0.1:$A_PORT/api/v1/stats" \
                | grep -o '"cache_entries":[0-9]*' | cut -d: -f2)
[[ "$entries_before" == "$entries_after" ]] \
    || { echo "FAIL: Fallback 就地读产生缓存条目 ($entries_before -> $entries_after)" >&2; exit 1; }
echo "-> Fallback 就地处理成功且不缓存（entries 保持 ${entries_after}）"

# ④ epoch 门控：属 node-a 的键带错误 epoch 打 node-a，
#    正常 200 + 回显 X-CppPools-Epoch: 1。
hdrs=$(curl -s -D - -o /dev/null -X POST -H "X-CppPools-Epoch: 999" \
       "http://127.0.0.1:$A_PORT/api/v1/select" -d "$(select_body "$owned_id")")
echo "$hdrs" | grep -q "^HTTP/1.1 200" || { echo "FAIL: epoch 门控响应非 200" >&2; exit 1; }
echo "$hdrs" | grep -qi "^X-CppPools-Epoch: 1" \
    || { echo "FAIL: 未回显 X-CppPools-Epoch: $hdrs" >&2; exit 1; }
echo "-> epoch 不匹配：正常处理 + 回显 X-CppPools-Epoch: 1"

# ⑤ 指标：redirects 与 bypass reason 已导出。
curl -s "http://127.0.0.1:$A_PORT/metrics" | grep -q "cpppools_redirects_total" \
    || { echo "FAIL: metrics 缺 cpppools_redirects_total" >&2; exit 1; }
curl -s "http://127.0.0.1:$A_PORT/metrics" \
    | grep -q 'cpppools_cache_bypass_total{pool="main",reason="epoch_mismatch"} 1' \
    || { echo "FAIL: metrics 缺 epoch_mismatch bypass 计数" >&2; exit 1; }
curl -s "http://127.0.0.1:$A_PORT/metrics" \
    | grep -q 'cpppools_cache_bypass_total{pool="main",reason="fallback"} 1' \
    || { echo "FAIL: metrics 缺 fallback bypass 计数" >&2; exit 1; }
echo "-> 指标族导出正确"

echo "PASS: 双节点 e2e 全部断言通过"
