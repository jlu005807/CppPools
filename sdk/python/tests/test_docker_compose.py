"""Docker Compose deployment checks for the remote SDK/CLI workflow."""

import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.request
import uuid
from unittest import mock


ROOT = Path(__file__).resolve().parents[3]
COMPOSE = ROOT / "docker-compose.yml"


class TestDockerComposeDefinition(unittest.TestCase):
    def test_definition_keeps_deployment_and_remote_operations_separate(self):
        compose = COMPOSE.read_text(encoding="utf-8")
        dockerfile = (ROOT / "docker/Dockerfile").read_text(encoding="utf-8")
        config = json.loads((ROOT / "docker/cpppools.compose.json").read_text(encoding="utf-8"))
        self.assertIn("stop_grace_period: 15s", compose)
        self.assertIn("cpppools-data:/var/lib/cpppools", compose)
        self.assertIn("healthcheck:", compose)
        self.assertIn("USER cpppools", dockerfile)
        self.assertIn('ENTRYPOINT ["/usr/local/bin/cpppools_server"]', dockerfile)
        self.assertIn('CMD ["--config", "/etc/cpppools/config.json"]', dockerfile)
        self.assertEqual(config["server"]["port"], 8080)
        self.assertEqual(config["datasources"][0]["sqlite"]["path"],
                         "/var/lib/cpppools/cpppools.db")

    def test_build_context_excludes_local_artifacts(self):
        # The build context is the repository root, so docker/.dockerignore is ignored.
        patterns = (ROOT / ".dockerignore").read_text(encoding="utf-8").splitlines()
        for pattern in (".git", "build*/", "_tmp/", "output/", "**/*.egg-info/"):
            self.assertIn(pattern, patterns)

    def test_default_published_port_is_local_and_overridable(self):
        compose = COMPOSE.read_text(encoding="utf-8")
        self.assertIn('"127.0.0.1:${CPPPOOLS_PORT:-8080}:8080"', compose)


class TestDockerComposeIsolation(unittest.TestCase):
    def test_acceptance_cleanup_targets_only_its_unique_project(self):
        completed = subprocess.CompletedProcess([], 0, stdout="", stderr="")
        with mock.patch.object(shutil, "which", return_value="docker"), \
                mock.patch.object(subprocess, "run", return_value=completed) as run:
            TestDockerComposeLive.setUpClass()
            first_command = list(TestDockerComposeLive.compose_command)
            environment = TestDockerComposeLive.environment
            try:
                with open(environment["CPPPOOLS_CONFIG_FILE"], encoding="utf-8") as handle:
                    advertised = json.load(handle)["cluster"]["nodes"][0]
                self.assertEqual(advertised["port"], int(environment["CPPPOOLS_PORT"]))
                self.assertGreater(int(environment["CPPPOOLS_PORT"]), 0)
                self.assertIn("--project-name", first_command)
            finally:
                TestDockerComposeLive.doClassCleanups()
            cleanup = run.call_args
            self.assertEqual(cleanup.args[0][:-3], first_command)
            self.assertEqual(cleanup.args[0][-3:], ["down", "-v", "--remove-orphans"])
            TestDockerComposeLive.setUpClass()
            try:
                self.assertNotEqual(first_command, TestDockerComposeLive.compose_command)
            finally:
                TestDockerComposeLive.doClassCleanups()


@unittest.skipUnless(os.environ.get("CPL_RUN_DOCKER") == "1",
                     "set CPL_RUN_DOCKER=1 for a real Compose build and CRUD smoke")
class TestDockerComposeLive(unittest.TestCase):
    compose_command = None

    @classmethod
    def setUpClass(cls):
        if shutil.which("docker") is None:
            raise RuntimeError("CPL_RUN_DOCKER=1 requires docker on PATH")
        result = subprocess.run(["docker", "compose", "version"], cwd=ROOT,
                                capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError("Docker Compose is unavailable: " + result.stderr.strip())
        cls.compose_command = ["docker", "compose", "--project-name",
                               "cpppools-test-" + uuid.uuid4().hex[:12],
                               "-f", str(COMPOSE)]
        temporary = tempfile.TemporaryDirectory(prefix="cpppools_compose_")
        cls.addClassCleanup(temporary.cleanup)
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        config = json.loads((ROOT / "docker/cpppools.compose.json").read_text(encoding="utf-8"))
        config["cluster"]["nodes"][0]["port"] = port
        config_path = Path(temporary.name) / "config.json"
        config_path.write_text(json.dumps(config), encoding="utf-8")
        cls.environment = dict(os.environ, CPPPOOLS_PORT=str(port),
                               CPPPOOLS_CONFIG_FILE=str(config_path))
        # Register before up/build: even a partially failed build/start is cleaned.
        cls.addClassCleanup(cls.cleanup_compose, list(cls.compose_command),
                            dict(cls.environment))

    @classmethod
    def cleanup_compose(cls, command, environment):
        result = subprocess.run(command + ["down", "-v", "--remove-orphans"],
                                cwd=ROOT, env=environment, capture_output=True,
                                text=True, timeout=60)
        if result.returncode != 0:
            raise RuntimeError("Could not clean test Compose project: " + result.stderr)

    def run_compose(self, *args):
        result = subprocess.run(self.compose_command + list(args), cwd=ROOT,
                                env=self.environment, capture_output=True, text=True,
                                timeout=600)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def test_real_compose_health_and_sdk_cli_crud(self):
        from cpppools.client import ClusterClient

        self.run_compose("config", "--quiet")
        self.run_compose("up", "-d", "--build", "--wait", "--wait-timeout", "60")
        endpoint = self.endpoint()
        self.assertEqual(self.run_compose("exec", "-T", "cpppools", "id", "-u")
                         .stdout.strip(), "10001")
        client = ClusterClient([endpoint])
        where = {"column": "id", "value": 1}
        self.assertEqual(client.insert("users", {"id": 1, "name": "sdk"})["affected_rows"], 1)
        self.assertEqual(client.select("users", where, columns=["name"])["rows"], [["sdk"]])
        self.assertEqual(client.update("users", {"name": "persisted"}, where)["affected_rows"], 1)

        # Compose stop must let the actual Linux process finish on SIGTERM.
        self.run_compose("stop")
        container = self.run_compose("ps", "-a", "-q", "cpppools").stdout.strip()
        inspected = subprocess.run(["docker", "inspect", container], cwd=ROOT,
                                   capture_output=True, text=True, timeout=30)
        self.assertEqual(inspected.returncode, 0, inspected.stderr)
        state = json.loads(inspected.stdout)[0]["State"]
        self.assertEqual(state["Status"], "exited")
        self.assertEqual(state["ExitCode"], 0)
        self.assertFalse(state["OOMKilled"])
        # Recreate the container, retaining its project volume; seed must not overwrite it.
        self.run_compose("down")
        self.run_compose("up", "-d", "--wait", "--wait-timeout", "60")
        endpoint = self.endpoint()
        client = ClusterClient([endpoint])
        self.assertEqual(client.select("users", where, columns=["name"])["rows"],
                         [["persisted"]])
        self.assertEqual(client.delete("users", where)["affected_rows"], 1)
        self.assertEqual(client.select("users", where)["rows"], [])

        environment = dict(os.environ, PYTHONPATH=str(ROOT / "sdk/python"))

        def cli(*args):
            result = subprocess.run([sys.executable, "-m", "cpppools", *args,
                                     "-u", endpoint, "-o", "json"], cwd=ROOT,
                                    env=environment, capture_output=True, text=True,
                                    timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(result.stdout)

        self.assertTrue(cli("health")["results"][0]["ok"])
        self.assertEqual(cli("insert", "users", "-D", '{"id":2,"name":"cli"}')
                         ["affected_rows"], 1)
        clause = '{"column":"id","value":2}'
        self.assertEqual(cli("select", "users", "-w", clause, "-C", "name")["rows"],
                         [["cli"]])
        self.assertEqual(cli("update", "users", "-w", clause, "-D", '{"name":"updated"}')
                         ["affected_rows"], 1)
        self.assertEqual(cli("select", "users", "-w", clause, "-C", "name")["rows"],
                         [["updated"]])
        self.assertEqual(cli("delete", "users", "-w", clause)["affected_rows"], 1)
        self.assertEqual(cli("select", "users", "-w", clause)["rows"], [])

    def endpoint(self):
        binding = self.run_compose("port", "cpppools", "8080").stdout.strip()
        endpoint = "http://" + binding
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            try:
                with urllib.request.urlopen(endpoint + "/health", timeout=2) as response:
                    if response.status == 200:
                        with urllib.request.urlopen(endpoint + "/api/v1/cluster", timeout=2) as topology:
                            nodes = json.load(topology)["nodes"]
                        self.assertEqual(len(nodes), 1)
                        self.assertEqual("http://%s:%d" % (nodes[0]["host"], nodes[0]["port"]),
                                         endpoint, "refusing CRUD on a mismatched advertised address")
                        return endpoint
            except OSError:
                time.sleep(1)
        self.fail("Compose published port did not become healthy: " + endpoint)
