"""Small POSIX-focused manager for locally launched CppPools processes.

The manager owns only the instance metadata and process it launched.  The
server remains responsible for graceful drain; this layer waits for health and
observes exit, then publishes an atomic terminal state.
"""

import errno
import contextlib
import json
import os
import re
import signal
import shutil
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
import uuid


_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$")


class LocalNodeError(Exception):
    code = 6
    kind = "start_error"


class LocalStateError(LocalNodeError):
    code = 10
    kind = "local_state_error"


class IdentityError(LocalNodeError):
    code = 8
    kind = "identity_error"


class StopTimeout(LocalNodeError):
    code = 7
    kind = "stop_timeout"


def _now():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def _pid_alive(pid):
    if not isinstance(pid, int) or pid <= 0:
        return False
    if os.name == "posix":
        try:
            with open("/proc/%d/stat" % pid, "r", encoding="ascii") as handle:
                fields = handle.read().split()
            if len(fields) > 2 and fields[2] == "Z":
                return False
        except (OSError, UnicodeError):
            pass
    try:
        os.kill(pid, 0)
    except OSError as exc:
        return exc.errno == errno.EPERM
    return True


def _process_executable(pid):
    if os.name == "posix":
        try:
            return os.path.realpath("/proc/%d/exe" % pid)
        except OSError:
            return None
    return None


def _process_start_time(pid):
    if os.name == "posix":
        try:
            with open("/proc/%d/stat" % pid, "r", encoding="ascii") as handle:
                text = handle.read()
            fields = text.rsplit(") ", 1)[1].split()
            return fields[19]
        except (OSError, IndexError, UnicodeError):
            return None
    return None


def _identity_matches(state):
    pid = state.get("pid")
    if not _pid_alive(pid):
        return False
    expected = state.get("binary")
    actual = _process_executable(pid)
    if expected and actual:
        try:
            executable_matches = os.path.samefile(expected, actual)
        except OSError:
            executable_matches = os.path.realpath(expected) == os.path.realpath(actual)
    else:
        executable_matches = False
    if not executable_matches:
        return False
    expected_start = state.get("process_start_time")
    if expected_start is not None:
        return str(expected_start) == str(_process_start_time(pid))
    return True


def _reap_child(pid):
    if os.name != "posix" or not isinstance(pid, int) or pid <= 0:
        return None
    try:
        waited, status = os.waitpid(pid, os.WNOHANG)
    except (ChildProcessError, OSError):
        return None
    if waited != pid:
        return None
    if os.WIFEXITED(status):
        return os.WEXITSTATUS(status)
    if os.WIFSIGNALED(status):
        return 128 + os.WTERMSIG(status)
    return None


def _atomic_json(path, document):
    directory = os.path.dirname(path)
    os.makedirs(directory, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".%s." % os.path.basename(path),
                                     dir=directory, text=True)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as handle:
            json.dump(document, handle, ensure_ascii=False, separators=(",", ":"))
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


class LocalNodeManager:
    def __init__(self, state_root, name="default"):
        if not isinstance(name, str) or not _NAME.fullmatch(name):
            raise LocalStateError("invalid instance name")
        self.state_root = os.path.abspath(os.path.expanduser(state_root))
        self.name = name
        self.instance_dir = os.path.join(self.state_root, name)
        self.state_path = os.path.join(self.instance_dir, "state.json")
        self.lock_path = os.path.join(self.instance_dir, "instance.lock")
        self._process = None

    def read_state(self):
        try:
            with open(self.state_path, "r", encoding="utf-8") as handle:
                state = json.load(handle)
        except FileNotFoundError:
            raise LocalStateError("instance state does not exist") from None
        except (OSError, ValueError) as exc:
            raise LocalStateError("cannot read instance state: %s" % exc) from None
        if not isinstance(state, dict) or state.get("protocol_version") != 1:
            raise LocalStateError("instance state is malformed")
        return state

    def write_state(self, state):
        if not isinstance(state, dict) or state.get("protocol_version") != 1:
            raise LocalStateError("instance state is malformed")
        _atomic_json(self.state_path, state)

    @contextlib.contextmanager
    def _exclusive(self):
        os.makedirs(self.instance_dir, exist_ok=True)
        handle = open(self.lock_path, "a+")
        try:
            if os.name == "posix":
                import fcntl
                try:
                    fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                except (BlockingIOError, OSError) as exc:
                    raise LocalStateError("instance is busy") from exc
            yield
        finally:
            if os.name == "posix":
                import fcntl
                fcntl.flock(handle.fileno(), fcntl.LOCK_UN)
            handle.close()

    def _status_for(self, state):
        alive = _pid_alive(state.get("pid"))
        identity = _identity_matches(state) if alive else False
        result = dict(state)
        result["process_alive"] = alive
        result["identity_verified"] = identity
        result["last_known_state"] = state.get("state")
        return result

    def status(self):
        return self._status_for(self.read_state())

    def _config(self, path):
        try:
            with open(path, "r", encoding="utf-8") as handle:
                document = json.load(handle)
            port = int(document.get("server", {}).get("port", 8080))
        except (OSError, ValueError, TypeError, AttributeError) as exc:
            raise LocalStateError("cannot read server config: %s" % exc) from None
        if not 1 <= port <= 65535:
            raise LocalStateError("server config has invalid port")
        return port

    def resolve_binary(self, binary=None):
        candidate = binary or "cpppools_server"
        if os.path.isabs(candidate) or os.path.dirname(candidate):
            resolved = os.path.abspath(os.path.expanduser(candidate))
        else:
            resolved = shutil.which(candidate)
        if not resolved:
            raise LocalNodeError("server binary is not executable: %s" % candidate)
        resolved = os.path.realpath(resolved)
        if not os.path.isfile(resolved) or not os.access(resolved, os.X_OK):
            raise LocalNodeError("server binary is not executable: %s" % resolved)
        return resolved

    def _health(self, endpoint, timeout):
        try:
            with urllib.request.urlopen(endpoint + "/health", timeout=timeout) as response:
                return response.status == 200 and response.read(4096)
        except (OSError, urllib.error.URLError, ValueError):
            return False

    def _base_state(self, launch_id, config, binary, workdir, endpoint, log_file):
        return {
            "protocol_version": 1, "state_revision": 1, "name": self.name,
            "launch_id": launch_id, "pid": None, "process_start_time": None,
            "binary": binary, "config": config, "workdir": workdir,
            "endpoint": endpoint, "log_file": log_file, "state": "starting",
            "exit_kind": None, "last_exit_code": None, "updated_at": _now(),
        }

    def start(self, config, binary=None, workdir=None, log_file=None,
              timeout=15.0, foreground=False):
        if os.name == "nt":
            raise LocalNodeError("local lifecycle is unavailable on Windows until the control channel stage")
        config = os.path.abspath(os.path.expanduser(config))
        if not os.path.isfile(config):
            raise LocalNodeError("config file does not exist: %s" % config)
        binary = self.resolve_binary(binary)
        workdir = os.path.abspath(os.path.expanduser(workdir or os.getcwd()))
        port = self._config(config)
        endpoint = "http://127.0.0.1:%d" % port
        with self._exclusive():
            try:
                old = self.read_state()
                if old.get("state") in ("starting", "running", "stopping"):
                    if self._status_for(old)["identity_verified"]:
                        return dict(self._status_for(old), already_running=True)
                    if self._status_for(old)["process_alive"]:
                        raise LocalStateError("instance is active but identity cannot be verified")
            except LocalStateError as exc:
                if "does not exist" not in str(exc):
                    raise
            launch_id = uuid.uuid4().hex
            launch_dir = os.path.join(self.instance_dir, "launches", launch_id)
            os.makedirs(launch_dir, exist_ok=True)
            log_file = os.path.abspath(log_file or os.path.join(self.instance_dir, "server.log"))
            state = self._base_state(launch_id, config, binary, workdir, endpoint, log_file)
            self.write_state(state)
        log = open(log_file, "ab")
        try:
            process = subprocess.Popen([binary, "--config", config], cwd=workdir,
                                       stdout=log, stderr=subprocess.STDOUT,
                                       start_new_session=True)
            self._process = process
        except OSError as exc:
            log.close()
            state.update(state="crashed", last_exit_code=None, exit_kind="unexpected",
                         state_revision=2, updated_at=_now())
            self.write_state(state)
            raise LocalNodeError("failed to start server: %s" % exc) from None
        with self._exclusive():
            state.update(pid=process.pid,
                         process_start_time=_process_start_time(process.pid),
                         state_revision=2, updated_at=_now())
            self.write_state(state)
        deadline = time.monotonic() + max(float(timeout), 0.01)
        ready = False
        while time.monotonic() < deadline:
            if process.poll() is not None:
                state.update(state="crashed", last_exit_code=process.returncode,
                             exit_kind="unexpected", state_revision=3, updated_at=_now())
                self.write_state(state)
                log.close()
                raise LocalNodeError("server exited before health became ready")
            if self._health(endpoint, min(0.2, max(deadline - time.monotonic(), 0.01))):
                ready = True
                break
            time.sleep(0.05)
        if not ready:
            try:
                process.send_signal(signal.SIGTERM)
            except OSError:
                pass
            with self._exclusive():
                state.update(state="stopping", state_revision=3, updated_at=_now())
                self.write_state(state)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                log.close()
                raise StopTimeout("server did not stop after startup timeout")
            state.update(state="crashed", last_exit_code=process.returncode,
                         exit_kind="unexpected", state_revision=4, updated_at=_now())
            with self._exclusive():
                self.write_state(state)
            log.close()
            raise LocalNodeError("server did not become healthy before timeout")
        with self._exclusive():
            state.update(state="running", state_revision=3, updated_at=_now())
            self.write_state(state)
        log.close()
        result = self._status_for(state)
        result["already_running"] = False
        if foreground:
            process.wait()
            state.update(state="stopped" if process.returncode == 0 else "crashed",
                         exit_kind="graceful" if process.returncode == 0 else "unexpected",
                         last_exit_code=process.returncode,
                         state_revision=state["state_revision"] + 1, updated_at=_now())
            with self._exclusive():
                self.write_state(state)
            result = self._status_for(state)
        return result

    def stop(self, timeout=10.0, force=False):
        with self._exclusive():
            state = self.read_state()
            result = self._status_for(state)
        if state.get("state") == "stopped" and not result["process_alive"]:
            return result
        if not result["identity_verified"]:
            if result["process_alive"]:
                raise IdentityError("refusing to stop an unverified process")
            raise LocalStateError("instance process is not running")
        pid = state["pid"]
        with self._exclusive():
            state.update(state="stopping", state_revision=state.get("state_revision", 0) + 1,
                         updated_at=_now())
            self.write_state(state)
        try:
            os.kill(pid, signal.SIGKILL if force else signal.SIGTERM)
        except OSError as exc:
            raise IdentityError("failed to signal verified process: %s" % exc) from None
        deadline = time.monotonic() + max(float(timeout), 0.01)
        while time.monotonic() < deadline:
            if not _pid_alive(pid):
                exit_code = _reap_child(pid)
                if self._process is not None and exit_code is not None:
                    self._process.returncode = exit_code
                state.update(state="crashed" if force else "stopped",
                             exit_kind="forced" if force else "graceful",
                             last_exit_code=exit_code,
                             state_revision=state["state_revision"] + 1,
                             updated_at=_now())
                with self._exclusive():
                    self.write_state(state)
                return self._status_for(state)
            time.sleep(0.05)
        raise StopTimeout("server did not stop before timeout")
