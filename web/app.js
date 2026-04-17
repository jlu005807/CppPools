(function () {
  "use strict";

  // Signal Weave:
  // The background field is intentionally deterministic. For the same seed and
  // same parameters, users will observe reproducible particle behavior.
  // This makes visual debugging and demo comparison easier.

  // ---------------------------------------------------------------------------
  // DOM references
  // ---------------------------------------------------------------------------
  const canvas = document.getElementById("bgCanvas");
  const ctx = canvas.getContext("2d");

  const output = document.getElementById("output");
  const historyList = document.getElementById("historyList");

  const userInput = document.getElementById("userId");
  const queryBtn = document.getElementById("queryBtn");
  const burstBtn = document.getElementById("burstBtn");

  const densityRange = document.getElementById("densityRange");
  const trailRange = document.getElementById("trailRange");
  const swirlRange = document.getElementById("swirlRange");
  const seedValue = document.getElementById("seedValue");
  const seedPrevBtn = document.getElementById("seedPrevBtn");
  const seedNextBtn = document.getElementById("seedNextBtn");
  const seedRandomBtn = document.getElementById("seedRandomBtn");

  const densityValue = document.getElementById("densityValue");
  const trailValue = document.getElementById("trailValue");
  const swirlValue = document.getElementById("swirlValue");

  const metricRequestId = document.getElementById("metricRequestId");
  const metricLatency = document.getElementById("metricLatency");
  const metricDb = document.getElementById("metricDb");
  const metricMem = document.getElementById("metricMem");
  const metricTotal = document.getElementById("metricTotal");
  const metricPending = document.getElementById("metricPending");

  // ---------------------------------------------------------------------------
  // Visualization state
  // ---------------------------------------------------------------------------
  const state = {
    // baseSeed is what user sees and controls.
    baseSeed: 20260417,
    // rngState is the mutable internal state used by LCG each frame.
    rngState: 20260417,
    particleCount: Number(densityRange.value),
    trailAlpha: Number(trailRange.value),
    swirlStrength: Number(swirlRange.value)
  };

  /** @type {Array<{x:number,y:number,vx:number,vy:number,life:number}>} */
  let particles = [];

  /** @type {Array<{x:number,y:number,r:number,alpha:number}>} */
  let pulses = [];

  /** @type {Array<{requestId:number,userId:number,latency:number,dbFree:number,score:number}>} */
  let history = [];

  /** @type {{db_pool_total:number|null}} */
  const latestStats = {
    db_pool_total: null
  };

  // ---------------------------------------------------------------------------
  // Utility functions
  // ---------------------------------------------------------------------------
  function clampSeed(seed) {
    if (!Number.isFinite(seed)) {
      return 1;
    }
    const normalized = Math.floor(Math.abs(seed)) % 4294967295;
    return Math.max(1, normalized);
  }

  function updateSeedLabel() {
    seedValue.textContent = String(state.baseSeed);
  }

  function reseed(seed) {
    state.baseSeed = clampSeed(seed);
    state.rngState = state.baseSeed;
    updateSeedLabel();
    resetParticles();
  }

  function rand() {
    state.rngState = (state.rngState * 1664525 + 1013904223) % 4294967296;
    return state.rngState / 4294967296;
  }

  function resizeCanvas() {
    canvas.width = window.innerWidth;
    canvas.height = window.innerHeight;
  }

  function makeParticle() {
    return {
      x: rand() * canvas.width,
      y: rand() * canvas.height,
      vx: 0,
      vy: 0,
      life: 140 + rand() * 260
    };
  }

  function resetParticles() {
    // Rebuild particle field from current seed and density.
    state.rngState = state.baseSeed;
    particles = new Array(state.particleCount);
    for (let i = 0; i < state.particleCount; i += 1) {
      particles[i] = makeParticle();
    }
  }

  function addPulse() {
    pulses.push({
      x: canvas.width * (0.45 + rand() * 0.1),
      y: canvas.height * (0.45 + rand() * 0.1),
      r: 8,
      alpha: 0.8
    });
  }

  function flow(x, y, t) {
    // Coordinate normalization keeps behavior stable across resolutions.
    const nx = x / canvas.width - 0.5;
    const ny = y / canvas.height - 0.5;

    const scale = state.swirlStrength;
    const a = Math.sin(nx * (6 + scale * 20) + t * 0.0012) + Math.cos(ny * (9 + scale * 15) - t * 0.0011);
    const b = Math.cos(nx * (10 + scale * 14) - t * 0.0008) - Math.sin(ny * (7 + scale * 19) + t * 0.0013);

    return {
      ax: a * (0.05 + scale * 0.45),
      ay: b * (0.05 + scale * 0.45)
    };
  }

  function drawBackground(t) {
    ctx.fillStyle = "rgba(7, 15, 26," + state.trailAlpha.toFixed(3) + ")";
    ctx.fillRect(0, 0, canvas.width, canvas.height);

    for (let i = 0; i < particles.length; i += 1) {
      const p = particles[i];
      const f = flow(p.x, p.y, t);

      p.vx = p.vx * 0.91 + f.ax;
      p.vy = p.vy * 0.91 + f.ay;

      const nx = p.x + p.vx;
      const ny = p.y + p.vy;

      ctx.beginPath();
      ctx.moveTo(p.x, p.y);
      ctx.lineTo(nx, ny);

      const speed = Math.hypot(p.vx, p.vy);
      const alpha = Math.min(0.45, 0.03 + speed * 0.05);
      const hueMix = Math.min(255, Math.floor(150 + speed * 140));
      ctx.strokeStyle = "rgba(98," + hueMix + ",217," + alpha.toFixed(3) + ")";
      ctx.stroke();

      p.x = nx;
      p.y = ny;
      p.life -= 1;

      if (p.life <= 0 || p.x < -30 || p.y < -30 || p.x > canvas.width + 30 || p.y > canvas.height + 30) {
        particles[i] = makeParticle();
      }
    }

    for (let i = pulses.length - 1; i >= 0; i -= 1) {
      const pulse = pulses[i];
      pulse.r += 1.8;
      pulse.alpha *= 0.94;

      ctx.beginPath();
      ctx.arc(pulse.x, pulse.y, pulse.r, 0, Math.PI * 2);
      ctx.strokeStyle = "rgba(255,184,77," + pulse.alpha.toFixed(3) + ")";
      ctx.lineWidth = 1.6;
      ctx.stroke();

      if (pulse.alpha < 0.05) {
        pulses.splice(i, 1);
      }
    }

    requestAnimationFrame(drawBackground);
  }

  function updateMetricsFromProcess(data, latencyMs) {
    metricRequestId.textContent = String(data.request_id ?? "-");
    metricLatency.textContent = latencyMs.toFixed(1);
    const dbTotal = latestStats.db_pool_total == null ? "?" : String(latestStats.db_pool_total);
    metricDb.textContent = String(data.db_free ?? "-") + " / " + dbTotal;
    metricMem.textContent = String(data.memory_pool_in_use ?? "-");
  }

  function updateMetricsFromStats(stats) {
    latestStats.db_pool_total = Number.isFinite(stats.db_pool_total) ? stats.db_pool_total : null;
    metricTotal.textContent = String(stats.total_requests ?? "-");
    metricPending.textContent = String(stats.thread_pool_pending ?? "-");
    metricDb.textContent = String(stats.db_pool_free ?? "-") + " / " + String(stats.db_pool_total ?? "-");
    metricMem.textContent = String(stats.memory_pool_in_use ?? "-");

    // Keep seed UI in sync if future versions support seed persistence from server.
    updateSeedLabel();
  }

  function renderHistory() {
    historyList.innerHTML = "";
    for (const item of history) {
      const li = document.createElement("li");
      li.textContent =
        "#" + item.requestId +
        " user=" + item.userId +
        " score=" + item.score +
        " db_free=" + item.dbFree +
        " latency=" + item.latency.toFixed(1) + "ms";
      historyList.appendChild(li);
    }
  }

  function prettyPrint(obj) {
    return JSON.stringify(obj, null, 2);
  }

  async function fetchWithTimeout(url, timeoutMs) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs);
    try {
      return await fetch(url, { signal: controller.signal });
    } finally {
      clearTimeout(timer);
    }
  }

  // ---------------------------------------------------------------------------
  // Network functions
  // ---------------------------------------------------------------------------
  async function refreshStats() {
    try {
      const resp = await fetchWithTimeout("/api/stats", 2000);
      if (!resp.ok) {
        return;
      }
      const stats = await resp.json();
      updateMetricsFromStats(stats);
    } catch (_err) {
      // Keep UI resilient if stats endpoint is temporarily unavailable.
    }
  }

  async function requestOne(userId, silentOutput) {
    const t0 = performance.now();
    const resp = await fetchWithTimeout("/api/process?user=" + encodeURIComponent(userId), 3000);
    const text = await resp.text();
    const latency = performance.now() - t0;

    if (!resp.ok) {
      if (!silentOutput) {
        output.textContent = "HTTP " + resp.status + "\n" + text;
      }
      return;
    }

    let data;
    try {
      data = JSON.parse(text);
    } catch (_err) {
      if (!silentOutput) {
        output.textContent = text;
      }
      return;
    }

    updateMetricsFromProcess(data, latency);
    addPulse();

    history.unshift({
      requestId: data.request_id ?? -1,
      userId,
      latency,
      dbFree: data.db_free ?? -1,
      score: data.payload?.score ?? -1
    });

    if (history.length > 12) {
      history.length = 12;
    }

    renderHistory();
    if (!silentOutput) {
      output.textContent = prettyPrint(data);
    }
  }

  async function onSendRequest() {
    const userId = Number(userInput.value || 0);
    output.textContent = "Requesting...";

    try {
      await requestOne(userId, false);
      await refreshStats();
    } catch (err) {
      output.textContent = "Request failed: " + String(err);
    }
  }

  async function onBurst() {
    const base = Number(userInput.value || 0);
    output.textContent = "Burst mode: dispatching 5 concurrent requests...";

    try {
      const tasks = [];
      for (let i = 0; i < 5; i += 1) {
        tasks.push(requestOne(base + i, true));
      }
      await Promise.allSettled(tasks);
      await refreshStats();
      output.textContent = "Burst complete. Check recent requests and metrics.";
    } catch (err) {
      output.textContent = "Burst failed: " + String(err);
    }
  }

  function onSeedPrev() {
    reseed(state.baseSeed - 1);
    addPulse();
  }

  function onSeedNext() {
    reseed(state.baseSeed + 1);
    addPulse();
  }

  function onSeedRandom() {
    const randomSeed = Math.floor(Math.random() * 2147483647) + 1;
    reseed(randomSeed);
    addPulse();
  }

  // ---------------------------------------------------------------------------
  // Event bindings
  // ---------------------------------------------------------------------------
  queryBtn.addEventListener("click", onSendRequest);
  burstBtn.addEventListener("click", onBurst);
  seedPrevBtn.addEventListener("click", onSeedPrev);
  seedNextBtn.addEventListener("click", onSeedNext);
  seedRandomBtn.addEventListener("click", onSeedRandom);

  densityRange.addEventListener("input", () => {
    state.particleCount = Number(densityRange.value);
    densityValue.textContent = String(state.particleCount);
    resetParticles();
  });

  trailRange.addEventListener("input", () => {
    state.trailAlpha = Number(trailRange.value);
    trailValue.textContent = state.trailAlpha.toFixed(2);
  });

  swirlRange.addEventListener("input", () => {
    state.swirlStrength = Number(swirlRange.value);
    swirlValue.textContent = state.swirlStrength.toFixed(2);
  });

  window.addEventListener("resize", () => {
    resizeCanvas();
    resetParticles();
  });

  // ---------------------------------------------------------------------------
  // Bootstrap
  // ---------------------------------------------------------------------------
  updateSeedLabel();
  densityValue.textContent = String(state.particleCount);
  trailValue.textContent = state.trailAlpha.toFixed(2);
  swirlValue.textContent = state.swirlStrength.toFixed(2);

  resizeCanvas();
  resetParticles();
  requestAnimationFrame(drawBackground);

  refreshStats();
  setInterval(refreshStats, 2000);
})();
