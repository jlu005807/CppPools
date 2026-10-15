import hashlib
import json
import http.client
import os
import signal
import shutil
import contextlib
import tempfile
import time
import unittest
import socket
import sqlite3
import stat
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from unittest import mock

from cpppools import local_node


class TestExitTransition(unittest.TestCase):
    def test_exit_evidence_maps_force_and_graceful_proof_to_stable_outcomes(self):
        cases = [
            ({}, False, 137,
             ({"state": "crashed", "exit_kind": "unexpected",
               "last_exit_code": 137}, 10)),
            ({}, True, None,
             ({"state": "stopped", "exit_kind": "graceful",
               "last_exit_code": 0}, 0)),
            ({}, True, 1,
             ({"state": "crashed", "exit_kind": "unexpected",
               "last_exit_code": 1}, 10)),
            ({"force_delivery": "pending"}, True, 137,
             ({"state": "crashed", "exit_kind": None,
               "last_exit_code": 137}, 10)),
            ({"force_delivery": "pending"}, False, None,
             ({"state": "crashed", "exit_kind": None,
               "last_exit_code": None}, 10)),
            ({"force_delivery": "accepted"}, True, 137,
             ({"state": "crashed", "exit_kind": "forced",
               "last_exit_code": 137}, 9)),
            ({"force_delivery": "rejected"}, True, None,
             ({"state": "stopped", "exit_kind": "graceful",
               "last_exit_code": 0}, 0)),
        ]
        for state, proof, exit_code, expected in cases:
            with self.subTest(state=state, proof=proof, exit_code=exit_code):
                self.assertEqual(
                    local_node._exit_transition(state, proof, exit_code), expected)


class TestVerifiedProcessSignal(unittest.TestCase):
    def test_pidfd_rechecks_identity_after_open_before_signalling(self):
        pidfd = os.open(os.devnull, os.O_RDONLY)
        pidfd_open = mock.Mock(return_value=pidfd)
        pidfd_send = mock.Mock()
        state = {"pid": 43210, "process_start_time": "old-start",
                 "binary": "/bin/false"}
        try:
            with mock.patch.object(local_node, "_pidfd_functions",
                                   return_value=(pidfd_open, pidfd_send), create=True), \
                    mock.patch.object(local_node, "_identity_matches",
                                      return_value=False), \
                    mock.patch.object(local_node.os, "kill") as numeric_kill:
                with self.assertRaises(local_node.IdentityError):
                    local_node._send_verified_signal(state, signal.SIGTERM)

            pidfd_open.assert_called_once_with(state["pid"], 0)
            pidfd_send.assert_not_called()
            numeric_kill.assert_not_called()
            with self.assertRaises(OSError):
                os.fstat(pidfd)
        finally:
            with contextlib.suppress(OSError):
                os.close(pidfd)

    def test_verified_pidfd_is_used_instead_of_numeric_pid(self):
        pidfd = os.open(os.devnull, os.O_RDONLY)
        pidfd_open = mock.Mock(return_value=pidfd)
        pidfd_send = mock.Mock()
        state = {"pid": 43210, "process_start_time": "start",
                 "binary": "/bin/false"}
        try:
            with mock.patch.object(local_node, "_pidfd_functions",
                                   return_value=(pidfd_open, pidfd_send), create=True), \
                    mock.patch.object(local_node, "_identity_matches", return_value=True), \
                    mock.patch.object(local_node.os, "kill") as numeric_kill:
                local_node._send_verified_signal(state, signal.SIGTERM)

            pidfd_send.assert_called_once_with(pidfd, signal.SIGTERM, None, 0)
            numeric_kill.assert_not_called()
            with self.assertRaises(OSError):
                os.fstat(pidfd)
        finally:
            with contextlib.suppress(OSError):
                os.close(pidfd)

    def test_numeric_fallback_rechecks_identity_before_signalling(self):
        state = {"pid": 43210, "process_start_time": "start",
                 "binary": "/bin/false"}
        with mock.patch.object(local_node, "_pidfd_functions",
                               return_value=(None, None), create=True), \
                mock.patch.object(local_node, "_identity_matches", return_value=True), \
                mock.patch.object(local_node.os, "kill") as numeric_kill:
            local_node._send_verified_signal(state, signal.SIGTERM)

        numeric_kill.assert_called_once_with(state["pid"], signal.SIGTERM)


def _write_fake_handshake(command, pid, port=8080):
    launch_dir = command[command.index("--control-dir") + 1]
    with open(os.path.join(launch_dir, "control.meta"), encoding="utf-8") as handle:
        meta = json.load(handle)
    identity = {
        "protocol_version": 1,
        "launch_id": meta["launch_id"],
        "pid": pid,
        "process_start_time": "fake-start",
    }
    local_node._atomic_json(os.path.join(launch_dir, "starting.json"),
                            dict(identity, state="starting"))
    local_node._atomic_json(os.path.join(launch_dir, "ready.json"),
                            dict(identity, state="ready", port=port, node_id="single"))


class TestLocalNodeState(unittest.TestCase):
    def setUp(self):
        self.pidfd_patcher = mock.patch.object(
            local_node, "_pidfd_functions", return_value=(None, None), create=True)
        self.pidfd_patcher.start()
        self.addCleanup(self.pidfd_patcher.stop)

    def test_config_digest_uses_original_file_bytes(self):
        path = None
        try:
            # Windows does not allow reopening a NamedTemporaryFile while its
            # handle is open.  Close it before exercising the production read.
            with tempfile.NamedTemporaryFile(delete=False) as handle:
                path = handle.name
                handle.write(b'{ "server" : { "port" : 8080 } }\\n')
                handle.flush()
            expected = hashlib.sha256(b'{ "server" : { "port" : 8080 } }\\n').hexdigest()
            self.assertEqual(local_node._config_sha256(path), expected)
            self.assertNotEqual(expected, hashlib.sha256(
                b'{"server":{"port":8080}}').hexdigest())
        finally:
            if path is not None:
                with contextlib.suppress(OSError):
                    os.unlink(path)

    def test_missing_optional_ready_marker_preserves_starting_handshake(self):
        with tempfile.TemporaryDirectory() as root:
            control_dir = os.path.join(root, "launches", "launch-1")
            local_node._ensure_private_control_dir(control_dir)
            identity = {
                "protocol_version": 1,
                "launch_id": "launch-1",
                "pid": 43210,
                "process_start_time": "process-start",
            }
            local_node._atomic_json(os.path.join(control_dir, "control.meta"), {
                "protocol_version": 1, "launch_id": "launch-1",
            })
            local_node._atomic_json(os.path.join(control_dir, "starting.json"),
                                    dict(identity, state="starting"))
            state = dict(identity, control_dir=control_dir, binary="/bin/false",
                         endpoint="http://127.0.0.1:8080")

            with mock.patch.object(local_node, "_identity_matches", return_value=True):
                self.assertIsNone(local_node._read_control_json(
                    control_dir, "ready.json", missing_ok=True))
                self.assertEqual(local_node._verified_handshake(state), (True, None))
                self.assertEqual(local_node._verified_handshake(
                    state, require_ready=True), (False, None))

    def test_deadline_exit_is_reconciled_before_reporting_stop_timeout(self):
        with tempfile.TemporaryDirectory() as root:
            manager = local_node.LocalNodeManager(root, "demo")
            control_dir = os.path.join(
                manager.instance_dir, "launches", "launch-deadline")
            os.makedirs(control_dir)
            manager.write_state({
                "protocol_version": 1, "state_revision": 1, "name": "demo",
                "launch_id": "launch-deadline", "pid": 43211,
                "process_start_time": "process-start", "binary": "/bin/false",
                "control_dir": control_dir, "state": "running",
                "exit_kind": None, "last_exit_code": None, "updated_at": "now",
            })
            local_node._atomic_json(os.path.join(control_dir, "stopped.json"), {
                "protocol_version": 1, "launch_id": "launch-deadline",
                "pid": 43211, "process_start_time": "process-start",
                "state": "stopped", "exit_kind": "graceful", "exit_code": 0,
            })

            alive = {"value": True}

            def sleep_until_exit(_duration):
                alive["value"] = False

            with mock.patch.object(local_node, "_pid_alive",
                                  side_effect=lambda _pid: alive["value"]), \
                    mock.patch.object(local_node, "_identity_matches", return_value=True), \
                    mock.patch.object(local_node, "_write_stop_request", return_value=True), \
                    mock.patch.object(local_node, "_reap_child", return_value=None), \
                    mock.patch.object(local_node.time, "monotonic",
                                      side_effect=[0.0, 0.0, 0.11]), \
                    mock.patch.object(local_node.time, "sleep",
                                      side_effect=sleep_until_exit):
                result = manager.stop(timeout=0.1)

            self.assertEqual(result["state"], "stopped")
            self.assertEqual(result["exit_kind"], "graceful")
            self.assertFalse(result["process_alive"])

    def test_startup_cleanup_reaps_owned_process_when_state_is_unreadable(self):
        with tempfile.TemporaryDirectory() as root:
            manager = local_node.LocalNodeManager(root, "demo")

            class FakeProcess:
                pid = 43212

                def __init__(self):
                    self.returncode = None
                    self.signals = []

                def poll(self):
                    return self.returncode

                def send_signal(self, sig):
                    self.signals.append(sig)
                    self.returncode = -sig

                def wait(self, timeout=None):
                    if self.returncode is None:
                        raise subprocess.TimeoutExpired("fake-server", timeout)
                    return self.returncode

                def kill(self):
                    self.returncode = -signal.SIGKILL

            process = FakeProcess()
            with mock.patch.object(
                    manager, "read_state",
                    side_effect=local_node.LocalStateError("state is unreadable")):
                reaped, exit_code = manager._terminate_and_reap(process)

            self.assertTrue(reaped)
            self.assertEqual(exit_code, -signal.SIGTERM)
            self.assertEqual(process.signals, [signal.SIGTERM])

    def test_failed_start_cleanup_removes_only_reaped_launch_secrets_without_state(self):
        with tempfile.TemporaryDirectory() as root:
            manager = local_node.LocalNodeManager(root, "demo")
            launch_id = "launch-reaped"
            other_launch_id = "launch-other"
            control_dirs = {}
            for current_id in (launch_id, other_launch_id):
                control_dir = os.path.join(
                    manager.instance_dir, "launches", current_id)
                local_node._ensure_private_control_dir(control_dir)
                control_dirs[current_id] = control_dir
                for filename in ("control.token", "stop.request", "starting.json"):
                    with open(os.path.join(control_dir, filename), "wb") as handle:
                        handle.write(b"test")

            class ExitedProcess:
                pid = 43213

                def poll(self):
                    return 0

            with mock.patch.object(
                    manager, "read_state",
                    side_effect=local_node.LocalStateError("state is unreadable")):
                manager._cleanup_failed_start(
                    launch_id, ExitedProcess(), state_written=True)

            for filename in ("control.token", "stop.request"):
                self.assertFalse(os.path.exists(os.path.join(
                    control_dirs[launch_id], filename)))
                self.assertTrue(os.path.exists(os.path.join(
                    control_dirs[other_launch_id], filename)))
            self.assertTrue(os.path.exists(os.path.join(
                control_dirs[launch_id], "starting.json")))

    def test_control_directory_is_secured_and_rejects_symlink(self):
        with tempfile.TemporaryDirectory() as root:
            control_dir = os.path.join(root, "control")
            os.mkdir(control_dir, 0o777)
            if os.name == "posix":
                os.chmod(control_dir, 0o777)
            local_node._ensure_private_control_dir(control_dir)
            if os.name == "posix":
                self.assertEqual(stat.S_IMODE(os.stat(control_dir).st_mode), 0o700)

            if os.name == "posix":
                target = os.path.join(root, "target")
                os.mkdir(target)
                link = os.path.join(root, "control-link")
                os.symlink(target, link)
                with self.assertRaises(local_node.LocalStateError):
                    local_node._ensure_private_control_dir(link)

    def test_windows_local_start_remains_explicitly_unsupported(self):
        manager = local_node.LocalNodeManager(tempfile.gettempdir(), "unsupported")
        with mock.patch.object(local_node.os, "name", "nt"):
            with self.assertRaisesRegex(local_node.LocalNodeError,
                                        "Windows local start is not supported"):
                manager.start("config-does-not-need-to-exist.json")

    @unittest.skipUnless(sys.platform == "linux", "local lifecycle requires Linux /proc")
    def test_concurrent_start_does_not_spawn_twice_before_pid_registration(self):
        with tempfile.TemporaryDirectory() as root:
            config = os.path.join(root, "server.json")
            with open(config, "w", encoding="utf-8") as handle:
                json.dump({"server": {"port": 8080}}, handle)
            binary = os.path.join(root, "cpppools_server")
            with open(binary, "w", encoding="ascii") as handle:
                handle.write("#!/bin/sh\n")
            os.chmod(binary, 0o755)

            first_manager = local_node.LocalNodeManager(root, "demo")
            second_manager = local_node.LocalNodeManager(root, "demo")
            first_spawn_entered = threading.Event()
            release_first_spawn = threading.Event()
            spawn_lock = threading.Lock()
            spawned_launches = []
            first_result = []
            first_errors = []

            class FakeProcess:
                def __init__(self, pid):
                    self.pid = pid
                    self.returncode = None

                def poll(self):
                    return None

            def fake_popen(command, **_kwargs):
                launch_dir = command[command.index("--control-dir") + 1]
                with spawn_lock:
                    spawned_launches.append(launch_dir)
                    spawn_number = len(spawned_launches)
                if spawn_number == 1:
                    first_spawn_entered.set()
                    if not release_first_spawn.wait(5):
                        raise AssertionError("test did not release the first spawn")
                pid = 42000 + spawn_number
                _write_fake_handshake(command, pid)
                return FakeProcess(pid)

            def first_start():
                try:
                    first_result.append(first_manager.start(config, binary=binary, timeout=1))
                except BaseException as exc:  # preserve worker-thread failures for the assertion
                    first_errors.append(exc)

            first_thread = threading.Thread(target=first_start)
            with mock.patch.object(subprocess, "Popen", side_effect=fake_popen), \
                    mock.patch.object(local_node, "_pid_alive", side_effect=lambda pid: pid is not None), \
                    mock.patch.object(local_node, "_identity_matches", return_value=True), \
                    mock.patch.object(local_node, "_process_start_time", return_value="fake-start"):
                first_manager._health = lambda *_args: True
                second_manager._health = lambda *_args: True
                first_thread.start()
                try:
                    self.assertTrue(first_spawn_entered.wait(5), "first start did not reach Popen")
                    with self.assertRaises(local_node.LocalStateError):
                        second_manager.start(config, binary=binary, timeout=1)
                finally:
                    release_first_spawn.set()
                    first_thread.join(5)

            self.assertFalse(first_thread.is_alive(), "first start did not finish")
            self.assertEqual(first_errors, [])
            self.assertEqual(len(spawned_launches), 1,
                             "a concurrent start spawned a second server before PID registration")

    @unittest.skipUnless(sys.platform == "linux", "local lifecycle requires Linux /proc")
    def test_unregistered_starting_intent_is_not_replaced(self):
        with tempfile.TemporaryDirectory() as root:
            config = os.path.join(root, "server.json")
            with open(config, "w", encoding="utf-8") as handle:
                json.dump({"server": {"port": 8080}}, handle)
            binary = os.path.join(root, "cpppools_server")
            with open(binary, "w", encoding="ascii") as handle:
                handle.write("#!/bin/sh\n")
            os.chmod(binary, 0o755)
            manager = local_node.LocalNodeManager(root, "demo")
            manager.write_state(manager._base_state(
                "unfinished-launch", config, binary, root, "http://127.0.0.1:8080",
                os.path.join(root, "server.log"),
                os.path.join(root, "state", "demo", "launches", "unfinished-launch")))
            popen_calls = []

            with mock.patch.object(subprocess, "Popen", side_effect=popen_calls.append):
                with self.assertRaises(local_node.LocalNodeError) as raised:
                    manager.start(config, binary=binary, timeout=1)

            self.assertEqual(raised.exception.code, 6)
            self.assertEqual(popen_calls, [])
            self.assertEqual(manager.read_state()["launch_id"], "unfinished-launch")

    @unittest.skipUnless(sys.platform == "linux", "local lifecycle requires Linux /proc")
    def test_registered_starting_and_stopping_are_rejected_without_spawn(self):
        with tempfile.TemporaryDirectory() as root:
            config = os.path.join(root, "server.json")
            with open(config, "w", encoding="utf-8") as handle:
                json.dump({"server": {"port": 8080}}, handle)
            binary = os.path.join(root, "cpppools_server")
            with open(binary, "w", encoding="ascii") as handle:
                handle.write("#!/bin/sh\n")
            os.chmod(binary, 0o755)
            manager = local_node.LocalNodeManager(root, "demo")

            for state_name in ("starting", "stopping"):
                with self.subTest(state=state_name):
                    launch_id = "registered-%s" % state_name
                    current = manager._base_state(
                        launch_id, config, binary, root, "http://127.0.0.1:8080",
                        os.path.join(root, "server.log"),
                        os.path.join(manager.instance_dir, "launches", launch_id))
                    current.update(pid=45000, state=state_name, state_revision=2,
                                   process_start_time="fake-start")
                    manager.write_state(current)
                    popen_calls = []

                    with mock.patch.object(subprocess, "Popen",
                                           side_effect=popen_calls.append):
                        with self.assertRaises(local_node.LocalNodeError) as raised:
                            manager.start(config, binary=binary, timeout=1)

                    self.assertEqual(raised.exception.code, 6)
                    self.assertEqual(popen_calls, [])
                    self.assertEqual(manager.read_state(), current)

    @unittest.skipUnless(sys.platform == "linux", "local lifecycle requires Linux /proc")
    def test_running_start_requires_exact_launch_configuration(self):
        with tempfile.TemporaryDirectory() as root:
            config = os.path.join(root, "server.json")
            config_bytes = b'{"server":{"port":8080}}'
            with open(config, "wb") as handle:
                handle.write(config_bytes)
            other_config = os.path.join(root, "other.json")
            with open(other_config, "wb") as handle:
                handle.write(config_bytes)
            binary = os.path.join(root, "cpppools_server")
            other_binary = os.path.join(root, "other_server")
            for path in (binary, other_binary):
                with open(path, "w", encoding="ascii") as handle:
                    handle.write("#!/bin/sh\n")
                os.chmod(path, 0o755)
            other_workdir = os.path.join(root, "other-workdir")
            os.mkdir(other_workdir)

            manager = local_node.LocalNodeManager(os.path.join(root, "state"), "demo")
            launch_id = "existing-running-launch"
            state = manager._base_state(
                launch_id, os.path.realpath(config), os.path.realpath(binary),
                os.path.realpath(root), "http://127.0.0.1:8080",
                os.path.join(manager.instance_dir, "server.log"),
                os.path.join(manager.instance_dir, "launches", launch_id))
            state.update(pid=45001, process_start_time="known-start", state="running",
                         state_revision=3,
                         config_sha256=hashlib.sha256(config_bytes).hexdigest())
            manager.write_state(state)

            mismatches = [
                ("different config path", other_config, binary, root, config_bytes),
                ("different workdir", config, binary, other_workdir, config_bytes),
                ("different binary", config, other_binary, root, config_bytes),
                ("changed config bytes", config, binary, root,
                 b'{"server":{"port":8081}}'),
                ("invalid changed config", config, binary, root, b"not-json"),
            ]
            with mock.patch.object(local_node, "_pid_alive", return_value=True), \
                    mock.patch.object(local_node, "_identity_matches", return_value=True), \
                    mock.patch.object(subprocess, "Popen") as popen:
                repeated = manager.start(config, binary=binary, workdir=root)
                self.assertTrue(repeated["already_running"])
                self.assertEqual((repeated["pid"], repeated["launch_id"]),
                                 (state["pid"], state["launch_id"]))

                with mock.patch.object(local_node, "_config_sha256",
                                       side_effect=[state["config_sha256"], "0" * 64]):
                    with self.assertRaises(local_node.LocalNodeError):
                        manager.start(config, binary=binary, workdir=root)

                for label, requested_config, requested_binary, requested_workdir, contents in mismatches:
                    with self.subTest(mismatch=label):
                        with open(config, "wb") as handle:
                            handle.write(config_bytes)
                        if requested_config == config:
                            with open(config, "wb") as handle:
                                handle.write(contents)
                        manager.write_state(state)
                        with self.assertRaises(local_node.LocalNodeError) as raised:
                            manager.start(requested_config, binary=requested_binary,
                                          workdir=requested_workdir)
                        self.assertEqual(raised.exception.code, 6)
                        current = manager.read_state()
                        self.assertEqual((current["pid"], current["launch_id"]),
                                         (state["pid"], state["launch_id"]))
                popen.assert_not_called()

    @unittest.skipUnless(sys.platform == "linux", "local lifecycle requires Linux /proc")
    def test_running_start_rejects_unknown_identity_and_legacy_config_digest(self):
        with tempfile.TemporaryDirectory() as root:
            config = os.path.join(root, "server.json")
            with open(config, "w", encoding="utf-8") as handle:
                json.dump({"server": {"port": 8080}}, handle)
            binary = os.path.join(root, "cpppools_server")
            with open(binary, "w", encoding="ascii") as handle:
                handle.write("#!/bin/sh\n")
            os.chmod(binary, 0o755)
            manager = local_node.LocalNodeManager(os.path.join(root, "state"), "demo")
            launch_id = "legacy-running-launch"
            state = manager._base_state(
                launch_id, os.path.realpath(config), os.path.realpath(binary),
                os.path.realpath(root), "http://127.0.0.1:8080",
                os.path.join(manager.instance_dir, "server.log"),
                os.path.join(manager.instance_dir, "launches", launch_id))
            state.update(pid=45002, process_start_time="known-start", state="running",
                         state_revision=3)
            manager.write_state(state)

            with mock.patch.object(local_node, "_pid_alive", return_value=True), \
                    mock.patch.object(subprocess, "Popen") as popen:
                with mock.patch.object(local_node, "_identity_matches", return_value=False):
                    with self.assertRaises(local_node.IdentityError) as raised:
                        manager.start(config, binary=binary, workdir=root)
                self.assertEqual(raised.exception.code, 8)
                self.assertEqual(manager.read_state(), state)

                with mock.patch.object(local_node, "_identity_matches", return_value=True):
                    with self.assertRaises(local_node.LocalNodeError) as raised:
                        manager.start(config, binary=binary, workdir=root)
                self.assertEqual(raised.exception.code, 6)
                self.assertEqual(manager.read_state(), state)
                popen.assert_not_called()

    @unittest.skipUnless(sys.platform == "linux", "local lifecycle requires Linux /proc")
    def test_config_change_before_spawn_prevents_process_creation(self):
        with tempfile.TemporaryDirectory() as root:
            config = os.path.join(root, "server.json")
            with open(config, "w", encoding="utf-8") as handle:
                json.dump({"server": {"port": 8080}}, handle)
            binary = os.path.join(root, "cpppools_server")
            with open(binary, "w", encoding="ascii") as handle:
                handle.write("#!/bin/sh\n")
            os.chmod(binary, 0o755)
            manager = local_node.LocalNodeManager(os.path.join(root, "state"), "demo")
            with open(config, "rb") as handle:
                initial_digest = hashlib.sha256(handle.read()).hexdigest()
            digests = [initial_digest, initial_digest, "0" * 64]

            with mock.patch.object(local_node, "_config_sha256", side_effect=digests), \
                    mock.patch.object(subprocess, "Popen") as popen:
                with self.assertRaises(local_node.LocalNodeError) as raised:
                    manager.start(config, binary=binary, workdir=root)
            self.assertEqual(raised.exception.code, 6)
            popen.assert_not_called()
            self.assertEqual(manager.read_state()["state"], "crashed")

    @unittest.skipUnless(sys.platform == "linux", "local lifecycle requires Linux /proc")
    def test_config_change_after_ready_requests_stop_before_running_commit(self):
        with tempfile.TemporaryDirectory() as root:
            config = os.path.join(root, "server.json")
            with open(config, "w", encoding="utf-8") as handle:
                json.dump({"server": {"port": 8080}}, handle)
            binary = os.path.join(root, "cpppools_server")
            with open(binary, "w", encoding="ascii") as handle:
                handle.write("#!/bin/sh\n")
            os.chmod(binary, 0o755)
            manager = local_node.LocalNodeManager(os.path.join(root, "state"), "demo")

            class FakeProcess:
                pid = 45003
                returncode = None

                def poll(self):
                    return self.returncode

                def wait(self, timeout=None):
                    self.returncode = 0
                    return 0

            process = FakeProcess()

            def fake_popen(command, **_kwargs):
                _write_fake_handshake(command, process.pid)
                with open(config, "w", encoding="utf-8") as handle:
                    json.dump({"server": {"port": 8081}}, handle)
                return process

            with mock.patch.object(subprocess, "Popen", side_effect=fake_popen), \
                    mock.patch.object(local_node, "_process_start_time",
                                      return_value="fake-start"), \
                    mock.patch.object(local_node, "_identity_matches", return_value=True), \
                    mock.patch.object(local_node, "_pid_alive", return_value=False), \
                    mock.patch.object(local_node, "_write_stop_request",
                                      wraps=local_node._write_stop_request) as stop_request:
                manager._health = lambda *_args: True
                with self.assertRaises(local_node.LocalNodeError) as raised:
                    manager.start(config, binary=binary, workdir=root)

            self.assertEqual(raised.exception.code, 6)
            stop_request.assert_called_once()
            self.assertEqual(manager.read_state()["state"], "crashed")

    @unittest.skipUnless(sys.platform == "linux", "local lifecycle requires Linux /proc")
    def test_config_change_stop_timeout_preserves_stopping_instance(self):
        with tempfile.TemporaryDirectory() as root:
            config = os.path.join(root, "server.json")
            with open(config, "w", encoding="utf-8") as handle:
                json.dump({"server": {"port": 8080}}, handle)
            binary = os.path.join(root, "cpppools_server")
            with open(binary, "w", encoding="ascii") as handle:
                handle.write("#!/bin/sh\n")
            os.chmod(binary, 0o755)
            manager = local_node.LocalNodeManager(os.path.join(root, "state"), "demo")

            class FakeProcess:
                pid = 45004

                def poll(self):
                    return None

                def wait(self, timeout=None):
                    raise subprocess.TimeoutExpired("fake-server", timeout)

            process = FakeProcess()

            def fake_popen(command, **_kwargs):
                _write_fake_handshake(command, process.pid)
                with open(config, "w", encoding="utf-8") as handle:
                    json.dump({"server": {"port": 8081}}, handle)
                return process

            with mock.patch.object(subprocess, "Popen", side_effect=fake_popen) as popen, \
                    mock.patch.object(local_node, "_process_start_time",
                                      return_value="fake-start"), \
                    mock.patch.object(local_node, "_identity_matches", return_value=True), \
                    mock.patch.object(local_node, "_pid_alive", return_value=True), \
                    mock.patch.object(local_node, "_write_stop_request",
                                      wraps=local_node._write_stop_request) as stop_request:
                manager._health = lambda *_args: True
                with self.assertRaises(local_node.StopTimeout) as raised:
                    manager.start(config, binary=binary, workdir=root)

            self.assertEqual(raised.exception.code, 7)
            stop_request.assert_called_once()
            popen.assert_called_once()
            state = manager.read_state()
            self.assertEqual(state["state"], "stopping")
            self.assertEqual(state["pid"], process.pid)
            self.assertTrue(os.path.isfile(os.path.join(
                state["control_dir"], "stop.request")))

    @unittest.skipUnless(sys.platform == "linux", "local lifecycle requires Linux /proc")
    def test_unexpected_setup_error_clears_starting_intent(self):
        with tempfile.TemporaryDirectory() as root:
            config = os.path.join(root, "server.json")
            with open(config, "w", encoding="utf-8") as handle:
                json.dump({"server": {"port": 8080}}, handle)
            binary = os.path.join(root, "cpppools_server")
            with open(binary, "w", encoding="ascii") as handle:
                handle.write("#!/bin/sh\n")
            os.chmod(binary, 0o755)
            manager = local_node.LocalNodeManager(root, "demo")

            with mock.patch.object(local_node.secrets, "token_hex",
                                   side_effect=RuntimeError("token setup failed")), \
                    mock.patch.object(subprocess, "Popen") as popen:
                with self.assertRaisesRegex(RuntimeError, "token setup failed"):
                    manager.start(config, binary=binary, timeout=1)

            state = manager.read_state()
            self.assertEqual(state["state"], "crashed")
            self.assertEqual(state["state_revision"], 2)
            popen.assert_not_called()

    @unittest.skipUnless(sys.platform == "linux", "local lifecycle requires Linux /proc")
    def test_registration_write_failure_reaps_spawned_process_before_crashed(self):
        with tempfile.TemporaryDirectory() as root:
            config = os.path.join(root, "server.json")
            with open(config, "w", encoding="utf-8") as handle:
                json.dump({"server": {"port": 8080}}, handle)
            binary = os.path.join(root, "cpppools_server")
            with open(binary, "w", encoding="ascii") as handle:
                handle.write("#!/bin/sh\n")
            os.chmod(binary, 0o755)
            manager = local_node.LocalNodeManager(root, "demo")

            class FakeProcess:
                pid = 46001

                def __init__(self):
                    self.returncode = None
                    self.signals = []
                    self.kill_called = False
                    self.wait_calls = 0

                def poll(self):
                    return self.returncode

                def send_signal(self, sig):
                    self.signals.append(sig)

                def wait(self, timeout=None):
                    self.wait_calls += 1
                    if self.returncode is None:
                        raise subprocess.TimeoutExpired("fake-server", timeout)
                    return self.returncode

                def kill(self):
                    self.kill_called = True
                    self.returncode = -signal.SIGKILL

            process = FakeProcess()
            original_write_state = manager.write_state
            write_count = 0

            def fail_pid_registration_once(state):
                nonlocal write_count
                write_count += 1
                if write_count == 2:
                    raise OSError("simulated PID registration failure")
                original_write_state(state)

            def fake_popen(command, **_kwargs):
                _write_fake_handshake(command, process.pid)
                return process

            with mock.patch.object(subprocess, "Popen", side_effect=fake_popen) as popen, \
                    mock.patch.object(local_node, "_process_start_time",
                                      return_value="fake-start"), \
                    mock.patch.object(local_node, "_identity_matches", return_value=True), \
                    mock.patch.object(manager, "write_state",
                                      side_effect=fail_pid_registration_once), \
                    mock.patch.object(
                        local_node, "_send_verified_signal",
                        side_effect=lambda _state, sig: process.kill()
                        if sig == signal.SIGKILL else process.send_signal(sig)):
                with self.assertRaises(local_node.LocalNodeError):
                    manager.start(config, binary=binary, timeout=1)

            self.assertEqual(popen.call_count, 1)
            self.assertEqual(process.signals, [])
            self.assertTrue(process.kill_called,
                            "failed startup must escalate when SIGTERM does not reap the child")
            self.assertEqual(process.returncode, -signal.SIGKILL)
            self.assertGreaterEqual(process.wait_calls, 2)
            state = manager.read_state()
            self.assertEqual(state["state"], "crashed")
            self.assertEqual(state["launch_id"], os.path.basename(state["control_dir"]))
            self.assertEqual(state["pid"], process.pid)
            self.assertEqual(state["last_exit_code"], -signal.SIGKILL)
            self.assertFalse(os.path.exists(os.path.join(
                state["control_dir"], "control.token")))
            self.assertFalse(os.path.exists(os.path.join(
                state["control_dir"], "stop.request")))

    @unittest.skipUnless(sys.platform == "linux", "local lifecycle requires Linux /proc")
    def test_unreaped_startup_child_remains_registered_as_stopping(self):
        with tempfile.TemporaryDirectory() as root:
            config = os.path.join(root, "server.json")
            with open(config, "w", encoding="utf-8") as handle:
                json.dump({"server": {"port": 8080}}, handle)
            binary = os.path.join(root, "cpppools_server")
            with open(binary, "w", encoding="ascii") as handle:
                handle.write("#!/bin/sh\n")
            os.chmod(binary, 0o755)
            manager = local_node.LocalNodeManager(root, "demo")

            class StubbornProcess:
                pid = 46002
                returncode = None

                def __init__(self):
                    self.kill_called = False

                def poll(self):
                    return None

                def send_signal(self, _sig):
                    pass

                def wait(self, timeout=None):
                    raise subprocess.TimeoutExpired("stubborn-server", timeout)

                def kill(self):
                    self.kill_called = True

            process = StubbornProcess()
            original_write_state = manager.write_state
            write_count = 0

            def fail_pid_registration_once(state):
                nonlocal write_count
                write_count += 1
                if write_count == 2:
                    raise OSError("simulated PID registration failure")
                original_write_state(state)

            def fake_popen(command, **_kwargs):
                _write_fake_handshake(command, process.pid)
                return process

            with mock.patch.object(subprocess, "Popen", side_effect=fake_popen), \
                    mock.patch.object(local_node, "_process_start_time",
                                      return_value="fake-start"), \
                    mock.patch.object(local_node, "_identity_matches", return_value=True), \
                    mock.patch.object(manager, "write_state",
                                      side_effect=fail_pid_registration_once), \
                    mock.patch.object(
                        local_node, "_send_verified_signal",
                        side_effect=lambda _state, sig: process.kill()
                        if sig == signal.SIGKILL else process.send_signal(sig)):
                with self.assertRaises(local_node.LocalNodeError):
                    manager.start(config, binary=binary, timeout=1)

            self.assertTrue(process.kill_called,
                            "failed startup should try to reap a handshaken child")
            self.assertIs(manager._process, process)
            state = manager.read_state()
            self.assertEqual(state["state"], "stopping")
            self.assertEqual(state["pid"], process.pid)
            self.assertEqual(state["process_start_time"], "fake-start")
            self.assertIsNone(state["last_exit_code"])

    @unittest.skipUnless(sys.platform == "linux", "local lifecycle requires Linux /proc")
    def test_health_exception_reaps_registered_startup_child(self):
        with tempfile.TemporaryDirectory() as root:
            config = os.path.join(root, "server.json")
            with open(config, "w", encoding="utf-8") as handle:
                json.dump({"server": {"port": 8080}}, handle)
            binary = os.path.join(root, "cpppools_server")
            with open(binary, "w", encoding="ascii") as handle:
                handle.write("#!/bin/sh\n")
            os.chmod(binary, 0o755)
            manager = local_node.LocalNodeManager(root, "demo")

            class FakeProcess:
                pid = 46003

                def __init__(self):
                    self.returncode = None
                    self.signals = []

                def poll(self):
                    return self.returncode

                def send_signal(self, sig):
                    self.signals.append(sig)
                    self.returncode = -sig

                def wait(self, timeout=None):
                    if self.returncode is None:
                        raise subprocess.TimeoutExpired("health-failure-server", timeout)
                    return self.returncode

                def kill(self):
                    self.returncode = -signal.SIGKILL

            process = FakeProcess()
            manager._health = lambda _endpoint, _timeout: True

            def fake_popen(command, **_kwargs):
                _write_fake_handshake(command, process.pid)
                return process

            with mock.patch.object(subprocess, "Popen", side_effect=fake_popen), \
                    mock.patch.object(local_node, "_process_start_time",
                                      return_value="fake-start"), \
                    mock.patch.object(local_node, "_identity_matches", return_value=True), \
                    mock.patch.object(
                        local_node, "_send_verified_signal",
                        side_effect=lambda _state, sig: process.kill()
                        if sig == signal.SIGKILL else process.send_signal(sig)), \
                    mock.patch.object(manager, "_health",
                                      side_effect=RuntimeError("health probe failed")):
                with self.assertRaisesRegex(RuntimeError, "health probe failed"):
                    manager.start(config, binary=binary, timeout=1)

            self.assertEqual(process.signals, [])
            state = manager.read_state()
            self.assertEqual(state["state"], "crashed")
            self.assertEqual(state["pid"], process.pid)
            self.assertEqual(state["last_exit_code"], -signal.SIGKILL)

    @unittest.skipUnless(sys.platform == "linux", "local lifecycle requires Linux /proc")
    def test_readiness_stop_timeout_preserves_stopping_child(self):
        with tempfile.TemporaryDirectory() as root:
            config = os.path.join(root, "server.json")
            with open(config, "w", encoding="utf-8") as handle:
                json.dump({"server": {"port": 8080}}, handle)
            binary = os.path.join(root, "cpppools_server")
            with open(binary, "w", encoding="ascii") as handle:
                handle.write("#!/bin/sh\n")
            os.chmod(binary, 0o755)
            manager = local_node.LocalNodeManager(root, "demo")

            class SlowProcess:
                pid = 46004
                returncode = None

                def __init__(self):
                    self.signals = []
                    self.wait_timeouts = []
                    self.kill_called = False

                def poll(self):
                    return None

                def send_signal(self, sig):
                    self.signals.append(sig)

                def wait(self, timeout=None):
                    self.wait_timeouts.append(timeout)
                    raise subprocess.TimeoutExpired("slow-start-server", timeout)

                def kill(self):
                    self.kill_called = True

            process = SlowProcess()

            def fake_popen(command, **_kwargs):
                _write_fake_handshake(command, process.pid)
                return process

            with mock.patch.object(subprocess, "Popen", side_effect=fake_popen), \
                    mock.patch.object(local_node, "_process_start_time",
                                      return_value="fake-start"), \
                    mock.patch.object(local_node, "_identity_matches", return_value=True), \
                    mock.patch.object(local_node.time, "monotonic",
                                      side_effect=[0, 0, 0, 2]), \
                    mock.patch.object(local_node.time, "sleep"):
                with self.assertRaises(local_node.StopTimeout) as raised:
                    manager.start(config, binary=binary, timeout=1)

            self.assertEqual(raised.exception.code, 7)
            self.assertEqual(process.signals, [])
            self.assertEqual(process.wait_timeouts, [10])
            self.assertFalse(process.kill_called,
                             "code 7 must not trigger an unrequested force kill")
            self.assertIs(manager._process, process)
            state = manager.read_state()
            self.assertEqual(state["state"], "stopping")
            self.assertEqual(state["state_revision"], 3)
            self.assertEqual(state["pid"], process.pid)
            self.assertEqual(state["process_start_time"], "fake-start")
            self.assertIsNone(state["last_exit_code"])

    @unittest.skipUnless(sys.platform == "linux", "local lifecycle requires Linux /proc")
    def test_startup_timeout_without_handshake_never_signals_child(self):
        with tempfile.TemporaryDirectory() as root:
            config = os.path.join(root, "server.json")
            with open(config, "w", encoding="utf-8") as handle:
                json.dump({"server": {"port": 8080}}, handle)
            binary = os.path.join(root, "cpppools_server")
            with open(binary, "w", encoding="ascii") as handle:
                handle.write("#!/bin/sh\n")
            os.chmod(binary, 0o755)
            manager = local_node.LocalNodeManager(root, "demo")

            class UnhandshakenProcess:
                pid = 46006
                returncode = None

                def __init__(self):
                    self.signals = []
                    self.kill_called = False
                    self.wait_timeouts = []

                def poll(self):
                    return None

                def send_signal(self, sig):
                    self.signals.append(sig)

                def wait(self, timeout=None):
                    self.wait_timeouts.append(timeout)
                    raise subprocess.TimeoutExpired("unhandshaken-server", timeout)

                def kill(self):
                    self.kill_called = True

            process = UnhandshakenProcess()
            with mock.patch.object(subprocess, "Popen", return_value=process), \
                    mock.patch.object(local_node, "_process_start_time",
                                      return_value="fake-start"), \
                    mock.patch.object(local_node.time, "monotonic",
                                      side_effect=[0, 0, 2]), \
                    mock.patch.object(local_node.time, "sleep"):
                with self.assertRaises(local_node.StopTimeout) as raised:
                    manager.start(config, binary=binary, timeout=1)

            self.assertEqual(raised.exception.code, 7)
            self.assertEqual(process.signals, [])
            self.assertFalse(process.kill_called)
            self.assertEqual(process.wait_timeouts, [10])
            state = manager.read_state()
            self.assertEqual(state["state"], "stopping")
            self.assertEqual(state["pid"], process.pid)
            self.assertEqual(state["process_start_time"], "fake-start")
            self.assertIsNotNone(local_node._read_control_json(
                state["control_dir"], "stop.request"))

    def test_ordinary_stop_before_starting_handshake_never_signals_pid(self):
        with tempfile.TemporaryDirectory() as root:
            manager = local_node.LocalNodeManager(root, "demo")
            config = os.path.join(root, "server.json")
            binary = os.path.join(root, "cpppools_server")
            control_dir = os.path.join(manager.instance_dir, "launches", "unready")
            os.makedirs(os.path.dirname(control_dir), exist_ok=True)
            local_node._ensure_private_control_dir(control_dir)
            local_node._atomic_json(os.path.join(control_dir, "control.meta"), {
                "protocol_version": 1, "launch_id": "unready",
            })
            local_node._atomic_bytes(os.path.join(control_dir, "control.token"),
                                     b"a" * 64)
            state = manager._base_state("unready", config, binary, root,
                                        "http://127.0.0.1:8080",
                                        os.path.join(root, "server.log"), control_dir)
            state.update(pid=47001, process_start_time="fake-start",
                         state="starting", state_revision=2)
            manager.write_state(state)

            with mock.patch.object(local_node, "_pid_alive", return_value=True), \
                    mock.patch.object(local_node, "_identity_matches", return_value=True), \
                    mock.patch.object(local_node.os, "kill") as kill, \
                    mock.patch.object(local_node.time, "monotonic",
                                      side_effect=[0, 0, 2]), \
                    mock.patch.object(local_node.time, "sleep"):
                with self.assertRaises(local_node.StopTimeout) as raised:
                    manager.stop(timeout=1)

            self.assertEqual(raised.exception.code, 7)
            kill.assert_not_called()
            current = manager.read_state()
            self.assertEqual(current["state"], "stopping")
            self.assertTrue(os.path.isfile(os.path.join(control_dir, "stop.request")))

    def test_ordinary_stop_retry_republishes_request_for_stopping_launch(self):
        with tempfile.TemporaryDirectory() as root:
            manager = local_node.LocalNodeManager(root, "demo")
            control_dir = os.path.join(
                manager.instance_dir, "launches", "launch-retry")
            manager.write_state({
                "protocol_version": 1, "state_revision": 1, "name": "demo",
                "launch_id": "launch-retry", "pid": 47002,
                "process_start_time": "process-start", "binary": "/bin/false",
                "control_dir": control_dir, "state": "running",
                "exit_kind": None, "last_exit_code": None, "updated_at": "now",
            })

            with mock.patch.object(local_node, "_pid_alive", return_value=True), \
                    mock.patch.object(local_node, "_identity_matches", return_value=True), \
                    mock.patch.object(local_node, "_verified_handshake",
                                      return_value=(False, None)), \
                    mock.patch.object(local_node, "_write_stop_request",
                                      side_effect=[False, True]) as write_request, \
                    mock.patch.object(local_node, "_send_verified_signal") as send_signal:
                with self.assertRaises(local_node.StopTimeout):
                    manager.stop(timeout=0.01)
                with self.assertRaises(local_node.StopTimeout):
                    manager.stop(timeout=0.01)

            self.assertEqual(write_request.call_count, 2)
            send_signal.assert_not_called()
            self.assertEqual(manager.read_state()["state"], "stopping")

    @unittest.skipUnless(sys.platform == "linux", "local lifecycle requires Linux /proc")
    def test_old_ready_marker_and_healthy_port_do_not_ready_new_launch(self):
        with tempfile.TemporaryDirectory() as root:
            health_reached = threading.Event()

            class HealthHandler(BaseHTTPRequestHandler):
                def do_GET(self):
                    health_reached.set()
                    self.send_response(200)
                    self.send_header("Content-Length", "2")
                    self.end_headers()
                    self.wfile.write(b"ok")

                def log_message(self, *_args):
                    pass

            old_server = ThreadingHTTPServer(("127.0.0.1", 0), HealthHandler)
            old_thread = threading.Thread(target=old_server.serve_forever, daemon=True)
            old_thread.start()
            try:
                port = old_server.server_address[1]
                config = os.path.join(root, "server.json")
                with open(config, "w", encoding="utf-8") as handle:
                    json.dump({"server": {"port": port}}, handle)
                binary = os.path.join(root, "cpppools_server")
                with open(binary, "w", encoding="ascii") as handle:
                    handle.write("#!/bin/sh\n")
                os.chmod(binary, 0o755)
                manager = local_node.LocalNodeManager(root, "demo")

                class FakeProcess:
                    pid = 47002
                    returncode = None

                    def poll(self):
                        return None

                    def wait(self, timeout=None):
                        raise subprocess.TimeoutExpired("new-launch", timeout)

                process = FakeProcess()

                def fake_popen(command, **_kwargs):
                    control_dir = command[command.index("--control-dir") + 1]
                    with open(os.path.join(control_dir, "control.meta"),
                              encoding="utf-8") as handle:
                        meta = json.load(handle)
                    stale_identity = {
                        "protocol_version": 1,
                        "launch_id": "stale-launch",
                        "pid": process.pid,
                        "process_start_time": "fake-start",
                    }
                    local_node._atomic_json(
                        os.path.join(control_dir, "starting.json"),
                        dict(stale_identity, state="starting"))
                    local_node._atomic_json(
                        os.path.join(control_dir, "ready.json"),
                        dict(stale_identity, state="ready", port=port,
                             node_id="stale-node"))
                    self.assertNotEqual(meta["launch_id"], "stale-launch")
                    return process

                with mock.patch.object(subprocess, "Popen", side_effect=fake_popen), \
                        mock.patch.object(local_node, "_process_start_time",
                                          return_value="fake-start"), \
                        mock.patch.object(local_node, "_identity_matches",
                                          return_value=True), \
                        mock.patch.object(local_node.time, "monotonic",
                                          side_effect=[0, 0, 2]), \
                        mock.patch.object(local_node.time, "sleep"):
                    with self.assertRaises(local_node.StopTimeout) as raised:
                        manager.start(config, binary=binary, timeout=1)

                self.assertEqual(raised.exception.code, 7)
                self.assertFalse(health_reached.is_set(),
                                 "old ready marker let a new launch trust the old port")
                self.assertEqual(manager.read_state()["state"], "stopping")
            finally:
                old_server.shutdown()
                old_server.server_close()
                old_thread.join(5)

    @unittest.skipUnless(sys.platform == "linux", "local lifecycle requires Linux /proc")
    def test_log_close_error_does_not_override_readiness_stop_timeout(self):
        with tempfile.TemporaryDirectory() as root:
            config = os.path.join(root, "server.json")
            with open(config, "w", encoding="utf-8") as handle:
                json.dump({"server": {"port": 8080}}, handle)
            binary = os.path.join(root, "cpppools_server")
            with open(binary, "w", encoding="ascii") as handle:
                handle.write("#!/bin/sh\n")
            os.chmod(binary, 0o755)
            manager = local_node.LocalNodeManager(root, "demo")
            log_path = os.path.abspath(os.path.join(manager.instance_dir, "server.log"))
            real_open = open

            class FailingLog:
                def close(self):
                    raise OSError("simulated log close failure")

            log = FailingLog()

            def fake_open(path, *args, **kwargs):
                if os.path.abspath(os.fspath(path)) == log_path:
                    return log
                return real_open(path, *args, **kwargs)

            class SlowProcess:
                pid = 46005
                returncode = None

                def __init__(self):
                    self.signals = []
                    self.kill_called = False

                def poll(self):
                    return None

                def send_signal(self, sig):
                    self.signals.append(sig)

                def wait(self, timeout=None):
                    raise subprocess.TimeoutExpired("slow-start-server", timeout)

                def kill(self):
                    self.kill_called = True

            process = SlowProcess()

            def fake_popen(command, **_kwargs):
                _write_fake_handshake(command, process.pid)
                return process

            with mock.patch.object(local_node, "open", side_effect=fake_open, create=True), \
                    mock.patch.object(subprocess, "Popen", side_effect=fake_popen), \
                    mock.patch.object(local_node, "_process_start_time",
                                      return_value="fake-start"), \
                    mock.patch.object(local_node, "_identity_matches", return_value=True), \
                    mock.patch.object(local_node.time, "monotonic",
                                      side_effect=[0, 0, 0, 2]), \
                    mock.patch.object(local_node.time, "sleep"):
                with self.assertRaises(local_node.StopTimeout) as raised:
                    manager.start(config, binary=binary, timeout=1)

            self.assertEqual(raised.exception.code, 7)
            self.assertEqual(process.signals, [])
            self.assertFalse(process.kill_called)
            self.assertIs(manager._process, process)
            state = manager.read_state()
            self.assertEqual(state["state"], "stopping")
            self.assertEqual(state["state_revision"], 3)
            self.assertEqual(state["pid"], process.pid)
            self.assertEqual(state["process_start_time"], "fake-start")
            self.assertIsNone(state["last_exit_code"])

    @unittest.skipUnless(sys.platform == "linux", "local lifecycle requires Linux /proc")
    def test_ready_does_not_revive_same_launch_after_stop_wins(self):
        with tempfile.TemporaryDirectory() as root:
            config = os.path.join(root, "server.json")
            with open(config, "w", encoding="utf-8") as handle:
                json.dump({"server": {"port": 8080}}, handle)
            binary = os.path.join(root, "cpppools_server")
            with open(binary, "w", encoding="ascii") as handle:
                handle.write("#!/bin/sh\n")
            os.chmod(binary, 0o755)

            manager = local_node.LocalNodeManager(root, "demo")
            health_entered = threading.Event()
            release_health = threading.Event()
            result = []
            errors = []

            class FakeProcess:
                pid = 43001
                returncode = None

                def __init__(self):
                    self.signals = []
                    self.kill_called = False

                def poll(self):
                    return None

                def send_signal(self, sig):
                    self.signals.append(sig)

                def wait(self, timeout=None):
                    raise subprocess.TimeoutExpired("cancelled-start", timeout)

                def kill(self):
                    self.kill_called = True

            process = FakeProcess()

            def fake_popen(command, **_kwargs):
                _write_fake_handshake(command, 43001)
                return process

            def health(_endpoint, _timeout):
                health_entered.set()
                if not release_health.wait(5):
                    raise AssertionError("test did not release health check")
                return True

            def start():
                try:
                    result.append(manager.start(config, binary=binary, timeout=1))
                except BaseException as exc:
                    errors.append(exc)

            thread = threading.Thread(target=start)
            with mock.patch.object(subprocess, "Popen", side_effect=fake_popen), \
                    mock.patch.object(local_node, "_process_start_time", return_value="fake-start"), \
                    mock.patch.object(local_node, "_identity_matches", return_value=True), \
                    mock.patch.object(local_node, "_pid_alive", return_value=True):
                manager._health = health
                thread.start()
                try:
                    self.assertTrue(health_entered.wait(5), "start did not reach health check")
                    current = manager.read_state()
                    stopped_startup = manager._commit_state(
                        (current["launch_id"], current["state_revision"], current["state"]),
                        {"state": "stopping"})
                    self.assertIsNotNone(stopped_startup)
                finally:
                    release_health.set()
                    thread.join(5)

            self.assertFalse(thread.is_alive(), "start thread did not finish")
            self.assertEqual(result, [])
            self.assertEqual(len(errors), 1)
            self.assertIsInstance(errors[0], local_node.LocalNodeError)
            self.assertEqual(errors[0].code, 6)
            self.assertEqual(errors[0].result["state"], "stopping")
            self.assertEqual(process.signals, [])
            self.assertFalse(process.kill_called)
            self.assertEqual(manager.read_state()["state"], "stopping")

    def test_old_stop_does_not_reconcile_replacement_launch(self):
        with tempfile.TemporaryDirectory() as root:
            manager = local_node.LocalNodeManager(root, "demo")
            old_control = os.path.join(
                manager.instance_dir, "launches", "old-launch")
            new_control = os.path.join(
                manager.instance_dir, "launches", "new-launch")

            def state(launch_id, pid, control_dir):
                return {
                    "protocol_version": 1, "state_revision": 1, "name": "demo",
                    "launch_id": launch_id, "pid": pid,
                    "process_start_time": "start-%d" % pid,
                    "binary": "/bin/false", "control_dir": control_dir,
                    "state": "running", "exit_kind": None,
                    "last_exit_code": None, "updated_at": "now",
                }

            manager.write_state(state("old-launch", 47003, old_control))
            new_state = state("new-launch", 47004, new_control)
            old_checks = []

            def process_alive(pid):
                if pid == 47003:
                    old_checks.append(pid)
                    if len(old_checks) == 2:
                        manager.write_state(new_state)
                    return len(old_checks) == 1
                return False

            with mock.patch.object(local_node, "_pid_alive",
                                   side_effect=process_alive), \
                    mock.patch.object(local_node, "_identity_matches", return_value=True), \
                    mock.patch.object(local_node, "_write_stop_request", return_value=True), \
                    mock.patch.object(local_node, "_reap_child", return_value=None):
                with self.assertRaises(local_node.LocalStateError) as raised:
                    manager.stop(timeout=1)

            self.assertEqual(raised.exception.code, 10)
            self.assertEqual(raised.exception.result["launch_id"], "new-launch")
            self.assertEqual(manager.read_state()["launch_id"], "new-launch")
            self.assertEqual(manager.read_state()["state"], "running")

    @unittest.skipUnless(sys.platform == "linux", "local lifecycle requires Linux /proc")
    def test_foreground_interrupt_uses_graceful_stop_without_kill(self):
        with tempfile.TemporaryDirectory() as root:
            config = os.path.join(root, "server.json")
            with open(config, "w", encoding="utf-8") as handle:
                json.dump({"server": {"port": 8080}}, handle)
            binary = os.path.join(root, "cpppools_server")
            with open(binary, "w", encoding="ascii") as handle:
                handle.write("#!/bin/sh\n")
            os.chmod(binary, 0o755)
            manager = local_node.LocalNodeManager(root, "demo")

            class FakeProcess:
                pid = 43213

                def __init__(self):
                    self.exit_code = None
                    self.interrupted = False
                    self.signals = []
                    self.kill_called = False

                def poll(self):
                    return self.exit_code

                def wait(self, timeout=None):
                    if timeout is None and not self.interrupted:
                        self.interrupted = True
                        raise KeyboardInterrupt()
                    if self.exit_code is not None:
                        return self.exit_code
                    raise subprocess.TimeoutExpired("foreground-server", timeout)

                def send_signal(self, sig):
                    self.signals.append(sig)

                def kill(self):
                    self.kill_called = True

            process = FakeProcess()
            manager._health = lambda _endpoint, _timeout: True

            def fake_popen(command, **_kwargs):
                _write_fake_handshake(command, process.pid)
                return process

            write_stop_request = local_node._write_stop_request
            requests = []

            def stop_and_finish(state):
                sent = write_stop_request(state)
                if sent:
                    requests.append(state["launch_id"])
                    process.exit_code = 0
                    local_node._atomic_json(
                        os.path.join(state["control_dir"], "stopped.json"), {
                            "protocol_version": 1,
                            "launch_id": state["launch_id"],
                            "pid": process.pid,
                            "process_start_time": "fake-start",
                            "state": "stopped",
                            "exit_kind": "graceful",
                            "exit_code": 0,
                        })
                return sent

            with mock.patch.object(subprocess, "Popen", side_effect=fake_popen), \
                    mock.patch.object(local_node, "_process_start_time",
                                      return_value="fake-start"), \
                    mock.patch.object(local_node, "_identity_matches", return_value=True), \
                    mock.patch.object(local_node, "_pid_alive",
                                      side_effect=lambda _pid: process.poll() is None), \
                    mock.patch.object(local_node, "_write_stop_request",
                                      side_effect=stop_and_finish):
                with self.assertRaises(local_node.ExitOutcome) as raised:
                    manager.start(config, binary=binary, timeout=1, foreground=True)

            self.assertEqual(raised.exception.code, 130)
            self.assertEqual(raised.exception.result["state"], "stopped")
            self.assertEqual(requests, [manager.read_state()["launch_id"]])
            self.assertEqual(process.signals, [])
            self.assertFalse(process.kill_called)
            self.assertEqual(manager.read_state()["exit_kind"], "graceful")

    @unittest.skipUnless(sys.platform == "linux", "local lifecycle requires Linux /proc")
    def test_old_start_cannot_overwrite_new_launch_registered_during_health(self):
        with tempfile.TemporaryDirectory() as root:
            config = os.path.join(root, "server.json")
            with open(config, "w", encoding="utf-8") as handle:
                json.dump({"server": {"port": 8080}}, handle)
            binary = os.path.join(root, "cpppools_server")
            with open(binary, "w", encoding="ascii") as handle:
                handle.write("#!/bin/sh\n")
            os.chmod(binary, 0o755)

            first_manager = local_node.LocalNodeManager(root, "demo")
            second_manager = local_node.LocalNodeManager(root, "demo")
            health_entered = threading.Event()
            release_first_health = threading.Event()
            call_lock = threading.Lock()
            health_calls = 0
            launch_dirs = []
            first_result = []
            first_errors = []

            class FakeProcess:
                def __init__(self, pid):
                    self.pid = pid
                    self.returncode = None

                def poll(self):
                    return None

            def fake_popen(command, **_kwargs):
                launch_dir = command[command.index("--control-dir") + 1]
                launch_dirs.append(launch_dir)
                pid = 44000 + len(launch_dirs)
                _write_fake_handshake(command, pid)
                return FakeProcess(pid)

            def health(_endpoint, _timeout):
                nonlocal health_calls
                with call_lock:
                    health_calls += 1
                    call_number = health_calls
                if call_number == 1:
                    health_entered.set()
                    if not release_first_health.wait(5):
                        raise AssertionError("test did not release old launch health check")
                return True

            def first_start():
                try:
                    first_result.append(first_manager.start(config, binary=binary, timeout=1))
                except BaseException as exc:
                    first_errors.append(exc)

            thread = threading.Thread(target=first_start)
            with mock.patch.object(subprocess, "Popen", side_effect=fake_popen), \
                    mock.patch.object(local_node, "_process_start_time", return_value="fake-start"), \
                    mock.patch.object(local_node, "_identity_matches", return_value=True):
                first_manager._health = health
                second_manager._health = health
                thread.start()
                try:
                    self.assertTrue(health_entered.wait(5), "old launch did not reach health check")
                    old = first_manager.read_state()
                    failed_old = first_manager._commit_state(
                        (old["launch_id"], old["state_revision"], old["state"]),
                        {"state": "crashed", "exit_kind": "unexpected"})
                    self.assertIsNotNone(failed_old)

                    new_result = second_manager.start(config, binary=binary, timeout=1)
                    new_launch_id = new_result["launch_id"]
                finally:
                    release_first_health.set()
                    thread.join(5)

            self.assertFalse(thread.is_alive(), "old start thread did not finish")
            self.assertEqual(first_result, [])
            self.assertEqual(len(first_errors), 1)
            self.assertIsInstance(first_errors[0], local_node.LocalNodeError)
            self.assertEqual(len(launch_dirs), 2)
            final = first_manager.read_state()
            self.assertEqual(final["launch_id"], new_launch_id)
            self.assertEqual(final["state"], "running")

    def test_force_acceptance_invalidates_stale_graceful_completion(self):
        with tempfile.TemporaryDirectory() as root:
            manager = local_node.LocalNodeManager(root, "demo")
            manager.write_state({
                "protocol_version": 1, "state_revision": 1, "name": "demo",
                "launch_id": "launch-force", "pid": 12345, "state": "running",
                "binary": "/bin/false", "control_dir": None,
                "exit_kind": None, "last_exit_code": None, "updated_at": "now",
            })
            reaping = threading.Event()
            release_reap = threading.Event()
            alive_lock = threading.Lock()
            alive_calls = 0
            stop_results = []
            stop_errors = []

            def pid_alive(_pid):
                nonlocal alive_calls
                with alive_lock:
                    alive_calls += 1
                    return alive_calls == 1

            def reap_child(_pid):
                reaping.set()
                if not release_reap.wait(5):
                    raise AssertionError("test did not release the graceful stop checkpoint")
                return 0

            def graceful_stop():
                try:
                    stop_results.append(manager.stop(timeout=1))
                except BaseException as exc:
                    stop_errors.append(exc)

            stop_thread = threading.Thread(target=graceful_stop)
            with mock.patch.object(local_node, "_pid_alive", side_effect=pid_alive), \
                    mock.patch.object(local_node, "_identity_matches", return_value=True), \
                    mock.patch.object(local_node, "_reap_child", side_effect=reap_child), \
                    mock.patch.object(local_node.os, "kill", return_value=None):
                stop_thread.start()
                try:
                    self.assertTrue(reaping.wait(5), "graceful stop did not reach its exit checkpoint")
                    stopping = manager.read_state()
                    self.assertEqual(stopping["state"], "stopping")
                    pending = manager._commit_state(
                        (stopping["launch_id"], stopping["state_revision"], stopping["state"]),
                        {"stop_mode": "force", "force_delivery": "pending"})
                    self.assertIsNotNone(pending)
                    accepted = manager._commit_state(
                        (pending["launch_id"], pending["state_revision"], pending["state"]),
                        {"force_delivery": "accepted", "force_accepted_at": "now"})
                    self.assertIsNotNone(accepted)
                finally:
                    release_reap.set()
                    stop_thread.join(5)

            self.assertFalse(stop_thread.is_alive(), "graceful stop thread did not finish")
            self.assertEqual(stop_results, [])
            self.assertEqual(len(stop_errors), 1)
            self.assertIsInstance(stop_errors[0], local_node.ExitOutcome)
            self.assertEqual(stop_errors[0].code, 9)
            self.assertEqual(stop_errors[0].kind, "forced_stop")
            self.assertEqual(stop_errors[0].result["state"], "crashed")
            self.assertEqual(stop_errors[0].result["exit_kind"], "forced")

            current = manager.read_state()
            self.assertEqual(current["state"], "crashed")
            self.assertEqual(current["force_delivery"], "accepted")
            self.assertEqual(current["exit_kind"], "forced")

            with self.assertRaises(local_node.LocalStateError):
                manager._commit_state(
                    (current["launch_id"], current["state_revision"], current["state"]),
                    {"force_delivery": "pending"})

    @unittest.skipUnless(os.name == "posix", "force delivery uses POSIX SIGKILL")
    def test_force_stop_records_pending_before_signal_and_returns_forced_result(self):
        with tempfile.TemporaryDirectory() as root:
            manager = local_node.LocalNodeManager(root, "demo")
            manager.write_state({
                "protocol_version": 1, "state_revision": 1, "name": "demo",
                "launch_id": "launch-force", "pid": 12345, "state": "running",
                "binary": "/bin/false", "control_dir": None,
                "exit_kind": None, "last_exit_code": None, "updated_at": "now",
            })
            alive = [True]
            signals = []

            def send_signal(pid, signal_number):
                signals.append((pid, signal_number))
                current = manager.read_state()
                self.assertEqual(current["state"], "stopping")
                self.assertEqual(current["stop_mode"], "force")
                self.assertEqual(current["force_delivery"], "pending")
                alive[0] = False

            with mock.patch.object(local_node, "_pid_alive",
                                   side_effect=lambda _pid: alive[0]), \
                    mock.patch.object(local_node, "_identity_matches", return_value=True), \
                    mock.patch.object(local_node, "_reap_child", return_value=137), \
                    mock.patch.object(local_node.os, "kill", side_effect=send_signal):
                with self.assertRaises(local_node.ExitOutcome) as raised:
                    manager.stop(timeout=1, force=True)

            self.assertEqual(raised.exception.code, 9)
            self.assertEqual(raised.exception.kind, "forced_stop")
            self.assertEqual(raised.exception.result["state"], "crashed")
            self.assertEqual(raised.exception.result["exit_kind"], "forced")
            self.assertEqual(raised.exception.result["last_exit_code"], 137)
            self.assertEqual(signals, [(12345, signal.SIGKILL)])
            current = manager.read_state()
            self.assertEqual(current["force_delivery"], "accepted")
            self.assertTrue(current["force_accepted_at"])

    @unittest.skipUnless(os.name == "posix", "force delivery uses POSIX SIGKILL")
    def test_pending_force_recovery_does_not_guess_and_accepted_force_is_not_resent(self):
        with tempfile.TemporaryDirectory() as root:
            pending_manager = local_node.LocalNodeManager(root, "pending")
            pending_manager.write_state({
                "protocol_version": 1, "state_revision": 1, "name": "pending",
                "launch_id": "launch-pending", "pid": 12345, "state": "stopping",
                "binary": "/bin/false", "control_dir": None,
                "stop_mode": "force", "force_delivery": "pending",
                "exit_kind": None, "last_exit_code": None, "updated_at": "now",
            })
            with mock.patch.object(local_node, "_pid_alive", return_value=False), \
                    mock.patch.object(local_node, "_reap_child", return_value=None), \
                    mock.patch.object(local_node.os, "kill") as kill:
                with self.assertRaises(local_node.ExitOutcome) as raised:
                    pending_manager.stop(timeout=0.1, force=True)
            self.assertEqual(raised.exception.code, 10)
            self.assertEqual(raised.exception.result["exit_kind"], None)
            self.assertEqual(raised.exception.result["force_delivery"], "pending")
            kill.assert_not_called()

            accepted_manager = local_node.LocalNodeManager(root, "accepted")
            accepted_manager.write_state({
                "protocol_version": 1, "state_revision": 1, "name": "accepted",
                "launch_id": "launch-accepted", "pid": 12346, "state": "stopping",
                "binary": "/bin/false", "control_dir": None,
                "stop_mode": "force", "force_delivery": "accepted",
                "force_accepted_at": "then", "exit_kind": None,
                "last_exit_code": None, "updated_at": "now",
            })
            with mock.patch.object(local_node, "_pid_alive", return_value=True), \
                    mock.patch.object(local_node, "_identity_matches", return_value=True), \
                    mock.patch.object(local_node.time, "monotonic",
                                      side_effect=[0, 0.02]), \
                    mock.patch.object(local_node.time, "sleep"), \
                    mock.patch.object(local_node.os, "kill") as kill:
                with self.assertRaises(local_node.StopTimeout) as raised:
                    accepted_manager.stop(timeout=0.01, force=True)
            self.assertEqual(raised.exception.code, 7)
            self.assertEqual(raised.exception.result["state"], "stopping")
            self.assertEqual(raised.exception.result["force_delivery"], "accepted")
            kill.assert_not_called()

    @unittest.skipUnless(os.name == "posix", "force delivery uses POSIX SIGKILL")
    def test_explicit_force_retries_a_previously_rejected_delivery(self):
        with tempfile.TemporaryDirectory() as root:
            manager = local_node.LocalNodeManager(root, "demo")
            manager.write_state({
                "protocol_version": 1, "state_revision": 1, "name": "demo",
                "launch_id": "launch-retry", "pid": 12347, "state": "stopping",
                "binary": "/bin/false", "control_dir": None,
                "stop_mode": "force", "force_delivery": "rejected",
                "force_rejected_at": "previous", "exit_kind": None,
                "last_exit_code": None, "updated_at": "now",
            })
            alive = [True]

            def send_signal(_pid, _signal_number):
                state = manager.read_state()
                self.assertEqual(state["force_delivery"], "pending")
                self.assertNotEqual(state["force_requested_at"], "previous")
                alive[0] = False

            with mock.patch.object(local_node, "_pid_alive",
                                   side_effect=lambda _pid: alive[0]), \
                    mock.patch.object(local_node, "_identity_matches", return_value=True), \
                    mock.patch.object(local_node, "_reap_child", return_value=137), \
                    mock.patch.object(local_node.os, "kill", side_effect=send_signal):
                with self.assertRaises(local_node.ExitOutcome) as raised:
                    manager.stop(timeout=1, force=True)

            self.assertEqual(raised.exception.code, 9)
            self.assertEqual(manager.read_state()["force_delivery"], "accepted")

    @unittest.skipUnless(os.name == "posix", "force delivery uses POSIX SIGKILL")
    def test_rejected_force_signal_is_persisted_and_returns_identity_error(self):
        with tempfile.TemporaryDirectory() as root:
            manager = local_node.LocalNodeManager(root, "demo")
            manager.write_state({
                "protocol_version": 1, "state_revision": 1, "name": "demo",
                "launch_id": "launch-denied", "pid": 12348, "state": "running",
                "binary": "/bin/false", "control_dir": None,
                "exit_kind": None, "last_exit_code": None, "updated_at": "now",
            })
            with mock.patch.object(local_node, "_pid_alive", return_value=True), \
                    mock.patch.object(local_node, "_identity_matches", return_value=True), \
                    mock.patch.object(
                        local_node.os, "kill",
                        side_effect=PermissionError(local_node.errno.EPERM, "denied")):
                with self.assertRaises(local_node.IdentityError) as raised:
                    manager.stop(timeout=0.1, force=True)

            self.assertEqual(raised.exception.code, 8)
            state = manager.read_state()
            self.assertEqual(state["state"], "stopping")
            self.assertEqual(state["stop_mode"], "force")
            self.assertEqual(state["force_delivery"], "rejected")

    def test_conditional_commit_rejects_terminal_state_rollback(self):
        with tempfile.TemporaryDirectory() as root:
            manager = local_node.LocalNodeManager(root, "demo")
            manager.write_state({
                "protocol_version": 1, "state_revision": 2, "name": "demo",
                "launch_id": "launch-terminal", "pid": 12345, "state": "stopped",
                "exit_kind": "graceful", "last_exit_code": 0, "updated_at": "now",
            })
            terminal = manager.read_state()
            with self.assertRaises(local_node.LocalStateError):
                manager._commit_state(
                    (terminal["launch_id"], terminal["state_revision"], terminal["state"]),
                    {"state": "running"})

    def test_binary_without_path_is_resolved_from_path(self):
        with tempfile.TemporaryDirectory() as root:
            binary = os.path.join(root, "cpppools_server")
            with open(binary, "w", encoding="ascii") as handle:
                handle.write("#!/bin/sh\n")
            os.chmod(binary, 0o755)
            manager = local_node.LocalNodeManager(root, "demo")
            old_path = os.environ.get("PATH", "")
            os.environ["PATH"] = root + os.pathsep + old_path
            try:
                self.assertEqual(manager.resolve_binary(None), os.path.realpath(binary))
            finally:
                os.environ["PATH"] = old_path

    def test_atomic_state_round_trip_and_status_detects_dead_process(self):
        with tempfile.TemporaryDirectory() as root:
            manager = local_node.LocalNodeManager(root, "demo")
            state = {
                "protocol_version": 1,
                "state_revision": 1,
                "name": "demo",
                "launch_id": "launch-1",
                "pid": 999999,
                "binary": "/bin/false",
                "config": "/tmp/config.json",
                "workdir": "/tmp",
                "endpoint": "http://127.0.0.1:1",
                "log_file": "/tmp/server.log",
                "state": "running",
                "process_start_time": None,
                "exit_kind": None,
                "last_exit_code": None,
                "updated_at": "now",
            }
            manager.write_state(state)
            self.assertEqual(manager.read_state()["launch_id"], "launch-1")
            status = manager.status()
            self.assertEqual(status["state"], "crashed")
            self.assertEqual(status["last_known_state"], "crashed")
            self.assertEqual(status["exit_kind"], "unexpected")
            self.assertIsNone(status["last_exit_code"])
            self.assertFalse(status["process_alive"])
            self.assertFalse(status["identity_verified"])
            persisted = manager.read_state()
            self.assertEqual(persisted["state"], "crashed")
            revision = persisted["state_revision"]
            manager.status()
            self.assertEqual(manager.read_state()["state_revision"], revision)

    def test_status_does_not_treat_unregistered_starting_state_as_exited(self):
        with tempfile.TemporaryDirectory() as root:
            manager = local_node.LocalNodeManager(root, "demo")
            state = {
                "protocol_version": 1, "state_revision": 1, "name": "demo",
                "launch_id": "launch-before-spawn", "pid": None,
                "process_start_time": None, "binary": "/bin/false",
                "state": "starting", "exit_kind": None,
                "last_exit_code": None, "updated_at": "now",
            }
            manager.write_state(state)
            status = manager.status()
            self.assertEqual(status["state"], "starting")
            self.assertEqual(status["last_known_state"], "starting")
            self.assertFalse(status["process_alive"])
            self.assertEqual(manager.read_state(), state)

    def test_process_identity_without_start_time_is_unverified(self):
        state = {"pid": 43212, "binary": sys.executable,
                 "process_start_time": None}
        with mock.patch.object(local_node, "_pid_alive", return_value=True), \
                mock.patch.object(local_node, "_process_executable",
                                  return_value=sys.executable):
            self.assertFalse(local_node._identity_matches(state))

    def test_status_reconciles_pid_reuse_without_reaping_the_new_process(self):
        with tempfile.TemporaryDirectory() as root:
            manager = local_node.LocalNodeManager(root, "demo")
            manager.write_state({
                "protocol_version": 1, "state_revision": 1, "name": "demo",
                "launch_id": "launch-old", "pid": 12349,
                "process_start_time": "old-start", "binary": "/bin/false",
                "state": "running", "exit_kind": None,
                "last_exit_code": None, "updated_at": "now",
            })
            with mock.patch.object(local_node, "_pid_alive", return_value=True), \
                    mock.patch.object(local_node, "_identity_matches", return_value=False), \
                    mock.patch.object(local_node, "_process_start_time",
                                      return_value="new-start"), \
                    mock.patch.object(local_node, "_reap_child") as reap:
                status = manager.status()

            self.assertEqual(status["state"], "crashed")
            self.assertEqual(status["exit_kind"], "unexpected")
            self.assertTrue(status["process_alive"])
            self.assertFalse(status["identity_verified"])
            reap.assert_not_called()

    def test_terminal_cleanup_cannot_delete_controls_from_another_launch(self):
        with tempfile.TemporaryDirectory() as root:
            manager = local_node.LocalNodeManager(root, "demo")
            other_dir = os.path.join(root, "other", "launches", "foreign-launch")
            os.makedirs(other_dir)
            token_path = os.path.join(other_dir, "control.token")
            with open(token_path, "w", encoding="ascii") as handle:
                handle.write("secret")
            manager.write_state({
                "protocol_version": 1, "state_revision": 1, "name": "demo",
                "launch_id": "launch-1", "pid": 999999,
                "process_start_time": "process-start", "binary": "/bin/false",
                "control_dir": other_dir, "state": "running",
                "exit_kind": None, "last_exit_code": None, "updated_at": "now",
            })

            with mock.patch.object(local_node, "_pid_alive", return_value=False), \
                    mock.patch.object(local_node, "_reap_child", return_value=None):
                with self.assertRaises(local_node.LocalStateError):
                    manager.status()

            self.assertTrue(os.path.isfile(token_path))

    def test_malformed_force_state_is_a_local_state_error(self):
        with tempfile.TemporaryDirectory() as root:
            manager = local_node.LocalNodeManager(root, "demo")
            manager.write_state({
                "protocol_version": 1, "state_revision": 1, "name": "demo",
                "launch_id": "launch-invalid", "pid": 999999,
                "process_start_time": "process-start", "binary": "/bin/false",
                "state": "running", "force_delivery": "sent",
                "exit_kind": None, "last_exit_code": None, "updated_at": "now",
            })
            with mock.patch.object(local_node, "_pid_alive", return_value=True), \
                    mock.patch.object(local_node, "_identity_matches", return_value=True):
                with self.assertRaises(local_node.LocalStateError):
                    manager.status()

    def test_repeated_stop_on_unexpected_terminal_returns_the_same_code_and_result(self):
        with tempfile.TemporaryDirectory() as root:
            manager = local_node.LocalNodeManager(root, "demo")
            manager.write_state({
                "protocol_version": 1, "state_revision": 2, "name": "demo",
                "launch_id": "launch-dead", "pid": 999999,
                "process_start_time": "process-start", "binary": "/bin/false",
                "state": "crashed", "exit_kind": "unexpected",
                "last_exit_code": None, "updated_at": "now",
            })
            with mock.patch.object(local_node, "_pid_alive", return_value=False), \
                    mock.patch.object(local_node, "_reap_child", return_value=None), \
                    mock.patch.object(local_node.os, "kill") as kill:
                results = []
                for _ in range(2):
                    with self.assertRaises(local_node.ExitOutcome) as raised:
                        manager.stop(timeout=0.1)
                    self.assertEqual(raised.exception.code, 10)
                    results.append(raised.exception.result)

            self.assertEqual(results[0]["state"], "crashed")
            self.assertEqual(results[0]["exit_kind"], "unexpected")
            self.assertEqual(results[0], results[1])
            self.assertEqual(manager.read_state()["state_revision"], 2)
            kill.assert_not_called()

    def test_status_requires_same_launch_complete_stopped_marker_schema(self):
        scenarios = ("valid", "missing", "old_launch", "corrupt", "extra_field",
                     "valid_nonzero_exit", "valid_known_nonzero_exit")
        for scenario in scenarios:
            with self.subTest(marker=scenario), tempfile.TemporaryDirectory() as root:
                manager = local_node.LocalNodeManager(root, "demo")
                control_dir = os.path.join(manager.instance_dir, "launches", "launch-1")
                os.makedirs(control_dir)
                state = {
                    "protocol_version": 1, "state_revision": 1, "name": "demo",
                    "launch_id": "launch-1", "pid": 999999,
                    "process_start_time": "process-start", "binary": "/bin/false",
                    "control_dir": control_dir, "state": "stopping",
                    "exit_kind": None,
                    "last_exit_code": (137 if scenario == "valid_known_nonzero_exit" else None),
                    "updated_at": "now",
                }
                manager.write_state(state)
                if scenario != "missing":
                    marker = {
                        "protocol_version": 1,
                        "launch_id": "old-launch" if scenario == "old_launch" else "launch-1",
                        "pid": 999999, "process_start_time": "process-start",
                        "state": "stopped", "exit_kind": "graceful", "exit_code": 0,
                    }
                    if scenario == "corrupt":
                        with open(os.path.join(control_dir, "stopped.json"), "w",
                                  encoding="utf-8") as handle:
                            handle.write("{")
                    else:
                        if scenario == "extra_field":
                            marker["unexpected"] = True
                        local_node._atomic_json(
                            os.path.join(control_dir, "stopped.json"), marker)

                with mock.patch.object(local_node, "_pid_alive", return_value=False), \
                        mock.patch.object(
                            local_node, "_reap_child",
                            return_value=(1 if scenario == "valid_nonzero_exit" else None)):
                    status = manager.status()

                if scenario == "valid":
                    self.assertEqual(status["state"], "stopped")
                    self.assertEqual(status["exit_kind"], "graceful")
                    self.assertEqual(status["last_exit_code"], 0)
                elif scenario == "valid_known_nonzero_exit":
                    self.assertEqual(status["state"], "crashed")
                    self.assertEqual(status["exit_kind"], "unexpected")
                    self.assertEqual(status["last_exit_code"], 137)
                elif scenario == "valid_nonzero_exit":
                    self.assertEqual(status["state"], "crashed")
                    self.assertEqual(status["exit_kind"], "unexpected")
                    self.assertEqual(status["last_exit_code"], 1)
                else:
                    self.assertEqual(status["state"], "crashed")
                    self.assertEqual(status["exit_kind"], "unexpected")

    def test_force_delivery_and_stopped_marker_recovery_match_for_status_and_stop(self):
        deliveries = (None, "pending", "accepted", "rejected")
        markers = ("valid", "missing", "old_launch", "corrupt")
        for delivery in deliveries:
            for marker_kind in markers:
                with self.subTest(delivery=delivery, marker=marker_kind), \
                        tempfile.TemporaryDirectory() as root:
                    manager = local_node.LocalNodeManager(root, "demo")
                    control_dir = os.path.join(
                        manager.instance_dir, "launches", "launch-1")
                    os.makedirs(control_dir)
                    state = {
                        "protocol_version": 1, "state_revision": 1,
                        "name": "demo", "launch_id": "launch-1", "pid": 999999,
                        "process_start_time": "process-start", "binary": "/bin/false",
                        "control_dir": control_dir, "state": "stopping",
                        "force_delivery": delivery, "exit_kind": None,
                        "last_exit_code": None, "updated_at": "now",
                    }
                    manager.write_state(state)
                    if marker_kind != "missing":
                        marker = {
                            "protocol_version": 1,
                            "launch_id": ("old-launch" if marker_kind == "old_launch"
                                          else "launch-1"),
                            "pid": 999999,
                            "process_start_time": "process-start",
                            "state": "stopped", "exit_kind": "graceful",
                            "exit_code": 0,
                        }
                        marker_path = os.path.join(control_dir, "stopped.json")
                        if marker_kind == "corrupt":
                            with open(marker_path, "w", encoding="utf-8") as handle:
                                handle.write("{")
                        else:
                            local_node._atomic_json(marker_path, marker)

                    with mock.patch.object(local_node, "_pid_alive", return_value=False), \
                            mock.patch.object(local_node, "_reap_child", return_value=None):
                        status = manager.status()
                        persisted = manager.read_state()
                        if delivery == "accepted":
                            expected = ("crashed", "forced", 9)
                        elif delivery == "pending":
                            expected = ("crashed", None, 10)
                        elif marker_kind == "valid":
                            expected = ("stopped", "graceful", 0)
                        else:
                            expected = ("crashed", "unexpected", 10)

                        self.assertEqual(
                            (status["state"], status["exit_kind"]), expected[:2])
                        self.assertEqual(
                            (persisted["state"], persisted["exit_kind"]), expected[:2])
                        if expected[2] == 0:
                            stop_result = manager.stop(timeout=0.1)
                            self.assertEqual(stop_result["state"], expected[0])
                            self.assertEqual(stop_result["exit_kind"], expected[1])
                        else:
                            with self.assertRaises(local_node.ExitOutcome) as raised:
                                manager.stop(timeout=0.1)
                            self.assertEqual(raised.exception.code, expected[2])
                            self.assertEqual(raised.exception.result["state"], expected[0])
                            self.assertEqual(raised.exception.result["exit_kind"], expected[1])

    def test_invalid_instance_name_is_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaises(local_node.LocalStateError):
                local_node.LocalNodeManager(root, "../escape")

    def test_zombie_process_is_not_alive(self):
        if sys.platform != "linux":
            self.skipTest("Linux /proc process state probe")
        child = os.fork()
        if child == 0:
            os._exit(0)
        try:
            time.sleep(0.05)
            self.assertFalse(local_node._pid_alive(child))
        finally:
            os.waitpid(child, 0)

    def test_process_start_identity_is_recorded_for_live_launch(self):
        if sys.platform != "linux":
            self.skipTest("Linux /proc process identity probe")
        with tempfile.TemporaryDirectory() as root:
            manager = local_node.LocalNodeManager(root, "demo")
            state = {
                "protocol_version": 1, "state_revision": 1, "name": "demo",
                "launch_id": "launch-1", "pid": os.getpid(),
                "process_start_time": local_node._process_start_time(os.getpid()),
                "binary": local_node._process_executable(os.getpid()),
                "config": "/tmp/config.json", "workdir": "/tmp",
                "endpoint": None, "log_file": None, "state": "running",
                "exit_kind": None, "last_exit_code": None, "updated_at": "now",
            }
            manager.write_state(state)
            self.assertTrue(manager.status()["identity_verified"])

    def test_stop_does_not_signal_pid_when_identity_is_not_verified(self):
        with tempfile.TemporaryDirectory() as root:
            manager = local_node.LocalNodeManager(root, "demo")
            manager.write_state({
                "protocol_version": 1, "state_revision": 1,
                "name": "demo", "launch_id": "launch-1", "pid": os.getpid(),
                "binary": "/bin/false", "config": "/tmp/config.json",
                "workdir": "/tmp", "endpoint": None, "log_file": None,
                "state": "running", "process_start_time": None,
                "exit_kind": None, "last_exit_code": None,
                "updated_at": "now",
            })
            with self.assertRaises(local_node.IdentityError):
                manager.stop(timeout=0.1)


@unittest.skipUnless(os.environ.get("CPL_TEST_SERVER"),
                     "set CPL_TEST_SERVER for real local lifecycle smoke")
@unittest.skipUnless(sys.platform == "linux",
                     "local lifecycle requires Linux /proc")
class TestLiveLocalNode(unittest.TestCase):
    def test_config_drift_cleanup_survives_duplicate_sigterm(self):
        with tempfile.TemporaryDirectory(prefix="cpppools_config_drift_signals_") as root:
            db_path = os.path.join(root, "node.db")
            conn = sqlite3.connect(db_path)
            conn.execute("CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL)")
            conn.close()
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", 0))
                port = probe.getsockname()[1]
            config = os.path.join(root, "server.json")
            with open(config, "w", encoding="utf-8") as handle:
                json.dump({"server": {"port": port, "worker_threads": 2},
                           "datasources": [{"name": "main", "type": "sqlite",
                                            "default": True, "pool_size": 1,
                                            "sqlite": {"path": db_path}}]}, handle)

            manager = local_node.LocalNodeManager(os.path.join(root, "state"), "demo")
            health_reached = threading.Event()
            continue_start = threading.Event()
            original_health = manager._health
            errors = []

            def hold_ready_health(endpoint, timeout):
                healthy = original_health(endpoint, timeout)
                if healthy:
                    health_reached.set()
                    if not continue_start.wait(20):
                        raise TimeoutError("test did not release startup health gate")
                return healthy

            manager._health = hold_ready_health

            def start_manager():
                try:
                    manager.start(config, binary=os.environ["CPL_TEST_SERVER"],
                                  workdir=root, timeout=10)
                except BaseException as exc:
                    errors.append(exc)

            starter = threading.Thread(target=start_manager)
            starter.start()
            pending = None
            process = None
            try:
                self.assertTrue(health_reached.wait(10),
                                "server never reached healthy startup")
                process = manager._process
                self.assertIsNotNone(process)
                pending = socket.create_connection(("127.0.0.1", port), timeout=3)
                pending.settimeout(3)
                pending.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\n")

                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    try:
                        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=1)
                        conn.request("GET", "/api/v1/stats")
                        response = conn.getresponse()
                        stats = json.loads(response.read())
                        conn.close()
                        if stats.get("active_requests", 0) >= 2:
                            break
                    except (OSError, http.client.HTTPException, ValueError):
                        pass
                    time.sleep(0.03)
                else:
                    self.fail("partial request never entered the server's in-flight set")

                with open(config, "a", encoding="utf-8") as handle:
                    handle.write(" ")
                continue_start.set()

                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    state = manager.read_state()
                    if state.get("state") == "stopping":
                        break
                    time.sleep(0.02)
                else:
                    self.fail("config drift did not publish a stopping state")
                stopping_state = dict(state)

                os.kill(process.pid, signal.SIGTERM)
                time.sleep(0.25)
                self.assertIsNone(process.poll(),
                                  "in-flight request should keep graceful shutdown pending")
                os.kill(process.pid, signal.SIGTERM)
                self.assertIsNone(process.poll(),
                                  "duplicate SIGTERM should not abort graceful drain")
                pending.close()
                pending = None
                starter.join(timeout=10)
                self.assertFalse(starter.is_alive(), "startup cleanup did not finish")
            finally:
                continue_start.set()
                if pending is not None:
                    pending.close()
                if process is None:
                    process = manager._process
                if starter.is_alive() and process is not None:
                    if process.poll() is None:
                        with contextlib.suppress(ProcessLookupError):
                            os.kill(process.pid, signal.SIGTERM)
                    starter.join(timeout=10)
                if starter.is_alive() and process is not None and process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
                    starter.join(timeout=5)
                elif starter.is_alive():
                    starter.join(timeout=10)
                if process is not None and process.poll() is None:
                    with contextlib.suppress(local_node.ExitOutcome):
                        manager.stop(timeout=2, force=True)

            self.assertEqual(len(errors), 1, [str(exc) for exc in errors])
            self.assertIsInstance(errors[0], local_node.LocalNodeError)
            self.assertEqual(errors[0].code, 6)
            self.assertNotIn("control.token", str(errors[0]))
            state = manager.read_state()
            with open(os.path.join(state["control_dir"], "stopped.json"),
                      encoding="utf-8") as handle:
                stopped = json.load(handle)
            self.assertEqual(state["state"], "stopped")
            self.assertEqual(state["exit_kind"], "graceful")
            self.assertEqual(state["last_exit_code"], 0)
            self.assertEqual(stopped["state"], "stopped")
            self.assertEqual(stopped["exit_kind"], "graceful")
            self.assertEqual(stopped["exit_code"], 0)
            self.assertEqual(stopped["launch_id"], state["launch_id"])
            self.assertEqual(stopped["pid"], state["pid"])
            self.assertEqual(stopped["process_start_time"],
                             state["process_start_time"])
            self.assertGreater(state["state_revision"],
                               stopping_state["state_revision"])
            self.assertFalse(os.path.exists(os.path.join(state["control_dir"],
                                                         "control.token")))
            self.assertFalse(os.path.exists(os.path.join(state["control_dir"],
                                                         "stop.request")))

    def test_duplicate_sigterm_drains_inflight_request_and_publishes_terminal(self):
        with tempfile.TemporaryDirectory(prefix="cpppools_duplicate_signal_") as root:
            db_path = os.path.join(root, "node.db")
            conn = sqlite3.connect(db_path)
            conn.execute("CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL)")
            conn.close()
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", 0))
                port = probe.getsockname()[1]
            config = os.path.join(root, "server.json")
            with open(config, "w", encoding="utf-8") as handle:
                json.dump({"server": {"port": port, "worker_threads": 2},
                           "datasources": [{"name": "main", "type": "sqlite",
                                            "default": True, "pool_size": 1,
                                            "sqlite": {"path": db_path}}]}, handle)

            manager = local_node.LocalNodeManager(os.path.join(root, "state"), "demo")
            running = manager.start(config, binary=os.environ["CPL_TEST_SERVER"],
                                    workdir=root, timeout=10)
            process = manager._process
            control_dir = running["control_dir"]
            pending = socket.create_connection(("127.0.0.1", port), timeout=3)
            pending.settimeout(3)
            pending.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\n")
            try:
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    try:
                        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=1)
                        conn.request("GET", "/api/v1/stats")
                        response = conn.getresponse()
                        stats = json.loads(response.read())
                        conn.close()
                        if stats.get("active_requests", 0) >= 2:
                            break
                    except (OSError, http.client.HTTPException, ValueError):
                        pass
                    time.sleep(0.03)
                else:
                    self.fail("partial request never entered the server's in-flight set")

                os.kill(process.pid, signal.SIGTERM)
                # Wait for the 200 ms accept checkpoint to begin draining, while
                # the accepted partial request remains in flight, then deliver
                # the duplicate. The old handler exits 143.
                time.sleep(0.25)
                self.assertIsNone(process.poll(), "the partial request should still be draining")
                os.kill(process.pid, signal.SIGTERM)
                return_code = process.wait(timeout=10)
                self.assertEqual(return_code, 0,
                                 "duplicate SIGTERM must remain graceful while draining")
            finally:
                pending.close()
                if process.poll() is None:
                    with contextlib.suppress(local_node.ExitOutcome):
                        manager.stop(timeout=2, force=True)

            with open(os.path.join(control_dir, "stopped.json"), encoding="utf-8") as handle:
                stopped = json.load(handle)
            state = manager.read_state()
            self.assertEqual(stopped["launch_id"], state["launch_id"])
            self.assertEqual(stopped["state"], "stopped")
            self.assertEqual(stopped["exit_kind"], "graceful")

    def test_start_status_stop_real_server(self):
        with tempfile.TemporaryDirectory(prefix="cpppools_local_node_") as root:
            db_path = os.path.join(root, "node.db")
            conn = sqlite3.connect(db_path)
            conn.execute("CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL)")
            conn.close()
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]
            config = os.path.join(root, "server.json")
            with open(config, "w", encoding="utf-8") as handle:
                json.dump({"server": {"port": port, "worker_threads": 1},
                           "datasources": [{"name": "main", "type": "sqlite",
                                            "default": True, "pool_size": 1,
                                            "sqlite": {"path": db_path}}]}, handle)
            manager = local_node.LocalNodeManager(os.path.join(root, "state"), "demo")
            running = manager.start(config, binary=os.environ["CPL_TEST_SERVER"],
                                    workdir=root, timeout=10)
            self.assertEqual(running["state"], "running")
            self.assertTrue(running["process_alive"])
            control_dir = running["control_dir"]
            self.assertTrue(os.path.isfile(os.path.join(control_dir, "control.meta")))
            self.assertTrue(os.path.isfile(os.path.join(control_dir, "control.token")))
            status = manager.status()
            self.assertTrue(status["identity_verified"])
            stopped = manager.stop(timeout=10)
            self.assertEqual(stopped["state"], "stopped")
            self.assertFalse(stopped["process_alive"])

    def test_real_duplicate_start_requires_identical_configuration(self):
        with tempfile.TemporaryDirectory(prefix="cpppools_duplicate_start_") as root:
            db_path = os.path.join(root, "node.db")
            conn = sqlite3.connect(db_path)
            conn.execute("CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL)")
            conn.close()
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]
            config = os.path.join(root, "server.json")
            config_bytes = json.dumps({
                "server": {"port": port, "worker_threads": 1},
                "datasources": [{"name": "main", "type": "sqlite",
                                 "default": True, "pool_size": 1,
                                 "sqlite": {"path": db_path}}],
            }).encode("utf-8")
            with open(config, "wb") as handle:
                handle.write(config_bytes)
            other_config = os.path.join(root, "other.json")
            with open(other_config, "wb") as handle:
                handle.write(config_bytes)
            other_workdir = os.path.join(root, "other-workdir")
            os.mkdir(other_workdir)
            other_binary = os.path.join(root, "other-cpppools-server")
            shutil.copy2(os.environ["CPL_TEST_SERVER"], other_binary)
            os.chmod(other_binary, 0o755)

            state_root = os.path.join(root, "state")
            manager = local_node.LocalNodeManager(state_root, "demo")
            running = manager.start(config, binary=os.environ["CPL_TEST_SERVER"],
                                    workdir=root, timeout=10)
            original_identity = (running["pid"], running["launch_id"])
            env = os.environ.copy()
            env["PYTHONPATH"] = os.path.abspath(
                os.path.join(os.path.dirname(__file__), ".."))

            def cpl(*args):
                return subprocess.run(
                    [sys.executable, "-m", "cpppools"] + list(args),
                    env=env, text=True, capture_output=True, timeout=15,
                    check=False)

            try:
                repeated = cpl("start", "-n", "demo", "-R", state_root,
                                "-c", config, "-b", os.environ["CPL_TEST_SERVER"],
                                "-W", root, "-o", "json")
                self.assertEqual(repeated.returncode, 0, repeated.stderr)
                repeated_state = json.loads(repeated.stdout)
                self.assertTrue(repeated_state["already_running"])
                self.assertEqual((repeated_state["pid"], repeated_state["launch_id"]),
                                 original_identity)

                wrong_path = cpl(
                    "start", "-n", "demo", "-R", state_root, "-c", other_config,
                    "-b", os.environ["CPL_TEST_SERVER"], "-W", root, "-o", "json")
                self.assertEqual(wrong_path.returncode, 6)
                self.assertEqual(json.loads(wrong_path.stderr)["kind"], "start_error")

                with mock.patch.object(subprocess, "Popen", wraps=subprocess.Popen) as popen:
                    variations = [
                        ("changed bytes", config, os.environ["CPL_TEST_SERVER"], root,
                         json.dumps({"server": {"port": port, "worker_threads": 2},
                                     "datasources": [{"name": "main", "type": "sqlite",
                                                      "default": True, "pool_size": 1,
                                                      "sqlite": {"path": db_path}}]}).encode("utf-8")),
                        ("different workdir", config, os.environ["CPL_TEST_SERVER"],
                         other_workdir, config_bytes),
                        ("different binary", config, other_binary, root, config_bytes),
                    ]
                    for label, requested_config, requested_binary, requested_workdir, contents in variations:
                        with self.subTest(mismatch=label):
                            with open(config, "wb") as handle:
                                handle.write(contents)
                            with self.assertRaises(local_node.LocalNodeError) as raised:
                                manager.start(requested_config, binary=requested_binary,
                                              workdir=requested_workdir)
                            self.assertEqual(raised.exception.code, 6)
                            state = manager.read_state()
                            self.assertEqual((state["pid"], state["launch_id"]),
                                             original_identity)
                            self.assertIsNone(manager._process.poll())
                    popen.assert_not_called()
            finally:
                with contextlib.suppress(local_node.ExitOutcome):
                    manager.stop(timeout=10)

    def test_foreground_external_sigkill_is_not_reported_as_graceful(self):
        with tempfile.TemporaryDirectory(prefix="cpppools_foreground_kill_") as root:
            db_path = os.path.join(root, "node.db")
            conn = sqlite3.connect(db_path)
            conn.execute("CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL)")
            conn.close()
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]
            config = os.path.join(root, "server.json")
            with open(config, "w", encoding="utf-8") as handle:
                json.dump({"server": {"port": port, "worker_threads": 1},
                           "datasources": [{"name": "main", "type": "sqlite",
                                            "default": True, "pool_size": 1,
                                            "sqlite": {"path": db_path}}]}, handle)

            manager = local_node.LocalNodeManager(os.path.join(root, "state"), "demo")
            results, errors = [], []

            def start_foreground():
                try:
                    results.append(manager.start(
                        config, binary=os.environ["CPL_TEST_SERVER"],
                        workdir=root, timeout=10, foreground=True))
                except BaseException as exc:
                    errors.append(exc)

            thread = threading.Thread(target=start_foreground)
            thread.start()
            try:
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    try:
                        state = manager.read_state()
                    except local_node.LocalStateError:
                        state = None
                    if state is not None and state.get("state") == "running":
                        break
                    if not thread.is_alive():
                        self.fail("foreground start exited before reaching running")
                    time.sleep(0.02)
                else:
                    self.fail("foreground start did not reach running")

                os.kill(state["pid"], signal.SIGKILL)
                thread.join(timeout=10)
            finally:
                if manager._process is not None and manager._process.poll() is None:
                    manager._process.kill()
                    manager._process.wait(timeout=5)
                if thread.is_alive():
                    thread.join(timeout=5)

            self.assertFalse(thread.is_alive())
            self.assertEqual(results, [])
            self.assertEqual(len(errors), 1)
            self.assertIsInstance(errors[0], local_node.ExitOutcome)
            self.assertEqual(errors[0].code, 10)
            self.assertEqual(errors[0].result["state"], "crashed")
            self.assertEqual(errors[0].result["exit_kind"], "unexpected")
            self.assertEqual(manager.read_state()["exit_kind"], "unexpected")

    def test_cpl_start_status_stop_subprocess_contract(self):
        with tempfile.TemporaryDirectory(prefix="cpppools_cli_local_") as root:
            db_path = os.path.join(root, "node.db")
            conn = sqlite3.connect(db_path)
            conn.execute("CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL)")
            conn.close()
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]
            config = os.path.join(root, "server.json")
            with open(config, "w", encoding="utf-8") as handle:
                json.dump({"server": {"port": port, "worker_threads": 1},
                           "datasources": [{"name": "main", "type": "sqlite",
                                            "default": True, "pool_size": 1,
                                            "sqlite": {"path": db_path}}]}, handle)
            env = os.environ.copy()
            env["PYTHONPATH"] = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

            def cpl(*args):
                return subprocess.run([sys.executable, "-m", "cpppools"] + list(args),
                                      env=env, text=True, capture_output=True,
                                      timeout=15, check=False)

            started = cpl("start", "-n", "demo", "-R", os.path.join(root, "state"),
                          "-c", config, "-b", os.environ["CPL_TEST_SERVER"],
                          "-W", root, "-o", "json")
            self.assertEqual(started.returncode, 0, started.stderr)
            self.assertEqual(json.loads(started.stdout)["state"], "running")
            status = cpl("status", "-n", "demo", "-R", os.path.join(root, "state"),
                         "-o", "json")
            self.assertEqual(status.returncode, 0, status.stderr)
            self.assertTrue(json.loads(status.stdout)["identity_verified"])
            stopped = cpl("stop", "-n", "demo", "-R", os.path.join(root, "state"),
                          "-o", "json")
            self.assertEqual(stopped.returncode, 0, stopped.stderr)
            self.assertEqual(json.loads(stopped.stdout)["state"], "stopped")

    def test_force_stop_cli_returns_9_with_result_and_structured_error(self):
        with tempfile.TemporaryDirectory(prefix="cpppools_force_cli_") as root:
            db_path = os.path.join(root, "node.db")
            conn = sqlite3.connect(db_path)
            conn.execute("CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL)")
            conn.close()
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]
            config = os.path.join(root, "server.json")
            with open(config, "w", encoding="utf-8") as handle:
                json.dump({"server": {"port": port, "worker_threads": 1},
                           "datasources": [{"name": "main", "type": "sqlite",
                                            "default": True, "pool_size": 1,
                                            "sqlite": {"path": db_path}}]}, handle)

            state_root = os.path.join(root, "state")
            manager = local_node.LocalNodeManager(state_root, "demo")
            running = manager.start(config, binary=os.environ["CPL_TEST_SERVER"],
                                    workdir=root, timeout=10)
            with open(os.path.join(running["control_dir"], "control.token"),
                      encoding="ascii") as handle:
                token = handle.read()
            env = os.environ.copy()
            env["PYTHONPATH"] = os.path.abspath(
                os.path.join(os.path.dirname(__file__), ".."))
            try:
                stopped = subprocess.run(
                    [sys.executable, "-m", "cpppools", "stop", "-n", "demo",
                     "-R", state_root, "--force", "-o", "json"],
                    env=env, text=True, capture_output=True, timeout=10, check=False)
                if manager._process is not None:
                    manager._process.wait(timeout=10)
            finally:
                if manager._process is not None and manager._process.poll() is None:
                    with contextlib.suppress(BaseException):
                        manager.stop(timeout=2, force=True)

            self.assertEqual(stopped.returncode, 9, stopped.stderr)
            result = json.loads(stopped.stdout)
            error = json.loads(stopped.stderr)
            self.assertEqual(result["state"], "crashed")
            self.assertEqual(result["exit_kind"], "forced")
            self.assertEqual(error["kind"], "forced_stop")
            self.assertEqual(error["exit_code"], 9)
            self.assertNotIn(token, stopped.stdout + stopped.stderr)
            self.assertEqual(manager.read_state()["force_delivery"], "accepted")
            self.assertFalse(os.path.exists(os.path.join(
                running["control_dir"], "control.token")))

    def test_invalid_control_token_does_not_stop_process(self):
        with tempfile.TemporaryDirectory(prefix="cpppools_control_auth_") as root:
            db_path = os.path.join(root, "node.db")
            conn = sqlite3.connect(db_path)
            conn.execute("CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL)")
            conn.close()
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]
            config = os.path.join(root, "server.json")
            with open(config, "w", encoding="utf-8") as handle:
                json.dump({"server": {"port": port, "worker_threads": 1},
                           "datasources": [{"name": "main", "type": "sqlite",
                                            "default": True, "pool_size": 1,
                                            "sqlite": {"path": db_path}}]}, handle)
            manager = local_node.LocalNodeManager(os.path.join(root, "state"), "demo")
            running = manager.start(config, binary=os.environ["CPL_TEST_SERVER"],
                                    workdir=root, timeout=10)
            local_node._atomic_json(
                os.path.join(running["control_dir"], "stop.request"), {
                    "protocol_version": 1,
                    "launch_id": running["launch_id"],
                    "requested_at": local_node._now(),
                    "token": "0" * 64,
                })
            try:
                time.sleep(0.5)
                self.assertIsNone(manager._process.poll(),
                                  "server accepted a stop request with the wrong token")
                self.assertEqual(manager.read_state()["state"], "running")
            finally:
                with contextlib.suppress(local_node.ExitOutcome):
                    manager.stop(timeout=5, force=True)

    def test_bad_token_stop_then_external_sigkill_is_unexpected_and_exit_10(self):
        with tempfile.TemporaryDirectory(prefix="cpppools_bad_stop_kill_") as root:
            db_path = os.path.join(root, "node.db")
            conn = sqlite3.connect(db_path)
            conn.execute("CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL)")
            conn.close()
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]
            config = os.path.join(root, "server.json")
            with open(config, "w", encoding="utf-8") as handle:
                json.dump({"server": {"port": port, "worker_threads": 1},
                           "datasources": [{"name": "main", "type": "sqlite",
                                            "default": True, "pool_size": 1,
                                            "sqlite": {"path": db_path}}]}, handle)

            state_root = os.path.join(root, "state")
            manager = local_node.LocalNodeManager(state_root, "demo")
            running = manager.start(config, binary=os.environ["CPL_TEST_SERVER"],
                                    workdir=root, timeout=10)
            control_dir = running["control_dir"]
            env = os.environ.copy()
            env["PYTHONPATH"] = os.path.abspath(
                os.path.join(os.path.dirname(__file__), ".."))
            script = """
import os
import sys
from cpppools import cli, local_node

def send_bad_request(state):
    local_node._atomic_json(os.path.join(state[\"control_dir\"], \"stop.request\"), {
        \"protocol_version\": 1,
        \"launch_id\": state[\"launch_id\"],
        \"requested_at\": local_node._now(),
        \"token\": \"0\" * 64,
    })
    return True

local_node._write_stop_request = send_bad_request
raise SystemExit(cli.main([\"stop\", \"-n\", \"demo\", \"-R\", sys.argv[1],
                           \"-t\", \"5\", \"-o\", \"json\"]))
"""
            stop_cli = subprocess.Popen(
                [sys.executable, "-c", script, state_root], env=env,
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    try:
                        request = local_node._read_control_json(
                            control_dir, "stop.request", missing_ok=True)
                    except FileNotFoundError:
                        request = None
                    if request is not None and request.get("token") == "0" * 64:
                        break
                    if stop_cli.poll() is not None:
                        self.fail("stop CLI exited before publishing the bad-token request")
                    time.sleep(0.02)
                else:
                    self.fail("stop CLI did not publish the bad-token request")

                os.kill(running["pid"], signal.SIGKILL)
                manager._process.wait(timeout=10)
                stdout, stderr = stop_cli.communicate(timeout=10)
            finally:
                if manager._process is not None and manager._process.poll() is None:
                    with contextlib.suppress(BaseException):
                        manager.stop(timeout=2, force=True)
                if stop_cli.poll() is None:
                    stop_cli.kill()
                    stop_cli.communicate(timeout=5)

            self.assertEqual(stop_cli.returncode, 10, stderr)
            result = json.loads(stdout)
            error = json.loads(stderr)
            self.assertEqual(result["state"], "crashed")
            self.assertEqual(result["exit_kind"], "unexpected")
            self.assertEqual(result["last_exit_code"], None)
            self.assertEqual(error["kind"], "local_state_error")
            self.assertEqual(error["exit_code"], 10)
            self.assertNotIn("0" * 64, stdout + stderr)
            persisted = manager.read_state()
            self.assertEqual(persisted["state"], "crashed")
            self.assertEqual(persisted["exit_kind"], "unexpected")


if __name__ == "__main__":
    unittest.main()
