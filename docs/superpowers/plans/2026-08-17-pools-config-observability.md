# Pools, Configuration, and Observability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the stage-6 P2 work with deterministic coverage for pool fault paths and minimal fixes for missing configuration arguments, invalid observability output, and unsafe unsigned JSON conversion.

**Architecture:** Preserve the existing three-pool contracts, synchronous HTTP routing, configuration precedence, and signed `db::Value` model. Treat already-correct tried-slot, reset rollback, empty-factory, and atomic peak behavior as regression-only evidence; change production code only where a new test is RED. Keep JSON escaping and Prometheus label escaping separate because they follow different grammars.

**Tech Stack:** C++17, CMake/CTest, nlohmann/json, SQLite3, Prometheus text exposition, ASan/UBSan.

---

## Evidence and guardrails

- `DbConnectionPool::acquire()` already tracks `tried_this_call`, rotates past failed slots, and waits instead of spinning after every free slot has been tried.
- `ObjectPool::acquire()` already destroys an object whose `reset()` throws and rolls back `size`/`in_use`; the constructor already rejects an empty DB factory.
- `peak_active_requests_` and `peak_task_exec_us_` already use compare/exchange loops. `peak_pending_count_` is written only while `ThreadPool::mutex_` is held, so replacing it with speculative lock-free code is not justified.
- The deterministic defects are: direct `resolve_config_path()` fallback for a missing `--config` value, unescaped configured names in hand-built JSON, unescaped Prometheus label values, and unsigned JSON integers narrowed into signed `db::Value`.
- Do not change public pool signatures, borrow/return semantics, timeout semantics, metric names, label names, or `db::Value`. Stage only the paths named by each task; preserve all unrelated worktree changes, including script permission edits.

### Task 1: Freeze the proven pool fault-path behavior

**Files:**
- Modify: `test/test_db_pool.cpp:65-112,243-312,373-408,512-531`
- Modify: `test/test_object_pool.cpp:35-61,122-166,316-329`
- Modify: `test/test_thread_pool.cpp:470-565,632-650`
- Verify only: `src/db/db_connection_pool.cpp:78-215`
- Verify only: `include/cpppools/pool/object_pool.h:158-219`
- Verify only: `src/pool/thread_pool.cpp:89-101,207-220`
- Verify only: `src/server/http_server.cpp:134-145,450-460`

- [ ] **Step 1: Add an explicit multi-slot tried-slot regression**

Add `test_acquire_advances_past_a_failed_slot()` to `test/test_db_pool.cpp`. Use a two-slot factory with per-index call counters. After construction, make all old `FakeDatabase` instances unhealthy, make rebuild of slot 0 return `nullptr`, and make rebuild of slot 1 return a new instance. Assert one `acquire(100ms)` succeeds through slot 1 and that each slot's rebuild count is exactly one:

```cpp
std::array<std::atomic<int>, 2> calls{};
std::atomic<bool> inject_failure{false};
auto factory = [&](std::size_t slot) -> std::unique_ptr<Database> {
    calls[slot].fetch_add(1);
    if (inject_failure.load() && slot == 0) {
        return nullptr;
    }
    return std::make_unique<FakeDatabase>();
};

DbConnectionPool pool("advance", factory, 2, 0ms);
FakeDatabase::healthy_flag.store(false);
inject_failure.store(true);
Database* conn = pool.acquire(100ms);
assert(conn != nullptr);
assert(calls[0].load() == 2);  // construct + one failed rebuild
assert(calls[1].load() == 2);  // construct + one successful rebuild
pool.release(conn);
```

Register the test in `main()`. This test must not depend on pointer identity because an allocator may reuse an address.

- [ ] **Step 2: Strengthen exception and peak assertions without changing behavior**

In `test_reset_exception_destroys_object_and_rolls_back()`, also assert the first throwing call is included in `acquire_total` but not `acquire_failed_total`, then assert the second acquire restores `in_use + available == size`. Retain the existing empty-factory assertion in `test_constructor_rejects_invalid_arguments()`.

Add `test_peak_execution_time_is_monotonic_under_workers()` to `test/test_thread_pool.cpp`: submit four simultaneously released tasks sleeping for 10, 20, 30, and 40 ms, drain the pool, and assert `peak_task_exec_us >= 30000` and `peak_task_exec_us <= task_exec_us_total`. This exercises the existing CAS without depending on exact scheduler timing.

- [ ] **Step 3: Run the evidence gate**

```bash
cmake --build build-wsl -j2 --target test_db_pool test_object_pool test_thread_pool
ctest --test-dir build-wsl -R '^test_(db_pool|object_pool|thread_pool)$' --output-on-failure
```

Expected GREEN on the current tree. This is intentionally a regression-only task: the named implementations already satisfy the behavior. If any assertion is RED, stop and record the exact failure; do not redesign a pool or add a new API to make a timing test pass.

- [ ] **Step 4: Commit the evidence tests independently**

```bash
git add test/test_db_pool.cpp test/test_object_pool.cpp test/test_thread_pool.cpp
git commit -m "test(pool): cover stage 6 fault boundaries"
```

### Task 2: Reject `--config` without a value at the resolver boundary

**Files:**
- Modify: `include/cpppools/config/config.h:73-76`
- Modify: `src/config/config.cpp:69-77`
- Modify: `test/test_config.cpp:198-210,448-458`
- Verify only: `src/main.cpp:110-121`

- [ ] **Step 1: Write the failing resolver test**

Extend `test_resolve_config_path_precedence()` with a direct helper call so the behavior does not depend on the executable's duplicate guard:

```cpp
const char* argv_missing[] = {"cpppools_server", "--config"};
bool threw = false;
try {
    (void)resolve_config_path(2, argv_missing);
} catch (const std::invalid_argument& ex) {
    threw = std::string(ex.what()) == "--config requires a path argument";
}
assert(threw);
```

Clear `CPPPOOLS_CONFIG` before this assertion so an environment value cannot mask the bug.

- [ ] **Step 2: Run and record RED**

```bash
cmake --build build-wsl -j2 --target test_config
ctest --test-dir build-wsl -R '^test_config$' --output-on-failure
```

Expected RED: `resolve_config_path(2, argv_missing)` returns the environment/default path instead of rejecting the incomplete flag.

- [ ] **Step 3: Validate the flag before applying precedence**

Implement the complete `--config` branch before consulting the environment:

```cpp
if (argc >= 2 && argv != nullptr && argv[1] != nullptr &&
    std::string(argv[1]) == "--config") {
    if (argc < 3 || argv[2] == nullptr || argv[2][0] == '\0') {
        throw std::invalid_argument("--config requires a path argument");
    }
    return argv[2];
}
```

Update the header comment to state that an absent or empty value throws `std::invalid_argument`. Keep the current `main.cpp` guard so the CLI still prints the same message and exits 1 rather than terminating on an uncaught exception.

- [ ] **Step 4: Verify and commit**

```bash
ctest --test-dir build-wsl -R '^test_config$' --output-on-failure
./build-wsl/bin/cpppools_server --config >/tmp/cpppools-missing-config.out 2>&1; test $? -eq 1
grep -F -- '--config requires a path argument' /tmp/cpppools-missing-config.out
git add include/cpppools/config/config.h src/config/config.cpp test/test_config.cpp
git commit -m "fix(config): reject missing config path"
```

Expected GREEN: the unit test passes, the process exits 1, and stderr contains the exact diagnostic.

### Task 3: Keep configured names valid in JSON observability responses

**Files:**
- Modify: `src/server/http_server.cpp:929-1005,1013-1064`
- Modify: `test/test_metrics.cpp:1-82,186-224`

- [ ] **Step 1: Add endpoint-level JSON escaping coverage**

Include `<nlohmann/json.hpp>` in `test/test_metrics.cpp` and let its fixture accept a datasource name and `ClusterView`. Add `test_json_endpoints_escape_configured_names()` using these exact values:

```cpp
const std::string datasource = "db\"\\line\nnext";
const std::string node_id = "node\"\\line\nnext";
```

Parse `/api/v1/get-config` and `/api/v1/stats` with nlohmann/json, then assert the decoded datasource and node ID equal the original strings. Do not assert only on substrings; parsing is what proves the response remains valid JSON.

- [ ] **Step 2: Run and record RED**

```bash
cmake --build build-wsl -j2 --target test_metrics
ctest --test-dir build-wsl -R '^test_metrics$' --output-on-failure
```

Expected RED: parsing throws because `handle_get_config()` and `handle_stats()` insert configured strings into hand-built JSON without `json_escape()`.

- [ ] **Step 3: Escape only the dynamic string fields**

Use the existing `json_escape()` helper at both manual serialization sites:

```cpp
names += "\"" + json_escape(n) + "\"";
// ...
os << "\"node_id\":\"" << json_escape(config_.cluster_view.node_id) << "\",";
```

Do not replace numeric streaming with string conversion, and do not rewrite the endpoint schema. Endpoints already built with nlohmann/json (`/api/v1/cluster`, CRUD, and schema) need no change.

- [ ] **Step 4: Verify and commit**

```bash
ctest --test-dir build-wsl -R '^test_(http_util|metrics|cache_metrics|cluster_api)$' --output-on-failure
git add src/server/http_server.cpp test/test_metrics.cpp
git commit -m "fix(observability): escape configured names in JSON"
```

### Task 4: Escape every Prometheus label value

**Files:**
- Modify: `src/server/http_util.h:73-82`
- Modify: `src/server/http_util.cpp:135-162`
- Modify: `src/server/http_server.cpp:585-885`
- Modify: `test/test_http_util.cpp:145-162,333-365`
- Modify: `test/test_metrics.cpp:83-185`

- [ ] **Step 1: Write unit and endpoint RED tests**

Add a focused helper assertion:

```cpp
assert(prometheus_label_escape("plain") == "plain");
assert(prometheus_label_escape("a\\b\"c\nd") == "a\\\\b\\\"c\\nd");
```

Reuse the special-name metrics fixture from Task 3 and assert `/metrics` contains a single intact sample line with `pool="db\\\"\\\\line\\nnext"`. Also iterate every non-comment response line and assert it contains no literal newline inside the configured label value.

- [ ] **Step 2: Run and record RED**

```bash
cmake --build build-wsl -j2 --target test_http_util test_metrics
ctest --test-dir build-wsl -R '^test_(http_util|metrics)$' --output-on-failure
```

Expected RED: the helper is absent, and the endpoint currently emits a quote that closes the label plus a literal newline that splits the sample.

- [ ] **Step 3: Implement the Prometheus grammar once and use it everywhere**

Declare `prometheus_label_escape(std::string_view)` beside `json_escape()` and implement only the three escapes required for label values:

```cpp
switch (c) {
    case '\\': out += "\\\\"; break;
    case '"':  out += "\\\""; break;
    case '\n': out += "\\n";  break;
    default:   out += c;       break;
}
```

Apply it to thread-pool, object-pool, DB-pool, cache, and ownership-router `pool` values before interpolation. Cover both reusable `{pool="..."}` strings and the inline two-label `reason=...` samples. Metric names, label keys, fixed `reason` values, HELP text, and TYPE ordering must remain unchanged.

- [ ] **Step 4: Verify and commit**

```bash
ctest --test-dir build-wsl -R '^test_(http_util|metrics|cache_metrics|ownership)$' --output-on-failure
git add src/server/http_util.h src/server/http_util.cpp \
  src/server/http_server.cpp test/test_http_util.cpp test/test_metrics.cpp
git commit -m "fix(observability): escape Prometheus label values"
```

### Task 5: Reject unsigned JSON integers outside `db::Value` and close the stage

**Files:**
- Modify: `src/server/crud_api.cpp:13-20,82-101`
- Modify: `test/test_crud_api.cpp:240-330,374-391`
- Modify: `docs/api/backend_api.md:33-42,55-66`

- [ ] **Step 1: Add a signed-range regression**

Add `test_unsigned_integer_outside_value_range_is_400()` and exercise the shared conversion path in both column values and `where.value`:

```cpp
Fixture f;
const auto insert = f.post(
    "/api/v1/insert",
    R"({"table":"orders","values":{"id":18446744073709551615,"amount":1}})");
assert(insert.status_code == 400);
assert(contains(insert.body, "signed 64-bit"));

const auto select = f.post(
    "/api/v1/select",
    R"({"table":"orders","where":{"column":"id","value":18446744073709551615}})");
assert(select.status_code == 400);
assert(contains(select.body, "signed 64-bit"));
```

- [ ] **Step 2: Run and record RED**

```bash
cmake --build build-wsl -j2 --target test_crud_api
ctest --test-dir build-wsl -R '^test_crud_api$' --output-on-failure
```

Expected RED: nlohmann/json classifies the literal as unsigned, and `get<std::int64_t>()` narrows it into the signed `Value` domain instead of returning 400.

- [ ] **Step 3: Range-check before conversion**

Include `<limits>` and split unsigned handling from signed handling:

```cpp
if (j.is_number_unsigned()) {
    const std::uint64_t value = j.get<std::uint64_t>();
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        *error = "integer is outside the signed 64-bit range";
        return false;
    }
    *out = Value{static_cast<std::int64_t>(value)};
    return true;
}
if (j.is_number_integer()) {
    *out = Value{j.get<std::int64_t>()};
    return true;
}
```

Document that JSON integers accepted by CRUD are `[-2^63, 2^63-1]`; values outside that range receive 400. Do not add an unsigned alternative to `db::Value`.

- [ ] **Step 4: Run the targeted and full closeout gates**

```bash
ctest --test-dir build-wsl -R '^test_(crud_api|config|db_pool|object_pool|thread_pool|http_util|metrics|cache_metrics|ownership)$' --output-on-failure
ctest --test-dir build-wsl --output-on-failure
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-wsl-asan \
  -R '^test_(crud_api|db_pool|object_pool|thread_pool|metrics)$' --output-on-failure
python3 -m unittest discover -s sdk/python/tests
```

Expected GREEN: all configured CTests and 51 SDK tests pass. Record ASan/UBSan results separately; do not claim TSan, Windows, MySQL, or live-e2e coverage unless those environments actually ran.

- [ ] **Step 5: Commit the conversion fix**

```bash
git add src/server/crud_api.cpp test/test_crud_api.cpp docs/api/backend_api.md
git commit -m "fix(api): reject out-of-range unsigned JSON integers"
```

## Final self-review

- [ ] Confirm each production change is preceded by the named deterministic RED test; Task 1 is explicitly regression-only because the implementation is already present.
- [ ] Search this plan and the resulting diffs for `TODO`, placeholders, new public APIs, changed metric names, or changed pool timeout/borrow semantics; none are allowed.
- [ ] Confirm every hand-built JSON string uses JSON escaping and every dynamic Prometheus label value uses Prometheus escaping; do not interchange the helpers.
- [ ] Confirm signed/unsigned types remain consistent from JSON parsing through `db::Value`, backend binding, and response serialization.
- [ ] Run `git status --short` and verify unrelated user-owned changes were neither staged nor reverted.
