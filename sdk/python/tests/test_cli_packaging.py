import os
import subprocess
import sys
import tempfile
import unittest


class TestCliPackaging(unittest.TestCase):
    def test_pyproject_declares_cpl_entrypoint_and_package_root(self):
        root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
        pyproject = os.path.join(root, "pyproject.toml")
        self.assertTrue(os.path.isfile(pyproject))
        with open(pyproject, "r", encoding="utf-8") as handle:
            text = handle.read()
        self.assertIn('cpl = "cpppools.cli:main"', text)
        self.assertIn('where = ["sdk/python"]', text)

    def test_installed_console_script_runs_outside_source_tree(self):
        root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
        with tempfile.TemporaryDirectory(prefix="cpppools_venv_") as env_dir:
            subprocess.run([sys.executable, "-m", "venv", env_dir], check=True,
                           stdout=subprocess.DEVNULL)
            python = os.path.join(env_dir, "bin", "python")
            if os.name == "nt":
                python = os.path.join(env_dir, "Scripts", "python.exe")
            subprocess.run([python, "-m", "pip", "install", "--no-deps", root],
                           check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           text=True)
            result = subprocess.run([python, "-m", "cpppools", "--version"],
                                    cwd=tempfile.gettempdir(), check=False,
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("0+dev", result.stdout)
            cpl = os.path.join(os.path.dirname(python), "cpl")
            if os.name == "nt":
                # ``python`` already points into the venv's Scripts directory.
                # Adding another Scripts component breaks Windows console-script
                # smoke outside the source tree.
                cpl = os.path.join(os.path.dirname(python), "cpl.exe")
            script = subprocess.run([cpl, "--version"], cwd=tempfile.gettempdir(),
                                    check=False, text=True, capture_output=True)
            self.assertEqual(script.returncode, 0, script.stderr)
            self.assertIn("0+dev", script.stdout)


if __name__ == "__main__":
    unittest.main()
