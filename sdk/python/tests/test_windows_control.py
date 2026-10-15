"""Real Windows file-handle boundary checks, without symlink privileges."""

import ctypes
import os
import struct
import subprocess
import tempfile
import unittest
import uuid
from unittest import mock

from cpppools import local_node


def make_file_reparse(path, opener=None):
    """Use a private GUID tag: NTFS supports this without Developer Mode."""
    from cpppools import _windows_control as win

    handle = (opener or win._open)(path, 0x40000000, 0, 0x00200000)
    ioctl = win._bind(win._kernel, "DeviceIoControl", [win.w.HANDLE, win.w.DWORD,
        win.w.LPVOID, win.w.DWORD, win.w.LPVOID, win.w.DWORD,
        ctypes.POINTER(win.w.DWORD), win.w.LPVOID], win.w.BOOL)
    # Non-Microsoft tag + GUID; this fixture contains no redirection target.
    payload = struct.pack("<IHH", 0x42, 0, 0) + uuid.UUID(
        "aaac7164-cf2b-4d13-ae67-81be630ad3e0").bytes_le
    buffer = ctypes.create_string_buffer(payload)
    returned = win.w.DWORD()
    try:
        win._check(ioctl(handle, 0x900A4, buffer, len(payload), None, 0,
                         ctypes.byref(returned), None))
    finally:
        win._close(handle)


@unittest.skipUnless(os.name == "nt", "requires Windows file handles")
class TestWindowsControl(unittest.TestCase):
    def test_terminal_cleanup_rejects_junction_in_ancestor(self):
        with tempfile.TemporaryDirectory() as root:
            manager = local_node.LocalNodeManager(os.path.join(root, "state"), "demo")
            outside = os.path.join(root, "outside")
            launch_id = "cleanup-launch"
            target = os.path.join(outside, launch_id)
            os.makedirs(target)
            os.makedirs(manager.instance_dir)
            token = os.path.join(target, "control.token")
            with open(token, "w", encoding="ascii") as handle:
                handle.write("outside sentinel")
            junction = os.path.join(manager.instance_dir, "launches")
            script = "New-Item -ItemType Junction -Path '%s' -Target '%s' | Out-Null" % (
                junction.replace("'", "''"), outside.replace("'", "''"))
            result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive",
                                     "-Command", script], capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            try:
                with self.assertRaisesRegex(local_node.LocalStateError, "reparse"):
                    manager._cleanup_launch_secrets({
                        "launch_id": launch_id,
                        "control_dir": os.path.join(junction, launch_id)})
                with open(token, encoding="ascii") as handle:
                    self.assertEqual(handle.read(), "outside sentinel")
            finally:
                os.rmdir(junction)

    def test_control_directory_creation_does_not_follow_junction(self):
        with tempfile.TemporaryDirectory() as root:
            state = os.path.join(root, "state")
            outside = os.path.join(root, "outside")
            os.makedirs(outside)
            script = "New-Item -ItemType Junction -Path '%s' -Target '%s' | Out-Null" % (
                state.replace("'", "''"), outside.replace("'", "''"))
            result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive",
                                     "-Command", script], capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            try:
                with self.assertRaises(local_node.LocalStateError):
                    local_node._ensure_private_control_dir(os.path.join(state, "launches", "id"))
                self.assertFalse(os.path.exists(os.path.join(outside, "launches")))
            finally:
                os.rmdir(state)

    def test_directory_and_ancestor_cannot_be_renamed_while_in_use(self):
        from cpppools import _windows_control as win

        with tempfile.TemporaryDirectory() as root:
            parent = os.path.join(root, "parent")
            directory = os.path.join(parent, "control")
            os.makedirs(directory)
            with win.directory(directory):
                for path in (parent, directory):
                    with self.subTest(path=path), self.assertRaises(OSError):
                        os.rename(path, path + "-moved")
            os.rename(directory, directory + "-moved")

    def test_opened_file_reparse_is_rejected_before_read(self):
        from cpppools import _windows_control as win

        with tempfile.TemporaryDirectory() as root:
            path = os.path.join(root, "control.meta")
            local_node._atomic_bytes(path, b"plain file")
            make_file_reparse(path)
            self.assertTrue(os.lstat(path).st_file_attributes & 0x400)
            fd = None
            try:
                with self.assertRaisesRegex(OSError, "regular file"):
                    fd = win.open_file(path)
            finally:
                if fd is not None:
                    os.close(fd)

    def test_replacement_at_file_open_is_checked_on_the_opened_handle(self):
        from cpppools import _windows_control as win

        with tempfile.TemporaryDirectory() as root:
            path = os.path.join(root, "control.meta")
            local_node._atomic_bytes(path, b"original")
            original = win._open

            def replace_before_open(candidate, *args, **kwargs):
                if candidate == path:
                    make_file_reparse(path, opener=original)
                return original(candidate, *args, **kwargs)

            with mock.patch.object(win, "_open", side_effect=replace_before_open):
                with self.assertRaisesRegex(OSError, "regular file"):
                    local_node._read_control_bytes(root, "control.meta")


if __name__ == "__main__":
    unittest.main()
