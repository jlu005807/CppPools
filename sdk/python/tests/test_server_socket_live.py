"""Live socket regressions for the native CppPools server executable."""

import contextlib
import json
import os
import re
import secrets
import shutil
import socket
import sqlite3
import subprocess
import tempfile
import time
import unittest
import uuid


@unittest.skipUnless(os.environ.get("CPL_TEST_SERVER"),
                     "set CPL_TEST_SERVER to run native server socket tests")
class TestServerSocketLive(unittest.TestCase):
    def _create_control_files(self, temp_dir, launch_id=None):
        from cpppools import local_node

        control_dir = os.path.join(temp_dir, "control")
        os.mkdir(control_dir)
        launch_id = launch_id or uuid.uuid4().hex
        token = secrets.token_hex(32)
        local_node._atomic_json(os.path.join(control_dir, "control.meta"), {
            "protocol_version": 1, "launch_id": launch_id,
        })
        local_node._atomic_bytes(os.path.join(control_dir, "control.token"),
                                 token.encode("ascii"))
        return control_dir, launch_id, token

    def _write_control_request(self, control_dir, launch_id, token):
        from cpppools import local_node

        local_node._atomic_json(os.path.join(control_dir, "stop.request"), {
            "protocol_version": 1,
            "launch_id": launch_id,
            "requested_at": local_node._now(),
            "token": token,
        })

    def _free_port(self):
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            return sock.getsockname()[1]

    def _create_database(self, db_path, large_schema=False):
        connection = sqlite3.connect(db_path)
        try:
            connection.execute(
                "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL)")
            if large_schema:
                # The wide response must exceed local TCP send/receive buffers so
                # that the no-read client can exercise the server's send deadline.
                for table_index in range(512):
                    columns = [
                        '"col_%04d_%s" TEXT' % (column_index, "x" * 39)
                        for column_index in range(1024)
                    ]
                    connection.execute(
                        'CREATE TABLE "wide_%02d" (%s)' %
                        (table_index, ",".join(columns)))
            connection.commit()
        finally:
            connection.close()

    @contextlib.contextmanager
    def _running_server(self, large_schema=False):
        temp_dir = tempfile.mkdtemp(prefix="cpppools_server_socket_")
        server = None
        try:
            db_path = os.path.join(temp_dir, "server.sqlite")
            self._create_database(db_path, large_schema=large_schema)
            port = self._free_port()
            config_path = os.path.join(temp_dir, "server.json")
            with open(config_path, "w", encoding="utf-8") as handle:
                json.dump({
                    "server": {"port": port, "worker_threads": 1},
                    "datasources": [{
                        "name": "main", "type": "sqlite", "default": True,
                        "pool_size": 1, "sqlite": {"path": db_path},
                    }],
                }, handle)

            server = subprocess.Popen(
                [os.environ["CPL_TEST_SERVER"], "--config", config_path],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self._wait_until_healthy(server, port)
            yield server, port, db_path
        finally:
            if server is not None and server.poll() is None:
                server.terminate()
                try:
                    server.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=3)
            shutil.rmtree(temp_dir, ignore_errors=True)

    def _wait_until_healthy(self, server, port):
        deadline = time.monotonic() + 30
        last_error = None
        while time.monotonic() < deadline:
            if server.poll() is not None:
                self.fail("CppPools server exited before health was ready")
            try:
                status, body = self._request(port, b"GET /health HTTP/1.1\r\n"
                                                  b"Host: localhost\r\n"
                                                  b"Connection: close\r\n\r\n")
                if status == 200:
                    self.assertEqual(body, b"cpppools worker is active")
                    return
            except (OSError, AssertionError) as exc:
                last_error = exc
            time.sleep(0.05)
        self.fail("CppPools server did not become healthy: %r" % (last_error,))

    def _read_response(self, sock, initial=b"", capture_body=True):
        response = bytearray(initial)
        while b"\r\n\r\n" not in response:
            part = sock.recv(4096)
            if not part:
                self.fail("server closed before sending complete HTTP headers")
            response.extend(part)

        header_end = response.index(b"\r\n\r\n")
        header = bytes(response[:header_end])
        body_prefix = response[header_end + 4:]
        first_line = header.split(b"\r\n", 1)[0]
        match = re.match(rb"HTTP/1\.[01] ([0-9]{3})", first_line)
        self.assertIsNotNone(match, "malformed HTTP status line: %r" % first_line)
        status = int(match.group(1))
        length_match = re.search(rb"(?:^|\r\n)Content-Length:\s*([0-9]+)",
                                 header, re.IGNORECASE)
        self.assertIsNotNone(length_match, "response must declare Content-Length")
        content_length = int(length_match.group(1))
        body_received = len(body_prefix)
        body = bytearray(body_prefix) if capture_body else None
        while body_received < content_length:
            part = sock.recv(min(65536, content_length - body_received))
            if not part:
                self.fail("server closed before sending the full HTTP body")
            body_received += len(part)
            if body is not None:
                body.extend(part)
        return (status, bytes(body[:content_length]) if body is not None else b"",
                content_length)

    def _request(self, port, request):
        status, body, _ = self._request_details(port, request)
        return status, body

    def _request_details(self, port, request, capture_body=True):
        with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
            sock.settimeout(3)
            sock.sendall(request)
            return self._read_response(sock, capture_body=capture_body)

    def test_delayed_initial_header_is_received(self):
        with self._running_server() as (_, port, _):
            with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
                sock.settimeout(3)
                time.sleep(0.25)
                sock.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\n"
                             b"Connection: close\r\n\r\n")
                status, body, _ = self._read_response(sock)
            self.assertEqual(status, 200)
            self.assertEqual(body, b"cpppools worker is active")

    def test_split_header_is_received(self):
        with self._running_server() as (_, port, _):
            with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
                sock.settimeout(3)
                sock.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\n")
                time.sleep(0.25)
                sock.sendall(b"Connection: close\r\n\r\n")
                status, body, _ = self._read_response(sock)
            self.assertEqual(status, 200)
            self.assertEqual(body, b"cpppools worker is active")

    def test_split_crud_body_is_received_and_inserted_once(self):
        with self._running_server() as (_, port, db_path):
            body = json.dumps({
                "table": "users",
                "values": {"id": 7001, "name": "split body"},
            }, separators=(",", ":")).encode("utf-8")
            request_header = (
                b"POST /api/v1/insert HTTP/1.1\r\nHost: localhost\r\n"
                b"Content-Type: application/json\r\nContent-Length: " +
                str(len(body)).encode("ascii") + b"\r\nConnection: close\r\n\r\n")
            split_at = len(body) // 2
            with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
                sock.settimeout(5)
                sock.sendall(request_header + body[:split_at])
                time.sleep(0.25)
                sock.sendall(body[split_at:])
                status, response_body, _ = self._read_response(sock)
            self.assertEqual(status, 201, response_body.decode("utf-8", "replace"))
            self.assertEqual(json.loads(response_body)["affected_rows"], 1)

            connection = sqlite3.connect(db_path)
            try:
                rows = connection.execute(
                    "SELECT id, name FROM users WHERE id = ?", (7001,)).fetchall()
            finally:
                connection.close()
            self.assertEqual(rows, [(7001, "split body")])

    def test_slow_nonreading_client_obeys_send_deadline_and_worker_recovers(self):
        if os.name == "nt":
            self.skipTest(
                "Windows loopback accepted a 65.6 MB response, and a WSL peer "
                "accepted a 131.1 MB response into the send queue with a 2.3 KB "
                "peer receive buffer; this host cannot prove remote-drain "
                "backpressure reliably")

        with self._running_server(large_schema=True) as (_, port, _):
            stats_status, stats_body = self._request(
                port, b"GET /api/v1/stats HTTP/1.1\r\nHost: localhost\r\n"
                      b"Connection: close\r\n\r\n")
            self.assertEqual(stats_status, 200)
            self.assertEqual(json.loads(stats_body)["thread_pool_workers"], 1)

            status, baseline, baseline_length = self._request_details(
                port, b"GET /api/v1/schema HTTP/1.1\r\nHost: localhost\r\n"
                      b"Connection: close\r\n\r\n", capture_body=False)
            self.assertEqual(status, 200)
            self.assertGreater(baseline_length, 1024 * 1024)
            self.assertEqual(baseline, b"")

            slow = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            try:
                slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
                slow.settimeout(10)
                slow.connect(("127.0.0.1", port))
                slow.sendall(b"GET /api/v1/schema HTTP/1.1\r\nHost: localhost\r\n"
                             b"Connection: close\r\n\r\n")
                # Read only enough to prove serialization finished and the
                # server entered response sending; leave the body unread.
                header_bytes = bytearray()
                while b"\r\n\r\n" not in header_bytes:
                    part = slow.recv(4096)
                    if not part:
                        self.fail("server closed before sending slow-response headers")
                    header_bytes.extend(part)
                header_end = header_bytes.index(b"\r\n\r\n")
                slow_header = bytes(header_bytes[:header_end])
                slow_length_match = re.search(
                    rb"(?:^|\r\n)Content-Length:\s*([0-9]+)",
                    slow_header, re.IGNORECASE)
                self.assertIsNotNone(slow_length_match)
                slow_length = int(slow_length_match.group(1))
                receive_buffer = slow.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)
                self.assertGreater(slow_length, 1024 * 1024)
                self.assertLess(receive_buffer, slow_length)
                send_started = time.monotonic()

                # With one worker, health must queue behind the send. If this
                # returns, the full response fitted in socket buffers and this
                # fixture did not exercise the send deadline.
                probe = socket.create_connection(("127.0.0.1", port), timeout=3)
                try:
                    probe.settimeout(1.5)
                    probe.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\n"
                                  b"Connection: close\r\n\r\n")
                    try:
                        probe_response = self._read_response(probe)
                    except socket.timeout:
                        pass
                    else:
                        self.fail(
                            "large slow response did not block the only worker "
                            "(body=%d bytes, client receive buffer=%d bytes, "
                            "health status=%d)" %
                            (slow_length, receive_buffer, probe_response[0]))
                finally:
                    probe.close()

                # send_all_until has a 10-second total budget. Allow 3 seconds
                # for scheduling, then require the same worker to serve health.
                deadline = send_started + 13
                recovery_deadline = send_started + 13.5
                remaining = deadline - time.monotonic()
                if remaining > 0:
                    time.sleep(remaining)
                with socket.create_connection(("127.0.0.1", port), timeout=3) as recovered:
                    recovered.settimeout(
                        max(0.1, recovery_deadline - time.monotonic()))
                    recovered.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\n"
                                      b"Connection: close\r\n\r\n")
                    status, body, _ = self._read_response(recovered)
                self.assertEqual(status, 200)
                self.assertEqual(body, b"cpppools worker is active")
                self.assertLess(time.monotonic() - send_started, 13.5)
            finally:
                slow.close()

    def test_pending_control_request_is_consumed_before_listener_initialization(self):
        from cpppools import local_node

        temp_dir = tempfile.mkdtemp(prefix="cpppools_control_init_stop_")
        server = None
        try:
            db_path = os.path.join(temp_dir, "server.sqlite")
            self._create_database(db_path)
            port = self._free_port()
            config_path = os.path.join(temp_dir, "server.json")
            with open(config_path, "w", encoding="utf-8") as handle:
                json.dump({
                    "server": {"port": port, "worker_threads": 1},
                    "datasources": [{
                        "name": "main", "type": "sqlite", "default": True,
                        "pool_size": 1, "sqlite": {"path": db_path},
                    }],
                }, handle)
            control_dir, launch_id, token = self._create_control_files(temp_dir)
            self._write_control_request(control_dir, launch_id, token)

            server = subprocess.Popen(
                [os.environ["CPL_TEST_SERVER"], "--config", config_path,
                 "--control-dir", control_dir],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.assertEqual(server.wait(timeout=10), 0,
                             "preexisting authenticated stop was not consumed at init checkpoint")
            server.communicate()
            starting = local_node._read_control_json(control_dir, "starting.json")
            stopped = local_node._read_control_json(control_dir, "stopped.json")
            self.assertFalse(os.path.exists(os.path.join(control_dir, "ready.json")),
                             "server listened despite stop being pending before initialization")
            self.assertEqual(starting.get("state"), "starting")
            self.assertEqual(stopped.get("state"), "stopped")
            self.assertEqual(stopped.get("exit_kind"), "graceful")
            self.assertEqual(stopped.get("exit_code"), 0)
            for key in ("launch_id", "pid", "process_start_time"):
                self.assertEqual(stopped.get(key), starting.get(key))
            self.assertEqual(stopped.get("launch_id"), launch_id)
        finally:
            if server is not None and server.poll() is None:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=3)
            shutil.rmtree(temp_dir, ignore_errors=True)

    def test_stopped_marker_failure_makes_managed_exit_nonzero(self):
        from cpppools import local_node

        temp_dir = tempfile.mkdtemp(prefix="cpppools_control_terminal_failure_")
        server = None
        try:
            db_path = os.path.join(temp_dir, "server.sqlite")
            self._create_database(db_path)
            port = self._free_port()
            config_path = os.path.join(temp_dir, "server.json")
            with open(config_path, "w", encoding="utf-8") as handle:
                json.dump({
                    "server": {"port": port, "worker_threads": 1},
                    "datasources": [{
                        "name": "main", "type": "sqlite", "default": True,
                        "pool_size": 1, "sqlite": {"path": db_path},
                    }],
                }, handle)
            control_dir, launch_id, token = self._create_control_files(temp_dir)
            server = subprocess.Popen(
                [os.environ["CPL_TEST_SERVER"], "--config", config_path,
                 "--control-dir", control_dir],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            deadline = time.monotonic() + 30
            ready = None
            while time.monotonic() < deadline:
                if server.poll() is not None:
                    stdout, stderr = server.communicate()
                    self.fail("managed server exited before ready (%s)\nstdout=%s\nstderr=%s" %
                              (server.returncode, stdout, stderr))
                try:
                    ready = local_node._read_control_json(control_dir, "ready.json",
                                                          missing_ok=True)
                except (OSError, local_node.LocalStateError):
                    ready = None
                if ready is not None:
                    break
                time.sleep(0.05)
            self.assertIsNotNone(ready, "managed server did not publish ready")

            stopped_path = os.path.join(control_dir, "stopped.json")
            os.mkdir(stopped_path)
            self._write_control_request(control_dir, launch_id, token)
            self.assertEqual(server.wait(timeout=10), 1,
                             "failed terminal marker must make managed service exit nonzero")
            stdout, stderr = server.communicate()
            self.assertIn("control stopped marker failed", stderr)
            self.assertTrue(os.path.isdir(stopped_path))
        finally:
            if server is not None and server.poll() is None:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=3)
            shutil.rmtree(temp_dir, ignore_errors=True)

    def test_invalid_control_token_is_ignored_and_warned_once_without_secret(self):
        from cpppools import local_node

        temp_dir = tempfile.mkdtemp(prefix="cpppools_control_bad_token_")
        server = None
        try:
            db_path = os.path.join(temp_dir, "server.sqlite")
            self._create_database(db_path)
            port = self._free_port()
            config_path = os.path.join(temp_dir, "server.json")
            with open(config_path, "w", encoding="utf-8") as handle:
                json.dump({
                    "server": {"port": port, "worker_threads": 1},
                    "datasources": [{
                        "name": "main", "type": "sqlite", "default": True,
                        "pool_size": 1, "sqlite": {"path": db_path},
                    }],
                }, handle)
            control_dir, launch_id, token = self._create_control_files(temp_dir)
            invalid_token = secrets.token_hex(32)
            self._write_control_request(control_dir, launch_id, invalid_token)
            server = subprocess.Popen(
                [os.environ["CPL_TEST_SERVER"], "--config", config_path,
                 "--control-dir", control_dir],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

            ready = None
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                if server.poll() is not None:
                    stdout, stderr = server.communicate()
                    self.fail("managed server exited before ready (%s)\nstdout=%s\nstderr=%s" %
                              (server.returncode, stdout, stderr))
                try:
                    ready = local_node._read_control_json(
                        control_dir, "ready.json", missing_ok=True)
                except (OSError, local_node.LocalStateError):
                    ready = None
                if ready is not None:
                    break
                time.sleep(0.05)
            self.assertIsNotNone(ready, "managed server did not publish ready")
            self.assertEqual(ready.get("launch_id"), launch_id)

            time.sleep(0.65)
            self.assertIsNone(server.poll(), "server accepted an invalid control token")

            self._write_control_request(control_dir, launch_id, token)
            self.assertEqual(server.wait(timeout=10), 0)
            stdout, stderr = server.communicate()
            warning = "ignored control stop request: stop.request token does not match"
            self.assertEqual(stderr.count(warning), 1,
                             "stdout=%r stderr=%r" % (stdout, stderr))
            self.assertNotIn(invalid_token, stderr)
        finally:
            if server is not None and server.poll() is None:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=3)
                server.communicate()
            shutil.rmtree(temp_dir, ignore_errors=True)

    @unittest.skipUnless(os.name == "nt",
                         "native Windows reparse rejection is a Windows acceptance case")
    def test_windows_reparse_control_directory_is_rejected(self):
        temp_dir = tempfile.mkdtemp(prefix="cpppools_windows_reparse_")
        try:
            control_dir, _launch_id, _token = self._create_control_files(temp_dir)
            junction = os.path.join(temp_dir, "control-junction")
            powershell = shutil.which("powershell.exe") or shutil.which("powershell")
            self.assertIsNotNone(powershell, "Windows PowerShell is required for junction setup")

            def ps_literal(value):
                return "'" + value.replace("'", "''") + "'"

            create_junction = (
                "New-Item -ItemType Junction -Path %s -Target %s -ErrorAction Stop | Out-Null" %
                (ps_literal(junction), ps_literal(control_dir)))
            setup = subprocess.run(
                [powershell, "-NoProfile", "-NonInteractive", "-Command", create_junction],
                capture_output=True, text=True, timeout=10)
            self.assertEqual(setup.returncode, 0,
                             "could not create an unprivileged Windows directory junction: %s" %
                             setup.stderr)

            result = subprocess.run(
                [os.environ["CPL_TEST_SERVER"], "--control-dir", junction],
                capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn("control error: control path is not a regular directory",
                          result.stderr)
        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)

    @unittest.skipUnless(os.name == "nt",
                         "native idle control stop is a Windows acceptance case")
    def test_windows_idle_control_request_stops_without_http_connection(self):
        from cpppools import local_node

        temp_dir = tempfile.mkdtemp(prefix="cpppools_windows_control_")
        server = None
        try:
            db_path = os.path.join(temp_dir, "server.sqlite")
            self._create_database(db_path)
            port = self._free_port()
            config_path = os.path.join(temp_dir, "server.json")
            with open(config_path, "w", encoding="utf-8") as handle:
                json.dump({
                    "server": {"port": port, "worker_threads": 1},
                    "datasources": [{
                        "name": "main", "type": "sqlite", "default": True,
                        "pool_size": 1, "sqlite": {"path": db_path},
                    }],
                }, handle)

            control_dir, launch_id, token = self._create_control_files(temp_dir)

            server = subprocess.Popen(
                [os.environ["CPL_TEST_SERVER"], "--config", config_path,
                 "--control-dir", control_dir],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            deadline = time.monotonic() + 30
            ready = None
            while time.monotonic() < deadline:
                if server.poll() is not None:
                    stdout, stderr = server.communicate()
                    self.fail("managed CppPools server exited before ready (%s)\nstdout=%s\nstderr=%s" %
                              (server.returncode, stdout, stderr))
                try:
                    ready = local_node._read_control_json(control_dir, "ready.json",
                                                          missing_ok=True)
                except (OSError, local_node.LocalStateError):
                    ready = None
                if ready is not None:
                    break
                time.sleep(0.05)
            self.assertIsNotNone(ready, "managed CppPools server did not publish ready")
            identity = {
                "launch_id": launch_id,
                "pid": server.pid,
                "process_start_time": ready.get("process_start_time"),
            }
            self.assertEqual(ready.get("state"), "ready")
            self.assertEqual(ready.get("protocol_version"), 1)
            self.assertEqual(ready.get("launch_id"), launch_id)
            self.assertEqual(ready.get("pid"), server.pid)
            self.assertTrue(identity["process_start_time"])
            self.assertEqual(ready.get("port"), port)

            self._write_control_request(control_dir, launch_id, token)
            self.assertEqual(server.wait(timeout=10), 0,
                             "idle managed server did not stop from its control checkpoint")
            server.communicate()
            stopped = local_node._read_control_json(control_dir, "stopped.json")
            self.assertEqual(stopped.get("protocol_version"), 1)
            self.assertEqual(stopped.get("state"), "stopped")
            self.assertEqual(stopped.get("exit_kind"), "graceful")
            self.assertEqual(stopped.get("exit_code"), 0)
            for key, value in identity.items():
                self.assertEqual(stopped.get(key), value)
        finally:
            if server is not None and server.poll() is None:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=3)
            shutil.rmtree(temp_dir, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
