# Schema and Cache Correctness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended; use executing-plans if working inline). Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make schema uniqueness metadata and row-cache behavior match the existing CRUD and cluster-cache contracts, including allowlists, complete rows, generation isolation, and metrics.

**Architecture:** Keep `CachingDatabase` as the existing decorator and keep `RowCache` sharded/LRU/singleflight. Add only private table eligibility and flight-generation metadata. Empty `cache.tables` remains “all tables”; non-empty is a whitelist. No key encoding, TTL value, shard count, or ownership decision changes.

**Tech Stack:** C++17, SQLite3, optional MySQL, CTest, ASan.

---

### Task 1: Prove and fix the cache table allowlist

**Files:**
- Modify: `src/db/caching_database.h:22-45,37-55,81-151`
- Modify: `src/db/datasource_registry.cpp:87-125`
- Modify: `test/test_config.cpp`
- Modify: `test/test_caching_database.cpp`

- [ ] **Step 1: Add configuration and decorator tests**

Parse both configurations and assert their semantics:

```cpp
config::AppConfig::Cache all;
all.enabled = true;
assert(all.tables.empty());  // empty means all

config::AppConfig::Cache selected;
selected.enabled = true;
selected.tables = {"users"};
assert(selected.tables.size() == 1);
```

Build a registry with `users` and `audit` tables, set `tables={"users"}`, perform an external raw
SQLite update on `audit`, and assert the next `audit` select observes it immediately (no cache wrapper).
Also assert `users` still produces a cache hit on its second select. Add the empty-list case and assert
both tables can hit.

- [ ] **Step 2: Run targeted tests and record RED**

```bash
cmake --build build-wsl -j2 --target test_config test_caching_database
ctest --test-dir build-wsl -R '^test_(config|caching_database)$' --output-on-failure
```

Expected RED: registry currently wraps every introspected table because it only checks `cache_cfg.enabled`.

- [ ] **Step 3: Pass and enforce the whitelist privately**

Add a constructor parameter with a default preserving direct test callers:

```cpp
CachingDatabase(std::unique_ptr<Database> inner,
                std::shared_ptr<RowCache> cache,
                std::vector<std::string> cache_tables = {})
    : Database(inner->schema(), inner->name()),
      inner_(std::move(inner)), cache_(std::move(cache)),
      cache_tables_(std::move(cache_tables)) {}
```

Implement `cacheable_table(table)` as `cache_tables_.empty()` or a stable linear search. In every
`do_select`, `do_insert`, `do_update`, and `do_remove`, return `inner_->...` without cache lookup or
invalidation when the table is not eligible. Capture `cache_cfg.tables` in the registry factory and pass
it to every slot decorator.

- [ ] **Step 4: Verify and commit**

```bash
ctest --test-dir build-wsl -R '^test_(config|caching_database|cache_metrics)$' --output-on-failure
ctest --test-dir build-wsl --output-on-failure
git add src/db/caching_database.h src/db/datasource_registry.cpp \
  test/test_config.cpp test/test_caching_database.cpp
git commit -m "fix: honor cache table allowlist"
```

### Task 2: Always backfill a complete row and project at the decorator boundary

**Files:**
- Modify: `src/db/caching_database.h:37-79,154-195`
- Modify: `test/test_caching_database.cpp:145-174`

- [ ] **Step 1: Add the projection-order reversal regression**

Insert a row, issue a first miss requesting only `name`, then request `id` and `rating` using the same
where key. Assert the second result contains the stored integer and real values, not `Value{}`/NULL.
Add a concurrent two-thread version where one request asks `name` and the other asks `id`; assert both
requests return their requested values and the inner fake database observes one full-row read.

- [ ] **Step 2: Run and record RED**

```bash
cmake --build build-wsl -j2 --target test_caching_database
ctest --test-dir build-wsl -R '^test_caching_database$' --output-on-failure
```

Expected RED: the second projection receives a default `Value{}` because the first miss cached only
`name`.

- [ ] **Step 3: Backfill full rows and project the returned result**

At the leader boundary, copy the request and clear `columns` before calling the inner database:

```cpp
const auto read_started = std::chrono::steady_clock::now();
SelectRequest full = req;
full.columns.clear();
DbResult<ResultSet> result = inner_->select(full);
```

Convert the first full row with `to_row_data`, finish the flight with the read-start timestamp, and
return a projection of that row matching the original request. For a zero-row result, construct the
original requested column list with an empty `rows` vector; do not expose the full-row column list to the
caller. Followers always call the existing `make_hit(req, row)` path.

- [ ] **Step 4: Verify targeted, conformance, and sanitizer tests**

```bash
ctest --test-dir build-wsl -R '^(test_caching_database|test_database_conformance)$' --output-on-failure
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-wsl-asan -R '^test_caching_database$' --output-on-failure
```

Expected GREEN: projection order/duplicates/full-row semantics remain unchanged, and no non-requested
columns leak into the response.

- [ ] **Step 5: Commit**

```bash
git add src/db/caching_database.h test/test_caching_database.cpp
git commit -m "fix: backfill complete rows before cache projection"
```

### Task 3: Correct single-column uniqueness for composite primary keys

**Files:**
- Modify: `src/db/sqlite_database.cpp:217-243`
- Modify: `src/db/mysql_database.cpp:180-266`
- Modify: `test/test_database_conformance.cpp`
- Modify: `test/test_mysql_introspection.cpp`

- [ ] **Step 1: Add composite-primary-key assertions**

Seed `pk_pairs(a INTEGER, b INTEGER, PRIMARY KEY(a,b))`. Assert both columns have
`primary_key == true` and `unique == false`, then call select/update/delete with only `a` and assert
the NVI returns `kInvalidRequest` with the existing unique-key message.

- [ ] **Step 2: Run and record RED**

```bash
cmake --build build-wsl -j2 --target test_database_conformance
ctest --test-dir build-wsl -R '^test_database_conformance$' --output-on-failure
```

Expected RED: SQLite currently marks every `pk > 0` column unique. The optional MySQL test has the
same defect in its initial primary-key mapping.

- [ ] **Step 3: Mark unique only after the primary-key cardinality is known**

During column introspection set `col.unique = false` for primary-key columns, count primary-key columns,
then set `unique=true` only when exactly one primary-key column exists. Leave single-column UNIQUE index
handling unchanged and keep composite UNIQUE indexes non-unique at the column level.

- [ ] **Step 4: Verify and commit**

```bash
ctest --test-dir build-wsl -R '^test_database_conformance$' --output-on-failure
ctest --test-dir build-wsl --output-on-failure
git add src/db/sqlite_database.cpp src/db/mysql_database.cpp \
  test/test_database_conformance.cpp test/test_mysql_introspection.cpp
git commit -m "fix: keep composite primary keys non-unique per column"
```

### Task 4: Retire stale flights and make cache metrics/TTL precise

**Files:**
- Modify: `src/db/row_cache.h:109-243,250-262,321-337`
- Modify: `src/db/caching_database.h:47-95`
- Modify: `test/test_row_cache.cpp`
- Modify: `test/test_caching_database.cpp`
- Modify: `test/test_cache_metrics.cpp`

- [ ] **Step 1: Add deterministic generation tests**

Add tests for these ordered calls:

```cpp
auto old = cache.join_flight(key);            // old leader
cache.invalidate(key);                        // write completes
auto fresh = cache.join_flight(key);          // must be a new leader
assert(fresh.leader);
cache.finish_flight(key, make_row(1, "new"), cache.generation(key));
assert(old.future->get() == std::nullopt);    // retired waiter is not given old data
assert(std::get<std::string>((*fresh.future->get())[1].second) == "new");
```

Add `clear()` during an in-flight read and assert the old result cannot repopulate the cache. Add N
concurrent miss callers and assert `stats().misses == 1` and `singleflight_merged == N-1`.

- [ ] **Step 2: Run and record RED**

```bash
cmake --build build-wsl -j2 --target test_row_cache test_caching_database test_cache_metrics
ctest --test-dir build-wsl -R '^test_(row_cache|caching_database|cache_metrics)$' --output-on-failure
```

Expected RED: a new join can attach to the old flight after invalidation, `clear()` leaves its generation
unchanged, and every `get` miss increments the counter before singleflight is known.

- [ ] **Step 3: Add flight generation and retire promises outside shard locks**

Store the generation with each flight. `invalidate` and `clear` must remove the old map entry and collect
its promise, increment generation, then fulfill the retired promise with `std::nullopt` after releasing the
lock. `finish_flight` may claim a promise only when its stored generation equals
`expected_generation`; a stale leader must never erase a newer flight. Mark the mutable shard bookkeeping
needed by the existing const `clear()` API as `mutable` rather than changing `DataSourceRegistry` APIs.

Change `get()` to report hits only; increment `misses` exactly when `join_flight` creates a new leader.
Pass `read_started` from `CachingDatabase` to `finish_flight` and store it as `cached_at` so DB latency
does not extend TTL. Keep direct `put()` timestamped at its call time.

- [ ] **Step 4: Verify concurrency and sanitizer behavior**

```bash
ctest --test-dir build-wsl -R '^test_(row_cache|caching_database|cache_metrics)$' --output-on-failure
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-wsl-asan -R '^test_(row_cache|caching_database)$' --output-on-failure
ctest --test-dir build-wsl --output-on-failure
```

Expected GREEN: no stale value reaches a post-write waiter, one source read produces one miss, clear
retires old backfills, and TTL starts at source-read initiation.

- [ ] **Step 5: Commit**

```bash
git add src/db/row_cache.h src/db/caching_database.h \
  test/test_row_cache.cpp test/test_caching_database.cpp test/test_cache_metrics.cpp
git commit -m "fix: isolate stale cache flights and metrics"
```
