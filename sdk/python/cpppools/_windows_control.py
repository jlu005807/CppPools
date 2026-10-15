"""Win32 handles and private ACLs for the local control files (stdlib only)."""

import contextlib
import ctypes
import functools
import os
from ctypes import wintypes as w


_kernel = ctypes.WinDLL("kernel32", use_last_error=True)
_security = ctypes.WinDLL("advapi32", use_last_error=True)


def _bind(library, name, arguments, result):
    function = getattr(library, name)
    function.argtypes = arguments
    function.restype = result
    return function


_close = _bind(_kernel, "CloseHandle", [w.HANDLE], w.BOOL)
_free = _bind(_kernel, "LocalFree", [w.HLOCAL], w.HLOCAL)
_current_process = _bind(_kernel, "GetCurrentProcess", [], w.HANDLE)
_create = _bind(_kernel, "CreateFileW", [w.LPCWSTR, w.DWORD, w.DWORD,
                w.LPVOID, w.DWORD, w.DWORD, w.HANDLE], w.HANDLE)
_attributes = _bind(_kernel, "GetFileInformationByHandleEx",
                   [w.HANDLE, ctypes.c_int, w.LPVOID, w.DWORD], w.BOOL)
_open_token = _bind(_security, "OpenProcessToken",
                   [w.HANDLE, w.DWORD, ctypes.POINTER(w.HANDLE)], w.BOOL)
_token_info = _bind(_security, "GetTokenInformation",
                   [w.HANDLE, ctypes.c_int, w.LPVOID, w.DWORD,
                    ctypes.POINTER(w.DWORD)], w.BOOL)
_sid_text = _bind(_security, "ConvertSidToStringSidW",
                 [w.LPVOID, ctypes.POINTER(w.LPWSTR)], w.BOOL)
_parse_sd = _bind(_security, "ConvertStringSecurityDescriptorToSecurityDescriptorW",
                 [w.LPCWSTR, w.DWORD, ctypes.POINTER(w.LPVOID), w.LPVOID], w.BOOL)
_get_dacl = _bind(_security, "GetSecurityDescriptorDacl",
                 [w.LPVOID, ctypes.POINTER(w.BOOL), ctypes.POINTER(w.LPVOID),
                  ctypes.POINTER(w.BOOL)], w.BOOL)
_set_security = _bind(_security, "SetSecurityInfo",
                     [w.HANDLE, ctypes.c_int, w.DWORD, w.LPVOID, w.LPVOID,
                      w.LPVOID, w.LPVOID], w.DWORD)
_get_security = _bind(_security, "GetSecurityInfo",
                     [w.HANDLE, ctypes.c_int, w.DWORD, ctypes.POINTER(w.LPVOID),
                      w.LPVOID, w.LPVOID, w.LPVOID, ctypes.POINTER(w.LPVOID)], w.DWORD)
_sd_text = _bind(_security, "ConvertSecurityDescriptorToStringSecurityDescriptorW",
                [w.LPVOID, w.DWORD, w.DWORD, ctypes.POINTER(w.LPWSTR), w.LPVOID], w.BOOL)


def _check(ok):
    if not ok:
        raise ctypes.WinError(ctypes.get_last_error())


def _sid_string(sid):
    text = w.LPWSTR()
    _check(_sid_text(sid, ctypes.byref(text)))
    try:
        return text.value
    finally:
        _free(text)


@functools.lru_cache(maxsize=1)
def _user_sid():
    token = w.HANDLE()
    _check(_open_token(_current_process(), 8, ctypes.byref(token)))  # TOKEN_QUERY
    try:
        size = w.DWORD()
        _token_info(token, 1, None, 0, ctypes.byref(size))  # TokenUser
        buffer = ctypes.create_string_buffer(size.value)
        _check(_token_info(token, 1, buffer, size, ctypes.byref(size)))
        return _sid_string(ctypes.cast(buffer, ctypes.POINTER(w.LPVOID))[0])
    finally:
        _close(token)


def _security_descriptor(handle):
    owner, descriptor = w.LPVOID(), w.LPVOID()
    code = _get_security(handle, 1, 1 | 4, ctypes.byref(owner),
                         None, None, None, ctypes.byref(descriptor))
    if code:
        raise ctypes.WinError(code)
    return owner, descriptor


def _private_acl(handle):
    owner, previous = _security_descriptor(handle)
    try:
        if _sid_string(owner) != _user_sid():
            raise OSError("local control path is not owned by the current user")
    finally:
        _free(previous)
    inheritance = "OICI" if _file_attributes(handle) & 0x10 else ""
    expected = "D:P(A;%s;FA;;;SY)(A;%s;FA;;;%s)" % (
        inheritance, inheritance, _user_sid())
    descriptor, dacl = w.LPVOID(), w.LPVOID()
    present, defaulted = w.BOOL(), w.BOOL()
    _check(_parse_sd(expected, 1, ctypes.byref(descriptor), None))
    try:
        _check(_get_dacl(descriptor, ctypes.byref(present), ctypes.byref(dacl),
                         ctypes.byref(defaulted)))
        code = _set_security(handle, 1, 4 | 0x80000000, None, None, dacl, None)
        if code:
            raise ctypes.WinError(code)
    finally:
        _free(descriptor)
    _, actual = _security_descriptor(handle)
    text = w.LPWSTR()
    try:
        _check(_sd_text(actual, 1, 4, ctypes.byref(text), None))
        # Windows can retain AUTO_INHERITED history even with a protected DACL.
        # The ACEs themselves must still be exactly the two explicit grants.
        if text.value not in (expected, expected.replace("D:P", "D:PAI", 1)):
            raise OSError("local control ACL could not be verified")
    finally:
        if text:
            _free(text)
        _free(actual)


def _open(path, access, share, flags, disposition=3):
    handle = _create(path, access, share, None, disposition, flags, None)
    if handle == w.HANDLE(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    return handle


def _file_attributes(handle):
    # FileAttributeTagInfo: attributes and reparse tag, both DWORD.
    info = (w.DWORD * 2)()
    _check(_attributes(handle, 9, info, ctypes.sizeof(info)))
    return info[0]


@contextlib.contextmanager
def directory(path, secure_leaf=True):
    """Pin every path component against rename/delete; secure the owned leaf."""
    absolute = os.path.abspath(path)
    drive, tail = os.path.splitdrive(absolute)
    current = drive + os.sep
    parts = [part for part in tail.split(os.sep) if part]
    handles = []
    try:
        for index, part in enumerate(parts):
            current = os.path.join(current, part)
            leaf = index == len(parts) - 1
            access = 0x1 | 0x80 | (0x20000 | 0x40000 if leaf else 0)
            handle = _open(current, access, 1 | 2, 0x02000000 | 0x00200000)
            handles.append(handle)  # no FILE_SHARE_DELETE, including ancestors
            attrs = _file_attributes(handle)
            if not attrs & 0x10 or attrs & 0x400:
                raise OSError("local control path contains a reparse point or non-directory")
        if not parts:
            raise OSError("local control directory cannot be a drive root")
        if secure_leaf:
            _private_acl(handles[-1])
        yield
    finally:
        for handle in reversed(handles):
            _close(handle)


def open_file(path, writable=False, create=False):
    """Return a CRT fd after checking the opened object, never following links."""
    import msvcrt

    access = 0x80000000 | 0x20000 | 0x40000 | (0x40000000 if writable else 0)
    handle = _open(path, access, 1 | 2 | 4, 0x00200000, 4 if create else 3)
    try:
        if _file_attributes(handle) & (0x10 | 0x400):
            raise OSError("local control file is not a regular file")
        _private_acl(handle)
        fd = msvcrt.open_osfhandle(handle, (os.O_RDWR if writable else os.O_RDONLY) |
                                  os.O_BINARY)
        handle = None  # CRT now owns this handle.
        return fd
    finally:
        if handle is not None:
            _close(handle)
