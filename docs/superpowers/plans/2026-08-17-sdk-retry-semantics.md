# Python SDK Retry Semantics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enforce the documented retry matrix for non-idempotent inserts and preserve conservative cluster hints while following 307 responses.

**Architecture:** Keep `urllib` as the transport and keep routing/topology ownership in `ClusterClient`. Refactor only private request helpers so redirect targets use the same bounded retry/error classification as ordinary requests. No public method signatures, hash-ring inputs, or server behavior changes.

**Tech Stack:** Python 3 standard library, `unittest`, injected fake HTTP callables.

---

### Task 1: Lock down the auto-increment no-failover invariant

**Files:**
- Modify: `sdk/python/tests/test_client.py` near the existing insert retry tests
- Modify: `sdk/python/cpppools/client.py:173-222`

- [ ] **Step 1: Add a commit-then-disconnect fake**

Construct a client with two nodes and a fake transport that records POSTs. For the first node's
auto-increment insert, mutate a `committed` list and then raise `errors.NodeUnavailable`; make the second
node return a successful response if it is ever called:

```python
def fake(method, url, payload=None, headers=None):
    calls.append((url, payload, headers))
    if url.endswith("/api/v1/insert") and url.startswith(node_a):
        committed.append(payload)
        raise errors.NodeUnavailable("response lost after commit")
    return response(201, {"affected_rows": 1, "inserted_id": 9})
```

Assert that `client.insert("items", {"name": "x"})` raises `NodeUnavailable`, exactly one insert was
sent, and no request reached node B. Add a companion test proving explicit-key insert still retries under
the existing matrix.

- [ ] **Step 2: Run the focused tests and record RED**

```bash
python3 -m unittest sdk.python.tests.test_client -v
```

Expected RED: `_execute` catches `NodeUnavailable`, blacklists node A, and sends the auto-increment
payload to node B.

- [ ] **Step 3: Stop the outer failover when retry is forbidden**

In `_execute`, preserve the existing blacklist bookkeeping but re-raise before advancing the candidate:

```python
            except errors.NodeUnavailable:
                self._blacklist_node(node_id)
                if not retry_allowed:
                    raise
                next_attempt = used_attempt + 1
                continue
```

The same `retry_allowed` value must be passed into redirect following. Do not reset it at an epoch
refresh; epoch responses are never resent.

- [ ] **Step 4: Run focused and full SDK tests**

```bash
python3 -m unittest sdk.python.tests.test_client -v
python3 -m unittest discover -s sdk/python/tests
```

Expected GREEN: all existing tests plus the new commit-then-disconnect test pass, with the baseline 51
tests increased by the new cases.

- [ ] **Step 5: Commit the non-idempotent fix**

```bash
git add sdk/python/cpppools/client.py sdk/python/tests/test_client.py
git commit -m "fix: do not fail over non-idempotent inserts"
```

### Task 2: Preserve Fallback monotonicity through 307

**Files:**
- Modify: `sdk/python/tests/test_client.py` around existing redirect tests
- Modify: `sdk/python/cpppools/client.py:199-216,257-267`

- [ ] **Step 1: Add normal and fallback redirect header tests**

The fake transport returns a 307 from node A with `Location` pointing at node B, then records the follow.
Assert that a normal owner request's follow has no `X-CppPools-Fallback`, while a request that already
had that hint keeps exactly `X-CppPools-Fallback: 1`:

```python
assert "X-CppPools-Fallback" not in follow_headers
fallback_values = [k for k in follow_headers if k.lower() == "x-cpppools-fallback"]
assert fallback_values == ["X-CppPools-Fallback"]
assert follow_headers["X-CppPools-Fallback"] == "1"
```

- [ ] **Step 2: Run the focused redirect tests and record RED**

```bash
python3 -m unittest sdk.python.tests.test_client -v
```

Expected RED: `_follow_redirects` currently calls `_build_headers(is_fallback=True)` for every 307.

- [ ] **Step 3: Carry the current conservative hint into `_follow_redirects`**

Change the private call shape to include `is_fallback` and `retry_allowed`:

```python
resp = self._follow_redirects(resp, payload, is_fallback, retry_allowed)

def _follow_redirects(self, resp, payload, is_fallback, retry_allowed):
    ...
    headers = self._build_headers(is_fallback=is_fallback)
```

Never compute `is_fallback=True` merely because a response was 307. A redirect may only preserve or
remove conservativeness according to the request that produced it; it may not become more aggressive.

- [ ] **Step 4: Run full SDK tests and commit**

```bash
python3 -m unittest discover -s sdk/python/tests
git add sdk/python/cpppools/client.py sdk/python/tests/test_client.py
git commit -m "fix: preserve fallback hint across redirects"
```

### Task 3: Use controlled target retry and attribution for redirects

**Files:**
- Modify: `sdk/python/tests/test_client.py:214-313`
- Modify: `sdk/python/cpppools/client.py:199-267,291-315`

- [ ] **Step 1: Add target 503 and target disconnect tests**

Cover three facts separately:

1. A known redirect target returns 503 once then 200; the target gets the configured retry/backoff and
   the redirecting responder is not blacklisted.
2. A known redirect target is unreachable; only the target is blacklisted and the outer request may
   refresh/choose another candidate according to `retry_allowed`.
3. An auto-increment insert redirected to a target that becomes unreachable is not sent to another node.

- [ ] **Step 2: Run and record RED**

```bash
python3 -m unittest sdk.python.tests.test_client -v
```

Expected RED: direct `_http` in `_follow_redirects` performs no retry/backoff and the outer catch cannot
identify the redirect target.

- [ ] **Step 3: Add a private URL-to-node and controlled send helper**

Implement private helpers without changing public API:

```python
def _node_for_url(self, url):
    for node_id, base in self._topology.nodes.items():
        if url == base or url.startswith(base + "/"):
            return node_id, url[len(base):]
    return None, None

def _send_url_with_retry(self, url, payload, headers, retry_allowed):
    retries = 0
    while True:
        try:
            return self._http("POST", url, payload, headers)
        except _KNOWN_RETRYABLE:
            if not retry_allowed or retries >= self._retry_max:
                raise
            retries += 1
            time.sleep(self._backoff_delay(retries))
```

For a known target, apply `_blacklist_node(target_id)` on `NodeUnavailable` and re-raise. For an unknown
external Location, re-raise without blacklisting the responder. `_execute` remains responsible for the
outer candidate scan; it must not blacklist the node that merely returned a valid 307.

- [ ] **Step 4: Run focused, full, and optional live tests**

```bash
python3 -m unittest sdk.python.tests.test_client sdk.python.tests.test_transport -v
python3 -m unittest discover -s sdk/python/tests
bash scripts/sdk_e2e.sh  # only when a built server and sqlite3 CLI are available
```

Expected GREEN: redirect retry count, target attribution, fallback monotonicity, and auto-increment
no-failover all hold.

- [ ] **Step 5: Commit redirect transport policy**

```bash
git add sdk/python/cpppools/client.py sdk/python/tests/test_client.py
git commit -m "fix: apply retry policy to redirect targets"
```

### Task 4: Classify truncated HTTP error bodies consistently

**Files:**
- Modify: `sdk/python/cpppools/transport.py:51-104`
- Modify: `sdk/python/tests/test_transport.py:90-164`

- [ ] **Step 1: Add an HTTPError whose `read()` raises**

Inject an `urllib.error.HTTPError` with status 503 whose `read()` raises
`http.client.IncompleteRead`. Assert `transport.request` raises `errors.NodeUnavailable`, not the raw
`IncompleteRead`. Repeat with a normal 503 body to ensure it remains `UnavailableError`.

- [ ] **Step 2: Run the focused test and record RED**

```bash
python3 -m unittest sdk.python.tests.test_transport -v
```

Expected RED: the exception escapes from the `except HTTPError` block before the sibling network-error
handler can classify it.

- [ ] **Step 3: Centralize response-body reads**

Use a small private function and call it for both normal responses and `HTTPError` responses:

```python
def _read_body(response, url):
    try:
        return response.read()
    except (urllib.error.URLError, socket.timeout, TimeoutError,
            ConnectionError, http.client.HTTPException, OSError) as exc:
        raise errors.NodeUnavailable(
            "node response truncated: %s: %s" % (url, exc)) from None
```

Keep status mapping unchanged after a body is successfully read.

- [ ] **Step 4: Run all SDK tests and commit**

```bash
python3 -m unittest discover -s sdk/python/tests
git add sdk/python/cpppools/transport.py sdk/python/tests/test_transport.py
git commit -m "fix: classify truncated HTTP error responses"
```
