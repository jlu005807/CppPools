# Learning Log

## Mastered Topics

- ThreadPool MVP (fixed workers, task queue, future, graceful shutdown) - 2026-04-14
- ObjectPool<T> with RAII return handle - 2026-04-17
- Mock DBConnectionPool with timeout acquire and guard-based release - 2026-04-17
- End-to-end request pipeline (network -> pools -> response) - 2026-04-17

## Areas for Review

- ThreadPool exception handling and optional rejection strategy
- Advanced memory pool design (size classes, cache locality)
- Real DB health-check and stale-connection replacement
- HTTP parser robustness and persistent connections

## Goals

- Complete ThreadPool baseline and tests - Status: Completed
- Integrate ObjectPool into request path - Status: Completed
- Integrate DBConnectionPool with RAII - Status: Completed
- Build frontend observability dashboard - Status: Completed
- Add benchmark and optimization report - Status: In Progress

---

### Session 1 - 2026-04-14

**Topics Covered**:
- ThreadPool structure and lifecycle
- `packaged_task` + `future` for async result delivery
- `condition_variable` based worker wakeup

**Problems Solved**:
- Implemented reusable ThreadPool MVP
- Added safe post-shutdown submit protection

**Skills Practiced**:
- Project Guidance mode
- Concept Learning mode

**Notes**:
- ThreadPool demo and basic tests were successfully run.

---

### Session 2 - 2026-04-14

**Topics Covered**:
- Compile, link, run fundamentals
- CMake role and target-based organization
- Static vs dynamic linking concepts

**Problems Solved**:
- Clarified why libraries are defined before executables
- Clarified include propagation via `target_include_directories`

**Skills Practiced**:
- Concept Learning mode

**Notes**:
- Prepared to extend project beyond a single module.

---

### Session 3 - 2026-04-17

**Topics Covered**:
- Three-pool integration in one backend service
- Socket-based HTTP server architecture
- Frontend-to-backend integration for live demo

**Problems Solved**:
- Completed full request path through all three pools
- Added platform-safe socket handling for Windows/Unix

**Skills Practiced**:
- Project Guidance mode

**Notes**:
- API flow verified with real requests.

---

### Session 4 - 2026-04-17

**Topics Covered**:
- Frontend dashboard redesign and flow-field visualization controls
- Runtime observability endpoint (`/api/stats`)
- Educational comments on core C++ modules

**Problems Solved**:
- Added metrics cards + history list + burst request tool
- Exposed ThreadPool queue depth and worker count
- Added server counters (total/api/static/error/uptime)

**Skills Practiced**:
- Code mentor project coaching
- Algorithmic-art inspired visualization refinement

**Notes**:
- Build/tests passed and API verification succeeded after refactor.

---

### Session 5 - 2026-04-17

**Topics Covered**:
- Algorithmic visualization reproducibility (seed controls + deterministic regeneration)
- More detailed inline documentation for core C++ modules
- README restructuring for build/run/test onboarding

**Problems Solved**:
- Added seed navigation controls (Prev/Next/Random) and reproducible particle reset
- Improved API fetch resilience with timeout wrapper in frontend
- Expanded testing instructions with both CMake and fallback direct-compile paths

**Skills Practiced**:
- Algorithmic-art workflow applied to frontend generation logic
- Documentation-first refactor for maintainability

**Notes**:
- Dashboard now better supports demonstration and debugging workflows.

---

### Session 6 - 2026-04-17

**Topics Covered**:
- Chinese README rewrite for onboarding clarity
- Full API documentation split into backend/frontend docs
- Deterministic seed-control UX documentation alignment

**Problems Solved**:
- Added `docs/api/backend_api.md` for route/params/status/payload references
- Added `docs/api/frontend_api.md` for frontend call flow and internal function contracts
- Updated README with clear structure/build/run/test instructions and doc links

**Skills Practiced**:
- Documentation-first engineering workflow
- API contract communication for interview-ready projects

**Notes**:
- Documentation now matches current endpoint behavior and frontend interaction model.

---

### Session 7 - 2026-04-17

**Topics Covered**:
- Explicit multi-pool demo executable for backend-only validation
- Dedicated multi-pool frontend page for clearer pool observability
- Server route extension for new static page assets

**Problems Solved**:
- Added `demo_multi_pools` to show all three pools in one console run
- Added `/multi` page with pool metrics bars and pressure request action
- Updated README and API docs to include the new demo and page paths

**Skills Practiced**:
- End-to-end demo productization for interview presentation
- Docs and runtime behavior alignment

**Notes**:
- User can now choose CLI demo or browser demo to inspect three-pool behavior.

---

### Session 8 - 2026-04-17

**Topics Covered**:
- Build output directory normalization (`output/bin`, `output/lib`)
- HTTP route parsing robustness (path/query split)
- API/documentation refinement and health endpoint

**Problems Solved**:
- Updated CMake so final binaries/libraries are emitted under `output/`
- Fixed query parsing approach for `/api/process` by separating route path and query string
- Added `GET /api/health` and refreshed README + API docs in Chinese

**Skills Practiced**:
- Build-system hygiene
- Service hardening and API contract clarity

**Notes**:
- Tests and demo flow continue to run after refactor.

---

### Session 9 - 2026-04-17

**Topics Covered**:
- Latency percentile telemetry for `/api/stats` (P50/P95/P99)
- Real-time pending/latency trend chart for `/multi` dashboard
- DB pool upgrade path to real MySQL (config + build option)

**Problems Solved**:
- Added bounded latency sample window and percentile computation in server
- Added trend canvas visualization driven by `thread_pool_pending` and `p95_latency_ms`
- Added `CPPPOLS_USE_MYSQL` build option and runtime DB env configuration

**Skills Practiced**:
- Observability-first backend design
- Frontend operational visualization
- Progressive migration from mock dependency to real service integration

**Notes**:
- MySQL mode is now supported when dependencies are available at build time.

---
### Session 10 - 2026-04-17

**Topics Covered**:
- Learning-friendly hardening for `HttpServer` request handling and telemetry
- Detailed inline comments around timeout/window tuning and pool safety contracts
- Stronger pool test coverage for RAII and defensive release semantics

**Problems Solved**:
- Added `HttpServerConfig` and env-driven knobs: `CPPPOOLS_DB_ACQUIRE_TIMEOUT_MS`, `CPPPOOLS_LATENCY_WINDOW_SIZE`
- Added `/api/stats` fields: `avg_latency_ms`, `db_acquire_timeout_ms`, `latency_window_size`
- Added strict `/api/process?user=` validation with `400 Bad Request` for invalid non-negative integer input
- Added defensive duplicate-release protection in `DBConnectionPool` via `in_use_conns_`
- Expanded `tests/test_pools.cpp` with timeout, RAII auto-release, and duplicate-release scenarios

**Skills Practiced**:
- Service robustness improvements with minimal API surface changes
- Writing explanatory comments that map implementation to runtime behavior
- Test-driven confidence for resource-pool concurrency invariants

**Notes**:
- Mock mode and MySQL mode both remain runnable after these upgrades.

---
