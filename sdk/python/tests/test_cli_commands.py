import contextlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import os
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.request
from unittest import mock

from cpppools import cli, errors, transport


class TestStrictJsonInputs(unittest.TestCase):
    def test_literal_file_stdin_and_bom(self):
        self.assertEqual(cli.parse_json_source('{"n":1}')['n'], 1)
        with tempfile.NamedTemporaryFile(delete=False) as handle:
            handle.write(b'\xef\xbb\xbf{"name":"\xe4\xb8\xad"}')
            path = handle.name
        try:
            self.assertEqual(cli.parse_json_source("@" + path)['name'], "中")
        finally:
            os.unlink(path)
        self.assertEqual(cli.parse_json_source("-", io.BytesIO(b'{"ok":true}'))['ok'], True)

    def test_rejects_duplicate_keys_special_numbers_and_integer_overflow(self):
        for text in ('{"a":1,"a":2}', '{"a":NaN}',
                     '{"a":Infinity}', '{"a":-Infinity}',
                     '{"a":9223372036854775808}'):
            with self.subTest(text=text), self.assertRaises(cli.InputError):
                cli.parse_json_source(text)

    def test_rejects_invalid_utf8_and_oversized_file(self):
        with tempfile.NamedTemporaryFile(delete=False) as handle:
            handle.write(b'\xff')
            invalid = handle.name
        try:
            with self.assertRaises(cli.InputError):
                cli.parse_json_source("@" + invalid)
        finally:
            os.unlink(invalid)
        with tempfile.NamedTemporaryFile(delete=False) as handle:
            handle.write(b' ' * 65537)
            oversized = handle.name
        try:
            with self.assertRaises(cli.InputError):
                cli.parse_json_source("@" + oversized)
        finally:
            os.unlink(oversized)

    def test_stdin_read_is_bounded(self):
        class BoundedReader:
            def __init__(self):
                self.requested = None

            def read(self, size=-1):
                self.requested = size
                return b" " * (size + 1)

        reader = BoundedReader()
        with self.assertRaises(cli.InputError):
            cli.parse_json_source("-", reader)
        self.assertEqual(reader.requested, cli.MAX_INPUT_BYTES + 1)


class TestHealthCommand(unittest.TestCase):
    def response(self, status, body=b"body"):
        return transport.Response(status, [], body)

    def invoke_health(self, mapping, urls):
        def fake_request(method, url, **kwargs):
            value = mapping.get(url, mapping[url.rsplit("/health", 1)[0]])
            if isinstance(value, BaseException):
                raise value
            return value

        args = cli.build_parser().parse_args(["health", "-o", "json"] +
                                             sum((["-u", u] for u in urls), []))
        out = io.StringIO()
        err = io.StringIO()
        with mock.patch.object(cli.transport, "request", side_effect=fake_request):
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = cli.run_health(args)
        return code, json.loads(out.getvalue()), err.getvalue()

    def test_success_and_307_do_not_follow(self):
        code, doc, _ = self.invoke_health({
            "http://a": self.response(200, b"cpppools worker is active"),
            "http://b": self.response(307, b"redirect"),
        }, ["http://a", "http://b"])
        self.assertEqual(code, 4)
        self.assertTrue(doc["results"][0]["ok"])
        self.assertFalse(doc["results"][1]["ok"])
        self.assertEqual(doc["results"][1]["http_status"], 307)

    def test_real_http_307_is_not_followed(self):
        requests = []

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                requests.append(self.path)
                if self.path == "/health":
                    self.send_response(307)
                    self.send_header("Location", "/redirect-target")
                    self.end_headers()
                else:
                    self.send_response(200)
                    self.end_headers()
                    self.wfile.write(b"should not be reached")

            def log_message(self, *_args):
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            args = cli.build_parser().parse_args([
                "health", "-u", "http://127.0.0.1:%d" % server.server_port,
                "-o", "json"])
            out = io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
                code = cli.run_health(args)
            result = json.loads(out.getvalue())["results"][0]
            self.assertEqual(code, 4)
            self.assertEqual(result["http_status"], 307)
            self.assertEqual(requests, ["/health"])
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)

    def test_all_success_returns_zero_and_emits_json_stdout_only(self):
        code, doc, err = self.invoke_health({
            "http://a": self.response(200, "存活".encode("utf-8")),
            "http://b": self.response(200, b"alive"),
        }, ["http://a", "http://b"])
        self.assertEqual(code, 0)
        self.assertEqual([item["body"] for item in doc["results"]], ["存活", "alive"])
        self.assertEqual(err, "")

    def test_exit_priority_is_independent_of_url_order(self):
        mapping = {
            "http://a": errors.ClientError("bad", 400),
            "http://b": errors.ServerError("down", 500),
            "http://c": errors.NodeUnavailable("offline"),
        }
        code, doc, _ = self.invoke_health(mapping, ["http://a", "http://b", "http://c"])
        self.assertEqual(code, 3)
        self.assertEqual(len(doc["results"]), 3)
        reverse_code, _, _ = self.invoke_health(
            mapping, ["http://c", "http://b", "http://a"])
        self.assertEqual(reverse_code, 3)

    def test_server_error_priority_over_client_error(self):
        mapping = {
            "http://a": errors.ServerError("down", 500),
            "http://b": errors.ClientError("bad", 400),
        }
        code, _, _ = self.invoke_health(mapping, ["http://a", "http://b"])
        self.assertEqual(code, 5)
        reverse_code, _, _ = self.invoke_health(mapping, ["http://b", "http://a"])
        self.assertEqual(reverse_code, 5)

    def test_invalid_url_is_usage_error_before_transport(self):
        args = cli.build_parser().parse_args(["health", "-u", "file:///tmp/x"])
        with self.assertRaises(cli.InputError):
            cli.health_urls(args)


@unittest.skipUnless(os.environ.get("CPL_RUN_E2E") == "1" and
                     os.environ.get("CPL_TEST_SERVER"),
                     "set CPL_RUN_E2E=1 and CPL_TEST_SERVER to run live CppPools health smoke")
class TestLiveCppPoolsHealth(unittest.TestCase):
    def test_cli_health_against_real_server(self):
        with tempfile.TemporaryDirectory(prefix="cpppools_cli_health_") as temp_dir:
            db_path = os.path.join(temp_dir, "health.db")
            conn = sqlite3.connect(db_path)
            conn.execute("CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL)")
            conn.close()

            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]

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
            try:
                url = "http://127.0.0.1:%d" % port
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    if server.poll() is not None:
                        self.fail("CppPools server exited before health was ready")
                    try:
                        with urllib.request.urlopen(url + "/health", timeout=0.2) as response:
                            if response.status == 200:
                                break
                    except Exception:
                        time.sleep(0.05)
                else:
                    self.fail("CppPools server did not become ready")

                env = os.environ.copy()
                env["PYTHONPATH"] = os.path.abspath(
                    os.path.join(os.path.dirname(__file__), ".."))
                result = subprocess.run(
                    [sys.executable, "-m", "cpppools", "health", "-u", url, "-o", "json"],
                    env=env, text=True, capture_output=True, timeout=5, check=False)
                self.assertEqual(result.returncode, 0, result.stderr)
                doc = json.loads(result.stdout)
                self.assertEqual(len(doc["results"]), 1)
                self.assertTrue(doc["results"][0]["ok"])
                self.assertEqual(doc["results"][0]["http_status"], 200)
                self.assertEqual(doc["results"][0]["body"], "cpppools worker is active")
            finally:
                if server.poll() is None:
                    server.terminate()
                    try:
                        server.wait(timeout=8)
                    except subprocess.TimeoutExpired:
                        server.kill()
                        server.wait(timeout=3)

    def test_invalid_port_and_nonpositive_or_nonfinite_timeout_fail_before_transport(self):
        for url in ("http://example.test:bad", "http://example.test:99999",
                    "http://[invalid/", "http://user@example.test",
                    "http://bad host"):
            with self.subTest(url=url):
                args = cli.build_parser().parse_args(["health", "-u", url])
                with self.assertRaises(cli.InputError):
                    cli.health_urls(args)
        for timeout in ("0", "-1", "nan", "inf"):
            with self.subTest(timeout=timeout):
                args = cli.build_parser().parse_args(["health", "-t", timeout])
                with self.assertRaises(cli.InputError):
                    cli.validate_health_args(args)

    def test_main_rejects_invalid_health_options_before_transport(self):
        with mock.patch.object(cli.transport, "request") as request:
            out = io.StringIO()
            err = io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = cli.main(["health", "-u", "http://example.test:bad"])
        self.assertEqual(code, 2)
        self.assertEqual(out.getvalue(), "")
        self.assertIn("invalid URL", err.getvalue())
        request.assert_not_called()


if __name__ == "__main__":
    unittest.main()
