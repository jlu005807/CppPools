#!/usr/bin/env bash
# 以 SQLite 后端启动 CppPools 服务（Linux / macOS）。
#
# 用法:
#   scripts/run_server.sh                          # 端口/worker 数取配置文件默认
#   scripts/run_server.sh --port 9000 --workers 32
#   scripts/run_server.sh --db /path/to/existing.db   # 使用已有库（不覆盖）
#
# 数据源建连完全由 JSON 配置驱动（期 3 起，环境变量建连已删除）。
# 演示库 data/cpppools_demo.db 不存在时用 scripts/init_demo_db.sql 预置
# （中间件不建表 —— 表必须由使用方预置，演示脚本代劳）。
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

PORT=8080
WORKERS=6
DB_PATH="data/cpppools_demo.db"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --port)     PORT="$2"; shift 2 ;;
        --workers)  WORKERS="$2"; shift 2 ;;
        --db)       DB_PATH="$2"; shift 2 ;;
        -h|--help)  sed -n '2,10p' "$0"; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

EXE="build/bin/cpppools_server"
if [[ ! -x "$EXE" ]]; then
    echo "错误: 未找到 $EXE，请先运行 scripts/build.sh" >&2
    exit 1
fi

# 演示库种子：库文件不存在才建（绝不覆盖使用方已有数据）。
if [[ ! -f "$DB_PATH" ]]; then
    if ! command -v sqlite3 >/dev/null 2>&1; then
        echo "错误: $DB_PATH 不存在且未安装 sqlite3 CLI，无法预置演示库" >&2
        echo "      请先建库（表结构见 scripts/init_demo_db.sql）或用 --db 指定已有库" >&2
        exit 1
    fi
    mkdir -p "$(dirname "$DB_PATH")"
    sqlite3 "$DB_PATH" < scripts/init_demo_db.sql
    echo "已预置演示库: $DB_PATH"
fi

# 端口/worker 数 + 数据源写入临时配置。
TMP_CONFIG="$(mktemp "${TMPDIR:-/tmp}/cpppools_config.XXXXXX.json")"
trap 'rm -f "$TMP_CONFIG"' EXIT
cat > "$TMP_CONFIG" <<CFGEOF
{
  "server": { "port": $PORT, "worker_threads": $WORKERS },
  "datasources": [
    { "name": "main", "type": "sqlite", "default": true, "pool_size": 4,
      "sqlite": { "path": "$DB_PATH" } }
  ]
}
CFGEOF

echo "启动 CppPools 服务"
echo "  后端    : sqlite ($DB_PATH)"
echo "  监听    : http://127.0.0.1:$PORT/"
echo "  workers : $WORKERS"
echo "  探针    : curl http://127.0.0.1:$PORT/health"
echo

exec "$EXE" --config "$TMP_CONFIG"
