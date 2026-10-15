"""Windows native LocalNodeManager lifecycle acceptance tests."""

import contextlib
import json
import os
import socket
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from cpppools import local_node


@unittest.skipUnless(os.name == "nt", "requires Windows")
class TestWindowsLocalLive(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.environ.get("CPL_TEST_SERVER"):
            if os.environ.get("CPL_RUN_E2E") == "1":
                raise RuntimeError("CPL_RUN_E2E=1 requires CPL_TEST_SERVER")
            raise unittest.SkipTest("requires CPL_TEST_SERVER")

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="cpppools_windows_local_")
        self.root = self.temporary.name
        self.manager = local_node.LocalNodeManager(os.path.join(self.root, "state"), "demo")
        self.addCleanup(self.cleanup_node)
        database = os.path.join(self.root, "node.db")
        connection = sqlite3.connect(database)
        try:
            connection.execute("CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL)")
            connection.commit()
        finally:
            connection.close()
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        self.config = os.path.join(self.root, "server.json")
        with open(self.config, "w", encoding="utf-8") as handle:
            json.dump({"server": {"port": port, "worker_threads": 1},
                       "datasources": [{"name": "main", "type": "sqlite", "default": True,
                                        "pool_size": 1, "sqlite": {"path": database}}]}, handle)

    def cleanup_node(self):
        process = self.manager._process
        if process is not None:
            if process.poll() is None:
                process.kill()  # Only this fixture's own Popen, including failed start.
            process.wait(timeout=10)
        else:
            # The CLI launch belongs to a different parent. Use its verified
            # identity rather than killing a numeric PID in failure cleanup.
            try:
                state = self.manager.read_state()
            except local_node.LocalStateError:
                state = None
            if state is not None and local_node._pid_alive(state.get("pid")):
                with contextlib.suppress(local_node.ExitOutcome):
                    self.manager.stop(timeout=10, force=True)
        self.temporary.cleanup()

    def start(self):
        return self.manager.start(self.config, binary=os.environ["CPL_TEST_SERVER"],
                                  workdir=self.root, timeout=15)

    def test_start_status_stop_real_server(self):
        running = self.start()
        self.assertEqual(running["state"], "running")
        self.assertTrue(running["process_alive"])
        self.assertTrue(running["identity_verified"])
        self.assertEqual(self.manager.status()["state"], "running")
        stopped = self.manager.stop(timeout=15)
        self.assertEqual(stopped["state"], "stopped")
        self.assertEqual(stopped["exit_kind"], "graceful")
        self.assertFalse(stopped["process_alive"])

    def test_unicode_state_directory_real_server(self):
        self.manager = local_node.LocalNodeManager(
            os.path.join(self.root, "节点-🌲", "state"), "demo")
        running = self.start()
        self.assertEqual(running["state"], "running")
        self.assertTrue(running["identity_verified"])
        self.assertEqual(self.manager.status()["launch_id"], running["launch_id"])
        self.assertEqual(self.manager.stop(timeout=15)["exit_kind"], "graceful")

    def test_duplicate_start_is_idempotent_only_for_identical_configuration(self):
        running = self.start()
        repeated = self.start()
        self.assertTrue(repeated["already_running"])
        self.assertEqual(repeated["launch_id"], running["launch_id"])
        with self.assertRaises(local_node.LocalNodeError):
            self.manager.start(self.config, binary=os.environ["CPL_TEST_SERVER"],
                               workdir=os.path.dirname(self.root), timeout=5)

    def test_force_stop_requires_explicit_force_and_reports_forced_exit(self):
        self.start()
        with self.assertRaises(local_node.ExitOutcome) as outcome:
            self.manager.stop(timeout=15, force=True)
        self.assertEqual(outcome.exception.code, 9)
        self.assertEqual(outcome.exception.result["exit_kind"], "forced")
        self.assertFalse(outcome.exception.result["process_alive"])

    def test_control_write_failure_is_local_state_error_and_preserves_process(self):
        self.start()
        with mock.patch.object(local_node, "_write_stop_request", return_value=False):
            with self.assertRaises(local_node.LocalStateError):
                self.manager.stop(timeout=0.1)
        self.assertEqual(self.manager.read_state()["state"], "stopping")
        self.assertIsNone(self.manager._process.poll())
        self.assertEqual(self.manager.stop(timeout=15)["exit_kind"], "graceful")

    def test_exit_before_graceful_wait_handle_is_reconciled(self):
        self.start()
        original = local_node._open_verified_windows_process

        def finish_before_open(state, **kwargs):
            self.assertTrue(local_node._write_stop_request(state))
            self.assertEqual(self.manager._process.wait(timeout=15), 0)
            return original(state, **kwargs)

        with mock.patch.object(local_node, "_open_verified_windows_process",
                               side_effect=finish_before_open):
            result = self.manager.stop(timeout=15)
        self.assertEqual(result["state"], "stopped")
        self.assertEqual(result["exit_kind"], "graceful")
        self.assertFalse(result["process_alive"])
        self.assertEqual(self.manager.read_state()["state"], "stopped")

    def test_separate_cli_processes_start_status_stop(self):
        def invoke(*arguments):
            return subprocess.run([sys.executable, "-m", "cpppools", *arguments],
                                  cwd=self.root, capture_output=True, text=True, timeout=25,
                                  env=dict(os.environ, PYTHONPATH=os.path.abspath("sdk/python")))

        common = ["-n", "demo", "-R", self.manager.state_root, "-o", "json"]
        started = invoke("start", *common, "-c", self.config,
                         "-b", os.environ["CPL_TEST_SERVER"], "--workdir", self.root)
        self.assertEqual(started.returncode, 0, started.stderr)
        running = json.loads(started.stdout)
        status = invoke("status", *common)
        self.assertEqual(status.returncode, 0, status.stderr)
        observed = json.loads(status.stdout)
        self.assertEqual(observed["launch_id"], running["launch_id"])
        self.assertTrue(observed["identity_verified"])
        stopped = invoke("stop", *common, "-t", "15")
        self.assertEqual(stopped.returncode, 0, stopped.stderr)
        self.assertEqual(json.loads(stopped.stdout)["exit_kind"], "graceful")


if __name__ == "__main__":
    unittest.main()
