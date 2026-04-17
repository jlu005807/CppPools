(function () {
  "use strict";

  const output = document.getElementById("output");
  const userIdInput = document.getElementById("userId");
  const sendBtn = document.getElementById("sendBtn");
  const pressureBtn = document.getElementById("pressureBtn");

  const mWorkers = document.getElementById("mWorkers");
  const mPending = document.getElementById("mPending");
  const mMemInUse = document.getElementById("mMemInUse");
  const mMemAvail = document.getElementById("mMemAvail");
  const mDb = document.getElementById("mDb");
  const mTotalReq = document.getElementById("mTotalReq");
  const mP95 = document.getElementById("mP95");

  const threadBar = document.getElementById("threadBar");
  const memoryBar = document.getElementById("memoryBar");
  const dbBar = document.getElementById("dbBar");

  const threadBarText = document.getElementById("threadBarText");
  const memoryBarText = document.getElementById("memoryBarText");
  const dbBarText = document.getElementById("dbBarText");

  const trendCanvas = document.getElementById("trendCanvas");
  const trendCtx = trendCanvas.getContext("2d");

  const trendMaxPoints = 80;
  const pendingSeries = [];
  const latencySeries = [];

  function clampPct(v) {
    if (!Number.isFinite(v)) {
      return 0;
    }
    return Math.max(0, Math.min(100, v));
  }

  function setBar(bar, textNode, pct) {
    const safe = clampPct(pct);
    bar.style.width = safe.toFixed(1) + "%";
    textNode.textContent = safe.toFixed(1) + "%";
  }

  function showJson(obj) {
    output.textContent = JSON.stringify(obj, null, 2);
  }

  function resizeTrendCanvas() {
    const w = trendCanvas.clientWidth;
    const h = trendCanvas.clientHeight;
    if (trendCanvas.width !== w || trendCanvas.height !== h) {
      trendCanvas.width = w;
      trendCanvas.height = h;
    }
  }

  function pushTrendPoint(pending, latency) {
    pendingSeries.push(Number.isFinite(pending) ? pending : 0);
    latencySeries.push(Number.isFinite(latency) ? latency : 0);
    if (pendingSeries.length > trendMaxPoints) {
      pendingSeries.shift();
    }
    if (latencySeries.length > trendMaxPoints) {
      latencySeries.shift();
    }
  }

  function drawSeries(ctx, values, maxValue, color, x0, y0, w, h) {
    if (values.length === 0) {
      return;
    }

    ctx.beginPath();
    ctx.strokeStyle = color;
    ctx.lineWidth = 2;

    const last = Math.max(values.length - 1, 1);
    for (let i = 0; i < values.length; ++i) {
      const x = x0 + (i / last) * w;
      const normalized = maxValue > 0 ? values[i] / maxValue : 0;
      const y = y0 + h - normalized * h;
      if (i === 0) {
        ctx.moveTo(x, y);
      } else {
        ctx.lineTo(x, y);
      }
    }
    ctx.stroke();
  }

  function drawTrend() {
    resizeTrendCanvas();
    const w = trendCanvas.width;
    const h = trendCanvas.height;
    trendCtx.clearRect(0, 0, w, h);

    const padding = { left: 42, right: 16, top: 16, bottom: 28 };
    const plotW = Math.max(1, w - padding.left - padding.right);
    const plotH = Math.max(1, h - padding.top - padding.bottom);

    trendCtx.strokeStyle = "rgba(255,255,255,0.12)";
    trendCtx.lineWidth = 1;
    for (let i = 0; i <= 4; ++i) {
      const y = padding.top + (i / 4) * plotH;
      trendCtx.beginPath();
      trendCtx.moveTo(padding.left, y);
      trendCtx.lineTo(padding.left + plotW, y);
      trendCtx.stroke();
    }

    const maxPending = Math.max(1, ...pendingSeries);
    const maxLatency = Math.max(1, ...latencySeries);

    drawSeries(trendCtx, pendingSeries, maxPending, "#70f4e0", padding.left, padding.top, plotW, plotH);
    drawSeries(trendCtx, latencySeries, maxLatency, "#ff9a75", padding.left, padding.top, plotW, plotH);

    trendCtx.fillStyle = "#a9bdd2";
    trendCtx.font = "12px Segoe UI";
    trendCtx.fillText("0", 10, padding.top + plotH + 4);
    trendCtx.fillText("pending max " + maxPending, padding.left + 6, padding.top + 12);
    trendCtx.fillText("latency max " + maxLatency.toFixed(1) + " ms", padding.left + 6, padding.top + 28);
  }

  async function fetchJson(url, timeoutMs) {
    const c = new AbortController();
    const timer = setTimeout(function () {
      c.abort();
    }, timeoutMs);

    try {
      const resp = await fetch(url, { signal: c.signal });
      if (!resp.ok) {
        throw new Error("HTTP " + resp.status);
      }
      return await resp.json();
    } finally {
      clearTimeout(timer);
    }
  }

  function updateStatsUi(stats) {
    const workers = Number(stats.thread_pool_workers || 0);
    const pending = Number(stats.thread_pool_pending || 0);
    const p95Latency = Number(stats.p95_latency_ms || 0);
    const memInUse = Number(stats.memory_pool_in_use || 0);
    const memTotal = Number(stats.memory_pool_total || 0);
    const memAvail = Number(stats.memory_pool_available || 0);
    const dbFree = Number(stats.db_pool_free || 0);
    const dbTotal = Number(stats.db_pool_total || 0);
    const totalReq = Number(stats.total_requests || 0);

    mWorkers.textContent = String(workers);
    mPending.textContent = String(pending);
    mMemInUse.textContent = String(memInUse);
    mMemAvail.textContent = String(memAvail);
    mDb.textContent = dbFree + " / " + dbTotal;
    mTotalReq.textContent = String(totalReq);
    mP95.textContent = p95Latency.toFixed(3);

    // pending 相对容量的近似占比：capacity ~= workers * 4。
    const threadPct = workers > 0 ? (pending / (workers * 4)) * 100 : 0;
    const memoryPct = memTotal > 0 ? (memInUse / memTotal) * 100 : 0;
    const dbBusy = dbTotal - dbFree;
    const dbPct = dbTotal > 0 ? (dbBusy / dbTotal) * 100 : 0;

    setBar(threadBar, threadBarText, threadPct);
    setBar(memoryBar, memoryBarText, memoryPct);
    setBar(dbBar, dbBarText, dbPct);

    pushTrendPoint(pending, p95Latency);
    drawTrend();
  }

  async function refreshStats() {
    try {
      const stats = await fetchJson("/api/stats", 2000);
      updateStatsUi(stats);
    } catch (err) {
      // stats 拉取失败时保留上一次绘制，避免页面闪烁。
      console.debug("refreshStats failed:", err);
    }
  }

  async function sendOne(userId) {
    const data = await fetchJson("/api/process?user=" + encodeURIComponent(userId), 3000);
    showJson(data);
    return data;
  }

  async function onSendOnce() {
    const userId = Number(userIdInput.value || 0);
    output.textContent = "Sending request...";
    try {
      await sendOne(userId);
      await refreshStats();
    } catch (err) {
      output.textContent = "Request failed: " + String(err);
    }
  }

  async function onPressureRun() {
    const base = Number(userIdInput.value || 0);
    output.textContent = "Pressure run: dispatching 20 concurrent requests...";
    try {
      const jobs = [];
      for (let i = 0; i < 20; ++i) {
        jobs.push(sendOne(base + i));
      }

      const results = await Promise.allSettled(jobs);
      let ok = 0;
      let fail = 0;
      for (const r of results) {
        if (r.status === "fulfilled") {
          ok += 1;
        } else {
          fail += 1;
        }
      }
      output.textContent = "Pressure run done. success=" + ok + ", fail=" + fail;
      await refreshStats();
    } catch (err) {
      output.textContent = "Pressure run failed: " + String(err);
    }
  }

  sendBtn.addEventListener("click", onSendOnce);
  pressureBtn.addEventListener("click", onPressureRun);
  window.addEventListener("resize", drawTrend);

  refreshStats();
  setInterval(refreshStats, 1500);
})();
