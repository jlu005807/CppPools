#!/usr/bin/env bash
# 以 mock 或 mysql 后端启动 CppPools 服务（Linux / macOS）。
#
# 用法:
#   scripts/run_server.sh                          # mock 后端，端口 8080
#   scripts/run_server.sh --port 9000 --workers 32
#   scripts/run_server.sh --mysql --password 123456
#
# 提示: worker 数 > 连接池大小 时才能观察到连接池耗尽（503）。
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

PORT=8080
WORKERS=6
MODE="mock"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --port)     PORT="$2"; shift 2 ;;
        --workers)  WORKERS="$2"; shift 2 ;;
        --mysql)    MODE="mysql"; shift ;;
        --host)     export CPPPOOLS_DB_HOST="$2"; shift 2 ;;
        --user)     export CPPPOOLS_DB_USER="$2"; shift 2 ;;
        --password) export CPPPOOLS_DB_PASSWORD="$2"; shift 2 ;;
        --database) export CPPPOOLS_DB_NAME="$2"; shift 2 ;;
        --table)    export CPPPOOLS_DB_TABLE="$2"; shift 2 ;;
        --pool)     export CPPPOOLS_DB_POOL_SIZE="$2"; shift 2 ;;
        -h|--help)  sed -n '2,9p' "$0"; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

EXE="build/bin/cpppools_server"
if [[ ! -x "$EXE" ]]; then
    echo "错误: 未找到 $EXE，请先运行 scripts/build.sh" >&2
    exit 1
fi

export CPPPOOLS_DB_MODE="$MODE"

echo "启动 CppPools 服务"
echo "  后端    : $MODE"
echo "  监听    : http://127.0.0.1:$PORT/"
echo "  workers : $WORKERS"
echo "  探针    : curl http://127.0.0.1:$PORT/health"
echo

exec "$EXE" "$PORT" "$WORKERS"
