import { BrokerClient, setStatus } from "./common.js";

const els = {
  stDot: document.getElementById("busStatusDot"),
  stText: document.getElementById("busStatusText"),
  ecuList: document.getElementById("ecuList"),
  frameLog: document.getElementById("frameLog"),
  frameCount: document.getElementById("frameCount"),
  lastCanId: document.getElementById("lastCanId"),
  monitorEcu: document.getElementById("monitorEcu"),
  clearLog: document.getElementById("clearLog"),
  pauseLog: document.getElementById("pauseLog"),
  graphMetric: document.getElementById("graphMetric"),
  graphWindow: document.getElementById("graphWindow"),
  graphScale: document.getElementById("graphScale"),
  graphNow: document.getElementById("graphNow"),
  graphMax: document.getElementById("graphMax"),
  graphYMax: document.getElementById("graphYMax"),
  graphLegend: document.getElementById("graphLegend"),
  trafficGraphLine: document.getElementById("trafficGraphLine"),
  trafficGraphFill: document.getElementById("trafficGraphFill"),
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
let graphTimer = null;

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

  network.forEach((ecu, idx) => {
    const card = document.createElement("div");
    card.className = "bus-ecu-card";
    card.dataset.ecuId = ecu.id;
    const filters = (ecu.rx?.filters || [])
      .map((f) => `${canIdText(f.can_id)}${f.label ? ` ${f.label}` : ""}`)
      .join("<br>") || "—";
    const href = `http://${location.hostname || "127.0.0.1"}:${ecu.port}/${ecu.page || "ecu.html"}`;
    card.innerHTML = `
      <div class="bus-ecu-card-head">
        <b>${ecu.id}</b>
        <a href="${href}" target="_blank">open</a>
      </div>
      <div class="mono">port :${ecu.port}</div>
      <div>TX: <span class="mono">${canIdText(ecu.tx?.can_id)}</span></div>
      <div>RX:<br><span class="mono">${filters}</span></div>
    `;
    els.ecuList.appendChild(card);
  });
}

function sameJson(a, b) {
  return JSON.stringify(a ?? null) === JSON.stringify(b ?? null);
}

function mergeRuntimeConfig(ecu, cfg) {
  let changed = false;
  if (cfg?.tx && !sameJson(ecu.tx, cfg.tx)) {
    ecu.tx = cfg.tx;
    changed = true;
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
  const bitLength = (delivered?.stuffed || "").length + (delivered?.trailer || "").length;
  graphEvents.push({
    t: performance.now(),
    id: canIdText(frame.id),
    bits: bitLength || 0,
    errors: (delivered?.errors || []).length,
  });
  trimGraphEvents();
}

function trimGraphEvents() {
  const windowMs = Number(els.graphWindow?.value || 10) * 1000;
  const cutoff = performance.now() - windowMs * 1.2;
  graphEvents = graphEvents.filter((ev) => ev.t >= cutoff);
}

function graphValueForEvent(ev, metric) {
  if (metric === "bits") return ev.bits;
  if (metric === "errors") return ev.errors;
  return 1;
}

function renderTrafficGraph() {
  if (!els.trafficGraphLine) return;
  trimGraphEvents();

  const metric = els.graphMetric.value;
  const windowSec = Number(els.graphWindow.value || 10);
  const windowMs = windowSec * 1000;
  const bucketCount = 60;
  const bucketMs = windowMs / bucketCount;
  const now = performance.now();
  const buckets = Array(bucketCount).fill(0);

  for (const ev of graphEvents) {
    const age = now - ev.t;
    if (age < 0 || age > windowMs) continue;
    const idx = bucketCount - 1 - Math.floor(age / bucketMs);
    if (idx >= 0 && idx < bucketCount) buckets[idx] += graphValueForEvent(ev, metric);
  }

  const perSecond = buckets.map((value) => value / (bucketMs / 1000));
  const maxValue = Math.max(1, ...perSecond);
  const fixedMax = metric === "bits" ? 10000 : metric === "errors" ? 10 : 50;
  const yMax = els.graphScale.value === "fixed" ? fixedMax : maxValue;
  const points = perSecond.map((value, idx) => {
    const x = (idx / (bucketCount - 1)) * 100;
    const y = 100 - Math.min(100, (value / yMax) * 100);
    return [x, y, value];
  });

  const line = points.map(([x, y], idx) => `${idx === 0 ? "M" : "L"}${x.toFixed(2)},${y.toFixed(2)}`).join(" ");
  const fill = `${line} L100,100 L0,100 Z`;
  els.trafficGraphLine.setAttribute("d", line);
  els.trafficGraphFill.setAttribute("d", fill);

  const latest = perSecond[perSecond.length - 1] || 0;
  els.graphNow.textContent = formatGraphValue(latest, metric);
  els.graphMax.textContent = formatGraphValue(maxValue, metric);
  els.graphYMax.textContent = formatGraphValue(yMax, metric);
  els.graphLegend.textContent = graphLegendText(metric);
}

function formatGraphValue(value, metric) {
  if (metric === "bits") return `${Math.round(value)} bit/s`;
  if (metric === "errors") return `${value.toFixed(1)} err/s`;
  return `${value.toFixed(1)} fps`;
}

function graphLegendText(metric) {
  if (metric === "bits") return "Суммарная битовая нагрузка успешных кадров на шине.";
  if (metric === "errors") return "Частота ошибок кадра: ACK/CRC/stuff/bit.";
  return "Частота доставленных CAN-кадров. Рост активности поднимает линию.";
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

async function boot() {
  const client = new BrokerClient();
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
  client.on("close", () => setStatus(els.stDot, els.stText, false, "Отключено"));

  const registered = await client.connect();
  setStatus(els.stDot, els.stText, true, "Мониторинг");
  els.monitorEcu.textContent = registered.ecu_id;
  upsertCtl(registered.controller);

  network = await fetch(`/network.json?ts=${Date.now()}`, { cache: "no-store" })
    .then((r) => r.json())
    .catch(() => registered.network || []);
  renderTopology();
  loadInitialControllerStates();
  setInterval(refreshRuntimeNetwork, 1000);
  graphTimer = setInterval(renderTrafficGraph, 250);
  [els.graphMetric, els.graphWindow, els.graphScale].forEach((el) => {
    el.addEventListener("change", renderTrafficGraph);
  });
  renderTrafficGraph();
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

boot().catch((e) => setStatus(els.stDot, els.stText, false, e.message));
