import os
import json
import pathlib
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile
import unittest
import time
import urllib.request
import zipfile


def copy_source(destination):
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
    os.makedirs(destination)
    for name in ("pyproject.toml", "README.md"):
        shutil.copy2(os.path.join(root, name), destination)
    shutil.copytree(os.path.join(root, "sdk", "python"),
                    os.path.join(destination, "sdk", "python"),
                    ignore=shutil.ignore_patterns("__pycache__", "*.egg-info"))
    return destination


def build_tool_command(python):
    args = [str(python), "-m", "pip", "install", "--upgrade"]
    if os.environ.get("CPL_PACKAGING_WHEELHOUSE"):
        args += ["--no-index", "--find-links", os.environ["CPL_PACKAGING_WHEELHOUSE"]]
    return args + ["pip==24.3.1", "setuptools==68.2.2", "wheel==0.45.1"]


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
        with tempfile.TemporaryDirectory(prefix="cpppools_venv_") as env_dir:
            subprocess.run([sys.executable, "-m", "venv", env_dir], check=True,
                           stdout=subprocess.DEVNULL)
            python = os.path.join(env_dir, "bin", "python")
            if os.name == "nt":
                python = os.path.join(env_dir, "Scripts", "python.exe")
            source = copy_source(os.path.join(env_dir, "source"))
            isolation = []
            if os.environ.get("CPL_PACKAGING_WHEELHOUSE"):
                subprocess.run(build_tool_command(python), check=True,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                isolation = ["--no-build-isolation"]
            subprocess.run([python, "-m", "pip", "install", "--no-deps"] + isolation + [source],
                           check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           text=True)
            result = subprocess.run([python, "-m", "cpppools", "--version"],
                                    cwd=tempfile.gettempdir(), check=False,
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            metadata_version = subprocess.run(
                [python, "-c", "from importlib.metadata import version; print(version('cpppools'))"],
                cwd=tempfile.gettempdir(), check=True, text=True, capture_output=True)
            self.assertEqual(result.stdout.strip(), metadata_version.stdout.strip())
            cpl = os.path.join(os.path.dirname(python), "cpl")
            if os.name == "nt":
                # ``python`` already points into the venv's Scripts directory.
                # Adding another Scripts component breaks Windows console-script
                # smoke outside the source tree.
                cpl = os.path.join(os.path.dirname(python), "cpl.exe")
            script = subprocess.run([cpl, "--version"], cwd=tempfile.gettempdir(),
                                    check=False, text=True, capture_output=True)
            self.assertEqual(script.returncode, 0, script.stderr)
            self.assertEqual(script.stdout.strip(), metadata_version.stdout.strip())
            installed_files = subprocess.run(
                [python, "-c", "from importlib.metadata import distribution; "
                 "print('\\n'.join(str(p) for p in distribution('cpppools').files))"],
                cwd=tempfile.gettempdir(), check=True, text=True, capture_output=True)
            self.assertFalse(any(name.startswith(("tests/", "examples/"))
                                 for name in installed_files.stdout.splitlines()),
                             "distribution must contain only the SDK and its resources")


@unittest.skipUnless(os.environ.get("CPL_RUN_PACKAGING") == "1",
                     "set CPL_RUN_PACKAGING=1 for independent distribution acceptance")
class TestDistributionAcceptance(unittest.TestCase):
    def run_command(self, args, cwd, env):
        result = subprocess.run(args, cwd=cwd, env=env, capture_output=True,
                                text=True, timeout=180)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_wheel_sdist_rebuild_and_install_matrix(self):
        builder = os.environ.get("CPL_BUILD_PYTHON", sys.executable)
        runtimes = [sys.executable]
        if os.environ.get("CPL_TEST_PYTHON38"):
            runtimes.append(os.environ["CPL_TEST_PYTHON38"])
        env = os.environ.copy()
        env.pop("PYTHONPATH", None)
        with tempfile.TemporaryDirectory(prefix="cpppools_distribution_") as directory:
            work = pathlib.Path(directory)
            source = pathlib.Path(copy_source(str(work / "source")))
            output = os.environ.get("CPL_PACKAGE_OUTPUT")
            artifacts = ((pathlib.Path(output).resolve() / work.name) if output else work / "artifacts")
            self.run_command([builder, "-m", "build", "--no-isolation", "--wheel",
                              "--outdir", str(artifacts), str(source)], directory, env)
            self.run_command([builder, "-m", "build", "--no-isolation", "--sdist",
                              "--outdir", str(artifacts), str(source)], directory, env)
            wheel = next(artifacts.glob("*.whl"))
            sdist = next(artifacts.glob("*.tar.gz"))
            unpacked = work / "unpacked"
            extraction = {"filter": "data"} if sys.version_info >= (3, 12) else {}
            shutil.unpack_archive(str(sdist), str(unpacked), **extraction)
            rebuilt_dir = artifacts / "rebuilt"
            self.run_command([builder, "-m", "build", "--no-isolation", "--wheel",
                              "--outdir", str(rebuilt_dir), str(next(unpacked.iterdir()))], directory, env)
            rebuilt = next(rebuilt_dir.glob("*.whl"))
            print("distribution artifacts:", artifacts, flush=True)
            for archive in (wheel, rebuilt):
                with zipfile.ZipFile(archive) as package:
                    names = package.namelist()
                    self.assertFalse(any(name.startswith(("tests/", "examples/")) for name in names))
                    for name in ("cpl.bash", "_cpl", "cpl.ps1"):
                        self.assertIn("cpppools/completions/" + name, names)

            # This fixture uses the server directly: installation checks must not
            # import SDK test helpers that insert the source tree into sys.path.
            server = None
            url = None
            if os.environ.get("CPL_TEST_SERVER"):
                database = str(work / "live.sqlite")
                connection = sqlite3.connect(database)
                try:
                    connection.execute("CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL)")
                    connection.commit()
                finally:
                    connection.close()
                with socket.socket() as listener:
                    listener.bind(("127.0.0.1", 0))
                    port = listener.getsockname()[1]
                config = work / "server.json"
                config.write_text(json.dumps({
                    "server": {"port": port, "worker_threads": 1},
                    "datasources": [{"name": "main", "type": "sqlite", "default": True,
                                     "pool_size": 1, "sqlite": {"path": database}}],
                }), encoding="utf-8")
                server = subprocess.Popen([os.environ["CPL_TEST_SERVER"], "--config", str(config)],
                                          env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                url = "http://127.0.0.1:%d" % port
            try:
                if server is not None:
                    deadline = time.monotonic() + 15
                    while True:
                        self.assertIsNone(server.poll(), "server exited before readiness")
                        try:
                            with urllib.request.urlopen(url + "/health", timeout=0.5) as response:
                                self.assertEqual(response.status, 200)
                            break
                        except OSError:
                            self.assertLess(time.monotonic(), deadline, "server readiness timeout")
                            time.sleep(0.05)
                for runtime_index, runtime in enumerate(runtimes):
                    for artifact_index, artifact in enumerate((wheel, sdist, rebuilt)):
                        with self.subTest(runtime=runtime, artifact=artifact.name):
                            venv = work / ("env_%d_%d" % (runtime_index, artifact_index))
                            self.run_command([runtime, "-m", "venv", str(venv)], directory, env)
                            python = venv / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
                            cpl = python.parent / ("cpl.exe" if os.name == "nt" else "cpl")
                            self.run_command(build_tool_command(python), directory, env)
                            self.run_command([str(python), "-m", "pip", "install", "--no-deps", "--no-build-isolation",
                                              str(artifact)], directory, env)
                            info = json.loads(self.run_command([str(python), "-c",
                                "import json,sys,cpppools; from importlib.metadata import version; "
                                "print(json.dumps([cpppools.__file__,version('cpppools'),sys.version.split()[0]]))"],
                                directory, env))
                            self.assertEqual(os.path.commonpath([str(venv), info[0]]), str(venv))
                            for entrypoint in ([str(python), "-m", "cpppools"], [str(cpl)]):
                                self.assertEqual(self.run_command(entrypoint + ["--version"], directory, env).strip(), info[1])
                                self.assertIn("COMMAND", self.run_command(entrypoint + ["--help"], directory, env))
                            self.run_command([str(python), "-W", "error", "-c",
                                "import importlib.resources as r; import cpppools.completions as c; "
                                "from pathlib import Path; "
                                "p=r.files(c) if hasattr(r,'files') else Path(c.__file__).parent; "
                                "assert all((p/n).is_file() for n in ('cpl.bash','_cpl','cpl.ps1'))"], directory, env)
                            if url:
                                common = ["-u", url, "-o", "json"]
                                where = '{"column":"id","value":41}'
                                expected_name = "alice"
                                for args in (["insert", "users", "-D", '{"id":41,"name":"alice"}'],
                                             ["select", "users", "-w", where],
                                             ["update", "users", "-w", where, "-D", '{"name":"bob"}'],
                                             ["select", "users", "-w", where],
                                             ["delete", "users", "-w", where]):
                                    result = json.loads(self.run_command([str(cpl)] + args + common, directory, env))
                                    if args[0] == "select":
                                        self.assertEqual(result["rows"], [[41, expected_name]])
                                    else:
                                        self.assertEqual(result["affected_rows"], 1)
                                    if args[0] == "update":
                                        expected_name = "bob"
                            print("distribution acceptance:", info[2], artifact.name, "version=" + info[1],
                                  "live_crud=" + str(bool(url)), flush=True)
            finally:
                if server is not None and server.poll() is None:
                    server.terminate()
                    try:
                        server.wait(timeout=8)
                    except subprocess.TimeoutExpired:
                        server.kill()
                        server.wait(timeout=3)


if __name__ == "__main__":
    unittest.main()
