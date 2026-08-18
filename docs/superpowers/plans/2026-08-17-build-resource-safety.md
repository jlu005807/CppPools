# Build and Database Resource Safety Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make dependency discovery and release-build tests trustworthy, then finalize every prepared statement before its owning database connection closes.

**Architecture:** Keep `StatementCache` as a private per-connection LRU and keep the existing backend factory/NVI interfaces. Add only cache cleanup/exception-safety methods and compiler-specific build flags; no public database signature changes.

**Tech Stack:** CMake 3.16+, C++17, SQLite3, optional MySQL, CTest, ASan/UBSan/LSan.

---

### Task 1: Discover SQLite and preserve assertions in MSVC tests

**Files:**
- Modify: `CMakeLists.txt:88-96,199-204`

- [ ] **Step 1: Write the configuration regression command**

Run:

```powershell
cmake -S . -B build-win -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

Current expected RED in this environment is a compiler error for `sqlite3.h`, after CMake linked the
unqualified name `sqlite3`. The desired failure is an actionable configure error if SQLite is absent.

- [ ] **Step 2: Use FindSQLite3 and undefine NDEBUG for every test compiler**

Replace the bare link with:

```cmake
find_package(SQLite3 REQUIRED)
target_link_libraries(cpppools PUBLIC SQLite::SQLite3)
```

In `cpppools_add_test`, use:

```cmake
target_compile_options(${name} PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>
)
```

- [ ] **Step 3: Verify Linux and the improved missing-dependency diagnostic**

```bash
cmake -S . -B build-wsl -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-wsl -j2
ctest --test-dir build-wsl --output-on-failure
```

Expected GREEN: CTest 18/18. On a machine without SQLite headers, configuration must stop at
`find_package(SQLite3 REQUIRED)` and identify the missing package; it is not a Windows pass.

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt
git commit -m "build: discover SQLite and preserve test assertions"
```

### Task 2: Make StatementCache construction strongly exception-safe

**Files:**
- Modify: `src/db/statement_cache.h:41-74`
- Modify: `test/test_statement_cache.cpp:11-75`

- [ ] **Step 1: Add a throwing factory test**

Add a test that throws while creating a missing key, then successfully creates two other keys in a
capacity-two cache. This catches an orphan LRU node even though whole-cache eviction is retained:

```cpp
static void test_factory_throw_does_not_leave_lru_or_capacity_corrupt() {
    StatementCache<int> cache(2);
    bool fail = true;
    try {
        cache.get_or_create("bad", [&]() -> int {
            if (fail) throw std::runtime_error("prepare failed");
            return 7;
        });
        assert(false && "factory must throw");
    } catch (const std::runtime_error&) {
    }
    assert(cache.size() == 0);
    fail = false;
    assert(cache.get_or_create("good1", [] { return 9; }) == 9);
    assert(cache.get_or_create("good2", [] { return 10; }) == 10);
    assert(cache.size() == 2);

    try {
        cache.get_or_create("bad-full", []() -> int {
            throw std::runtime_error("prepare failed while full");
        });
        assert(false && "factory must throw");
    } catch (const std::runtime_error&) {
    }
    assert(cache.size() == 2);
    assert(cache.get_or_create("good1", [] { return -1; }) == 9);
    assert(cache.get_or_create("good2", [] { return -1; }) == 10);
}
```

- [ ] **Step 2: Run the focused test and record RED**

```bash
cmake --build build-wsl -j2 --target test_statement_cache
ctest --test-dir build-wsl -R '^test_statement_cache$' --output-on-failure
```

Expected RED: the current code publishes `"bad"` in `order_` before the factory completes; after the
throw, inserting `good1` and `good2` can trigger whole-cache eviction and leave fewer entries than the
configured capacity.

- [ ] **Step 3: Construct before publishing the LRU node**

Create the statement before either publishing the LRU node or evicting a full cache. Then perform the
existing whole-cache eviction, insert the list node, and erase that node if `unordered_map::emplace`
throws. Preserve existing hit/miss counters:

```cpp
Stmt stmt = std::forward<Make>(make)();
if (order_.size() >= capacity_) {
    evict_all();
}
auto order_it = order_.insert(order_.begin(), key);
try {
    const auto inserted = cache_.emplace(
        key, Entry{std::move(stmt), order_it});
    return inserted.first->second.stmt;
} catch (...) {
    order_.erase(order_it);
    throw;
}
```

- [ ] **Step 4: Verify and commit**

```bash
ctest --test-dir build-wsl -R '^test_statement_cache$' --output-on-failure
ctest --test-dir build-wsl --output-on-failure
git add src/db/statement_cache.h test/test_statement_cache.cpp
git commit -m "fix: keep statement cache consistent on construction failure"
```

### Task 3: Expose private cache cleanup and fix backend destructor order

**Files:**
- Modify: `src/db/statement_cache.h:60-74`
- Modify: `src/db/sqlite_database.cpp:338-349,363-500`
- Modify: `src/db/mysql_database.cpp:680-687`
- Modify: `test/test_database_conformance.cpp` and `test/test_mysql_crud.cpp`

- [ ] **Step 1: Add a cleanup/lifetime regression**

In the SQLite conformance fixture, execute select/insert/update/delete statements and reset the
`Database`; use the existing LSan invocation as the backend-handle ownership check. A newly reopened raw
connection cannot observe whether the destroyed handle returned `SQLITE_BUSY`, so do not use that as a
false proxy. In the generic cache test, add a destructor-counting statement type and assert
`cache.clear()` destroys exactly the cached entries.

- [ ] **Step 2: Run and record the leak evidence**

```bash
ctest --test-dir build-wsl -R '^test_(statement_cache|database_conformance)$' --output-on-failure
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-wsl-asan -R '^test_database_conformance$' --output-on-failure
```

Expected RED before the fix: LSan reports prepared-statement allocations reachable from the leaked
SQLite handle; the destructor-count test proves `clear()` has a deterministic cleanup surface.

- [ ] **Step 3: Add `clear()` and call it before closing the backend handle**

The private template may expose this method without changing any public library header:

```cpp
void clear() noexcept {
    cache_.clear();
    order_.clear();
}
```

Backend destructors must explicitly sequence cleanup:

```cpp
~SqliteDatabase() override {
    cache_.clear();
    if (db_ != nullptr) {
        (void)sqlite3_close(db_);
        db_ = nullptr;
    }
}
```

Use the same order in `MysqlDatabase::~MysqlDatabase()` before `mysql_close`. Do not rely on C++ member
destruction order or leave a misleading comment claiming the opposite order.

- [ ] **Step 4: Rebuild, run sanitizers, and commit**

```bash
ctest --test-dir build-wsl -R '^test_(statement_cache|database_conformance|caching_database|cache_metrics)$' --output-on-failure
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-wsl-asan -R '^test_(database_conformance|caching_database|cache_metrics)$' --output-on-failure
ctest --test-dir build-wsl --output-on-failure
git add src/db/statement_cache.h src/db/sqlite_database.cpp src/db/mysql_database.cpp \
  test/test_statement_cache.cpp test/test_database_conformance.cpp test/test_mysql_crud.cpp
git commit -m "fix: finalize statement caches before backend close"
```

If LeakSanitizer is unavailable in the environment, record that fact and retain the SQLite close and
destructor-count assertions; never replace them with a claimed leak-free result.

### Task 4: Recover invalid SQLite statements and check every bind

**Files:**
- Modify: `src/db/sqlite_database.cpp:363-500`
- Modify: `test/test_database_conformance.cpp`

- [ ] **Step 1: Add a schema-change/prepare-failure regression**

Open a seeded database so the in-memory schema contains `users`, then use a second raw SQLite connection
to `DROP TABLE users`. Call `select` once and assert `kBackendError` (the prepare failure is cached).
Recreate `users` with the original columns through the raw connection, insert row id 1, then call the
same `select` key and assert it returns that row instead of the old cached error forever.

- [ ] **Step 2: Run the focused test and record RED**

```bash
cmake --build build-wsl -j2 --target test_database_conformance
ctest --test-dir build-wsl -R '^test_database_conformance$' --output-on-failure
```

Expected RED: `stmt_for` returns the same invalid `SqliteStmt` on every call.

- [ ] **Step 3: Replace invalid entries and check select/delete binds**

After `get_or_create`, if `!holder->valid()`, replace the cached object with a new `SqliteStmt` built
from the same SQL, mirroring the existing MySQL path. In `do_select` and `do_remove`, return a backend
error when `bind_value` is not `SQLITE_OK`; do not execute an unbound parameter.

- [ ] **Step 4: Verify and commit**

```bash
ctest --test-dir build-wsl -R '^test_database_conformance$' --output-on-failure
ctest --test-dir build-wsl --output-on-failure
git add src/db/sqlite_database.cpp test/test_database_conformance.cpp
git commit -m "fix: recover invalid SQLite statements and bind errors"
```
