#!/usr/bin/env python3
"""SDK ↔ 单节点服务端 e2e 测试（自包含，不依赖 scripts/sdk_e2e.sh 的三节点编排）。

与 sdk/python/tests/e2e_live.py 的区别：
  - e2e_live.py：三节点集群，由 scripts/sdk_e2e.sh 编排，断言路由/绕行/epoch。
  - 本测试：单节点，自管 server 子进程，断言 CRUD 四端点的契约往返。

用 stdlib 启动 build/bin/cpppools_server（临时库 + 临时配置），构造
ClusterClient 跑 insert/select/update/delete 往返 + 冲突映射 + 0 行语义。

前置：scripts/build.sh 已构建 build/bin/cpppools_server。
运行：python3 sdk/python/examples/e2e_crud.py
"""
import json
import os
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest

# 路径：本文件在 sdk/python/examples/，向上三级到仓库根（import cpppools 用一级）。
_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, ".."))  # → sdk/python

from cpppools.client import ClusterClient
from cpppools import errors

PROJECT_ROOT = os.path.abspath(os.path.join(_HERE, "..", "..", ".."))
SERVER = os.path.join(PROJECT_ROOT, "build", "bin", "cpppools_server")


def _free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class E2ECrudTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.path.exists(SERVER):
            raise unittest.SkipTest(
                f"server binary not found at {SERVER}; run scripts/build.sh first")
        cls.port = _free_port()
        cls.tmpdir = tempfile.mkdtemp(prefix="cpppools_e2e_")
        cls.db_path = os.path.join(cls.tmpdir, "test.db")
        # 裸连接建被测表（中间件不建库）。
        conn = sqlite3.connect(cls.db_path)
        conn.executescript(
            "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL,"
            " age INTEGER, score REAL, tag TEXT UNIQUE);")
        conn.commit()
        conn.close()
        cls.cfg_path = os.path.join(cls.tmpdir, "cfg.json")
        with open(cls.cfg_path, "w", encoding="utf-8") as f:
            json.dump({
                "server": {"port": cls.port, "worker_threads": 4},
                "datasources": [{
                    "name": "main", "type": "sqlite", "default": True,
                    "pool_size": 4, "sqlite": {"path": cls.db_path},
                }],
            }, f)
        cls.proc = subprocess.Popen(
            [SERVER, "--config", cls.cfg_path],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # 等服务就绪（轮询 /health）。
        import urllib.request
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            try:
                with urllib.request.urlopen(
                        f"http://127.0.0.1:{cls.port}/health", timeout=0.5) as r:
                    if r.status == 200:
                        break
            except OSError:
                pass
            time.sleep(0.2)
        else:
            cls.proc.terminate()
            raise RuntimeError("server failed to start within 10s")
        cls.client = ClusterClient([f"http://127.0.0.1:{cls.port}"])

    @classmethod
    def tearDownClass(cls):
        try:
            cls.proc.terminate()
            cls.proc.wait(timeout=5)
        finally:
            import shutil
            shutil.rmtree(cls.tmpdir, ignore_errors=True)

    def test_insert_select_roundtrip(self):
        r = self.client.insert("users",
                               {"name": "alice", "age": 30, "score": 91.5, "tag": "a-1"})
        self.assertEqual(r["affected_rows"], 1)
        self.assertIn("inserted_id", r)
        uid = r["inserted_id"]
        self.assertGreater(uid, 0)

        got = self.client.select("users", {"column": "id", "value": uid},
                                 columns=["name", "score"])
        self.assertEqual(got["columns"], ["name", "score"])
        self.assertEqual(got["rows"], [["alice", 91.5]])

    def test_update_merges_untouched(self):
        r = self.client.insert("users", {"name": "bob", "tag": "b-1"})
        uid = r["inserted_id"]
        self.client.update("users", {"score": 99.0}, {"column": "id", "value": uid})
        got = self.client.select("users", {"column": "id", "value": uid},
                                columns=["name", "score"])
        self.assertEqual(got["rows"], [["bob", 99.0]])  # name 未动

    def test_delete_and_zero_rows(self):
        r = self.client.insert("users", {"name": "carol", "tag": "c-1"})
        uid = r["inserted_id"]
        d = self.client.delete("users", {"column": "id", "value": uid})
        self.assertEqual(d["affected_rows"], 1)
        after = self.client.select("users", {"column": "id", "value": uid})
        self.assertEqual(after["rows"], [])  # 0 行 = 200 + 空集（非 404）

    def test_unique_conflict_maps_to_409(self):
        self.client.insert("users", {"name": "dave", "tag": "dup"})
        with self.assertRaises(errors.ConflictError):
            self.client.insert("users", {"name": "dave2", "tag": "dup"})

    def test_invalid_request_maps_to_400(self):
        # 缺 NOT NULL 列 name → 400 ClientError。
        with self.assertRaises(errors.ClientError):
            self.client.insert("users", {"age": 1})


if __name__ == "__main__":
    unittest.main(verbosity=2)
