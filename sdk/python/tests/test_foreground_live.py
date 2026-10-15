"""Foreground acceptance with a real controlling terminal and native server."""

import contextlib
import json
import os
import select
import shutil
import signal
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest

from cpppools import local_node


@unittest.skipUnless(sys.platform == "linux" and os.environ.get("CPL_TEST_SERVER"),
                     "requires Linux PTY and CPL_TEST_SERVER")
class TestForegroundLive(unittest.TestCase):
    def setUp(self):
        self.root = os.path.realpath(tempfile.mkdtemp(prefix="cpppools_foreground_pty_"))
        self.manager = None
        self.launch_identity = None
        self.addCleanup(self._cleanup)
        database = os.path.join(self.root, "node.db")
        connection = sqlite3.connect(database)
        connection.execute("CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL)")
        connection.close()
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            self.port = listener.getsockname()[1]
        self.config = os.path.join(self.root, "server.json")
        with open(self.config, "w", encoding="utf-8") as handle:
            json.dump({"server": {"port": self.port, "worker_threads": 1},
                       "datasources": [{"name": "main", "type": "sqlite", "default": True,
                                        "pool_size": 1, "sqlite": {"path": database}}]}, handle)
        self.manager = local_node.LocalNodeManager(os.path.join(self.root, "state"), "demo")

    def _cleanup(self):
        if self.manager is not None:
            with contextlib.suppress(local_node.LocalNodeError):
                self.manager.stop(timeout=3)
            process = self.manager._process
            if process is not None:
                if process.poll() is None:
                    process.kill()  # Test failure cleanup for our own Popen only.
                process.wait(timeout=5)
            elif os.path.exists(self.manager.state_path):
                state = self.manager.read_state()
                if state.get("state") == "starting" and state.get("pid") is None:
                    self.fail("startup PID is unknown; retaining test directory for recovery")
                if local_node._pid_alive(state.get("pid")):
                    self.assertEqual(state.get("config"), self.config)
                    self.assertEqual(state.get("workdir"), self.root)
                    self.assertEqual(state.get("binary"), os.path.realpath(os.environ["CPL_TEST_SERVER"]))
                    identity = tuple(state.get(key) for key in ("launch_id", "pid", "process_start_time"))
                    if self.launch_identity is not None:
                        self.assertEqual(identity, self.launch_identity)
                    local_node._send_verified_signal(state, signal.SIGKILL)
                    deadline = time.monotonic() + 5
                    while local_node._pid_alive(state["pid"]) and time.monotonic() < deadline:
                        time.sleep(0.02)
                    self.assertFalse(local_node._pid_alive(state["pid"]), "test server did not exit")
                    local_node._reap_child(state["pid"])
        # On unverified or unreaped failure, retain the directory for recovery.
        shutil.rmtree(self.root)

    def test_ready_callback_failure_preserves_registered_running_process(self):
        def fail_after_ready(result):
            self.launch_identity = tuple(result.get(key) for key in ("launch_id", "pid", "process_start_time"))
            self.assertEqual(result["state"], "running")
            self.assertEqual(self.manager.read_state()["state"], "running")
            raise RuntimeError("ready output failed")

        with self.assertRaisesRegex(RuntimeError, "ready output failed"):
            self.manager.start(self.config, binary=os.environ["CPL_TEST_SERVER"],
                               workdir=self.root, timeout=10, foreground=True,
                               on_ready=fail_after_ready)
        self.assertIsNone(self.manager._process.poll())
        self.assertEqual(self.manager.read_state()["state"], "running")
        self.assertTrue(self.manager.status()["identity_verified"])

    def test_real_terminal_ctrl_c_waits_for_graceful_exit(self):
        import fcntl
        import pty
        import termios

        def attach_terminal():
            os.setsid()
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)

        master, slave = pty.openpty()
        env = os.environ.copy()
        env["PYTHONPATH"] = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
        process = None
        try:
            process = subprocess.Popen(
                [sys.executable, "-m", "cpppools", "start", "-n", "demo", "-R",
                 self.manager.state_root, "-c", self.config, "-b", os.environ["CPL_TEST_SERVER"],
                 "-W", self.root, "-F", "-o", "json"], env=env, stdin=slave,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, preexec_fn=attach_terminal)
            diagnostic = bytearray()
            deadline = time.monotonic() + 10
            while b"\n" not in diagnostic:
                remaining = deadline - time.monotonic()
                self.assertGreater(remaining, 0, "foreground did not report readiness")
                self.assertIsNone(process.poll(), "CLI exited before readiness")
                readable, _, _ = select.select([process.stderr], [], [], remaining)
                self.assertTrue(readable, "foreground did not report readiness")
                part = os.read(process.stderr.fileno(), 65536)
                self.assertTrue(part, "CLI closed stderr before readiness")
                diagnostic.extend(part)
            ready = json.loads(diagnostic)
            self.launch_identity = tuple(ready.get(key) for key in ("launch_id", "pid", "process_start_time"))
            self.assertEqual(ready["state"], "running")
            self.assertEqual(self.manager.read_state()["state"], "running")
            self.assertFalse(select.select([process.stdout], [], [], 0)[0],
                             "foreground stdout must wait for the final result")
            self.assertEqual(os.tcgetpgrp(master), process.pid)
            self.assertNotEqual(os.getpgid(ready["pid"]), process.pid,
                                "server must be outside the terminal's foreground group")
            with open(os.path.join(ready["control_dir"], "control.token"), encoding="ascii") as handle:
                token = handle.read()
            # Keep the only worker receiving an incomplete body while Ctrl+C
            # arrives. The CLI must wait for real drain, never force the child.
            with socket.create_connection(("127.0.0.1", self.port), timeout=3) as slow:
                slow.sendall(b"POST /api/v1/select HTTP/1.1\r\nHost: localhost\r\n"
                             b"Content-Length: 50\r\n\r\n{")
                with socket.create_connection(("127.0.0.1", self.port), timeout=3) as probe:
                    probe.settimeout(0.3)
                    probe.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n")
                    with self.assertRaises(socket.timeout):
                        probe.recv(1)
                interrupted_at = time.monotonic()
                os.write(master, b"\x03")  # Terminal ISIG generates SIGINT for the CLI group.
                stdout, stderr = process.communicate(timeout=15)
            self.assertEqual(process.returncode, 130, stderr)
            final = json.loads(stdout)
            self.assertEqual((final["state"], final["exit_kind"]), ("stopped", "graceful"))
            self.assertEqual(final["launch_id"], ready["launch_id"])
            self.assertEqual(final["last_exit_code"], 0)
            self.assertEqual(json.loads(stderr)["kind"], "interrupted")
            self.assertGreater(time.monotonic() - interrupted_at, 0.5)
            self.assertNotIn(token.encode("ascii"), diagnostic + stdout + stderr)
            self.assertFalse(local_node._pid_alive(ready["pid"]))
            for filename in ("control.token", "stop.request"):
                self.assertFalse(os.path.exists(os.path.join(ready["control_dir"], filename)))
        finally:
            os.close(master)
            os.close(slave)
            if process is not None:
                if process.poll() is None:
                    process.terminate()
                try:
                    process.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate(timeout=5)


if __name__ == "__main__":
    unittest.main()
