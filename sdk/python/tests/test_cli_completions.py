"""Static completion resources must follow the actual command parser."""

import argparse
from importlib import resources
from importlib.util import find_spec
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

from cpppools import cli


def resource_path(name):
    if hasattr(resources, "files"):
        return resources.as_file(resources.files("cpppools.completions") / name)
    return resources.path("cpppools.completions", name)


class TestCliCompletions(unittest.TestCase):
    def test_all_commands_aliases_and_options_are_shipped(self):
        self.assertIsNotNone(find_spec("cpppools.completions"))
        parser = cli.build_parser()
        commands = next(action.choices for action in parser._actions
                        if isinstance(action, argparse._SubParsersAction))
        for name in ("cpl.bash", "_cpl", "cpl.ps1"):
            with self.subTest(resource=name):
                with resource_path(name) as path:
                    content = path.read_text(encoding="utf-8")
                for command, command_parser in commands.items():
                    self.assertIn(command, content)
                    for action in command_parser._actions:
                        for option in action.option_strings:
                            self.assertIn(option, content)

    @unittest.skipUnless(os.name == "posix" and shutil.which("bash"), "requires POSIX Bash")
    def test_bash_completion_respects_command_scope_and_output_values(self):
        with resource_path("cpl.bash") as path:
            for words, index, required, absent in (
                    ("cpl ''", 1, "select", "--where"),
                    ("cpl get --", 2, "--where", "--data"),
                    ("cpl health --", 2, "--url", "--where"),
                    ("cpl start --output ''", 3, "json", "--config")):
                script = ("source %s; COMP_WORDS=(%s); COMP_CWORD=%d; "
                          "_cpl_complete; printf '%%s\\n' \"${COMPREPLY[@]}\"") % (
                              shlex.quote(str(path)), words, index)
                result = subprocess.run(["bash", "-c", script], check=True,
                                        text=True, capture_output=True)
                self.assertIn(required, result.stdout.splitlines())
                self.assertNotIn(absent, result.stdout.splitlines())

    @unittest.skipUnless(shutil.which("powershell") or shutil.which("pwsh"),
                         "requires PowerShell")
    def test_powershell_completion_respects_command_scope(self):
        shell = shutil.which("pwsh") or shutil.which("powershell")
        self.assertIsNotNone(find_spec("cpppools.completions"))
        with resource_path("cpl.ps1") as path, tempfile.TemporaryDirectory() as bin_dir:
            # CompleteInput only invokes native completers for a resolvable executable.
            shutil.copy2(sys.executable, os.path.join(bin_dir, "cpl.exe" if os.name == "nt" else "cpl"))
            env = os.environ.copy()
            env["PATH"] = bin_dir + os.pathsep + env.get("PATH", "")
            for line, cursor, required, absent in (("cpl g", 5, "get", "--where"),
                                                   ("cpl get --w", 11, "--where", "--data"),
                                                   ("cpl get --da", 12, "--datasource", "--data"),
                                                   ("cpl health --u", 14, "--url", "--where"),
                                                   ("cpl start --output j", 20, "json", "--config"),
                                                   ("cpl get --w --output json", 11, "--where", "--data"),
                                                   ("cpl start --output j --config node.json", 20, "json", "--config")):
                script = (". '%s'; [System.Management.Automation.CommandCompletion]::"
                          "CompleteInput('%s', %d, $null).CompletionMatches | "
                          "ForEach-Object { $_.CompletionText }") % (
                              str(path).replace("'", "''"), line, cursor)
                result = subprocess.run([shell, "-NoProfile", "-NonInteractive", "-Command", script],
                                        check=True, text=True, capture_output=True, env=env)
                self.assertEqual(result.stderr, "")
                self.assertIn(required, result.stdout.splitlines())
                self.assertNotIn(absent, result.stdout.splitlines())


if __name__ == "__main__":
    unittest.main()
