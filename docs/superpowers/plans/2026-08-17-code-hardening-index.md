# CppPools Code Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the approved code-level reliability fixes from the 2026-08-17 design without changing public APIs, architecture boundaries, or cluster/cache decisions.

**Architecture:** Keep the existing NVI database pipeline, three pools, synchronous `Connection: close` HTTP server, ownership router, and zero-dependency SDK. Changes are confined to private resource management, existing contract enforcement, platform branches, tests, and documentation. Each subsystem plan ends in an independently testable commit.

**Tech Stack:** C++17, CMake/CTest, SQLite3, optional MySQL client, POSIX/Winsock sockets, Python 3 standard library, ASan/UBSan/LSan where available.

---

## Execution order and gates

The plans below are intentionally separate because they touch independent subsystems. Do not start a later plan until the prior plan's targeted tests, full regression, and review gate are green.

1. [Build and resource safety](2026-08-17-build-resource-safety.md)
2. [Schema and cache correctness](2026-08-17-cache-schema-correctness.md)
3. [Database transactions and MySQL strictness](2026-08-17-database-transaction-strictness.md)
4. [HTTP lifecycle and portability](2026-08-17-http-lifecycle-portability.md)
5. [Python SDK retry semantics](2026-08-17-sdk-retry-semantics.md)
6. [Pools, configuration, observability, and closeout](2026-08-17-pools-config-observability.md)

Every task follows this fixed loop:

```text
write one deterministic failing test
run the smallest command and record RED
make the smallest private implementation change
run the targeted test, then the full relevant suite
request/read a code review and fix review findings
run the verification command again
commit only the task's files with the configured user identity
```

Baseline commands (run from the repository root) are:

```bash
cmake -S . -B build-wsl -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-wsl -j2
ctest --test-dir build-wsl --output-on-failure
python3 -m unittest discover -s sdk/python/tests
```

For a sanitizer build, use the existing WSL toolchain and disable only LeakSanitizer
when the environment cannot unload it:

```bash
cmake -S . -B build-wsl-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-wsl-asan -j2
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-wsl-asan --output-on-failure
```

Do not claim Windows, MySQL, TSan, or live e2e success unless the corresponding
headers/libraries/services and commands actually ran. Existing user-owned script
permission changes remain unstaged in every commit.

## Plan self-review checklist

- [ ] Every release blocker in the design spec has a named failing test and an implementation task.
- [ ] No task changes a public header signature, hash-ring vector, ownership matrix, I0/I1, or pool borrow semantics.
- [ ] Every command names its build directory and expected RED/GREEN outcome.
- [ ] MySQL tests skip only when the required environment is absent; configured-but-failing environments fail.
- [ ] Final closeout records unverified platform/environment items instead of treating them as passes.
