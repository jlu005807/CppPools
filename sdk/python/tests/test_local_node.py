import json
import os
import signal
import tempfile
import time
import unittest
import socket
import sqlite3
import subprocess
import sys

from cpppools import local_node


class TestLocalNodeState(unittest.TestCase):
    def test_binary_without_path_is_resolved_from_path(self):
        with tempfile.TemporaryDirectory() as root:
            binary = os.path.join(root, "cpppools_server")
            with open(binary, "w", encoding="ascii") as handle:
                handle.write("#!/bin/sh\n")
            os.chmod(binary, 0o755)
            manager = local_node.LocalNodeManager(root, "demo")
            old_path = os.environ.get("PATH", "")
            os.environ["PATH"] = root + os.pathsep + old_path
            try:
                self.assertEqual(manager.resolve_binary(None), os.path.realpath(binary))
            finally:
                os.environ["PATH"] = old_path

    def test_atomic_state_round_trip_and_status_detects_dead_process(self):
        with tempfile.TemporaryDirectory() as root:
            manager = local_node.LocalNodeManager(root, "demo")
            state = {
                "protocol_version": 1,
                "state_revision": 1,
                "name": "demo",
                "launch_id": "launch-1",
                "pid": 999999,
                "binary": "/bin/false",
                "config": "/tmp/config.json",
                "workdir": "/tmp",
                "endpoint": "http://127.0.0.1:1",
                "log_file": "/tmp/server.log",
                "state": "running",
                "process_start_time": None,
                "exit_kind": None,
                "last_exit_code": None,
                "updated_at": "now",
            }
            manager.write_state(state)
            self.assertEqual(manager.read_state()["launch_id"], "launch-1")
            status = manager.status()
            self.assertEqual(status["state"], "running")
            self.assertFalse(status["process_alive"])
            self.assertFalse(status["identity_verified"])

    def test_invalid_instance_name_is_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaises(local_node.LocalStateError):
                local_node.LocalNodeManager(root, "../escape")

    def test_zombie_process_is_not_alive(self):
        if os.name != "posix":
            self.skipTest("POSIX process state probe")
        child = os.fork()
        if child == 0:
            os._exit(0)
        try:
            time.sleep(0.05)
            self.assertFalse(local_node._pid_alive(child))
        finally:
            os.waitpid(child, 0)

    def test_process_start_identity_is_recorded_for_live_launch(self):
        if os.name != "posix":
            self.skipTest("POSIX process identity probe")
        with tempfile.TemporaryDirectory() as root:
            manager = local_node.LocalNodeManager(root, "demo")
            state = {
                "protocol_version": 1, "state_revision": 1, "name": "demo",
                "launch_id": "launch-1", "pid": os.getpid(),
                "process_start_time": local_node._process_start_time(os.getpid()),
                "binary": local_node._process_executable(os.getpid()),
                "config": "/tmp/config.json", "workdir": "/tmp",
                "endpoint": None, "log_file": None, "state": "running",
                "exit_kind": None, "last_exit_code": None, "updated_at": "now",
            }
            manager.write_state(state)
            self.assertTrue(manager.status()["identity_verified"])

    def test_stop_does_not_signal_pid_when_identity_is_not_verified(self):
        with tempfile.TemporaryDirectory() as root:
            manager = local_node.LocalNodeManager(root, "demo")
            manager.write_state({
                "protocol_version": 1, "state_revision": 1,
                "name": "demo", "launch_id": "launch-1", "pid": os.getpid(),
                "binary": "/bin/false", "config": "/tmp/config.json",
                "workdir": "/tmp", "endpoint": None, "log_file": None,
                "state": "running", "process_start_time": None,
                "exit_kind": None, "last_exit_code": None,
                "updated_at": "now",
            })
            with self.assertRaises(local_node.IdentityError):
                manager.stop(timeout=0.1)


@unittest.skipUnless(os.environ.get("CPL_TEST_SERVER"),
                     "set CPL_TEST_SERVER for real local lifecycle smoke")
class TestLiveLocalNode(unittest.TestCase):
    def test_start_status_stop_real_server(self):
        with tempfile.TemporaryDirectory(prefix="cpppools_local_node_") as root:
            db_path = os.path.join(root, "node.db")
            conn = sqlite3.connect(db_path)
            conn.execute("CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL)")
            conn.close()
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]
            config = os.path.join(root, "server.json")
            with open(config, "w", encoding="utf-8") as handle:
                json.dump({"server": {"port": port, "worker_threads": 1},
                           "datasources": [{"name": "main", "type": "sqlite",
                                            "default": True, "pool_size": 1,
                                            "sqlite": {"path": db_path}}]}, handle)
            manager = local_node.LocalNodeManager(os.path.join(root, "state"), "demo")
            running = manager.start(config, binary=os.environ["CPL_TEST_SERVER"],
                                    workdir=root, timeout=10)
            self.assertEqual(running["state"], "running")
            self.assertTrue(running["process_alive"])
            status = manager.status()
            self.assertTrue(status["identity_verified"])
            stopped = manager.stop(timeout=10)
            self.assertEqual(stopped["state"], "stopped")
            self.assertFalse(stopped["process_alive"])

    def test_cpl_start_status_stop_subprocess_contract(self):
        with tempfile.TemporaryDirectory(prefix="cpppools_cli_local_") as root:
            db_path = os.path.join(root, "node.db")
            conn = sqlite3.connect(db_path)
            conn.execute("CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL)")
            conn.close()
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]
            config = os.path.join(root, "server.json")
            with open(config, "w", encoding="utf-8") as handle:
                json.dump({"server": {"port": port, "worker_threads": 1},
                           "datasources": [{"name": "main", "type": "sqlite",
                                            "default": True, "pool_size": 1,
                                            "sqlite": {"path": db_path}}]}, handle)
            env = os.environ.copy()
            env["PYTHONPATH"] = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

            def cpl(*args):
                return subprocess.run([sys.executable, "-m", "cpppools"] + list(args),
                                      env=env, text=True, capture_output=True,
                                      timeout=15, check=False)

            started = cpl("start", "-n", "demo", "-R", os.path.join(root, "state"),
                          "-c", config, "-b", os.environ["CPL_TEST_SERVER"],
                          "-W", root, "-o", "json")
            self.assertEqual(started.returncode, 0, started.stderr)
            self.assertEqual(json.loads(started.stdout)["state"], "running")
            status = cpl("status", "-n", "demo", "-R", os.path.join(root, "state"),
                         "-o", "json")
            self.assertEqual(status.returncode, 0, status.stderr)
            self.assertTrue(json.loads(status.stdout)["identity_verified"])
            stopped = cpl("stop", "-n", "demo", "-R", os.path.join(root, "state"),
                          "-o", "json")
            self.assertEqual(stopped.returncode, 0, stopped.stderr)
            self.assertEqual(json.loads(stopped.stdout)["state"], "stopped")


if __name__ == "__main__":
    unittest.main()
