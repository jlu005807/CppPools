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

from cpppools import cli, errors, local_node, transport


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
                     '{"a":9223372036854775808}',
                     '{"a":-9223372036854775809}'):
            with self.subTest(text=text), self.assertRaises(cli.InputError):
                cli.parse_json_source(text)

    def test_accepts_exact_signed_int64_boundaries(self):
        self.assertEqual(cli.parse_json_source('{"n":9223372036854775807}')['n'],
                         9223372036854775807)
        self.assertEqual(cli.parse_json_source('{"n":-9223372036854775808}')['n'],
                         -9223372036854775808)

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

    def test_huge_integer_and_deep_json_are_input_errors_for_all_sources(self):
        values = (
            '{"id":' + '1' * 4301 + '}',
            '{"id":' + '[' * 5000 + '0' + ']' * 5000 + '}',
        )
        for value in values:
            self.assertLess(len(value.encode("utf-8")), cli.MAX_INPUT_BYTES)
            with tempfile.NamedTemporaryFile(delete=False) as handle:
                handle.write(value.encode("utf-8"))
                path = handle.name
            try:
                sources = (
                    (value, None),
                    ("@" + path, None),
                    ("-", io.BytesIO(value.encode("utf-8"))),
                )
                for source, stdin in sources:
                    with self.subTest(size=len(value), source=source[:8]), \
                            self.assertRaises(cli.InputError):
                        cli.parse_json_source(source, stdin)
            finally:
                os.unlink(path)

    def test_json_depth_check_ignores_delimiters_inside_strings(self):
        value = "[{}]\\\"" * 2000
        self.assertEqual(cli.parse_json_source(json.dumps({"text": value})),
                         {"text": value})

    def test_decoder_value_and_recursion_errors_are_input_errors_only(self):
        for failure in (ValueError("decoder value error"),
                        RecursionError("decoder recursion error")):
            with self.subTest(error=type(failure).__name__), \
                    mock.patch.object(cli.json, "loads", side_effect=failure), \
                    self.assertRaises(cli.InputError):
                cli._load_json_bytes(b"{}")

        with mock.patch.object(cli.json, "loads",
                               side_effect=MemoryError("out of memory")), \
                self.assertRaises(MemoryError):
            cli._load_json_bytes(b"{}")

    def test_huge_integer_and_deep_json_subprocesses_have_no_traceback(self):
        values = (
            '{"id":' + '1' * 4301 + '}',
            '{"id":' + '[' * 5000 + '0' + ']' * 5000 + '}',
        )
        sdk_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
        env = dict(os.environ)
        env["PYTHONPATH"] = os.pathsep.join(filter(
            None, (sdk_root, env.get("PYTHONPATH"))))
        for value in values:
            with self.subTest(size=len(value)):
                completed = subprocess.run(
                    [sys.executable, "-m", "cpppools", "insert", "users",
                     "-D", value, "-o", "json"],
                    capture_output=True, text=True, encoding="utf-8",
                    env=env, timeout=10, check=False)
                self.assertEqual(completed.returncode, 2, completed.stderr)
                self.assertEqual(completed.stdout, "")
                self.assertNotIn("Traceback", completed.stderr)
                self.assertEqual(json.loads(completed.stderr)["kind"],
                                 "input_error")

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

    def test_main_reports_invalid_health_args_as_structured_input_error(self):
        cases = (
            ["health", "-u", "file:///tmp/x", "-o", "json"],
            ["health", "-t", "nan", "-o", "json"],
        )
        with mock.patch.object(cli.transport, "request") as request:
            for argv in cases:
                out, err = io.StringIO(), io.StringIO()
                with self.subTest(argv=argv), \
                        contextlib.redirect_stdout(out), \
                        contextlib.redirect_stderr(err):
                    code = cli.main(argv)

                self.assertEqual(code, 2)
                self.assertEqual(out.getvalue(), "")
                document = json.loads(err.getvalue())
                self.assertEqual(document["kind"], "input_error")
                self.assertEqual(document["exit_code"], 2)
                self.assertNotIn("Traceback", err.getvalue())
        request.assert_not_called()


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


class TestCrudCommands(unittest.TestCase):
    class StubClient:
        calls = []
        results = {}
        failures = {}

        def __init__(self, seeds, read_timeout=None):
            self.seeds = seeds
            self.read_timeout = read_timeout
            type(self).calls.append(("init", list(seeds), read_timeout))

        def _call(self, name, *args, **kwargs):
            type(self).calls.append((name, args, kwargs))
            failure = type(self).failures.get(name)
            if failure is not None:
                raise failure
            result = type(self).results.get(name)
            return result() if callable(result) else result

        def select(self, *args, **kwargs):
            return self._call("select", *args, **kwargs)

        def insert(self, *args, **kwargs):
            return self._call("insert", *args, **kwargs)

        def update(self, *args, **kwargs):
            return self._call("update", *args, **kwargs)

        def delete(self, *args, **kwargs):
            return self._call("delete", *args, **kwargs)

    def setUp(self):
        self.StubClient.calls = []
        self.StubClient.results = {}
        self.StubClient.failures = {}

    def invoke(self, argv):
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(cli, "ClusterClient", self.StubClient, create=True):
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = cli.main(argv)
        return code, out.getvalue(), err.getvalue()

    def test_crud_commands_map_payloads_and_timeout_to_sdk(self):
        self.StubClient.results = {
            "select": {"datasource": "main", "columns": ["id"],
                       "rows": [[7]], "row_count": 1},
            "insert": {"datasource": "main", "affected_rows": 1,
                       "inserted_id": 7},
            "update": {"datasource": "main", "affected_rows": 0},
            "delete": {"datasource": "main", "affected_rows": 0},
        }
        commands = [
            (["get", "users", "-w", '{"column":"id","value":7}',
              "-C", "id", "-d", "main", "-u", "http://seed", "-t", "1.25"],
             "select", ("users", {"column": "id", "value": 7}, ["id"], "main")),
            (["add", "users", "-D", '{"id":7,"name":"n"}',
              "-d", "main", "-u", "http://seed"],
             "insert", ("users", {"id": 7, "name": "n"}, "main")),
            (["set", "users", "-w", '{"column":"id","value":7}',
              "-D", '{"name":"n2"}', "-u", "http://seed"],
             "update", ("users", {"name": "n2"},
                         {"column": "id", "value": 7}, None)),
            (["del", "users", "-w", '{"column":"id","value":7}',
              "-d", "main", "-u", "http://seed"],
             "delete", ("users", {"column": "id", "value": 7}, "main")),
        ]
        for argv, method, expected in commands:
            with self.subTest(argv=argv):
                code, out, err = self.invoke(argv)
                self.assertEqual(code, 0, err)
                self.assertEqual(err, "")
                self.assertTrue(out.strip())
                call = [item for item in self.StubClient.calls
                        if item[0] == method][-1]
                if method == "select":
                    self.assertEqual(call[1], expected[:2])
                    self.assertEqual(call[2], {"columns": expected[2],
                                                "datasource": expected[3]})
                elif method == "update":
                    self.assertEqual(call[1], expected[:3])
                    self.assertEqual(call[2], {"datasource": expected[3]})
                else:
                    self.assertEqual(call[1], expected[:2])
                    self.assertEqual(call[2], {"datasource": expected[2]})
        init_calls = [item for item in self.StubClient.calls if item[0] == "init"]
        self.assertEqual(init_calls[0], ("init", ["http://seed"], 1.25))

    def test_zero_rows_are_success_and_json_is_raw_result(self):
        self.StubClient.results = {
            "select": {"datasource": "main", "columns": ["id"],
                       "rows": [], "row_count": 0},
            "update": {"datasource": "main", "affected_rows": 0},
        }
        for argv in (
            ["select", "users", "-w", '{"column":"id","value":9}', "-o", "json"],
            ["update", "users", "-w", '{"column":"id","value":9}',
             "-D", '{"name":"x"}', "-o", "json"],
        ):
            code, out, err = self.invoke(argv)
            self.assertEqual(code, 0, err)
            self.assertEqual(err, "")
            self.assertEqual(json.loads(out), self.StubClient.results[
                "select" if argv[0] == "select" else "update"])

    def test_sdk_errors_map_to_stable_exit_codes(self):
        cases = [
            (errors.NodeUnavailable("offline"), 3, "node_unavailable"),
            (errors.TopologyError("bad topology"), 3, "topology_error"),
            (errors.ClientError("bad", 400), 4, "client_error"),
            (errors.ConflictError("conflict", 409), 4, "client_error"),
            (errors.UnavailableError("busy", 503), 5, "server_error"),
            (errors.ServerError("down", 500), 5, "server_error"),
        ]
        for exception, code_expected, kind in cases:
            with self.subTest(exception=type(exception).__name__):
                self.StubClient.failures = {"delete": exception}
                code, out, err = self.invoke([
                    "delete", "users", "-w", '{"column":"id","value":1}',
                    "-o", "json"])
                self.assertEqual(code, code_expected)
                self.assertEqual(out, "")
                document = json.loads(err)
                self.assertEqual(document["kind"], kind)
                self.assertEqual(document["exit_code"], code_expected)

    def test_malformed_success_is_protocol_error_without_retry(self):
        self.StubClient.results = {"insert": {"affected_rows": 1}}
        code, out, err = self.invoke([
            "insert", "users", "-D", '{"id":1}', "-o", "json"])
        self.assertEqual(code, 5)
        self.assertEqual(out, "")
        self.assertEqual(json.loads(err)["kind"], "protocol_error")
        self.assertEqual(len([c for c in self.StubClient.calls if c[0] == "insert"]), 1)

    def test_invalid_crud_json_and_nested_values_do_not_construct_sdk(self):
        cases = [
            ["select", "users", "-w", "{}"],
            ["insert", "users", "-D", '{"id":{"nested":1}}'],
            ["update", "users", "-w", '{"column":"id","value":1}',
             "-D", "{}"],
        ]
        for argv in cases:
            with self.subTest(argv=argv):
                code, out, err = self.invoke(argv)
                self.assertEqual(code, 2)
                self.assertEqual(out, "")
                self.assertIn("input", err)
        self.assertFalse(any(call[0] == "init" for call in self.StubClient.calls))

    def test_post_parse_crud_input_errors_are_structured_before_sdk(self):
        with tempfile.NamedTemporaryFile(delete=False) as handle:
            oversized_value = "x" * (cli.MAX_INPUT_BYTES - 128)
            source = json.dumps({"name": oversized_value})
            while len(json.dumps({
                    "table": "users", "values": {"name": oversized_value}
            }).encode("utf-8")) <= cli.MAX_INPUT_BYTES:
                oversized_value += "x"
                source = json.dumps({"name": oversized_value})
            handle.write(source.encode("utf-8"))
            oversized_path = handle.name
        self.assertLess(os.path.getsize(oversized_path), cli.MAX_INPUT_BYTES)
        cases = [
            ["insert", "users", "-D", "", "-o", "json"],
            ["select", "users", "-w", "null", "-o", "json"],
            ["select", "users", "-w", "", "-o", "json"],
            ["insert", "users", "-D", '{"id":1,"id":2}', "-o", "json"],
            ["update", "users", "-w", '{"column":"id","value":1}',
             "-D", '{"name":NaN}', "-o", "json"],
            ["select", "users", "-w", '{"column":"id","value":1}',
             "-t", "inf", "-o", "json"],
            ["select", "users", "-w", '{"column":"id","value":1}',
             "-t", "0", "-o", "json"],
            ["delete", "users", "-w", '{"column":"id","value":1}',
             "-u", "file:///tmp/x", "-o", "json"],
            ["insert", "users", "-D", "@" + oversized_path, "-o", "json"],
        ]
        try:
            for argv in cases:
                with self.subTest(command=argv[0], input=argv[3]):
                    code, out, err = self.invoke(argv)
                    self.assertEqual(code, 2)
                    self.assertEqual(out, "")
                    document = json.loads(err)
                    self.assertEqual(document["kind"], "input_error")
                    self.assertEqual(document["exit_code"], 2)
                    self.assertNotIn("Traceback", err)
            self.assertFalse(any(call[0] == "init"
                                 for call in self.StubClient.calls))
        finally:
            os.unlink(oversized_path)


class TestLocalCommands(unittest.TestCase):
    class StubManager:
        calls = []

        def __init__(self, state_root, name):
            self.state_root = state_root
            self.name = name
            type(self).calls.append(("init", state_root, name))

        def start(self, **kwargs):
            type(self).calls.append(("start", kwargs))
            return {"name": self.name, "state": "running", "pid": 123,
                    "endpoint": "http://127.0.0.1:8080", "launch_id": "l1",
                    "log_file": "server.log", "already_running": False}

        def status(self):
            type(self).calls.append(("status",))
            return {"name": self.name, "state": "stopped", "pid": None,
                    "process_alive": False, "identity_verified": False}

        def stop(self, **kwargs):
            type(self).calls.append(("stop", kwargs))
            return {"name": self.name, "state": "stopped", "pid": 123,
                    "exit_kind": "graceful", "last_exit_code": None}

    def setUp(self):
        self.StubManager.calls = []

    def invoke(self, argv):
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(cli, "LocalNodeManager", self.StubManager,
                               create=True):
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = cli.main(argv)
        return code, out.getvalue(), err.getvalue()

    def test_status_start_stop_dispatch_to_local_manager(self):
        code, out, err = self.invoke(["status", "-n", "dev", "-R", "/tmp/state",
                                      "-o", "json"])
        self.assertEqual(code, 0, err)
        self.assertEqual(json.loads(out)["state"], "stopped")
        code, out, err = self.invoke(["start", "-n", "dev", "-R", "/tmp/state",
                                      "-c", "/tmp/config.json", "-b", "/tmp/server",
                                      "-t", "2", "-o", "json"])
        self.assertEqual(code, 0, err)
        start = [call for call in self.StubManager.calls if call[0] == "start"][-1]
        self.assertEqual(start[1]["config"], "/tmp/config.json")
        self.assertEqual(start[1]["binary"], "/tmp/server")
        self.assertEqual(start[1]["timeout"], 2.0)
        code, out, err = self.invoke(["stop", "-n", "dev", "-R", "/tmp/state",
                                      "-t", "1", "-o", "json"])
        self.assertEqual(code, 0, err)
        stop = [call for call in self.StubManager.calls if call[0] == "stop"][-1]
        self.assertEqual(stop[1], {"timeout": 1.0, "force": False})

    def test_local_manager_error_is_structured(self):
        class BrokenManager(self.StubManager):
            def status(self):
                raise local_node.LocalStateError("state is corrupt")

        with mock.patch.object(cli, "LocalNodeManager", BrokenManager, create=True):
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = cli.main(["status", "-o", "json"])
        self.assertEqual(code, 10)
        self.assertEqual(out.getvalue(), "")
        self.assertEqual(json.loads(err.getvalue())["kind"], "local_state_error")

    def test_foreground_readiness_is_immediate_diagnostic_and_stdout_is_final_json(self):
        out, err = io.StringIO(), io.StringIO()
        ready = {"name": "dev", "state": "running", "pid": 123}
        finished = dict(ready, state="stopped", exit_kind="graceful")

        class NotifyingManager(self.StubManager):
            def start(manager, **kwargs):
                self.assertTrue(kwargs["foreground"])
                self.assertEqual(out.getvalue(), "")
                kwargs["on_ready"](ready)
                self.assertEqual(out.getvalue(), "")
                self.assertEqual(json.loads(err.getvalue())["state"], "running")
                return finished

        with mock.patch.object(cli, "LocalNodeManager", NotifyingManager, create=True), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = cli.main(["start", "-n", "dev", "-c", "/tmp/server.json", "-F", "-o", "json"])
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(out.getvalue()), finished)

    def test_start_cancellation_and_foreground_interrupt_keep_exit_results(self):
        outcomes = [
            (local_node.LocalNodeError(
                "startup canceled", result={"name": "dev", "state": "stopping",
                                             "launch_id": "launch-1"}),
             6, "start_error"),
            (local_node.ExitOutcome(
                "foreground stopped", 130, "interrupted",
                {"name": "dev", "state": "stopped", "exit_kind": "graceful"}),
             130, "interrupted"),
        ]
        for exception, expected_code, expected_kind in outcomes:
            with self.subTest(code=expected_code):
                class OutcomeManager(self.StubManager):
                    def start(self, **_kwargs):
                        raise exception

                out, err = io.StringIO(), io.StringIO()
                with mock.patch.object(cli, "LocalNodeManager", OutcomeManager,
                                       create=True), \
                        contextlib.redirect_stdout(out), \
                        contextlib.redirect_stderr(err):
                    code = cli.main(["start", "-n", "dev", "-c",
                                     "/tmp/config.json", "-o", "json"])

                self.assertEqual(code, expected_code)
                self.assertEqual(json.loads(out.getvalue())["state"],
                                 exception.result["state"])
                self.assertEqual(json.loads(err.getvalue())["kind"], expected_kind)

    def test_local_stop_outcome_prints_result_before_structured_error(self):
        cases = [
            (local_node.ExitOutcome(
                "force accepted", 9, "forced_stop",
                {"name": "dev", "state": "crashed", "exit_kind": "forced"}),
             9, "forced_stop", "forced"),
            (local_node.ExitOutcome(
                "exit evidence missing", 10, "local_state_error",
                {"name": "dev", "state": "crashed", "exit_kind": None}),
             10, "local_state_error", None),
            (local_node.StopTimeout(
                "still stopping", result={"name": "dev", "state": "stopping"}),
             7, "stop_timeout", None),
        ]
        original_print_result = cli._print_local_result
        original_write_error = cli._write_runtime_error
        for exception, expected_code, expected_kind, expected_exit_kind in cases:
            with self.subTest(code=expected_code):
                events = []

                class OutcomeManager(self.StubManager):
                    def stop(self, **_kwargs):
                        raise exception

                out, err = io.StringIO(), io.StringIO()
                with mock.patch.object(cli, "LocalNodeManager", OutcomeManager,
                                       create=True), \
                        mock.patch.object(
                            cli, "_print_local_result",
                            side_effect=lambda result, output: (
                                events.append("result"),
                                original_print_result(result, output))), \
                        mock.patch.object(
                            cli, "_write_runtime_error",
                            side_effect=lambda kind, exc, code: (
                                events.append("error"),
                                original_write_error(kind, exc, code))), \
                        contextlib.redirect_stdout(out), \
                        contextlib.redirect_stderr(err):
                    code = cli.main(["stop", "-n", "dev", "-o", "json"])

                self.assertEqual(code, expected_code)
                self.assertEqual(events, ["result", "error"])
                result = json.loads(out.getvalue())
                self.assertEqual(result["state"], exception.result["state"])
                self.assertEqual(json.loads(err.getvalue())["kind"], expected_kind)
                if expected_exit_kind is not None:
                    self.assertEqual(result["exit_kind"], expected_exit_kind)


@unittest.skipUnless(os.environ.get("CPL_RUN_E2E") == "1" and
                     os.environ.get("CPL_TEST_SERVER"),
                     "set CPL_RUN_E2E=1 and CPL_TEST_SERVER for live CRUD smoke")
class TestLiveCppPoolsCrud(unittest.TestCase):
    def test_cli_crud_round_trip_against_real_server(self):
        with tempfile.TemporaryDirectory(prefix="cpppools_cli_crud_") as temp_dir:
            db_path = os.path.join(temp_dir, "crud.db")
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
                        self.fail("CppPools server exited before CRUD was ready")
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

                def invoke(arguments):
                    return subprocess.run(
                        [sys.executable, "-m", "cpppools"] + arguments,
                        env=env, text=True, capture_output=True,
                        timeout=5, check=False)

                inserted = invoke(["add", "users", "-D", '{"id":41,"name":"alice"}',
                                   "-u", url, "-o", "json"])
                self.assertEqual(inserted.returncode, 0, inserted.stderr)
                self.assertEqual(json.loads(inserted.stdout)["affected_rows"], 1)

                selected = invoke(["get", "users", "-w", '{"column":"id","value":41}',
                                   "-u", url, "-o", "json"])
                self.assertEqual(selected.returncode, 0, selected.stderr)
                self.assertEqual(json.loads(selected.stdout)["rows"], [[41, "alice"]])

                updated = invoke(["set", "users", "-w", '{"column":"id","value":41}',
                                  "-D", '{"name":"bob"}', "-u", url, "-o", "json"])
                self.assertEqual(updated.returncode, 0, updated.stderr)
                self.assertEqual(json.loads(updated.stdout)["affected_rows"], 1)

                deleted = invoke(["del", "users", "-w", '{"column":"id","value":41}',
                                  "-u", url, "-o", "json"])
                self.assertEqual(deleted.returncode, 0, deleted.stderr)
                self.assertEqual(json.loads(deleted.stdout)["affected_rows"], 1)
            finally:
                if server.poll() is None:
                    server.terminate()
                    try:
                        server.wait(timeout=8)
                    except subprocess.TimeoutExpired:
                        server.kill()
                        server.wait(timeout=3)


if __name__ == "__main__":
    unittest.main()
