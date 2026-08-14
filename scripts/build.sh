#!/usr/bin/env bash
# CppPools 构建 + 测试脚本（Linux / macOS）
#
# 用法:
#   scripts/build.sh                      # mock 后端，构建并跑测试
#   scripts/build.sh --mysql              # 启用 MySQL 后端
#   scripts/build.sh --no-test            # 只构建，不跑测试
#   scripts/build.sh --clean              # 先清掉 build/ 再构建
#
# 与 scripts/run_server_mysql.ps1 是同一套流程的两个平台版本。
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

BUILD_DIR="build"
USE_MYSQL="OFF"
RUN_TESTS=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --mysql)   USE_MYSQL="ON"; shift ;;
        --no-test) RUN_TESTS=0; shift ;;
        --clean)   rm -rf "$BUILD_DIR"; shift ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

if ! command -v cmake >/dev/null 2>&1; then
    echo "错误: 未找到 cmake。请先安装（macOS: brew install cmake）。" >&2
    exit 1
fi

echo "==> 配置 (CPPPOOLS_USE_MYSQL=$USE_MYSQL)"
cmake -S . -B "$BUILD_DIR" -DCPPPOOLS_USE_MYSQL="$USE_MYSQL"

echo "==> 构建"
cmake --build "$BUILD_DIR" -j "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

if [[ $RUN_TESTS -eq 1 ]]; then
    echo "==> 测试"
    ctest --test-dir "$BUILD_DIR" --output-on-failure
fi

echo
echo "构建产物位于 build/bin/:"
ls -1 build/bin 2>/dev/null | sed 's/^/  /'
