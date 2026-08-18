#!/usr/bin/env python3
"""最简 CRUD 使用示例（单节点）。

不依赖集群：用任一 CppPools 节点作 seed，跑一遍 insert → select → update →
select → delete → select 的完整往返，打印每步响应体。

前置：
  1. scripts/build.sh          # 构建 server
  2. scripts/run_server.sh     # 启动单节点（自动种子演示库 user_scores）

运行：
  python3 sdk/python/examples/crud_demo.py
  python3 sdk/python/examples/crud_demo.py http://127.0.0.1:8080 user_scores user_id

SDK 的 select/insert/update/delete 直接返回服务端 JSON 响应体（解析后的 dict）：
  insert  → {"datasource":..., "affected_rows":1, "inserted_id":<id>}
  select  → {"datasource":..., "columns":[...], "rows":[[...]], "row_count":n}
  update  → {"datasource":..., "affected_rows":1}
  delete  → {"datasource":..., "affected_rows":1}
"""
import sys
import os

# 让示例脚本无需安装即可 import cpppools（把 sdk/python 加入 sys.path）。
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from cpppools.client import ClusterClient
from cpppools import errors


def main():
    seed = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8080"
    table = sys.argv[2] if len(sys.argv) > 2 else "user_scores"
    key_col = sys.argv[3] if len(sys.argv) > 3 else "user_id"
    key_val = 12345  # 演示用键值，任意不冲突即可

    print(f"seed={seed} table={table} key={key_col}={key_val}")

    # 单节点即集群退化形态：seeds 只列一个，SDK 拉拓扑后环退化为单节点。
    client = ClusterClient([seed])

    def show(tag, result):
        print(f"  [{tag}] {result}")

    # 1) insert：自增主键列可缺省，这里给全列（演示库 user_scores 有 score）。
    try:
        r = client.insert(table, {key_col: key_val, "score": 99})
        show("insert", r)
    except errors.ConflictError as e:
        # 键已存在：演示可重入，视为已插入。
        print(f"  [insert] 已存在（409），继续演示: {e}")

    # 2) select：取回刚写入的行。
    r = client.select(table, {"column": key_col, "value": key_val},
                      columns=["score"])
    show("select", r)

    # 3) update：改 score。
    r = client.update(table, {"score": 88}, {"column": key_col, "value": key_val})
    show("update", r)

    # 4) select：确认改后值。
    r = client.select(table, {"column": key_col, "value": key_val},
                      columns=["score"])
    show("select-after-update", r)
    assert r["rows"][0][0] == 88, "update 后 score 应为 88"

    # 5) delete：删该行。
    r = client.delete(table, {"column": key_col, "value": key_val})
    show("delete", r)

    # 6) select：确认已删（200 + rows:[]）。
    r = client.select(table, {"column": key_col, "value": key_val})
    show("select-after-delete", r)
    assert r["rows"] == [], "delete 后应查无此行"

    print("\n[ok] CRUD 往返成功")


if __name__ == "__main__":
    main()
