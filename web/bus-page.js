import { BrokerClient, readAutoReconnect, setStatus, writeAutoReconnect } from "./common.js";
import { ecuPastelColor, ecuStrokeColor, formatMetricValue, metricById, normalizeMetricValue } from "./metrics.js";

const els = {
  stDot: document.getElementById("busStatusDot"),
  stText: document.getElementById("busStatusText"),
  ecuList: document.getElementById("ecuList"),
  frameLog: document.getElementById("frameLog"),
  frameCount: document.getElementById("frameCount"),
  lastCanId: document.getElementById("lastCanId"),
  monitorEcu: document.getElementById("monitorEcu"),
  autoReconnect: document.getElementById("autoReconnect"),
  clearLog: document.getElementById("clearLog"),
  pauseLog: document.getElementById("pauseLog"),
  graphWindow: document.getElementById("graphWindow"),
  graphScale: document.getElementById("graphScale"),
  graphNow: document.getElementById("graphNow"),
  graphMax: document.getElementById("graphMax"),
  graphYMax: document.getElementById("graphYMax"),
  graphYMin: document.getElementById("graphYMin"),
  graphLegend: document.getElementById("graphLegend"),
  graphSeriesSelector: document.getElementById("graphSeriesSelector"),
  trafficGraphSeries: document.getElementById("trafficGraphSeries"),
  bitsWinner: document.getElementById("bitsWinner"),
  bitsId: document.getElementById("bitsId"),
  bitsDlc: document.getElementById("bitsDlc"),
  bitsCrc: document.getElementById("bitsCrc"),
  bitsAck: document.getElementById("bitsAck"),
  bitsLen: document.getElementById("bitsLen"),
  bitsStuff: document.getElementById("bitsStuff"),
  bitsErrors: document.getElementById("bitsErrors"),
  bitsLine: document.getElementById("bitsLine"),
  arbList: document.getElementById("arbList"),
  ctlBody: document.getElementById("ctlBody"),
};

const ctlState = new Map();

let network = [];
let paused = false;
let frameCount = 0;
let bitLevelMonitorActive = false;
let runtimeRefreshBusy = false;
let graphEvents = [];
let graphRenderPending = false;
let client = null;
let reconnectTimer = null;
let reconnectAttempt = 0;
let connectGeneration = 0;
let autoReconnect = readAutoReconnect();
const selectedGraphEcus = new Set();
let graphSelectionInitialized = false;

function canIdText(value) {
  if (typeof value === "string") return value;
  if (Number.isFinite(value)) return `0x${Number(value).toString(16).toUpperCase()}`;
  return "—";
}

function valueText(payload) {
  const value = payload?.value ?? payload?.text ?? "";
  if (typeof value === "number") return value.toFixed(2).replace(/\\.00$/, "");
  return String(value || "—");
}

function renderTopology() {
  els.ecuList.innerHTML = "";

  network.forEach((ecu) => {
    if (!graphSelectionInitialized) selectedGraphEcus.add(ecu.id);
    const card = document.createElement("div");
    card.className = "bus-ecu-card";
    card.dataset.ecuId = ecu.id;
    card.style.borderLeft = `6px solid ${ecuPastelColor(ecu.id)}`;
    const filters = (ecu.rx?.filters || [])
      .map((f) => `${canIdText(f.can_id)}${f.label ? ` ${f.label}` : ""}`)
      .join("<br>") || "—";
    const metric = metricById(ecu.tx?.metric);
    const href = `http://${location.hostname || "127.0.0.1"}:${ecu.port}/${ecu.page || "ecu.html"}`;
    card.innerHTML = `
      <div class="bus-ecu-card-head">
        <b><span class="ecu-color-dot" style="background:${ecuPastelColor(ecu.id)}"></span>${ecu.id}</b>
        <a href="${href}" target="_blank">open</a>
      </div>
      <div class="mono">port :${ecu.port}</div>
      <div>TX: <span class="mono">${canIdText(ecu.tx?.can_id)}</span></div>
      <div>Metric: <span class="mono">${metric.label}${metric.unit ? ` [${metric.unit}]` : ""}</span></div>
      <div>RX:<br><span class="mono">${filters}</span></div>
    `;
    els.ecuList.appendChild(card);
  });
  graphSelectionInitialized = true;
  renderGraphSeriesSelector();
}

function renderGraphSeriesSelector() {
  els.graphSeriesSelector.innerHTML = "";
  for (const ecu of network) {
    const metric = metricById(ecu.tx?.metric);
    const label = document.createElement("label");
    label.className = "graph-series-check";
    label.style.borderColor = ecuPastelColor(ecu.id);

    const input = document.createElement("input");
    input.type = "checkbox";
    input.checked = selectedGraphEcus.has(ecu.id);
    input.addEventListener("change", () => {
      if (input.checked) selectedGraphEcus.add(ecu.id);
      else selectedGraphEcus.delete(ecu.id);
      renderTrafficGraph();
    });

    const swatch = document.createElement("span");
    swatch.className = "graph-series-swatch";
    swatch.style.background = ecuPastelColor(ecu.id);

    const text = document.createElement("span");
    text.textContent = `${ecu.id} · ${metric.label}${metric.unit ? ` (${metric.unit})` : ""}`;

    label.append(input, swatch, text);
    els.graphSeriesSelector.appendChild(label);
  }
}

function sameJson(a, b) {
  return JSON.stringify(a ?? null) === JSON.stringify(b ?? null);
}

function mergeRuntimeConfig(ecu, cfg) {
  let changed = false;
  if (cfg?.tx) {
    const nextTx = { ...cfg.tx };
    if (!nextTx.metric && ecu.tx?.metric) nextTx.metric = ecu.tx.metric;
    if (!sameJson(ecu.tx, nextTx)) {
      ecu.tx = nextTx;
      changed = true;
    }
  }
  if (cfg?.rx && !sameJson(ecu.rx, cfg.rx)) {
    ecu.rx = cfg.rx;
    changed = true;
  }
  if (cfg?.ecu_id && ecu.id !== cfg.ecu_id) {
    ecu.id = cfg.ecu_id;
    changed = true;
  }
  if (cfg?.page && ecu.page !== cfg.page) {
    ecu.page = cfg.page;
    changed = true;
  }
  return changed;
}

function clearTrafficMarksLater(elements) {
  setTimeout(() => {
    elements.forEach((el) => {
      el.classList.remove("tx-active");
      el.classList.remove("rx-active");
    });
  }, 450);
}

function markTraffic(canId) {
  const id = canIdText(canId);
  const touched = [];
  for (const ecu of network) {
    const card = document.querySelector(`.bus-ecu-card[data-ecu-id="${ecu.id}"]`);
    const txMatch = canIdText(ecu.tx?.can_id) === id;
    const rxMatch = (ecu.rx?.filters || []).some((f) => canIdText(f.can_id) === id);
    [card].forEach((el) => {
      if (!el) return;
      el.classList.toggle("tx-active", txMatch);
      el.classList.toggle("rx-active", rxMatch && !txMatch);
      if (txMatch || rxMatch) touched.push(el);
    });
  }
  clearTrafficMarksLater(touched);
}

function markDelivered(delivered) {
  const winner = delivered?.winner || "";
  const appReceivers = new Set(delivered?.app_receivers || []);
  const touched = [];

  for (const ecu of network) {
    const card = document.querySelector(`.bus-ecu-card[data-ecu-id="${ecu.id}"]`);
    const isTx = ecu.id === winner;
    const isAppRx = appReceivers.has(ecu.id);

    [card].forEach((el) => {
      if (!el) return;
      el.classList.toggle("tx-active", isTx);
      el.classList.toggle("rx-active", isAppRx && !isTx);
      if (isTx || isAppRx) touched.push(el);
    });
  }

  clearTrafficMarksLater(touched);
}

function recordGraphFrame(delivered) {
  const frame = delivered?.frame || {};
  const ecu = network.find((item) => item.id === delivered?.winner);
  const value = decodeFloat32(frame.data || []);
  graphEvents.push({
    t: performance.now(),
    ecuId: delivered?.winner || "",
    id: canIdText(frame.id),
    value,
    metric: ecu?.tx?.metric || "percent",
  });
  trimGraphEvents();
  scheduleTrafficGraphRender();
}

function scheduleTrafficGraphRender() {
  if (graphRenderPending) return;
  graphRenderPending = true;
  requestAnimationFrame(() => {
    graphRenderPending = false;
    renderTrafficGraph();
  });
}

function decodeFloat32(bytes) {
  if (!bytes || bytes.length < 4) return NaN;
  const view = new DataView(new ArrayBuffer(4));
  for (let i = 0; i < 4; i += 1) view.setUint8(i, Number(bytes[i]) & 0xff);
  return view.getFloat32(0, true);
}

function trimGraphEvents() {
  const windowMs = Number(els.graphWindow?.value || 10) * 1000;
  const cutoff = performance.now() - windowMs * 1.2;
  graphEvents = graphEvents.filter((ev) => ev.t >= cutoff);
}

function renderTrafficGraph() {
  if (!els.trafficGraphSeries) return;
  trimGraphEvents();

  const windowSec = Number(els.graphWindow.value || 10);
  const windowMs = windowSec * 1000;
  const now = performance.now();
  const selected = network.filter((ecu) => selectedGraphEcus.has(ecu.id));
  const selectedIds = new Set(selected.map((ecu) => ecu.id));
  const selectedMetricIds = new Set(selected.map((ecu) => metricById(ecu.tx?.metric).id));
  const useMetricScale = els.graphScale.value === "auto" && selectedMetricIds.size === 1;
  const visibleEvents = graphEvents.filter((ev) => (
    selectedIds.has(ev.ecuId) && Number.isFinite(ev.value) && now - ev.t <= windowMs
  ));
  const scale = makeGraphScale(selected, visibleEvents, useMetricScale);

  els.trafficGraphSeries.innerHTML = "";
  const legend = [];

  for (const ecu of selected) {
    const metric = metricById(ecu.tx?.metric);
    const samples = visibleEvents.filter((ev) => ev.ecuId === ecu.id).sort((a, b) => a.t - b.t);
    const points = samples.map((ev) => {
      const age = now - ev.t;
      const x = Math.max(0, Math.min(100, 100 - (age / windowMs) * 100));
      const plotted = useMetricScale ? scaleValue(ev.value, scale) : normalizeMetricValue(ev.value, metric.id);
      const pct = plotted;
      const y = 100 - Math.max(0, Math.min(100, pct));
      return [x, y, ev.value];
    });

    const line = points
      .map(([x, y], idx) => `${idx === 0 ? "M" : "L"}${x.toFixed(2)},${y.toFixed(2)}`)
      .join(" ");
    if (!line) continue;

    const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
    path.setAttribute("d", line);
    path.setAttribute("class", "graph-line-series");
    path.style.stroke = ecuStrokeColor(ecu.id);
    els.trafficGraphSeries.appendChild(path);

    const lastPoint = points[points.length - 1];
    const lastValue = lastPoint[2];
    legend.push(`<span><i style="background:${ecuPastelColor(ecu.id)}"></i>${ecu.id}: ${formatMetricValue(lastValue, metric.id)}</span>`);
  }

  els.graphNow.textContent = String(selected.length);
  els.graphMax.textContent = useMetricScale ? `${scale.minText}..${scale.maxText}` : "0..100%";
  els.graphYMax.textContent = useMetricScale ? scale.maxText : "100%";
  els.graphYMin.textContent = useMetricScale ? scale.minText : "0";
  els.graphLegend.innerHTML = legend.length
    ? legend.join("")
    : "Выбери ECU галочками справа/сверху, чтобы увидеть данные.";
}

function makeGraphScale(selected, visibleEvents, useMetricScale) {
  if (!useMetricScale) return { min: 0, max: 100, minText: "0", maxText: "100%" };

  const metric = metricById(selected[0]?.tx?.metric);
  const values = visibleEvents.map((ev) => ev.value).filter(Number.isFinite);
  let min = values.length ? Math.min(...values) : metric.min;
  let max = values.length ? Math.max(...values) : metric.max;
  if (min === max) {
    const fallbackSpan = Math.max(1, Math.abs(min) * 0.2, (metric.max - metric.min) * 0.1);
    min -= fallbackSpan / 2;
    max += fallbackSpan / 2;
  }
  const pad = (max - min) * 0.1;
  min -= pad;
  max += pad;
  return {
    min,
    max,
    minText: formatAxisValue(min, metric.id),
    maxText: formatAxisValue(max, metric.id),
  };
}

function scaleValue(value, scale) {
  return ((value - scale.min) / Math.max(1e-9, scale.max - scale.min)) * 100;
}

function formatAxisValue(value, metricId) {
  const metric = metricById(metricId);
  const rounded = Math.abs(value) >= 100 ? value.toFixed(0) : value.toFixed(1);
  return metric.unit ? `${rounded} ${metric.unit}` : rounded;
}

function addLogFrame(msg, { mark = true } = {}) {
  if (paused) return;

  frameCount += 1;
  const canId = canIdText(msg.from ?? msg.can_id);
  els.frameCount.textContent = String(frameCount);
  els.lastCanId.textContent = canId;

  const tr = document.createElement("tr");
  tr.innerHTML = `
    <td>${new Date().toLocaleTimeString()}</td>
    <td class="mono">${canId}</td>
    <td class="mono">${valueText(msg.payload)}</td>
    <td>${msg.filter_label || msg.payload?.label || ""}</td>
  `;
  els.frameLog.prepend(tr);
  while (els.frameLog.children.length > 120) {
    els.frameLog.lastChild.remove();
  }
  if (mark) markTraffic(canId);
}

function renderBits(delivered) {
  if (!delivered) return;
  const f = delivered.frame || {};
  const stuffed = delivered.stuffed || "";
  const stuffSet = new Set(delivered.stuff_positions || []);

  els.bitsWinner.textContent = delivered.winner || "—";
  els.bitsId.textContent = canIdText(f.id);
  els.bitsDlc.textContent = String(f.dlc ?? 0);
  els.bitsCrc.textContent = `0x${Number(delivered.crc || 0).toString(16).toUpperCase().padStart(4, "0")}`;
  els.bitsAck.textContent = delivered.ack ? "ok" : "—";
  els.bitsLen.textContent = stuffed.length + (delivered.trailer || "").length;
  els.bitsStuff.textContent = stuffSet.size;
  els.bitsErrors.textContent = (delivered.errors || []).map((e) => e.kind).join(", ") || "—";

  els.bitsLine.innerHTML = "";

  const tags = mapUnstuffedTags(delivered);
  let unIdx = 0;
  for (let i = 0; i < stuffed.length; i++) {
    const bit = stuffed[i];
    const span = document.createElement("span");
    span.className = "bit";
    span.textContent = bit;
    if (stuffSet.has(i)) {
      span.classList.add("stuff");
    } else {
      const tag = tags[unIdx++] || "ctl";
      span.classList.add("bit-" + tag);
    }
    els.bitsLine.appendChild(span);
  }
  // Trailer (CRC delim, ACK, EOF, IFS)
  for (const ch of (delivered.trailer || "")) {
    const span = document.createElement("span");
    span.className = "bit bit-trailer";
    span.textContent = ch;
    els.bitsLine.appendChild(span);
  }

  // arbitration log
  els.arbList.innerHTML = "";
  for (const step of (delivered.arbitration || [])) {
    const li = document.createElement("li");
    li.textContent = `bit ${step.bit_index}: bus=${step.bus_bit} → drop: ${step.dropped.join(", ")}`;
    els.arbList.appendChild(li);
  }
  if (!els.arbList.children.length) {
    const li = document.createElement("li");
    li.className = "muted";
    li.textContent = delivered.losers?.length
      ? `Проиграли (без подробностей): ${delivered.losers.join(", ")}`
      : "Без конкурентов";
    els.arbList.appendChild(li);
  }
}

function mapUnstuffedTags(delivered) {
  // Восстановим теги для unstuffed-битов из подсказок арбитража и DLC.
  // Здесь нет полной TLV-разметки с бэка, но мы знаем разделение SOF/ARB/CTL/DATA/CRC по позициям.
  const out = [];
  const arbBegin = delivered.arbitration_begin || 1;
  const arbEnd = delivered.arbitration_end || arbBegin;
  const f = delivered.frame || {};
  // control = arbEnd .. arbEnd + control_size; control_size = 2 + 4 (IDE/r0/DLC) for SFF; 3 + 4 for EFF
  const isExt = !!f.ide;
  const controlSize = isExt ? 7 : 6; // r1+r0+DLC(4) for EFF, IDE+r0+DLC(4) for SFF
  const dataBits = (f.dlc || 0) * 8;
  const lengthAll = (delivered.unstuffed || "").length;
  for (let i = 0; i < lengthAll; i++) {
    if (i === 0) out.push("sof");
    else if (i >= arbBegin && i < arbEnd) out.push("arb");
    else if (i >= arbEnd && i < arbEnd + controlSize) out.push("ctl");
    else if (i >= arbEnd + controlSize && i < arbEnd + controlSize + dataBits) out.push("data");
    else out.push("crc");
  }
  return out;
}

function upsertCtl(snap) {
  if (!snap?.node_id) return;
  ctlState.set(snap.node_id, snap);
  renderCtlTable();
}

function renderCtlTable() {
  els.ctlBody.innerHTML = "";
  const rows = Array.from(ctlState.values()).sort((a, b) => a.node_id.localeCompare(b.node_id));
  for (const s of rows) {
    const st = s.stats || {};
    const tr = document.createElement("tr");
    tr.dataset.mode = (s.mode || "").toLowerCase();
    tr.innerHTML = `
      <td><b>${s.node_id}</b></td>
      <td><span class="ctl-mode" data-mode="${(s.mode || "").toLowerCase()}">${s.mode || "—"}</span></td>
      <td class="mono">${s.tec ?? 0}</td>
      <td class="mono">${s.rec ?? 0}</td>
      <td class="mono">${st.tx_ok ?? 0} / ${st.tx_errors ?? 0}</td>
      <td class="mono">${st.rx_ok ?? 0} / ${st.rx_errors ?? 0}</td>
      <td class="mono">${st.app_rx_ok ?? 0}</td>
      <td class="mono">${st.arbitration_lost ?? 0}</td>
      <td class="mono">${st.bus_off_events ?? 0}</td>
    `;
    els.ctlBody.appendChild(tr);
  }
}

async function loadInitialControllerStates() {
  let changed = false;
  for (const ecu of network) {
    const host = location.hostname || "127.0.0.1";
    try {
      const cfg = await fetch(`http://${host}:${ecu.port}/ecu-config.json?ts=${Date.now()}`, {
        cache: "no-store",
      }).then((r) => r.json());
      changed = mergeRuntimeConfig(ecu, cfg) || changed;
      upsertCtl(cfg.controller);
    } catch {
      // ECU может перезапускаться; live controller_state догонит таблицу позже.
    }
  }
  if (changed) renderTopology();
}

async function refreshRuntimeNetwork() {
  if (runtimeRefreshBusy || !network.length) return;
  runtimeRefreshBusy = true;
  try {
    await loadInitialControllerStates();
  } finally {
    runtimeRefreshBusy = false;
  }
}

function scheduleReconnect(delayMs) {
  if (!autoReconnect || reconnectTimer) return;
  const delay = delayMs ?? Math.min(10000, 700 * 2 ** Math.min(reconnectAttempt, 4));
  reconnectAttempt += 1;
  setStatus(els.stDot, els.stText, false, `Переподключение через ${(delay / 1000).toFixed(1)} c`);
  reconnectTimer = setTimeout(() => {
    reconnectTimer = null;
    connectMonitor().catch(handleConnectFailure);
  }, delay);
}

function handleConnectFailure(error) {
  setStatus(els.stDot, els.stText, false, error?.message || "Нет связи");
  scheduleReconnect();
}

async function connectMonitor() {
  clearTimeout(reconnectTimer);
  reconnectTimer = null;
  const generation = ++connectGeneration;
  client = new BrokerClient();
  client.on("bus_frame", (msg) => addLogFrame(msg, { mark: !bitLevelMonitorActive }));
  client.on("message", addLogFrame);
  client.on("bus_delivered", (msg) => {
    bitLevelMonitorActive = true;
    renderBits(msg.data);
    markDelivered(msg.data);
    recordGraphFrame(msg.data);
  });
  client.on("controller_state", (msg) => upsertCtl(msg.data));
  client.on("error", (msg) => setStatus(els.stDot, els.stText, false, msg.message));
  client.on("close", ({ byApp } = {}) => {
    if (generation !== connectGeneration || byApp) return;
    setStatus(els.stDot, els.stText, false, autoReconnect ? "Отключено, переподключение…" : "Отключено");
    scheduleReconnect();
  });

  const registered = await client.connect();
  if (generation !== connectGeneration) return;
  reconnectAttempt = 0;
  setStatus(els.stDot, els.stText, true, "Мониторинг");
  els.monitorEcu.textContent = registered.ecu_id;
  upsertCtl(registered.controller);

  network = await fetch(`/network.json?ts=${Date.now()}`, { cache: "no-store" })
    .then((r) => r.json())
    .catch(() => registered.network || []);
  renderTopology();
  loadInitialControllerStates();
}

async function boot() {
  if (els.autoReconnect) {
    els.autoReconnect.checked = autoReconnect;
    els.autoReconnect.addEventListener("change", () => {
      autoReconnect = els.autoReconnect.checked;
      writeAutoReconnect(autoReconnect);
      if (!autoReconnect) {
        clearTimeout(reconnectTimer);
        reconnectTimer = null;
        setStatus(els.stDot, els.stText, client?.isOpen(), client?.isOpen() ? "Мониторинг" : "Отключено");
      } else if (!client?.isOpen()) {
        scheduleReconnect(0);
      }
    });
  }

  setInterval(refreshRuntimeNetwork, 1000);
  [els.graphWindow, els.graphScale].forEach((el) => {
    el.addEventListener("change", renderTrafficGraph);
  });
  renderTrafficGraph();
  await connectMonitor();
}

els.clearLog.onclick = () => {
  els.frameLog.innerHTML = "";
  frameCount = 0;
  els.frameCount.textContent = "0";
  els.lastCanId.textContent = "—";
};

els.pauseLog.onclick = () => {
  paused = !paused;
  els.pauseLog.textContent = paused ? "Продолжить" : "Пауза";
};

boot().catch(handleConnectFailure);
