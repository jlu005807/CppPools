# HTTP Lifecycle and Portability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make server shutdown memory-safe, accept fragmented HTTP headers, bound socket writes, and compile cleanly against Winsock without changing the synchronous connection-close HTTP model.

**Architecture:** Keep `HttpServer::start()` as the blocking accept loop and `handle_client()` as one-request/one-response work. Add only private socket helpers and private active-socket tracking; terminal shutdown drains or discards work while every referenced dependency is still alive. No keep-alive, async I/O, proxying, or third-party HTTP library is introduced.

**Tech Stack:** C++17, BSD sockets/Winsock, ThreadPool/ObjectPool, CTest, ASan.

---

### Task 1: Verify the shared build prerequisite

**Files:**
- Inspect: `CMakeLists.txt:88-96,199-204`

- [ ] **Step 1: Verify the shared build-system change**

The CMake/SQLite and MSVC assertion change is owned by
`2026-08-17-build-resource-safety.md` Task 1. Before starting HTTP work, run:

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

If SQLite development files are absent, stop with that plan's actionable configure diagnostic; do not
duplicate the CMake change in this plan.

- [ ] **Step 2: Reconfigure Linux and Windows**

Run:

```bash
cmake -S . -B build-wsl -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-wsl -j2
ctest --test-dir build-wsl --output-on-failure
```

Expected Linux baseline is CTest 18/18 after the shared build plan has been applied. On Windows without
SQLite development files, expected diagnostic is `Could NOT find SQLite3` at configure time. Do not label
Windows GREEN until headers, library, server, and CTest actually build.

- [ ] **Step 3: Record the prerequisite result**

No HTTP files are changed or committed by this prerequisite task. Continue to Task 2 only after the
configuration result is recorded in the implementation log.

### Task 2: Reproduce queued-request shutdown safety

**Files:**
- Modify: `test/test_http_server.cpp:323-417`
- Modify: `include/cpppools/server/http_server.h:126-132,183-228`
- Modify: `src/server/http_server.cpp:266-419,421-463`

- [ ] **Step 1: Add deterministic queued and stalled shutdown tests**

Add a helper that starts a one-worker server, holds the worker on an incomplete body, queues a second
real connection, requests stop, then releases the first request. The essential assertions are:

```cpp
assert(f.thread_pool.pending_tasks() == 1);
f.server.request_stop();
assert(!stopped.load());
send(first_fd, "abcde", 5, 0);
wait_until(stopped, std::chrono::seconds(3));
assert(stopped.load());
assert(f.thread_pool.pending_tasks() == 0);
assert(f.request_pool.in_use() == 0);
```

Add a second test whose peer never completes the request. It must assert that forced shutdown returns
after the configured receive deadline and that destroying the fixture does not crash under ASan.

- [ ] **Step 2: Run the shutdown tests and record RED**

```bash
cmake --build build-wsl -j2 --target test_http_server
ctest --test-dir build-wsl -R '^test_http_server$' --output-on-failure
```

Expected RED: `start()` may report no active request while queued work remains, or the fixture reaches
destruction with queued work still referencing `HttpServer`/`request_pool`.

- [ ] **Step 3: Track live sockets and finish pool work before returning**

Add private state only:

```cpp
mutable std::mutex active_sockets_mutex_;
std::unordered_set<std::intptr_t> active_sockets_;

void register_active_socket(std::intptr_t fd);
void unregister_active_socket(std::intptr_t fd);
void interrupt_active_sockets() noexcept;
```

At the start of `handle_client`, register the descriptor and install a scope guard that unregisters it.
After accept stops, wait for both `active_requests_ == 0` and `thread_pool_.pending_tasks() == 0`.
If the 5-second graceful window expires, call `shutdown(SHUT_RDWR/SD_BOTH)` on tracked sockets to
unblock recv/send, then call `thread_pool_.shutdown_now()` while `HttpServer`, request pool, and registry
are still alive. If the graceful window succeeds, call `thread_pool_.shutdown()` to join workers before
`start()` returns.

```cpp
while ((active_requests_.load(std::memory_order_relaxed) != 0 ||
        thread_pool_.pending_tasks() != 0) &&
       std::chrono::steady_clock::now() < drain_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
if (active_requests_.load(std::memory_order_relaxed) != 0 ||
    thread_pool_.pending_tasks() != 0) {
    interrupt_active_sockets();
    thread_pool_.shutdown_now();
} else {
    thread_pool_.shutdown();
}
```

- [ ] **Step 4: Run targeted tests and ASan**

```bash
ctest --test-dir build-wsl -R '^test_(http_server|thread_pool)$' --output-on-failure
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-wsl-asan -R '^test_http_server$' --output-on-failure
```

Expected GREEN: queued work is either completed or destroyed before `start()` returns; no ASan UAF.

- [ ] **Step 5: Review and commit lifecycle changes**

```bash
git add include/cpppools/server/http_server.h src/server/http_server.cpp test/test_http_server.cpp
git commit -m "fix: drain HTTP work before server destruction"
```

### Task 3: Read fragmented headers to a bounded terminator

**Files:**
- Modify: `test/test_http_server.cpp:200-285`
- Modify: `src/server/http_server.cpp:464-547`

- [ ] **Step 1: Add request-line and header fragmentation tests**

Add local `send_fragment`, `try_read_peer` and `read_response` helpers beside the existing
`Fixture::round_trip`, then drive `handle_client` on a background thread and send each fragment separately:

```cpp
send(peer, "POST /health HT", 15, 0);
send(peer, "TP/1.1\r\nContent-Len", 21, 0);
send(peer, "gth: 5\r\n", 9, 0);
assert(no_response_available(peer));
send(peer, "\r\nabcde", 7, 0);
assert(read_response(peer).find("405") != std::string::npos);
```

Add a 4096-byte header without `\r\n\r\n` and assert a bounded 400/414 response according to whether
the request line itself is complete. Add an incomplete-header timeout case that does not hang the test.

- [ ] **Step 2: Run and record RED**

```bash
cmake --build build-wsl -j2 --target test_http_server
ctest --test-dir build-wsl -R '^test_http_server$' --output-on-failure
```

Expected RED: the first segment is routed immediately and returns 400, or Content-Length is missed.

- [ ] **Step 3: Replace the one-shot header read with a bounded loop**

Use the existing `RequestContext::buffer` and never read beyond `kRequestBufferBytes` (the
platform-specific recv length conversion is added in Task 4):

```cpp
std::size_t received = 0;
std::size_t header_end = std::string_view::npos;
while (received < kRequestBufferBytes) {
    const SsizeType n = recv(guard.get(), ctx->buffer.data() + received,
                             static_cast<int>(kRequestBufferBytes - received), 0);
    if (n <= 0) break;
    received += static_cast<std::size_t>(n);
    std::string_view current(ctx->buffer.data(), received);
    header_end = current.find("\r\n\r\n");
    if (header_end != std::string_view::npos) break;
}
```

Only call `find_content_length` after the terminator exists. Preserve already-read body bytes after
`header_end + 4`; keep 414 for an overlong request line and return 400 for a complete request line whose
header block never terminates within the bound.

- [ ] **Step 4: Verify targeted and full HTTP tests**

```bash
ctest --test-dir build-wsl -R '^test_(http_util|http_server)$' --output-on-failure
ctest --test-dir build-wsl --output-on-failure
```

Expected GREEN: all existing body-segmentation tests plus the new header-segmentation tests pass.

- [ ] **Step 5: Commit header framing**

```bash
git add src/server/http_server.cpp test/test_http_server.cpp
git commit -m "fix: read complete bounded HTTP headers"
```

### Task 4: Bound sends and isolate Winsock/POSIX behavior

**Files:**
- Modify: `src/server/http_server.cpp:24-40,56-107,178-204,266-359,464-537`
- Modify: `test/test_http_server.cpp:1-25,200-417`

- [ ] **Step 1: Add a peer-not-reading test**

Create a socket pair, reduce its send buffer, request a response larger than that buffer, and leave the
peer unread. Assert the handler exits after the send deadline instead of blocking forever. Keep the
deadline assertion broad enough for CI (for example 1-8 seconds), but do not replace it with a sleep-only
test.

- [ ] **Step 2: Introduce private platform helpers**

Keep helpers in the anonymous namespace; no public socket API is added:

```cpp
int last_socket_error() noexcept;
bool socket_error_is_interrupted(int code) noexcept;
bool socket_error_is_would_block(int code) noexcept;
bool set_socket_timeout(SocketType fd, int option,
                        std::chrono::milliseconds timeout) noexcept;
bool wait_socket_readable(SocketType fd, std::chrono::milliseconds timeout) noexcept;
```

On Windows use `WSAGetLastError`, `WSAEINTR`, `WSAEWOULDBLOCK`, a millisecond `DWORD` for
`SO_RCVTIMEO/SO_SNDTIMEO`, `int` address lengths, and `select` for the accept wakeup. On POSIX retain
`errno`, `EINTR/EAGAIN/EWOULDBLOCK`, `timeval`, `socklen_t`, and `poll`.

- [ ] **Step 3: Set receive and send deadlines through the helper**

Before the first recv set 30 seconds, before body recv set 5 seconds, and before every response path set
the finite send deadline. Make `send_all` consult the platform error helper; a timeout returns `false` and
lets `SocketGuard` close the connection.

- [ ] **Step 4: Run Linux regression and native Windows build when available**

```bash
ctest --test-dir build-wsl -R '^test_http_server$' --output-on-failure
ctest --test-dir build-wsl --output-on-failure
```

Windows PowerShell, only after SQLite development files are installed:

```powershell
cmake -S . -B build-win -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-win --config RelWithDebInfo
ctest --test-dir build-win -C RelWithDebInfo --output-on-failure
```

Expected GREEN: Linux 18/18; native Windows compile and CTest must be reported exactly as executed.

- [ ] **Step 5: Commit transport portability**

```bash
git add src/server/http_server.cpp test/test_http_server.cpp
git commit -m "fix: bound socket I/O across platforms"
```
