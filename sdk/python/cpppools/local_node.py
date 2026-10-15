"""Manager for locally launched CppPools processes.

The manager owns only the instance metadata and process it launched.  The
server remains responsible for graceful drain; this layer waits for health and
observes exit, then publishes an atomic terminal state.
"""

import errno
import contextlib
import hashlib
import json
import os
import re
import signal
import secrets
import shutil
import stat
import subprocess
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid


_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$")
_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_CONTROL_MAX_BYTES = 4096
_FILE_ATTRIBUTE_REPARSE_POINT = 0x400
_FILE_ATTRIBUTE_DIRECTORY = 0x10
_FORCE_SIGNAL = getattr(signal, "SIGKILL", 9)

_STATE_TRANSITIONS = {
    "starting": {"running", "stopping", "stopped", "crashed"},
    "running": {"stopping", "stopped", "crashed"},
    "stopping": {"stopped", "crashed"},
    "stopped": {"crashed"},
    "crashed": {"stopped"},
}


class LocalNodeError(Exception):
    code = 6
    kind = "start_error"

    def __init__(self, message, result=None):
        super().__init__(message)
        self.result = result


class LocalStateError(LocalNodeError):
    code = 10
    kind = "local_state_error"


class IdentityError(LocalNodeError):
    code = 8
    kind = "identity_error"


class StopTimeout(LocalNodeError):
    code = 7
    kind = "stop_timeout"

    def __init__(self, message, result=None):
        super().__init__(message)
        self.result = result


class ExitOutcome(LocalNodeError):
    def __init__(self, message, code, kind, result):
        super().__init__(message)
        self.code = code
        self.kind = kind
        self.result = result


def _now():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def _config_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(65536), b""):
            digest.update(block)
    return digest.hexdigest()


def _pid_alive(pid):
    if not isinstance(pid, int) or pid <= 0:
        return False
    if os.name == "nt":
        if pid > 0xFFFFFFFF:
            return False
        # On Windows signal 0 is CTRL_C_EVENT, not a harmless existence probe.
        import ctypes
        from ctypes import wintypes

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
        kernel32.OpenProcess.restype = wintypes.HANDLE
        kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
        kernel32.WaitForSingleObject.restype = wintypes.DWORD
        kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
        kernel32.CloseHandle.restype = wintypes.BOOL
        handle = kernel32.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE
        if not handle:
            error = ctypes.get_last_error()
            if error == 5:  # ERROR_ACCESS_DENIED, like EPERM
                return True
            if error == 87:  # ERROR_INVALID_PARAMETER: no process with this PID.
                return False
            raise ctypes.WinError(error)
        try:
            result = kernel32.WaitForSingleObject(handle, 0)
            if result == 0xFFFFFFFF:  # WAIT_FAILED
                raise ctypes.WinError(ctypes.get_last_error())
            return result == 258  # WAIT_TIMEOUT means the process is still alive.
        finally:
            kernel32.CloseHandle(handle)
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
    if os.name == "nt":
        if type(pid) is not int or pid <= 0 or pid > 0xFFFFFFFF:
            return None
        import ctypes
        from ctypes import wintypes
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
        kernel32.OpenProcess.restype = wintypes.HANDLE
        kernel32.QueryFullProcessImageNameW.argtypes = (
            wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
            ctypes.POINTER(wintypes.DWORD))
        kernel32.QueryFullProcessImageNameW.restype = wintypes.BOOL
        kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
        kernel32.CloseHandle.restype = wintypes.BOOL
        handle = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            return None
        try:
            size = wintypes.DWORD(32768)
            buffer = ctypes.create_unicode_buffer(size.value)
            if not kernel32.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):
                return None
            return os.path.realpath(buffer.value)
        finally:
            kernel32.CloseHandle(handle)
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
    if os.name == "nt":
        if type(pid) is not int or pid <= 0 or pid > 0xFFFFFFFF:
            return None
        import ctypes
        from ctypes import wintypes
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
        kernel32.OpenProcess.restype = wintypes.HANDLE
        kernel32.GetProcessTimes.argtypes = (
            wintypes.HANDLE, ctypes.POINTER(wintypes.FILETIME),
            ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME),
            ctypes.POINTER(wintypes.FILETIME))
        kernel32.GetProcessTimes.restype = wintypes.BOOL
        kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
        kernel32.CloseHandle.restype = wintypes.BOOL
        handle = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            return None
        try:
            created = wintypes.FILETIME()
            exited = wintypes.FILETIME()
            kernel = wintypes.FILETIME()
            user = wintypes.FILETIME()
            if not kernel32.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited),
                                            ctypes.byref(kernel), ctypes.byref(user)):
                return None
            value = (created.dwHighDateTime << 32) | created.dwLowDateTime
            return str(value)
        finally:
            kernel32.CloseHandle(handle)
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
    if expected_start is None:
        return False
    return str(expected_start) == str(_process_start_time(pid))


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


def _pidfd_functions():
    if os.name != "posix":
        return None, None
    return (getattr(os, "pidfd_open", None),
            getattr(signal, "pidfd_send_signal", None))


def _open_verified_windows_process(state, terminate=False):
    import ctypes
    from ctypes import wintypes

    pid = state.get("pid")
    if type(pid) is not int or pid <= 0 or pid > 0xFFFFFFFF:
        raise IdentityError("invalid Windows process identity")
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.TerminateProcess.argtypes = (wintypes.HANDLE, wintypes.UINT)
    kernel32.TerminateProcess.restype = wintypes.BOOL
    kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
    kernel32.WaitForSingleObject.restype = wintypes.DWORD
    kernel32.GetProcessTimes.argtypes = (
        wintypes.HANDLE, ctypes.POINTER(wintypes.FILETIME),
        ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME),
        ctypes.POINTER(wintypes.FILETIME))
    kernel32.GetProcessTimes.restype = wintypes.BOOL
    kernel32.QueryFullProcessImageNameW.argtypes = (
        wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD))
    kernel32.QueryFullProcessImageNameW.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel32.CloseHandle.restype = wintypes.BOOL
    access = 0x1000 | 0x00100000 | (0x0001 if terminate else 0)
    handle = kernel32.OpenProcess(access, False, pid)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        created, exited, kernel, user = (wintypes.FILETIME() for _ in range(4))
        size = wintypes.DWORD(32768)
        image = ctypes.create_unicode_buffer(size.value)
        if (not kernel32.GetProcessTimes(handle, ctypes.byref(created),
                                        ctypes.byref(exited), ctypes.byref(kernel),
                                        ctypes.byref(user)) or
                not kernel32.QueryFullProcessImageNameW(handle, 0, image, ctypes.byref(size))):
            raise IdentityError("cannot verify the target process handle")
        created_at = str((created.dwHighDateTime << 32) | created.dwLowDateTime)
        expected_binary = state.get("binary")
        if (created_at != str(state.get("process_start_time")) or not expected_binary or
                os.path.normcase(os.path.realpath(image.value)) !=
                os.path.normcase(os.path.realpath(expected_binary))):
            raise IdentityError("process identity changed before control delivery")
        return kernel32, handle
    except BaseException:
        kernel32.CloseHandle(handle)
        raise


def _send_verified_signal(state, sig):
    """Deliver only to the verified process; Windows caller owns returned handle."""
    pid = state.get("pid")
    if type(pid) is not int or pid <= 0:
        raise IdentityError("refusing to signal an invalid process identity")
    if os.name == "nt":
        if sig != _FORCE_SIGNAL:
            raise OSError(errno.ENOTSUP, "Windows graceful stop requires the control request")
        import ctypes
        kernel32, handle = _open_verified_windows_process(state, terminate=True)
        try:
            if not kernel32.TerminateProcess(handle, 137):
                raise ctypes.WinError(ctypes.get_last_error())
            return kernel32, handle
        except BaseException:
            kernel32.CloseHandle(handle)
            raise

    pidfd_open, pidfd_send = _pidfd_functions()
    if callable(pidfd_open) and callable(pidfd_send):
        try:
            pidfd = pidfd_open(pid, 0)
        except OSError as exc:
            if exc.errno not in (errno.ENOSYS, errno.EINVAL):
                raise
        else:
            try:
                if not _identity_matches(state):
                    raise IdentityError("process identity changed before signal delivery")
                pidfd_send(pidfd, sig, None, 0)
                return
            finally:
                with contextlib.suppress(OSError):
                    os.close(pidfd)

    if not _identity_matches(state):
        raise IdentityError("refusing to signal an unverified process")
    os.kill(pid, sig)


def _exit_transition(state, graceful_proof, observed_exit_code):
    """Classify a confirmed process exit using only persisted evidence."""
    delivery = state.get("force_delivery")
    if delivery == "accepted":
        return ({"state": "crashed", "exit_kind": "forced",
                 "last_exit_code": observed_exit_code}, 9)
    if delivery == "pending":
        return ({"state": "crashed", "exit_kind": None,
                 "last_exit_code": observed_exit_code}, 10)
    if graceful_proof and observed_exit_code in (None, 0):
        return ({"state": "stopped", "exit_kind": "graceful",
                 "last_exit_code": 0}, 0)
    return ({"state": "crashed", "exit_kind": "unexpected",
             "last_exit_code": observed_exit_code}, 10)


def _atomic_json(path, document):
    payload = json.dumps(document, ensure_ascii=False, separators=(",", ":")) + "\n"
    _atomic_bytes(path, payload.encode("utf-8"))


def _atomic_bytes(path, payload):
    directory = os.path.dirname(path)
    if os.name == "nt":
        _ensure_windows_directory_tree(directory)
    else:
        os.makedirs(directory, mode=0o700, exist_ok=True)
        os.chmod(directory, 0o700)
    with _control_directory_guard(directory):
        _replace_bytes(path, payload)


def _replace_bytes(path, payload):
    directory = os.path.dirname(path)
    fd, temporary = tempfile.mkstemp(prefix=".%s." % os.path.basename(path),
                                     dir=directory)
    try:
        if os.name == "posix":
            os.fchmod(fd, 0o600)
        with os.fdopen(fd, "wb") as handle:
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


@contextlib.contextmanager
def _control_directory_guard(directory):
    if os.name == "nt":
        from . import _windows_control
        with contextlib.ExitStack() as stack:
            try:
                stack.enter_context(_windows_control.directory(directory))
            except OSError as exc:
                raise LocalStateError("cannot secure local control path: %s" % exc) from None
            yield
    else:
        yield


def _ensure_private_control_dir(directory):
    """Create and verify a private, non-link control directory before spawn."""
    directory = os.path.abspath(directory)
    if os.name == "nt":
        _ensure_windows_directory_tree(directory)
        return directory
    os.makedirs(directory, mode=0o700, exist_ok=True)
    try:
        before = os.lstat(directory)
    except OSError as exc:
        raise LocalStateError("cannot inspect control directory: %s" % exc) from None
    if (not stat.S_ISDIR(before.st_mode) or
            _control_path_is_reparse(directory, before)):
        raise LocalStateError("control path is not a regular directory")

    if os.name == "posix":
        flags = (os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) |
                 getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0))
        try:
            fd = os.open(directory, flags)
        except OSError as exc:
            raise LocalStateError("cannot securely open control directory: %s" % exc) from None
        try:
            opened = os.fstat(fd)
            if (not stat.S_ISDIR(opened.st_mode) or
                    (opened.st_dev, opened.st_ino) != (before.st_dev, before.st_ino) or
                    opened.st_uid != os.getuid()):
                raise LocalStateError("control directory changed or is not owned by this user")
            os.fchmod(fd, 0o700)
            if os.fstat(fd).st_mode & 0o077:
                raise LocalStateError("control directory permissions must be 0700")
        except OSError as exc:
            raise LocalStateError("cannot secure control directory: %s" % exc) from None
        finally:
            os.close(fd)
    return directory


def _ensure_windows_directory_tree(directory):
    """Create each Windows control path component while its parent is pinned."""
    absolute = os.path.abspath(directory)
    drive, tail = os.path.splitdrive(absolute)
    current = drive + os.sep
    parts = [part for part in tail.split(os.sep) if part]
    if not parts:
        raise LocalStateError("control directory cannot be a drive root")
    for index, part in enumerate(parts):
        candidate = os.path.join(current, part)
        leaf = index == len(parts) - 1
        try:
            os.lstat(candidate)
        except FileNotFoundError:
            if current != drive + os.sep:
                from . import _windows_control
                try:
                    with _windows_control.directory(current, secure_leaf=False):
                        try:
                            os.mkdir(candidate)
                        except FileExistsError:
                            pass
                except OSError as exc:
                    raise LocalStateError("cannot secure local control path: %s" % exc) from None
            else:
                try:
                    os.mkdir(candidate)
                except FileExistsError:
                    pass
        if leaf:
            with _control_directory_guard(candidate):
                pass
        current = candidate


def _control_path_is_reparse(path, st):
    attrs = getattr(st, "st_file_attributes", 0)
    if attrs & _FILE_ATTRIBUTE_REPARSE_POINT:
        return True
    if os.name == "posix" and stat.S_ISLNK(st.st_mode):
        return True
    return False


def _read_control_bytes(control_dir, filename, missing_ok=False):
    directory = os.path.abspath(control_dir)
    try:
        directory_stat = os.lstat(directory)
    except FileNotFoundError:
        if missing_ok:
            return None
        raise
    if (not stat.S_ISDIR(directory_stat.st_mode) or
            _control_path_is_reparse(directory, directory_stat)):
        raise LocalStateError("control directory is not a regular directory")
    path = os.path.join(directory, filename)
    if os.name == "posix":
        try:
            fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
        except FileNotFoundError:
            if missing_ok:
                return None
            raise
        try:
            info = os.fstat(fd)
            if not stat.S_ISREG(info.st_mode):
                raise LocalStateError("control file is not a regular file")
            if info.st_size > _CONTROL_MAX_BYTES:
                raise LocalStateError("control file exceeds 4 KiB")
            if info.st_mode & 0o077:
                raise LocalStateError("control file permissions must be private")
            chunks = []
            remaining = _CONTROL_MAX_BYTES + 1
            while remaining:
                part = os.read(fd, min(remaining, 1024))
                if not part:
                    break
                chunks.append(part)
                remaining -= len(part)
            payload = b"".join(chunks)
            if len(payload) > _CONTROL_MAX_BYTES:
                raise LocalStateError("control file exceeds 4 KiB")
            return payload
        finally:
            os.close(fd)

    from . import _windows_control
    with _control_directory_guard(directory):
        try:
            fd = _windows_control.open_file(path)
        except FileNotFoundError:
            if missing_ok:
                return None
            raise
        with os.fdopen(fd, "rb") as handle:
            info = os.fstat(handle.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_size > _CONTROL_MAX_BYTES:
                raise LocalStateError("control file is invalid or exceeds 4 KiB")
            payload = handle.read(_CONTROL_MAX_BYTES + 1)
    if len(payload) > _CONTROL_MAX_BYTES:
        raise LocalStateError("control file exceeds 4 KiB")
    return payload


def _read_control_json(control_dir, filename, missing_ok=False):
    payload = _read_control_bytes(control_dir, filename, missing_ok=missing_ok)
    if payload is None:
        return None
    try:
        document = json.loads(payload.decode("utf-8"))
    except (UnicodeError, ValueError) as exc:
        raise LocalStateError("control file is malformed JSON") from exc
    if (not isinstance(document, dict) or
            type(document.get("protocol_version")) is not int or
            document.get("protocol_version") != 1):
        raise LocalStateError("control file has an invalid protocol version")
    return document


def _control_identity(document, state, expected_state):
    if not isinstance(document, dict):
        return False
    pid = document.get("pid")
    start_time = document.get("process_start_time")
    if (document.get("launch_id") != state.get("launch_id") or
            type(pid) is not int or pid != state.get("pid") or
            not isinstance(start_time, str) or not start_time or
            str(state.get("process_start_time")) != start_time or
            document.get("state") != expected_state):
        return False
    return True


def _verified_handshake(state, require_ready=False):
    control_dir = state.get("control_dir")
    if not isinstance(control_dir, str) or not control_dir:
        return False, None
    try:
        meta = _read_control_json(control_dir, "control.meta")
        starting = _read_control_json(control_dir, "starting.json")
        if (set(meta) != {"protocol_version", "launch_id"} or
                meta.get("launch_id") != state.get("launch_id") or
                not _control_identity(starting, state, "starting")):
            return False, None
        if not _identity_matches(state):
            return False, None
        ready = _read_control_json(control_dir, "ready.json", missing_ok=True)
        if ready is None:
            return (False, None) if require_ready else (True, None)
        if (not _control_identity(ready, state, "ready") or
                type(ready.get("port")) is not int or
                not isinstance(ready.get("node_id"), str) or
                not ready.get("node_id")):
            return False, None
        expected_port = urllib.parse.urlsplit(state.get("endpoint", "")).port
        if expected_port is not None and ready["port"] != expected_port:
            return False, None
        return True, ready
    except (OSError, LocalStateError, ValueError, TypeError):
        return False, None


def _write_stop_request(state):
    control_dir = state.get("control_dir")
    if not isinstance(control_dir, str) or not control_dir:
        return False
    try:
        token = _read_control_bytes(control_dir, "control.token").decode("ascii")
        if not re.fullmatch(r"[0-9a-fA-F]{64,256}", token):
            return False
        _atomic_json(os.path.join(control_dir, "stop.request"), {
            "protocol_version": 1,
            "launch_id": state.get("launch_id"),
            "requested_at": _now(),
            "token": token,
        })
        return True
    except (OSError, UnicodeError, LocalStateError):
        return False


def _verified_terminal(state):
    try:
        stopped = _read_control_json(state.get("control_dir", ""), "stopped.json")
    except (OSError, LocalStateError, ValueError, TypeError):
        return None
    if (set(stopped) != {"protocol_version", "launch_id", "pid",
                         "process_start_time", "state", "exit_kind",
                         "exit_code"} or
            not _control_identity(stopped, state, "stopped") or
            stopped.get("exit_kind") != "graceful" or
            type(stopped.get("exit_code")) is not int):
        return None
    return stopped


class LocalNodeManager:
    def __init__(self, state_root, name="default"):
        if not isinstance(name, str) or not _NAME.fullmatch(name):
            raise LocalStateError("invalid instance name")
        if os.name == "nt" and re.fullmatch(r"CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9]",
                                            name, re.IGNORECASE):
            raise LocalStateError("instance name is a reserved Windows device name")
        self.state_root = os.path.abspath(os.path.expanduser(state_root))
        self.name = name
        self.instance_dir = os.path.join(self.state_root, name)
        self.state_path = os.path.join(self.instance_dir, "state.json")
        self.lock_path = os.path.join(self.instance_dir, "instance.lock")
        self._process = None

    def read_state(self):
        try:
            with _control_directory_guard(self.instance_dir):
                if os.name == "nt":
                    from . import _windows_control
                    handle = os.fdopen(_windows_control.open_file(self.state_path),
                                       "r", encoding="utf-8")
                else:
                    handle = open(self.state_path, "r", encoding="utf-8")
                with handle:
                    state = json.load(handle)
        except FileNotFoundError:
            raise LocalStateError("instance state does not exist") from None
        except (OSError, ValueError) as exc:
            raise LocalStateError("cannot read instance state: %s" % exc) from None
        if (not isinstance(state, dict) or
                type(state.get("protocol_version")) is not int or
                state.get("protocol_version") != 1):
            raise LocalStateError("instance state is malformed")
        pid = state.get("pid")
        if (state.get("state") not in _STATE_TRANSITIONS or
                type(state.get("state_revision")) is not int or
                state["state_revision"] < 1 or
                (pid is None and state.get("state") not in ("starting", "crashed")) or
                (pid is not None and (type(pid) is not int or pid <= 0)) or
                state.get("force_delivery") not in
                (None, "pending", "accepted", "rejected") or
                state.get("stop_mode") not in (None, "graceful", "force") or
                (state.get("last_exit_code") is not None and
                 type(state.get("last_exit_code")) is not int)):
            raise LocalStateError("instance state is malformed")
        config_sha256 = state.get("config_sha256")
        if (config_sha256 is not None and
                (not isinstance(config_sha256, str) or
                 not _SHA256.fullmatch(config_sha256))):
            raise LocalStateError("instance configuration digest is malformed")
        return state

    def write_state(self, state):
        if not isinstance(state, dict) or state.get("protocol_version") != 1:
            raise LocalStateError("instance state is malformed")
        _atomic_json(self.state_path, state)

    def _commit_state(self, expected, changes):
        """Apply a same-launch state update only if its snapshot is current."""
        if not isinstance(changes, dict):
            raise LocalStateError("state changes must be an object")
        if any(key in changes for key in
               ("protocol_version", "launch_id", "state_revision", "updated_at")):
            raise LocalStateError("state identity and revision fields are managed internally")
        with self._exclusive():
            current = self.read_state()
            actual = (current.get("launch_id"), current.get("state_revision"),
                      current.get("state"))
            if actual != tuple(expected):
                return None
            updated = dict(current)
            updated.update(changes)
            old_state = current.get("state")
            new_state = updated.get("state")
            if new_state != old_state and new_state not in _STATE_TRANSITIONS.get(old_state, set()):
                raise LocalStateError("invalid instance state transition: %s -> %s" %
                                      (old_state, new_state))

            old_force = current.get("force_delivery")
            new_force = updated.get("force_delivery")
            allowed_force_updates = {
                None: {None, "pending"},
                "pending": {"pending", "accepted", "rejected"},
                "accepted": {"accepted"},
                "rejected": {"rejected", "pending"},
            }
            if new_force not in allowed_force_updates.get(old_force, set()):
                raise LocalStateError("force delivery state cannot move backward")

            updated["state_revision"] = current["state_revision"] + 1
            updated["updated_at"] = _now()
            self.write_state(updated)
            return updated

    @contextlib.contextmanager
    def _exclusive(self):
        if os.name == "nt":
            _ensure_windows_directory_tree(self.instance_dir)
        else:
            os.makedirs(self.instance_dir, exist_ok=True)
        with _control_directory_guard(self.instance_dir):
            with self._lock():
                yield

    @contextlib.contextmanager
    def _lock(self):
        if os.name == "nt":
            from . import _windows_control
            handle = os.fdopen(_windows_control.open_file(
                self.lock_path, writable=True, create=True), "r+b")
        else:
            handle = open(self.lock_path, "a+")
        acquired = False
        try:
            if os.name == "posix":
                import fcntl
                try:
                    fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                except (BlockingIOError, OSError) as exc:
                    raise LocalStateError("instance is busy") from exc
            elif os.name == "nt":
                import msvcrt
                handle.seek(0)
                try:
                    msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
                except OSError as exc:
                    raise LocalStateError("instance is busy") from exc
            acquired = True
            yield
        finally:
            if acquired and os.name == "posix":
                import fcntl
                fcntl.flock(handle.fileno(), fcntl.LOCK_UN)
            elif acquired and os.name == "nt":
                import msvcrt
                handle.seek(0)
                with contextlib.suppress(OSError):
                    msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            handle.close()

    def _status_for(self, state):
        alive = _pid_alive(state.get("pid"))
        identity = _identity_matches(state) if alive else False
        result = dict(state)
        result["process_alive"] = alive
        result["identity_verified"] = identity
        result["last_known_state"] = state.get("state")
        return result

    def _observed_exit_code(self, state):
        pid = state.get("pid")
        if self._process is not None and getattr(self._process, "pid", None) == pid:
            try:
                return self._process.poll()
            except BaseException:
                return None
        return _reap_child(pid)

    def _cleanup_launch_secrets(self, state):
        control_dir = state.get("control_dir")
        if not isinstance(control_dir, str) or not control_dir:
            return
        launch_id = state.get("launch_id")
        if (not isinstance(launch_id, str) or not launch_id or
                "/" in launch_id or "\\" in launch_id or
                launch_id in (".", "..")):
            raise LocalStateError("launch identity is invalid during control cleanup")
        expected_dir = os.path.abspath(os.path.join(
            self.instance_dir, "launches", launch_id))
        if os.path.normcase(os.path.abspath(control_dir)) != os.path.normcase(expected_dir):
            raise LocalStateError("control directory does not match the current launch")
        with _control_directory_guard(control_dir):
            try:
                control_stat = os.lstat(control_dir)
            except OSError as exc:
                raise LocalStateError("cannot inspect terminal control directory: %s" % exc) from None
            if (not stat.S_ISDIR(control_stat.st_mode) or
                    _control_path_is_reparse(control_dir, control_stat)):
                raise LocalStateError("terminal control path is not a regular directory")
            for filename in ("control.token", "stop.request"):
                try:
                    os.unlink(os.path.join(control_dir, filename))
                except FileNotFoundError:
                    pass
                except OSError as exc:
                    raise LocalStateError(
                        "cannot clean terminal control file %s: %s" % (filename, exc)) from None

    def _reconcile_exited(self, state=None):
        """Conditionally merge exit evidence, never changing a live process."""
        snapshot = state
        expected_launch_id = (snapshot.get("launch_id")
                              if snapshot is not None else None)
        for _ in range(5):
            if snapshot is None:
                with self._exclusive():
                    snapshot = self.read_state()
            if (expected_launch_id is not None and
                    snapshot.get("launch_id") != expected_launch_id):
                return None
            pid = snapshot.get("pid")
            if pid is None and snapshot.get("state") == "starting":
                return None
            if pid is None and snapshot.get("state") == "crashed":
                self._cleanup_launch_secrets(snapshot)
                return snapshot, (9 if snapshot.get("exit_kind") == "forced" else 10)
            if type(pid) is not int or pid <= 0:
                raise LocalStateError("instance process identity is malformed")
            pid_reused = False
            if _pid_alive(pid):
                expected_start = snapshot.get("process_start_time")
                actual_start = (_process_start_time(pid)
                                if expected_start is not None else None)
                if (expected_start is None or actual_start is None or
                        str(expected_start) == str(actual_start)):
                    return None
                pid_reused = True

            if (snapshot.get("state") not in _STATE_TRANSITIONS or
                    snapshot.get("force_delivery") not in
                    (None, "pending", "accepted", "rejected") or
                    snapshot.get("stop_mode") not in (None, "graceful", "force") or
                    (snapshot.get("last_exit_code") is not None and
                     type(snapshot.get("last_exit_code")) is not int)):
                raise LocalStateError("instance exit state is malformed")
            if (pid_reused and
                    not (self._process is not None and
                         getattr(self._process, "pid", None) == pid)):
                observed_exit_code = snapshot.get("last_exit_code")
            else:
                observed_exit_code = self._observed_exit_code(snapshot)
            if observed_exit_code is None:
                observed_exit_code = snapshot.get("last_exit_code")
            terminal = _verified_terminal(snapshot)
            graceful_proof = (terminal is not None and
                              terminal.get("exit_code") == 0)
            changes, exit_code = _exit_transition(
                snapshot, graceful_proof, observed_exit_code)
            if all(snapshot.get(key) == value for key, value in changes.items()):
                self._cleanup_launch_secrets(snapshot)
                return snapshot, exit_code
            expected = (snapshot.get("launch_id"),
                        snapshot.get("state_revision"),
                        snapshot.get("state"))
            completed = self._commit_state(expected, changes)
            if completed is not None:
                self._cleanup_launch_secrets(completed)
                return completed, exit_code

            with self._exclusive():
                current = self.read_state()
            snapshot = current
        raise LocalStateError("instance state changed repeatedly during exit reconciliation")

    def _record_force_delivery(self, launch_id, delivery, **changes):
        for _ in range(5):
            with self._exclusive():
                current = self.read_state()
            if current.get("launch_id") != launch_id:
                raise LocalStateError("launch changed during force delivery")
            existing = current.get("force_delivery")
            if existing == "accepted":
                return current
            if existing == delivery and all(current.get(key) == value
                                            for key, value in changes.items()):
                return current
            expected = (current.get("launch_id"), current.get("state_revision"),
                        current.get("state"))
            updated = self._commit_state(expected, dict(force_delivery=delivery,
                                                        **changes))
            if updated is not None:
                return updated
        raise LocalStateError("instance state changed repeatedly during force delivery")

    def _stop_exit_result(self, state, exit_code):
        result = self._status_for(state)
        if exit_code == 0:
            return result
        if exit_code == 9:
            raise ExitOutcome("server exited after force delivery was accepted",
                              9, "forced_stop", result)
        raise ExitOutcome("server exited without trustworthy graceful or force evidence",
                          10, "local_state_error", result)

    def status(self):
        with self._exclusive():
            state = self.read_state()
        reconciled = self._reconcile_exited(state)
        if reconciled is not None:
            state, _ = reconciled
        else:
            with self._exclusive():
                state = self.read_state()
        return self._status_for(state)

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

    def _base_state(self, launch_id, config, binary, workdir, endpoint, log_file,
                    control_dir, config_sha256=None):
        return {
            "protocol_version": 1, "state_revision": 1, "name": self.name,
            "launch_id": launch_id, "pid": None, "process_start_time": None,
            "binary": binary, "config": config, "workdir": workdir,
            "config_sha256": config_sha256,
            "endpoint": endpoint, "log_file": log_file, "state": "starting",
            "control_dir": control_dir,
            "exit_kind": None, "last_exit_code": None, "updated_at": _now(),
        }

    def _terminate_and_reap(self, process):
        """Stop a child created during failed startup and report whether it exited."""
        try:
            returncode = process.poll()
        except BaseException:
            returncode = None
        if returncode is not None:
            return True, returncode

        try:
            state = self.read_state()
        except BaseException:
            state = {}
        if state.get("pid") != getattr(process, "pid", None):
            state = dict(state, pid=getattr(process, "pid", None),
                         process_start_time=_process_start_time(
                             getattr(process, "pid", None)))
        handshake, _ = _verified_handshake(state)
        if handshake:
            request_sent = _write_stop_request(state)
        else:
            request_sent = False
        if not request_sent:
            # This Popen is the child created by this start call. Its process
            # handle is the authority here even when persisted state is unreadable.
            with contextlib.suppress(BaseException):
                process.send_signal(signal.SIGTERM)
        try:
            return True, process.wait(timeout=1)
        except BaseException:
            pass

        with contextlib.suppress(BaseException):
            process.kill()
        try:
            return True, process.wait(timeout=1)
        except BaseException:
            pass

        try:
            returncode = process.poll()
        except BaseException:
            returncode = None
        if returncode is not None:
            return True, returncode
        return False, None

    def _cleanup_failed_start(self, launch_id, process, state_written):
        """Reap a failed launch and conditionally publish its final managed state."""
        if process is None:
            reaped, exit_code = True, None
            pid = None
            process_start_time = None
        else:
            self._process = process
            try:
                pid = process.pid
            except BaseException:
                pid = None
            try:
                process_start_time = _process_start_time(pid) if pid is not None else None
            except BaseException:
                process_start_time = None
            reaped, exit_code = self._terminate_and_reap(process)

        if not state_written:
            return

        def cleanup_reaped_launch():
            if not reaped:
                return
            control_dir = os.path.join(
                self.instance_dir, "launches", launch_id)
            with contextlib.suppress(BaseException):
                self._cleanup_launch_secrets({
                    "launch_id": launch_id,
                    "control_dir": control_dir,
                })

        try:
            current = self.read_state()
        except BaseException:
            cleanup_reaped_launch()
            return
        if current.get("launch_id") != launch_id:
            cleanup_reaped_launch()
            return
        if current.get("state") not in ("starting", "running", "stopping"):
            cleanup_reaped_launch()
            return

        changes = {}
        if pid is not None:
            changes.update(pid=pid, process_start_time=process_start_time)
        if reaped:
            terminal = _verified_terminal(current)
            graceful_proof = (terminal is not None and
                              terminal.get("exit_code") == 0)
            exit_changes, _ = _exit_transition(current, graceful_proof,
                                               exit_code)
            changes.update(exit_changes)
        else:
            changes["state"] = "stopping"
        expected = (current.get("launch_id"), current.get("state_revision"),
                    current.get("state"))
        try:
            completed = self._commit_state(expected, changes)
            if completed is not None and reaped:
                self._cleanup_launch_secrets(completed)
        except BaseException:
            pass

    def start(self, config, binary=None, workdir=None, log_file=None,
              timeout=15.0, foreground=False, on_ready=None):
        config = os.path.realpath(os.path.abspath(os.path.expanduser(config)))
        if not os.path.isfile(config):
            raise LocalNodeError("config file does not exist: %s" % config)
        binary = self.resolve_binary(binary)
        workdir = os.path.realpath(os.path.abspath(
            os.path.expanduser(workdir or os.getcwd())))
        try:
            config_sha256 = _config_sha256(config)
        except OSError as exc:
            raise LocalNodeError("cannot read server config: %s" % exc) from None
        process = None
        log = None
        state_written = False
        startup_error = None
        startup_traceback = None
        try:
            with self._exclusive():
                try:
                    try:
                        locked_config_sha256 = _config_sha256(config)
                    except OSError as exc:
                        raise LocalNodeError(
                            "cannot read server config: %s" % exc) from None
                    if locked_config_sha256 != config_sha256:
                        raise LocalNodeError(
                            "server config changed while preparing startup")
                    try:
                        old = self.read_state()
                        old_state = old.get("state")
                        if old_state == "starting":
                            raise LocalNodeError("instance startup is already in progress")
                        if old_state == "stopping":
                            raise LocalNodeError("instance is stopping")
                        if old_state == "running":
                            old_status = self._status_for(old)
                            if old_status["process_alive"]:
                                if not old_status["identity_verified"]:
                                    raise IdentityError(
                                        "instance is active but identity cannot be verified")
                                old_digest = old.get("config_sha256")
                                if not isinstance(old_digest, str):
                                    raise LocalNodeError(
                                        "cannot confirm the running instance configuration")
                                requested = (binary, config, workdir, config_sha256)
                                existing = (old.get("binary"), old.get("config"),
                                            old.get("workdir"), old_digest)
                                if existing != requested:
                                    raise LocalNodeError(
                                        "instance is running with a different configuration")
                                return dict(old_status, already_running=True)
                    except LocalStateError as exc:
                        if "does not exist" not in str(exc):
                            raise
                    port = self._config(config)
                    endpoint = "http://127.0.0.1:%d" % port
                    launch_id = uuid.uuid4().hex
                    launch_dir = os.path.join(self.instance_dir, "launches", launch_id)
                    log_file = os.path.abspath(log_file or os.path.join(self.instance_dir, "server.log"))
                    state = self._base_state(launch_id, config, binary, workdir, endpoint, log_file,
                                             launch_dir, config_sha256)
                    _ensure_private_control_dir(launch_dir)
                    log = open(log_file, "ab")
                    state_written = True
                    self.write_state(state)
                    _atomic_json(os.path.join(launch_dir, "control.meta"), {
                        "protocol_version": 1, "launch_id": launch_id,
                    })
                    _atomic_bytes(os.path.join(launch_dir, "control.token"),
                                  secrets.token_hex(32).encode("ascii"))
                    popen_options = {"cwd": workdir, "stdout": log,
                                     "stderr": subprocess.STDOUT}
                    if os.name == "posix":
                        popen_options["start_new_session"] = True
                    elif os.name == "nt":
                        popen_options["creationflags"] = (
                            subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_NO_WINDOW)
                        popen_options["stdin"] = subprocess.DEVNULL
                    if _config_sha256(config) != config_sha256:
                        raise LocalNodeError(
                            "server config changed before the process was spawned")
                    process = subprocess.Popen(
                        [binary, "--config", config, "--control-dir", launch_dir],
                        **popen_options)
                    self._process = process
                    state.update(pid=process.pid,
                                 process_start_time=_process_start_time(process.pid),
                                 state_revision=2, updated_at=_now())
                    self.write_state(state)
                except BaseException as exc:
                    startup_error = exc
                    startup_traceback = exc.__traceback__
                    if log is not None:
                        with contextlib.suppress(BaseException):
                            log.close()
        except BaseException as exc:
            if startup_error is None:
                if state_written or process is not None:
                    startup_error = exc
                    startup_traceback = exc.__traceback__
                    if log is not None:
                        with contextlib.suppress(BaseException):
                            log.close()
                else:
                    raise

        if startup_error is not None:
            if state_written or process is not None:
                with contextlib.suppress(BaseException):
                    self._cleanup_failed_start(launch_id, process, state_written)
            if isinstance(startup_error, OSError):
                raise LocalNodeError("failed to start server: %s" % startup_error) from None
            raise startup_error.with_traceback(startup_traceback)

        preserve_process_state = False
        try:
            expected_start = (launch_id, state["state_revision"], state["state"])
            deadline = time.monotonic() + max(float(timeout), 0.01)
            ready = False
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    reconciled = self._reconcile_exited(state)
                    log.close()
                    if reconciled is None:
                        current = self.read_state()
                        if current.get("launch_id") != launch_id:
                            raise LocalNodeError("launch was superseded before startup completed")
                    raise LocalNodeError("server exited before health became ready")
                handshake_ok, ready_marker = _verified_handshake(state, require_ready=True)
                if (handshake_ok and ready_marker is not None and
                        self._health(endpoint,
                                     min(0.2, max(deadline - time.monotonic(), 0.01)))):
                    ready = True
                    break
                time.sleep(0.05)
            if not ready:
                stopping = self._commit_state(expected_start, {"state": "stopping"})
                if stopping is None:
                    current = self.read_state()
                    if current.get("launch_id") != launch_id:
                        log.close()
                        raise LocalNodeError("launch was superseded while startup was waiting")
                    if current.get("state") == "stopping":
                        stopping = current
                    elif current.get("state") in ("stopped", "crashed"):
                        log.close()
                        raise LocalNodeError("server did not become healthy before timeout")
                    else:
                        log.close()
                        raise LocalNodeError("startup state changed before timeout handling")
                request_sent = _write_stop_request(state)
                if not request_sent:
                    handshake, _ = _verified_handshake(state)
                    if handshake:
                        try:
                            _send_verified_signal(state, signal.SIGTERM)
                        except (OSError, IdentityError):
                            pass
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    preserve_process_state = True
                    with contextlib.suppress(BaseException):
                        log.close()
                    raise StopTimeout("server did not stop after startup timeout")
                reconciled = self._reconcile_exited(stopping)
                log.close()
                if reconciled is None:
                    current = self.read_state()
                    if current.get("launch_id") != launch_id:
                        raise LocalNodeError("launch was superseded during startup cleanup")
                raise LocalNodeError("server did not become healthy before timeout")

            try:
                config_changed = _config_sha256(config) != config_sha256
            except OSError:
                config_changed = True
            if config_changed:
                stopping = self._commit_state(expected_start, {"state": "stopping"})
                if stopping is None:
                    current = self.read_state()
                    if current.get("launch_id") != launch_id:
                        log.close()
                        raise LocalNodeError(
                            "launch was superseded while checking server config")
                    if current.get("state") == "stopping":
                        stopping = current
                    else:
                        log.close()
                        raise LocalNodeError(
                            "startup state changed while checking server config")
                request_sent = _write_stop_request(state)
                if not request_sent:
                    handshake, _ = _verified_handshake(state)
                    if handshake:
                        try:
                            _send_verified_signal(state, signal.SIGTERM)
                        except (OSError, IdentityError):
                            pass
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    preserve_process_state = True
                    with contextlib.suppress(BaseException):
                        log.close()
                    raise StopTimeout(
                        "server config changed during startup and the process did not stop")
                reconciled = self._reconcile_exited(stopping)
                log.close()
                if reconciled is None:
                    current = self.read_state()
                    if current.get("launch_id") != launch_id:
                        raise LocalNodeError(
                            "launch was superseded during config-change cleanup")
                raise LocalNodeError(
                    "server config changed during startup; the launch was stopped")

            running = self._commit_state(expected_start, {"state": "running"})
            if running is None:
                current = self.read_state()
                if current.get("launch_id") != launch_id:
                    log.close()
                    raise LocalNodeError("launch was superseded before startup completed")
                if current.get("state") not in ("stopping", "stopped", "crashed"):
                    log.close()
                    raise LocalNodeError("startup state changed before readiness was recorded")
                preserve_process_state = True
                result = self._status_for(current)
                result["already_running"] = False
                raise LocalNodeError(
                    "startup was canceled by a stop request", result=result)
            state = running
            preserve_process_state = True
            log.close()
            result = self._status_for(state)
            result["already_running"] = False
            if foreground:
                try:
                    if on_ready is not None:
                        on_ready(result)
                    process.wait()
                except KeyboardInterrupt:
                    stopped = self.stop(timeout=10.0)
                    raise ExitOutcome(
                        "server stopped after foreground interrupt", 130,
                        "interrupted", stopped) from None
                reconciled = self._reconcile_exited(state)
                if reconciled is None:
                    current = self.read_state()
                    if current.get("launch_id") != launch_id:
                        raise LocalNodeError("launch was superseded during foreground wait")
                    raise LocalStateError("foreground process did not have a confirmed exit")
                state, exit_code = reconciled
                result = self._status_for(state)
                if exit_code != 0:
                    self._stop_exit_result(state, exit_code)
            return result

        except BaseException:
            if log is not None:
                with contextlib.suppress(BaseException):
                    log.close()
            if not preserve_process_state:
                with contextlib.suppress(BaseException):
                    self._cleanup_failed_start(launch_id, process, state_written)
            raise

    def stop(self, timeout=10.0, force=False):
        with contextlib.ExitStack() as handles:
            return self._stop(timeout, force, handles)

    def _stop(self, timeout, force, handles):
        force_handle = None

        def process_alive():
            if force_handle is None:
                return _pid_alive(pid)
            kernel32, handle = force_handle
            result = kernel32.WaitForSingleObject(handle, 0)
            if result == 0xFFFFFFFF:
                import ctypes
                raise ctypes.WinError(ctypes.get_last_error())
            return result == 258

        with self._exclusive():
            state = self.read_state()
        launch_id = state.get("launch_id")

        def require_original_launch(current):
            if current.get("launch_id") != launch_id:
                raise LocalStateError(
                    "launch was superseded while stop was waiting",
                    result=self._status_for(current))

        result = self._status_for(state)
        if not result["process_alive"]:
            reconciled = self._reconcile_exited(state)
            if reconciled is not None:
                return self._stop_exit_result(*reconciled)
            with self._exclusive():
                state = self.read_state()
            require_original_launch(state)
            result = self._status_for(state)
        if not result["identity_verified"]:
            reconciled = self._reconcile_exited(state)
            if reconciled is not None:
                return self._stop_exit_result(*reconciled)
            if result["process_alive"]:
                with self._exclusive():
                    current = self.read_state()
                require_original_launch(current)
                raise IdentityError("refusing to stop an unverified process")
            raise LocalStateError("instance process is not running")

        pid = state["pid"]
        stopping = None
        validated_result = result
        for _ in range(5):
            result = validated_result or self._status_for(state)
            validated_result = None
            if not result["process_alive"]:
                reconciled = self._reconcile_exited(state)
                if reconciled is not None:
                    return self._stop_exit_result(*reconciled)
                with self._exclusive():
                    state = self.read_state()
                require_original_launch(state)
                validated_result = self._status_for(state)
                continue
            if not result["identity_verified"]:
                reconciled = self._reconcile_exited(state)
                if reconciled is not None:
                    return self._stop_exit_result(*reconciled)
                raise IdentityError("refusing to stop an unverified process")
            if state.get("launch_id") != launch_id or state.get("pid") != pid:
                require_original_launch(state)
                raise LocalStateError("process identity changed before stop could be recorded")
            if state.get("state") in ("stopped", "crashed"):
                raise LocalStateError("terminal state still has a live process")

            changes = {}
            if state.get("state") != "stopping":
                changes["state"] = "stopping"
            if force and state.get("force_delivery") in (None, "rejected"):
                changes.update(stop_mode="force", force_delivery="pending",
                               force_requested_at=_now())
            elif not force and state.get("state") != "stopping":
                changes["stop_mode"] = "graceful"
            if not changes:
                stopping = state
                break
            stopping = self._commit_state(
                (launch_id, state.get("state_revision"), state.get("state")),
                changes)
            if stopping is not None:
                break
            with self._exclusive():
                state = self.read_state()
            require_original_launch(state)
            validated_result = self._status_for(state)
        if stopping is None:
            raise LocalStateError("instance state changed repeatedly before stop")

        control_dir = stopping.get("control_dir")
        if force:
            delivery = stopping.get("force_delivery")
            if delivery == "accepted":
                pass
            elif delivery == "rejected":
                if not _pid_alive(pid):
                    reconciled = self._reconcile_exited(stopping)
                    if reconciled is not None:
                        return self._stop_exit_result(*reconciled)
                raise IdentityError("previous force delivery was rejected")
            elif delivery == "pending":
                # A new explicit force command may retry an uncertain pending
                # delivery. Persisted accepted deliveries are never resent.
                if not _pid_alive(pid):
                    stopping = self._record_force_delivery(
                        launch_id, "rejected", force_rejected_at=_now())
                    reconciled = self._reconcile_exited(stopping)
                    if reconciled is not None:
                        return self._stop_exit_result(*reconciled)
                if stopping.get("force_delivery") != "accepted":
                    try:
                        delivered = _send_verified_signal(stopping, _FORCE_SIGNAL)
                        if os.name == "nt" and delivered is not None:
                            force_handle = delivered
                            handles.callback(delivered[0].CloseHandle, delivered[1])
                    except IdentityError:
                        stopping = self._record_force_delivery(
                            launch_id, "rejected", force_rejected_at=_now())
                        reconciled = self._reconcile_exited(stopping)
                        if reconciled is not None:
                            return self._stop_exit_result(*reconciled)
                        if stopping.get("force_delivery") != "accepted":
                            raise
                    except OSError as exc:
                        stopping = self._record_force_delivery(
                            launch_id, "rejected", force_rejected_at=_now())
                        reconciled = self._reconcile_exited(stopping)
                        if reconciled is not None:
                            return self._stop_exit_result(*reconciled)
                        if stopping.get("force_delivery") != "accepted":
                            if exc.errno in (errno.EPERM, errno.EACCES,
                                             errno.ESRCH):
                                raise IdentityError(
                                    "failed to signal verified process: %s" % exc) from None
                            raise LocalStateError(
                                "failed to deliver force signal: %s" % exc) from None
                    else:
                        stopping = self._record_force_delivery(
                            launch_id, "accepted", force_accepted_at=_now())
            else:
                raise LocalStateError("force delivery state is invalid")
        elif stopping.get("force_delivery") != "accepted":
            if os.name == "nt":
                try:
                    force_handle = _open_verified_windows_process(stopping)
                except (IdentityError, OSError):
                    reconciled = self._reconcile_exited(stopping)
                    if reconciled is not None:
                        return self._stop_exit_result(*reconciled)
                    raise
                handles.callback(force_handle[0].CloseHandle, force_handle[1])
            request_sent = _write_stop_request(stopping) if control_dir else False
            if not request_sent:
                if os.name == "nt":
                    reconciled = self._reconcile_exited(stopping)
                    if reconciled is not None:
                        return self._stop_exit_result(*reconciled)
                    raise LocalStateError("cannot publish the Windows stop request",
                                          result=self._status_for(stopping))
                handshake, _ = _verified_handshake(stopping, require_ready=True)
                if handshake:
                    try:
                        _send_verified_signal(stopping, signal.SIGTERM)
                    except IdentityError:
                        reconciled = self._reconcile_exited(stopping)
                        if reconciled is not None:
                            return self._stop_exit_result(*reconciled)
                        raise
                    except OSError as exc:
                        reconciled = self._reconcile_exited(stopping)
                        if reconciled is not None:
                            return self._stop_exit_result(*reconciled)
                        raise IdentityError(
                            "failed to signal verified process: %s" % exc) from None

        deadline = time.monotonic() + max(float(timeout), 0.01)
        while time.monotonic() < deadline:
            if not process_alive():
                reconciled = self._reconcile_exited(stopping)
                if reconciled is not None:
                    return self._stop_exit_result(*reconciled)
                with self._exclusive():
                    current = self.read_state()
                if current.get("launch_id") != launch_id:
                    require_original_launch(current)
            else:
                current = self.read_state()
                # The force handle was verified before delivery and pins this
                # process while Windows is tearing down its image.
                force_accepted = (force_handle is not None and
                                  current.get("launch_id") == launch_id)
                if (current.get("launch_id") == launch_id and
                        not force_accepted and not _identity_matches(current)):
                    reconciled = self._reconcile_exited(current)
                    if reconciled is not None:
                        return self._stop_exit_result(*reconciled)
                    raise IdentityError("process identity changed while waiting for stop")
            time.sleep(0.05)
        with self._exclusive():
            current = self.read_state()
        if current.get("launch_id") != launch_id:
            require_original_launch(current)
        reconciled = self._reconcile_exited(current)
        if reconciled is not None:
            return self._stop_exit_result(*reconciled)
        with self._exclusive():
            current = self.read_state()
        if current.get("launch_id") != launch_id:
            require_original_launch(current)
        result = self._status_for(current)
        if not result["process_alive"]:
            reconciled = self._reconcile_exited(current)
            if reconciled is not None:
                return self._stop_exit_result(*reconciled)
            raise LocalStateError("cannot reconcile process exit at stop deadline")
        if not result["identity_verified"] and force_handle is None:
            reconciled = self._reconcile_exited(current)
            if reconciled is not None:
                return self._stop_exit_result(*reconciled)
            raise IdentityError("refusing to time out an unverified process")
        raise StopTimeout("server did not stop before timeout", result=result)
