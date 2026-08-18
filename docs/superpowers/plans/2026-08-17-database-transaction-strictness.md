# Database Transaction and MySQL Strictness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended; use executing-plans if working inline). Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve the existing CRUD contract under concurrent read-modify-write and reject MySQL/SQLite driver states that cannot be represented safely by `Value`/`DbResult`.

**Architecture:** Reuse the current NVI, fixed SQL templates, prepared statements, and `DbConnectionPool`. Transaction changes remain inside backend `do_update`; metadata and type checks remain private to MySQL introspection/statement code. The idle-probe policy is unchanged; hot-connection poisoning is investigated only if it can be implemented without a public API or backend branch in the generic pool.

**Tech Stack:** C++17, SQLite3, MySQL C API when enabled, CTest, optional live MySQL service.

---

### Task 1: Add a deterministic SQLite concurrent-update regression

**Files:**
- Modify: `test/test_database_conformance.cpp`
- Modify: `src/db/sqlite_database.cpp:442-481`

- [ ] **Step 1: Add a two-connection update test**

Seed a row with two independently writable columns, open two `Database` instances on the same WAL file,
and use a barrier to start two `update` calls that modify different columns. Repeat the barrier sequence
enough times to expose the current read-then-write window, then read the final row and assert both latest
column values are present. Keep the test deterministic by synchronizing start and using SQLite's busy
timeout; do not assert a sleep duration.

- [ ] **Step 2: Run and record RED or classify the environment**

```bash
cmake --build build-wsl -j2 --target test_database_conformance
ctest --test-dir build-wsl -R '^test_database_conformance$' --output-on-failure
```

If the test cannot reliably produce the race on the available SQLite build, retain it as a stress/regression
test and record that the live race was not reproduced; do not claim the old implementation is proven safe.

- [ ] **Step 3: Bound SQLite read-modify-write with `BEGIN IMMEDIATE`**

In `SqliteDatabase::do_update`, start an immediate transaction before `do_select`, and use a small RAII
guard whose destructor rolls back unless explicitly committed. Commit only after the fixed UPDATE succeeds;
check every `sqlite3_exec` return code and map failures to `kBackendError`. The 0-row path must explicitly
rollback and still return `kOk + 0`.

- [ ] **Step 4: Verify and commit**

```bash
ctest --test-dir build-wsl -R '^test_database_conformance$' --output-on-failure
ctest --test-dir build-wsl --output-on-failure
git add src/db/sqlite_database.cpp test/test_database_conformance.cpp
git commit -m "fix: serialize SQLite read-modify-write updates"
```

### Task 2: Use a locking read and check MySQL transaction completion

**Files:**
- Modify: `src/db/mysql_database.cpp:354-365,672-850`
- Modify: `test/test_mysql_crud.cpp`

- [ ] **Step 1: Add MySQL lock/commit failure tests**

When a live MySQL service is configured, start two connections and use a barrier to update different
columns. Assert the final row keeps both changes. Add a fault-injection seam in the test-only connection
setup (or stop the test connection immediately before commit) and assert a commit failure is not reported
as `DbStatus::kOk`. If no MySQL environment is configured, print a skip; if credentials are configured
but connection/setup fails, return nonzero instead of silently passing.

- [ ] **Step 2: Run and record the current behavior**

```bash
cmake -S . -B build-wsl-mysql -DCPPPOOLS_USE_MYSQL=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-wsl-mysql -j2 --target test_mysql_crud
ctest --test-dir build-wsl-mysql -R '^test_mysql_crud$' --output-on-failure
```

Expected current RED where the service supports the race: `select_sql` has no `FOR UPDATE`, and the
TxGuard destructor ignores a failed COMMIT. If the service is absent, record `not run` rather than RED.

- [ ] **Step 3: Add a locking-read SQL variant and explicit commit**

Keep ordinary selects unchanged and add a private boolean to the SQL builder:

```cpp
std::string select_sql(const TableInfo& table,
                       const std::string& where_column,
                       bool lock_for_update = false);
// ... WHERE ... = ?
if (lock_for_update) sql += " FOR UPDATE";
```

`do_update` prepares/uses the locking variant. Replace the success path that only sets `guard.commit`
with an explicit `mysql_query(conn_, "COMMIT")` check. On failure, attempt `ROLLBACK`, mark the guard
finished, and return `kBackendError` with the driver message. Keep the guard as a rollback fallback for
all earlier returns and check the explicit 0-row rollback too.

- [ ] **Step 4: Make configured MySQL failures fail the test**

In both MySQL tests, distinguish missing user/password environment from a configured but unreachable
server. Only the former returns 0 with `[skip]`; `mysql_real_connect` failure after configuration prints
the error and returns 1.

- [ ] **Step 5: Verify and commit**

```bash
ctest --test-dir build-wsl-mysql -R '^(test_mysql_crud|test_mysql_introspection)$' --output-on-failure
ctest --test-dir build-wsl --output-on-failure
git add src/db/mysql_database.cpp test/test_mysql_crud.cpp test/test_mysql_introspection.cpp
git commit -m "fix: lock MySQL read-modify-write transactions"
```

### Task 3: Reject unsigned overflow and every numeric truncation

**Files:**
- Modify: `src/db/mysql_database.cpp:480-590`
- Modify: `src/db/mysql_database.h:84-100`
- Modify: `test/test_mysql_crud.cpp`

- [ ] **Step 1: Add unsigned and truncation fixtures**

Create a `BIGINT UNSIGNED` column and insert a value greater than `INT64_MAX` with raw SQL. Assert the
middleware returns a backend/foreign-data error rather than a negative integer. Add a numeric value that
exceeds the bound buffer and assert it is rejected, not silently truncated. Preserve the existing 64-KB
TEXT cap test.

- [ ] **Step 2: Run and record RED**

```bash
ctest --test-dir build-wsl-mysql -R '^test_mysql_crud$' --output-on-failure
```

Expected RED: `bind_result` forces `is_unsigned=false`, and `MYSQL_DATA_TRUNCATED` is currently accepted
for non-text numeric slots.

- [ ] **Step 3: Carry unsigned metadata and reject unsafe values**

Read `UNSIGNED_FLAG` from each `MYSQL_FIELD` when binding results. Check `ReadSlot.error` for integer and
real fields in `read_column`; for unsigned integer data, compare against `INT64_MAX` before converting.
Return the existing foreign-data/backend error shape on overflow or truncation. Do not widen `Value` or
change the public variant.

- [ ] **Step 4: Verify and commit**

```bash
ctest --test-dir build-wsl-mysql -R '^test_mysql_crud$' --output-on-failure
git add src/db/mysql_database.h src/db/mysql_database.cpp test/test_mysql_crud.cpp
git commit -m "fix: reject unsafe MySQL numeric results"
```

### Task 4: Escape MySQL metadata literals and verify special names

**Files:**
- Modify: `src/db/mysql_database.cpp:148-220`
- Modify: `test/test_mysql_introspection.cpp`

- [ ] **Step 1: Add a table-name apostrophe fixture**

Create a legal quoted table whose name contains an apostrophe, then run backend introspection and assert
the table and columns appear. Keep identifier quoting (`quote_ident`) for CRUD SQL separate from literal
escaping used in `information_schema` predicates.

- [ ] **Step 2: Run and record RED**

```bash
ctest --test-dir build-wsl-mysql -R '^test_mysql_introspection$' --output-on-failure
```

Expected RED: direct concatenation into `table_name = '...'` terminates the literal or changes the query.

- [ ] **Step 3: Add one private literal-escaping helper and use it in both metadata queries**

Use the active connection's `mysql_real_escape_string_quote` (or the equivalent C API available in the
supported client) before constructing the fixed metadata SQL. Do not accept client request strings here;
the value is still schema metadata, but it must be a valid SQL literal.

- [ ] **Step 4: Verify and commit**

```bash
ctest --test-dir build-wsl-mysql -R '^test_mysql_introspection$' --output-on-failure
git add src/db/mysql_database.cpp test/test_mysql_introspection.cpp
git commit -m "fix: escape MySQL metadata table literals"
```

### Task 5: Decide and document hot-connection recovery without changing pool semantics

**Files:**
- Inspect: `include/cpppools/db/db_connection_pool.h:59-68,120-147`
- Inspect: `src/db/db_connection_pool.cpp:148-222,225-256`
- Modify only if a no-API implementation is proven: corresponding source/tests
- Modify: `docs/superpowers/specs/2026-08-17-code-hardening-design.md` if deferred

- [ ] **Step 1: Add a live continuous-traffic disconnect test**

Force-close a MySQL connection while it is hot, issue requests without waiting 30 seconds, and record
whether the pool can rebuild. The test must measure `rebuild_total` and have a finite deadline.

- [ ] **Step 2: Do not change the generic pool just to satisfy the test**

Calling `healthy()` on every release would add a network probe to the documented hot path; dynamic-casting
to MySQL would reverse the dependency direction. If no private poison signal can be carried through the
existing `Database` contract, keep the current idle-probe behavior and document this as a spec-defined
limitation. Do not add a public `poison()` method in this plan.

- [ ] **Step 3: Verify and record the decision**

```bash
ctest --test-dir build-wsl --output-on-failure
```

The completion report must state whether a live MySQL hot-disconnect test ran and whether the behavior was
changed or explicitly deferred; an environment skip is not a correctness pass.
