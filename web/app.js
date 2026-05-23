(function () {
  "use strict";

  const $ = (id) => document.getElementById(id);

  const dom = {
    userId: $("userId"),
    workMs: $("workMs"),
    holdMs: $("holdMs"),
    burstCount: $("burstCount"),
    sendBtn: $("sendBtn"),
    burstBtn: $("burstBtn"),
    refreshBtn: $("refreshBtn"),
    status: $("status"),
    output: $("output"),
    serviceDot: $("serviceDot"),
    serviceText: $("serviceText"),
    dbMode: $("dbMode"),

    mWorkers: $("mWorkers"),
    mPending: $("mPending"),
    mActive: $("mActive"),
    mP95: $("mP95"),

    mMemInUse: $("mMemInUse"),
    mMemAvail: $("mMemAvail"),
    mMemTotal: $("mMemTotal"),
    mMemPeak: $("mMemPeak"),

    mDbFree: $("mDbFree"),
    mDbTotal: $("mDbTotal"),
    mDbMin: $("mDbMin"),
    mDbTimeout: $("mDbTimeout"),

    threadBar: $("threadBar"),
    memBar: $("memBar"),
    dbBar: $("dbBar")
  };

  function clamp(n, min, max) {
    return Math.max(min, Math.min(max, n));
  }

  function readInt(node, fallback, min, max) {
    const raw = Number(node.value);
    if (!Number.isFinite(raw)) return fallback;
    return clamp(Math.floor(raw), min, max);
  }

  function setText(node, value) {
    if (!node) return;
    node.textContent = value == null ? "–" : String(value);
  }

  function setNumber(node, value, digits) {
    if (!node) return;
    if (value == null || !Number.isFinite(Number(value))) {
      node.textContent = "–";
      return;
    }
    node.textContent = digits != null ? Number(value).toFixed(digits) : String(value);
  }

  function setBar(node, percent) {
    if (!node) return;
    node.style.width = clamp(percent, 0, 100).toFixed(1) + "%";
  }

  function setStatus(text, level) {
    dom.status.className = "status" + (level ? " " + level : "");
    dom.status.textContent = text;
  }

  function setService(ok, label) {
    dom.serviceDot.className = "dot " + (ok ? "ok" : "err");
    dom.serviceText.textContent = label;
  }

  async function fetchJson(url, timeoutMs) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs);
    try {
      const resp = await fetch(url, { signal: controller.signal });
      const text = await resp.text();
      let data = null;
      try { data = JSON.parse(text); } catch (_) { /* keep raw text */ }
      return { ok: resp.ok, status: resp.status, text, data };
    } finally {
      clearTimeout(timer);
    }
  }

  function buildProcessUrl(userId) {
    const workMs = readInt(dom.workMs, 0, 0, 5000);
    const holdMs = readInt(dom.holdMs, 0, 0, 5000);
    const q = new URLSearchParams({
      user: String(userId),
      work_ms: String(workMs),
      hold_ms: String(holdMs)
    });
    return "/api/process?" + q.toString();
  }

  function applyStats(s) {
    const uptimeSec = Math.round((s.uptime_ms || 0) / 1000);
    setService(true, "已运行 " + uptimeSec + "s · " + (s.db_mode || "mock"));
    setText(dom.dbMode, s.db_mode || "mock");

    const workers = s.thread_pool_workers || 0;
    const active = s.active_process_requests || 0;
    setText(dom.mWorkers, workers);
    setText(dom.mPending, s.thread_pool_pending);
    setText(dom.mActive, active);
    setNumber(dom.mP95, s.p95_latency_ms, 2);
    setBar(dom.threadBar, workers > 0 ? (active / workers) * 100 : 0);

    const memTotal = s.memory_pool_total || 0;
    const memInUse = s.memory_pool_in_use || 0;
    setText(dom.mMemInUse, memInUse);
    setText(dom.mMemAvail, s.memory_pool_available);
    setText(dom.mMemTotal, memTotal);
    setText(dom.mMemPeak, s.peak_memory_pool_in_use);
    setBar(dom.memBar, memTotal > 0 ? (memInUse / memTotal) * 100 : 0);

    const dbTotal = s.db_pool_total || 0;
    const dbFree = s.db_pool_free || 0;
    setText(dom.mDbFree, dbFree);
    setText(dom.mDbTotal, dbTotal);
    setText(dom.mDbMin, s.min_db_pool_free);
    setText(dom.mDbTimeout, s.db_timeout_requests);
    setBar(dom.dbBar, dbTotal > 0 ? ((dbTotal - dbFree) / dbTotal) * 100 : 0);
  }

  let serviceErrors = 0;

  async function refreshStats() {
    try {
      const r = await fetchJson("/api/stats", 3000);
      if (!r.ok || !r.data) {
        serviceErrors += 1;
        if (serviceErrors >= 2) setService(false, "服务异常");
        return;
      }
      serviceErrors = 0;
      applyStats(r.data);
    } catch (_err) {
      serviceErrors += 1;
      if (serviceErrors >= 2) setService(false, "无法连接");
    }
  }

  function setActionsDisabled(disabled) {
    dom.sendBtn.disabled = disabled;
    dom.burstBtn.disabled = disabled;
    dom.refreshBtn.disabled = disabled;
  }

  function showOutput(content) {
    dom.output.classList.remove("empty");
    dom.output.textContent = content;
  }

  async function handleSend() {
    const userId = readInt(dom.userId, 1, 0, 1000000);
    const url = buildProcessUrl(userId);

    setActionsDisabled(true);
    setStatus("发送 GET " + url, "busy");

    const t0 = performance.now();
    try {
      const r = await fetchJson(url, 9000);
      const cost = Math.round(performance.now() - t0);

      if (!r.ok) {
        showOutput("HTTP " + r.status + "\n" + r.text);
        setStatus("失败 · " + r.status + " · " + cost + "ms", "err");
      } else {
        showOutput(r.data ? JSON.stringify(r.data, null, 2) : r.text);
        setStatus("成功 · " + cost + "ms", "ok");
      }
      await refreshStats();
    } catch (err) {
      setStatus("异常 · " + String(err), "err");
    } finally {
      setActionsDisabled(false);
    }
  }

  async function handleBurst() {
    const base = readInt(dom.userId, 1, 0, 1000000);
    const burst = readInt(dom.burstCount, 20, 1, 80);

    setActionsDisabled(true);
    setStatus("并发发送 " + burst + " 次 GET /api/process", "busy");

    const poller = setInterval(refreshStats, 250);
    const t0 = performance.now();
    try {
      const jobs = [];
      for (let i = 0; i < burst; i += 1) {
        jobs.push(fetchJson(buildProcessUrl(base + i), 15000));
      }
      const results = await Promise.allSettled(jobs);
      const cost = Math.round(performance.now() - t0);

      let ok = 0;
      let fail = 0;
      let minDbFree = Number.POSITIVE_INFINITY;
      let peakMem = 0;
      let lastPayload = null;

      for (const r of results) {
        if (r.status === "fulfilled" && r.value.ok && r.value.data) {
          ok += 1;
          const d = r.value.data;
          if (typeof d.db_free === "number") minDbFree = Math.min(minDbFree, d.db_free);
          if (typeof d.memory_pool_in_use === "number") peakMem = Math.max(peakMem, d.memory_pool_in_use);
          lastPayload = d;
        } else {
          fail += 1;
        }
      }

      const summary = {
        requested: burst,
        success: ok,
        failed: fail,
        elapsed_ms: cost,
        observed_min_db_free: Number.isFinite(minDbFree) ? minDbFree : null,
        observed_peak_memory_in_use: peakMem,
        last_response: lastPayload
      };
      showOutput(JSON.stringify(summary, null, 2));

      setStatus(
        "完成 · 成功 " + ok + " / 失败 " + fail + " · " + cost + "ms",
        fail > 0 ? "err" : "ok"
      );
      await refreshStats();
    } catch (err) {
      setStatus("异常 · " + String(err), "err");
    } finally {
      clearInterval(poller);
      setActionsDisabled(false);
    }
  }

  dom.sendBtn.addEventListener("click", handleSend);
  dom.burstBtn.addEventListener("click", handleBurst);
  dom.refreshBtn.addEventListener("click", refreshStats);

  // 页面隐藏时暂停轮询，节省资源；显示时立刻拉一次再恢复轮询。
  let pollTimer = null;
  function startPolling() {
    if (pollTimer != null) return;
    pollTimer = setInterval(refreshStats, 1500);
  }
  function stopPolling() {
    if (pollTimer == null) return;
    clearInterval(pollTimer);
    pollTimer = null;
  }
  document.addEventListener("visibilitychange", function () {
    if (document.hidden) {
      stopPolling();
    } else {
      refreshStats();
      startPolling();
    }
  });

  refreshStats();
  startPolling();
})();
