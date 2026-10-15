"""Windows foreground CLI acceptance using a real winpty console."""

import contextlib
import json
import os
import queue
import shutil
import socket
import sqlite3
import sys
import tempfile
import threading
import time
import unittest
import re

from cpppools import local_node


@unittest.skipUnless(os.name == "nt" and os.environ.get("CPL_TEST_SERVER"),
                     "requires Windows, CPL_TEST_SERVER and winpty")
class TestWindowsForegroundLive(unittest.TestCase):
    def setUp(self):
        try:
            from winpty import PtyProcess
        except ImportError as exc:
            raise unittest.SkipTest("requires the winpty Python package") from exc
        self.PtyProcess = PtyProcess
        self.root = tempfile.mkdtemp(prefix="cpppools_windows_foreground_")
        self.state_root = os.path.join(self.root, "state")
        database = os.path.join(self.root, "node.db")
        connection = sqlite3.connect(database)
        try:
            connection.execute("CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL)")
            connection.commit()
        finally:
            connection.close()
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            self.port = listener.getsockname()[1]
        self.config = os.path.join(self.root, "server.json")
        with open(self.config, "w", encoding="utf-8") as handle:
            json.dump({"server": {"port": self.port, "worker_threads": 1},
                       "datasources": [{"name": "main", "type": "sqlite", "default": True,
                                        "pool_size": 1, "sqlite": {"path": database}}]}, handle)
        self.manager = local_node.LocalNodeManager(self.state_root, "demo")
        self.pty = None
        self.addCleanup(self.cleanup)

    def cleanup(self):
        if self.pty is not None and self.pty.isalive():
            with contextlib.suppress(Exception):
                self.pty.terminate(force=True)
        if self.pty is not None:
            with contextlib.suppress(Exception):
                self.pty.close(force=True)
        with contextlib.suppress(Exception):
            state = self.manager.read_state()
            if local_node._pid_alive(state.get("pid")):
                self.manager.stop(timeout=10, force=True)
        shutil.rmtree(self.root, ignore_errors=True)

    def _start_reader(self):
        output = queue.Queue()

        def read_loop():
            while True:
                try:
                    chunk = self.pty.read(4096)
                except (EOFError, OSError):
                    return
                except BaseException as exc:
                    output.put("[reader-error %s: %s]" % (type(exc).__name__, exc))
                    return
                # winpty uses an internal ``0011Ignore`` marker which
                # PtyProcess.read() maps to an empty string; it is not EOF.
                if not chunk:
                    continue
                output.put(chunk)

        thread = threading.Thread(target=read_loop, daemon=True)
        thread.start()
        return output, thread

    def _read_until_exit(self, output, reader, timeout):
        reader.join(timeout)
        if self.pty.isalive():
            chunks = []
            while not output.empty():
                chunks.append(output.get_nowait())
            state = self.manager.read_state()
            self.fail("foreground CLI did not exit before deadline: status=%r state=%r output=%r" %
                      (self.pty.exitstatus, state, "".join(chunks)))

    @staticmethod
    def _json_documents(text):
        """Extract JSON objects from winpty's ANSI-decorated console stream."""
        text = re.sub(r"\x1b\][^\x07]*(?:\x07|\x1b\\)", "", text)
        text = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", text)
        # winpty inserts CR/LF when the terminal wraps a long JSON line;
        # the CLI emits compact JSON, so those characters are presentation
        # artifacts and can be removed safely before decoding.
        text = text.replace("\r", "").replace("\n", "")
        # WinPTY can redraw the closing quote of a wrapped key once more.
        text = re.sub(r'""(?=\s*:)', '"', text)
        text = text.replace("faalse", "false").replace("ttrue", "true")
        documents = []
        # A real console may inject a cursor-position sequence between a
        # wrapped key and its value.  Compact CLI responses are flat objects,
        # so recover each object by its final closing brace and decode it.
        for match in re.finditer(r"\{.*?\}", text):
            with contextlib.suppress(ValueError):
                documents.append(json.loads(match.group(0)))
        return documents

    def test_real_terminal_ctrl_c_graceful_shutdown(self):
        environment = dict(os.environ, PYTHONPATH=os.path.abspath("sdk/python"),
                           PYWINPTY_BLOCK="0")
        self.pty = self.PtyProcess.spawn([
            sys.executable, "-u", "-m", "cpppools", "start", "-n", "demo",
            "-R", self.state_root, "-c", self.config,
            "-b", os.environ["CPL_TEST_SERVER"], "-W", self.root,
            "-F", "-o", "json"], cwd=self.root, env=environment)

        output, reader = self._start_reader()
        ready = None
        raw_output = []
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and ready is None:
            self.assertTrue(self.pty.isalive(), "CLI exited before readiness")
            try:
                text = output.get(timeout=0.1)
            except queue.Empty:
                continue
            raw_output.append(text)
            for candidate in self._json_documents(text):
                if candidate.get("state") == "running":
                    ready = candidate
                    break
        observed = ""
        while not output.empty():
            chunk = output.get_nowait()
            raw_output.append(chunk)
            observed += chunk
        observed = "".join(raw_output) if raw_output else observed
        with contextlib.suppress(OSError):
            with open(os.path.join(self.root, "server.log"), encoding="utf-8",
                      errors="replace") as handle:
                observed += "\n[server.log]\n" + handle.read()
        with contextlib.suppress(Exception):
            state = self.manager.read_state()
            observed += "\n[state=%r]\n" % state
            log_path = state.get("log_file")
            if log_path:
                with open(log_path, encoding="utf-8", errors="replace") as handle:
                    observed += "\n[state.log]\n" + handle.read()
        self.assertIsNotNone(ready,
                             "foreground CLI did not report running readiness: %r" % observed)
        self.assertEqual(self.manager.read_state()["state"], "running")
        token_path = os.path.join(ready["control_dir"], "control.token")
        with open(token_path, encoding="ascii") as handle:
            token = handle.read()

        # Hold the only worker in an incomplete request while the real console
        # receives Ctrl+C. This checks graceful drain rather than idle shutdown.
        with socket.create_connection(("127.0.0.1", self.port), timeout=3) as slow:
            slow.sendall(b"POST /api/v1/select HTTP/1.1\r\nHost: localhost\r\n"
                         b"Content-Length: 50\r\n\r\n{")
            self.pty.sendintr()
            self._read_until_exit(output, reader, 15)

        output_text = "".join(raw_output)
        while not output.empty():
            output_text += output.get_nowait()
        documents = self._json_documents(output_text)
        self.assertTrue(any(item.get("state") == "stopped" and
                            item.get("exit_kind") == "graceful" for item in documents),
                        "documents=%r raw=%r" % (documents, output_text))
        self.assertEqual(self.pty.exitstatus, 130, output_text)
        self.assertNotIn(token, output_text)
        self.assertFalse(os.path.exists(os.path.join(ready["control_dir"], "stop.request")))
        self.assertFalse(os.path.exists(token_path))
        self.assertFalse(local_node._pid_alive(ready["pid"]))


if __name__ == "__main__":
    unittest.main()
