(function () {
  "use strict";

  const userIdInput = document.getElementById("userId");
  const sendBtn = document.getElementById("sendBtn");
  const burstBtn = document.getElementById("burstBtn");
  const statusText = document.getElementById("statusText");
  const output = document.getElementById("output");

  const mWorkers = document.getElementById("mWorkers");
  const mPending = document.getElementById("mPending");
  const mDb = document.getElementById("mDb");
  const mMemInUse = document.getElementById("mMemInUse");
  const mMemAvail = document.getElementById("mMemAvail");
  const mTotal = document.getElementById("mTotal");
  const mAvg = document.getElementById("mAvg");
  const mP95 = document.getElementById("mP95");
  const mP99 = document.getElementById("mP99");

  function setStatus(text) {
    statusText.textContent = "状态：" + text;
  }

  function setMetric(node, value) {
    node.textContent = value == null ? "-" : String(value);
  }

  function toFixedOrDash(v) {
    return Number.isFinite(v) ? v.toFixed(3) : "-";
  }

  async function fetchJson(url, timeoutMs) {
    const controller = new AbortController();
    const timer = setTimeout(function () {
      controller.abort();
    }, timeoutMs);

    try {
      const resp = await fetch(url, { signal: controller.signal });
      const text = await resp.text();
      let data = null;
      try {
        data = JSON.parse(text);
      } catch (_e) {
        data = null;
      }
      return { ok: resp.ok, status: resp.status, text: text, data: data };
    } finally {
      clearTimeout(timer);
    }
  }

  async function refreshStats() {
    try {
      const r = await fetchJson("/api/stats", 2500);
      if (!r.ok || !r.data) {
        return;
      }
      const s = r.data;
      setMetric(mWorkers, s.thread_pool_workers);
      setMetric(mPending, s.thread_pool_pending);
      setMetric(mDb, String(s.db_pool_free) + " / " + String(s.db_pool_total));
      setMetric(mMemInUse, s.memory_pool_in_use);
      setMetric(mMemAvail, s.memory_pool_available);
      setMetric(mTotal, s.total_requests);
      setMetric(mAvg, toFixedOrDash(s.avg_latency_ms));
      setMetric(mP95, toFixedOrDash(s.p95_latency_ms));
      setMetric(mP99, toFixedOrDash(s.p99_latency_ms));
    } catch (_err) {
      // stats 失败不打断页面使用
    }
  }

  async function requestOnce(userId) {
    const t0 = performance.now();
    const r = await fetchJson("/api/process?user=" + encodeURIComponent(String(userId)), 3500);
    const cost = performance.now() - t0;

    if (!r.ok) {
      output.textContent = "HTTP " + r.status + "\n" + r.text;
      setStatus("请求失败，耗时 " + cost.toFixed(1) + " ms");
      return false;
    }

    output.textContent = r.data ? JSON.stringify(r.data, null, 2) : r.text;
    setStatus("请求成功，耗时 " + cost.toFixed(1) + " ms");
    return true;
  }

  async function handleSend() {
    const userId = Number(userIdInput.value || 0);
    if (!Number.isFinite(userId) || userId < 0) {
      setStatus("用户ID必须是非负整数");
      output.textContent = "请输入合法的用户ID";
      return;
    }

    sendBtn.disabled = true;
    burstBtn.disabled = true;
    setStatus("正在发送请求...");

    try {
      await requestOnce(Math.floor(userId));
      await refreshStats();
    } catch (err) {
      setStatus("请求异常");
      output.textContent = String(err);
    } finally {
      sendBtn.disabled = false;
      burstBtn.disabled = false;
    }
  }

  async function handleBurst() {
    const base = Number(userIdInput.value || 0);
    if (!Number.isFinite(base) || base < 0) {
      setStatus("用户ID必须是非负整数");
      output.textContent = "请输入合法的用户ID";
      return;
    }

    sendBtn.disabled = true;
    burstBtn.disabled = true;
    setStatus("正在执行并发请求 x5...");

    try {
      const tasks = [];
      for (let i = 0; i < 5; i += 1) {
        tasks.push(requestOnce(Math.floor(base) + i));
      }
      await Promise.all(tasks);
      await refreshStats();
      setStatus("并发请求完成");
    } catch (err) {
      setStatus("并发请求异常");
      output.textContent = String(err);
    } finally {
      sendBtn.disabled = false;
      burstBtn.disabled = false;
    }
  }

  sendBtn.addEventListener("click", handleSend);
  burstBtn.addEventListener("click", handleBurst);

  refreshStats();
  setInterval(refreshStats, 1000);
})();
