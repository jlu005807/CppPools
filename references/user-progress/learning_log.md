# Learning Log

## Mastered Topics
- Frontend observability debugging workflow (request path verification + stats interpretation) - 2026-04-20

## Areas for Review
- Sampling interval effects on short-lived resource metrics (active requests, pool occupancy)
- Quantifying performance impact (latency, throughput, pool utilization)

## Goals
- Stabilize frontend observability interactions for CppPools dashboard - Status: Completed
- Prepare concise interview narrative for CppPools - Status: In Progress
- Refine CppPools resume bullets - Status: In Progress

---

### Session 1 - 2026-04-20

**Topics Covered**:
- Frontend request flow for `/api/process` and `/api/stats`
- Why peak resource usage can be missed by low-frequency polling
- Simplifying UI controls for reliable load generation

**Problems Solved**:
- Dashboard looked static (DB idle, object pool available, thread pending unchanged)
- Reduced complex wave-based operations to a simpler single-burst workflow

**Skills Practiced**:
- Code review and refactoring mode
- Debugging with backend verification via concurrent API calls

**Notes**:
- Backend is functioning; issue was mainly observability timing and UX complexity.
- Added burst summary from `/api/process` responses to capture min/peak values directly.

---

### Session 2 - 2026-05-14

**Topics Covered**:
- Fast onboarding path: build, run, and verify core endpoints
- Interview-ready project story (problem -> solution -> impact)
- Key metrics to mention (latency percentiles, pool utilization, timeouts)

**Problems Solved**:
- Created a concise plan for explaining the project under interview time limits

**Skills Practiced**:
- Interview preparation mode
- Project architecture summarization

**Notes**:
- Still need concrete numbers to quantify impact.

---

### Session 3 - 2026-05-17

**Topics Covered**:
- Resume bullet validation against codebase (ThreadPool/ObjectPool/DBPool/HttpServer)
- Observability metrics phrasing (p50/p95/p99, pool snapshots, timeouts)
- Dashboard load generation and stat polling behavior

**Problems Solved**:
- Mapped resume claims to concrete implementation points
- Identified wording tweaks to align with current features

**Skills Practiced**:
- Code review and narrative refinement
- Project summarization for resumes

**Notes**:
- Add measurable results (latency, QPS, pool utilization) once benchmarked
- Confirm target role and language to tailor phrasing
- Provided simpler resume bullets due to low familiarity with project
- Removed frontend-focused wording per user request

---

### Session 4 - 2026-05-17

**Topics Covered**:
- Thread pool queue behavior, wait/notify, and spurious wakeups
- Connection pool timeout acquisition and RAII guard semantics
- /api/stats percentile computation and bounded memory window
- HTTP server I/O model and parsing limitations

**Problems Solved**:
- Prepared concise, code-accurate interview answers for CppPools

**Skills Practiced**:
- Interview preparation mode
- Backend design explanation

**Notes**:
- Call out current limitations (no bounded queue, simple HTTP parsing)
- Suggest concrete improvement paths when asked

---
