import contextlib
import io
import os
import subprocess
import sys
import unittest
from importlib import metadata
from unittest import mock

from cpppools import cli


class TestCliParser(unittest.TestCase):
    def parse(self, argv):
        return cli.build_parser().parse_args(argv)

    def test_formal_commands_and_aliases_parse(self):
        cases = [
            (["start", "-c", "server.json"], "start"),
            (["stop"], "stop"),
            (["status"], "status"),
            (["st"], "status"),
            (["health"], "health"),
            (["h"], "health"),
            (["select", "users", "-w", '{"column":"id","value":1}'], "select"),
            (["get", "users", "-w", '{"column":"id","value":1}'], "select"),
            (["insert", "users", "-D", '{"id":1}'], "insert"),
            (["add", "users", "-D", '{"id":1}'], "insert"),
            (["update", "users", "-w", '{"column":"id","value":1}',
              "-D", '{"name":"a"}'], "update"),
            (["set", "users", "-w", '{"column":"id","value":1}',
              "-D", '{"name":"a"}'], "update"),
            (["delete", "users", "-w", '{"column":"id","value":1}'], "delete"),
            (["del", "users", "-w", '{"column":"id","value":1}'], "delete"),
        ]
        for argv, expected in cases:
            with self.subTest(argv=argv):
                self.assertEqual(self.parse(argv).command, expected)

    def test_repeatable_and_command_scoped_options(self):
        args = self.parse([
            "select", "users", "-w", '{"column":"id","value":1}',
            "-u", "http://a", "-u", "http://b", "-C", "id", "-C", "name",
        ])
        self.assertEqual(args.urls, ["http://a", "http://b"])
        self.assertEqual(args.columns, ["id", "name"])

    def test_options_before_command_are_rejected(self):
        with self.assertRaises(SystemExit) as ctx:
            self.parse(["-u", "http://127.0.0.1:8080", "health"])
        self.assertEqual(ctx.exception.code, 2)

    def test_abbreviations_are_rejected(self):
        for argv in (["health", "--time", "1"],
                     ["select", "users", "-w", "{}", "--datas", "main"]):
            with self.subTest(argv=argv):
                with self.assertRaises(SystemExit) as ctx:
                    self.parse(argv)
                self.assertEqual(ctx.exception.code, 2)

    def test_single_value_options_cannot_repeat(self):
        with self.assertRaises(SystemExit) as ctx:
            self.parse(["start", "-c", "a.json", "-c", "b.json"])
        self.assertEqual(ctx.exception.code, 2)

    def test_help_and_version_do_not_construct_sdk(self):
        help_out = io.StringIO()
        with contextlib.redirect_stdout(help_out):
            with self.assertRaises(SystemExit) as help_exit:
                cli.main(["--help"])
        self.assertEqual(help_exit.exception.code, 0)
        self.assertIn("select", help_out.getvalue())
        self.assertIn("health", help_out.getvalue())

        version_out = io.StringIO()
        with contextlib.redirect_stdout(version_out):
            with self.assertRaises(SystemExit) as version_exit:
                cli.main(["--version"])
        self.assertEqual(version_exit.exception.code, 0)
        self.assertTrue(version_out.getvalue().strip())

    def test_version_uses_distribution_metadata_and_source_fallback(self):
        for installed, expected in (("2.3.4", "2.3.4"),
                                    (metadata.PackageNotFoundError("cpppools"), "0+dev")):
            with self.subTest(expected=expected):
                out = io.StringIO()
                kwargs = ({"side_effect": installed} if isinstance(installed, Exception)
                          else {"return_value": installed})
                with mock.patch.object(metadata, "version", **kwargs), \
                        contextlib.redirect_stdout(out):
                    with self.assertRaises(SystemExit) as result:
                        cli.main(["--version"])
                self.assertEqual(result.exception.code, 0)
                self.assertEqual(out.getvalue().strip(), expected)

    def test_local_management_missing_state_is_local_error(self):
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            code = cli.main(["status"])
        self.assertEqual(code, 10)
        self.assertIn("local_state_error", err.getvalue())

    def test_module_entrypoint_propagates_exit_code(self):
        env = os.environ.copy()
        env["PYTHONPATH"] = os.pathsep.join(
            [os.path.abspath(os.path.join(os.path.dirname(__file__), "..")),
             env.get("PYTHONPATH", "")])
        result = subprocess.run(
            [sys.executable, "-m", "cpppools", "status"],
            env=env, text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 10)
        self.assertIn("local_state_error", result.stderr)


if __name__ == "__main__":
    unittest.main()
