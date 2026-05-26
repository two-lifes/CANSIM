/**
 * Главный модуль страницы ECU.
 *
 * Слои:
 *   sensors/   — реестр типов датчиков (независимая, легко расширяемая часть)
 *   sensor-instance.js — экземпляр подключённого датчика
 *   ecu-logic.js — формула агрегации и преобразование TX
 *   wiring.js  — SVG «провода»
 *   common.js  — клиент к нашему C++ ECU (WebSocket)
 *
 * Связи: датчики тикают → пишут rawValue → modifier → inputValue
 *        ECU агрегирует inputValue → computed → transform → txValue → CAN
 */
import { BrokerClient, setStatus } from "./common.js";
import { listSensorTypes, getSensorType } from "./sensors/index.js";
import { SensorInstance } from "./sensor-instance.js";
import { FORMULAS, aggregate, transformTx } from "./ecu-logic.js";
import { Wiring } from "./wiring.js";

const NUMBER_FORMAT = new Intl.NumberFormat("ru-RU", {
  maximumFractionDigits: 2,
  minimumFractionDigits: 0,
});

const STORAGE_VERSION = 1;

function fmt(value, unit = "") {
  if (value === null || value === undefined || Number.isNaN(value)) return "—";
  const txt = NUMBER_FORMAT.format(Number(value));
  return unit ? `${txt} ${unit}` : txt;
}

class EcuApp {
  constructor(root) {
    this.root = root;
    this.client = null;
    this.cfg = null;
    this.sensors = [];

    this.formula = "avg";
    this.customExpr = "a + b";
    this.scale = 1;
    this.offset = 0;
    this.txId = "0x100";
    this.txMode = "manual";
    this.txIntervalMs = 5000;
    this.rxFilters = [];
    this.txTimer = null;
    this.stageTimer = null;
    this.storageKey = null;
    this.saveTimer = null;
    this.persistenceReady = false;
    this.computed = 0;
    this.txValue = 0;

    this.els = this._collectEls();
    this._initWiring();
    this._renderAvailableSensors();
    this._renderConnected();
    this._renderEcuInputs();
    this._wireControls();
    this._connectBackend().catch((e) => setStatus(this.els.stDot, this.els.stText, false, e.message));
  }

  // ---------- DOM ----------

  _collectEls() {
    return {
      titleEcu: this.root.querySelector("[data-el=titleEcuId]"),
      ecuComputed: this.root.querySelector("[data-el=ecuComputed]"),
      ecuTxValue: this.root.querySelector("[data-el=ecuTxValue]"),
      ecuFormula: this.root.querySelector("[data-el=ecuFormula]"),
      ecuFormulaCustom: this.root.querySelector("[data-el=ecuFormulaCustom]"),
      ecuFormulaCustomRow: this.root.querySelector("[data-el=ecuFormulaCustomRow]"),
      ecuScale: this.root.querySelector("[data-el=ecuScale]"),
      ecuOffset: this.root.querySelector("[data-el=ecuOffset]"),
      ecuTxId: this.root.querySelector("[data-el=ecuTxId]"),
      ecuTxMode: this.root.querySelector("[data-el=ecuTxMode]"),
      ecuTxInterval: this.root.querySelector("[data-el=ecuTxInterval]"),
      ecuTxNow: this.root.querySelector("[data-el=ecuTxNow]"),
      busRxFilters: this.root.querySelector("[data-el=busRxFilters]"),
      busRxAdd: this.root.querySelector("[data-el=busRxAdd]"),
      busRxApply: this.root.querySelector("[data-el=busRxApply]"),
      busRxLast: this.root.querySelector("[data-el=busRxLast]"),
      ctrlMode: this.root.querySelector("[data-el=ctrlMode]"),
      ctrlTec: this.root.querySelector("[data-el=ctrlTec]"),
      ctrlRec: this.root.querySelector("[data-el=ctrlRec]"),
      ctrlTransport: this.root.querySelector("[data-el=ctrlTransport]"),
      ctrlTxStats: this.root.querySelector("[data-el=ctrlTxStats]"),
      ctrlRxStats: this.root.querySelector("[data-el=ctrlRxStats]"),
      ctrlAppRxStats: this.root.querySelector("[data-el=ctrlAppRxStats]"),
      ctrlArbLost: this.root.querySelector("[data-el=ctrlArbLost]"),
      ctrlBusOff: this.root.querySelector("[data-el=ctrlBusOff]"),
      ctrlResetBusOff: this.root.querySelector("[data-el=ctrlResetBusOff]"),
      ecuInputs: this.root.querySelector("[data-el=ecuInputs]"),
      availableList: this.root.querySelector("[data-el=availableList]"),
      availableAdd: this.root.querySelector("[data-el=availableAdd]"),
      connectedList: this.root.querySelector("[data-el=connectedList]"),
      wiringContainer: this.root.querySelector("[data-el=wiringContainer]"),
      wiringSvg: this.root.querySelector("[data-el=wiringSvg]"),
      stDot: this.root.querySelector("[data-el=stDot]"),
      stText: this.root.querySelector("[data-el=stText]"),
      ecuInfo: this.root.querySelector("[data-el=ecuInfo]"),
    };
  }

  _initWiring() {
    this.wiring = new Wiring({
      svg: this.els.wiringSvg,
      container: this.els.wiringContainer,
    });
  }

  // ---------- Datchik catalog (Win7 list) ----------

  _renderAvailableSensors() {
    const ul = this.els.availableList;
    ul.innerHTML = "";
    for (const type of listSensorTypes()) {
      const li = document.createElement("li");
      li.textContent = `${type.name}${type.unit ? ` [${type.unit}]` : ""}`;
      li.dataset.typeId = type.id;
      li.tabIndex = 0;
      li.addEventListener("click", () => {
        ul.querySelectorAll(".selected").forEach((el) => el.classList.remove("selected"));
        li.classList.add("selected");
      });
      li.addEventListener("dblclick", () => this.addSensor(type.id));
      ul.appendChild(li);
    }
    if (ul.firstChild) ul.firstChild.classList.add("selected");

    this.els.availableAdd.addEventListener("click", () => {
      const sel = ul.querySelector(".selected");
      if (sel) this.addSensor(sel.dataset.typeId);
    });
  }

  // ---------- Подключённые датчики ----------

  addSensor(typeId, overrides = {}, shouldSave = true) {
    const inst = new SensorInstance(typeId, overrides);
    inst.onChange(() => this._onSensorTick());
    inst.start();
    this.sensors.push(inst);
    this._renderConnected();
    this._renderEcuInputs();
    if (this.persistenceReady) this._restartTxTimer();
    if (shouldSave) this._saveStateSoon();
  }

  removeSensor(id) {
    const idx = this.sensors.findIndex((s) => s.id === id);
    if (idx < 0) return;
    this.sensors[idx].stop();
    this.sensors.splice(idx, 1);
    this._renderConnected();
    this._renderEcuInputs();
    if (this.persistenceReady) this._restartTxTimer();
    this._saveStateSoon();
  }

  _renderConnected() {
    const host = this.els.connectedList;
    host.innerHTML = "";
    if (!this.sensors.length) {
      const empty = document.createElement("p");
      empty.className = "hint";
      empty.textContent = "Датчики не подключены. Выберите слева и нажмите «Добавить».";
      host.appendChild(empty);
      this.wiring.refresh();
      return;
    }

    for (const s of this.sensors) {
      host.appendChild(this._buildSensorCard(s));
    }
    requestAnimationFrame(() => this._updateWiring());
  }

  _buildSensorCard(sensor) {
    const card = document.createElement("div");
    card.className = "sensor-card";
    card.dataset.sensorId = sensor.id;

    const head = document.createElement("div");
    head.className = "sensor-head";
    const alias = document.createElement("input");
    alias.className = "sensor-alias";
    alias.value = sensor.alias;
    alias.title = "Имя датчика";
    alias.addEventListener("change", () => {
      sensor.setAlias(alias.value.trim() || sensor.type.name);
      this._renderEcuInputs();
      this._saveStateSoon();
    });
    const remove = document.createElement("button");
    remove.type = "button";
    remove.className = "sensor-remove";
    remove.textContent = "✕";
    remove.title = "Отключить датчик";
    remove.addEventListener("click", () => this.removeSensor(sensor.id));
    head.append(alias, remove);

    const settingsBox = document.createElement("div");
    settingsBox.className = "sensor-settings";
    for (const def of sensor.type.settings) {
      settingsBox.appendChild(this._buildSettingRow(sensor, def));
    }
    settingsBox.appendChild(
      this._buildSettingRow(sensor, { key: "tickMs", label: "Тик датч.", type: "number", min: 50, step: 50 }, sensor.tickMs),
    );

    const outRow = document.createElement("div");
    outRow.className = "sensor-out";
    const valueEl = document.createElement("span");
    valueEl.className = "sensor-out-value";
    valueEl.dataset.role = "value";
    valueEl.textContent = fmt(sensor.rawValue, sensor.type.unit);
    const outlet = document.createElement("span");
    outlet.className = "outlet";
    outlet.dataset.outletFor = sensor.id;
    outRow.append(
      Object.assign(document.createElement("span"), { className: "hint", textContent: "Выход:" }),
      valueEl,
      outlet,
    );

    card.append(head, settingsBox, outRow);
    return card;
  }

  _buildSettingRow(sensor, def, currentValue) {
    const row = document.createElement("label");
    row.className = "sensor-setting";

    const cap = document.createElement("span");
    cap.textContent = def.label;
    row.appendChild(cap);

    let input;
    const value = currentValue ?? sensor.settings[def.key];
    if (def.type === "select") {
      input = document.createElement("select");
      for (const opt of def.options) {
        const o = document.createElement("option");
        o.value = String(opt.value);
        o.textContent = opt.label;
        if (String(opt.value) === String(value)) o.selected = true;
        input.appendChild(o);
      }
    } else {
      input = document.createElement("input");
      input.type = def.type || "number";
      if (def.min !== undefined) input.min = def.min;
      if (def.max !== undefined) input.max = def.max;
      if (def.step !== undefined) input.step = def.step;
      input.value = value;
    }
    input.addEventListener("change", () => {
      const v = input.type === "number" || def.type === "select" ? Number(input.value) : input.value;
      sensor.updateSetting(def.key, v);
      this._saveStateSoon();
    });
    row.appendChild(input);
    return row;
  }

  // ---------- ECU блок (входы, формула, computed, TX) ----------

  _renderEcuInputs() {
    const host = this.els.ecuInputs;
    host.innerHTML = "";
    if (!this.sensors.length) {
      const empty = document.createElement("p");
      empty.className = "hint";
      empty.textContent = "Входы пусты";
      host.appendChild(empty);
      this.wiring.refresh();
      return;
    }
    this.sensors.forEach((s, idx) => {
      const row = document.createElement("div");
      row.className = "ecu-input";
      row.dataset.sensorId = s.id;

      const inlet = document.createElement("span");
      inlet.className = "inlet";
      inlet.dataset.inletFor = s.id;

      const name = document.createElement("span");
      name.className = "ecu-input-name";
      name.textContent = `${String.fromCharCode(97 + idx)} = ${s.alias}`;

      const val = document.createElement("span");
      val.className = "ecu-input-value";
      val.dataset.role = "value";
      val.textContent = fmt(s.inputValue, s.type.unit);

      row.append(inlet, name, val);
      host.appendChild(row);
    });
    requestAnimationFrame(() => this._updateWiring());
  }

  _updateWiring() {
    const connections = this.sensors.map((s) => ({
      from: this.root.querySelector(`[data-outlet-for="${s.id}"]`),
      to: this.root.querySelector(`[data-inlet-for="${s.id}"]`),
      color: "#2b6cb0",
    }));
    this.wiring.setConnections(connections);
  }

  _onSensorTick() {
    // Обновляем «живые» значения inline, без перерисовки структуры
    for (const s of this.sensors) {
      const out = this.root.querySelector(`.sensor-card[data-sensor-id="${s.id}"] [data-role="value"]`);
      if (out) out.textContent = fmt(s.rawValue, s.type.unit);
      const inp = this.root.querySelector(`.ecu-input[data-sensor-id="${s.id}"] [data-role="value"]`);
      if (inp) inp.textContent = fmt(s.inputValue, s.type.unit);
    }
    this._recompute();
    this._stagePayloadSoon();
  }

  _recompute() {
    const values = this.sensors.map((s) => s.inputValue);
    this.computed = aggregate(values, this.formula, this.customExpr);
    this.txValue = transformTx(this.computed, this.scale, this.offset);
    this.els.ecuComputed.textContent = fmt(this.computed);
    this.els.ecuTxValue.textContent = fmt(this.txValue);
  }

  // ---------- TX контролы + CAN ----------

  _wireControls() {
    const els = this.els;

    els.ecuFormula.innerHTML = "";
    for (const f of FORMULAS) {
      const o = document.createElement("option");
      o.value = f.id;
      o.textContent = f.label;
      els.ecuFormula.appendChild(o);
    }
    els.ecuFormula.value = this.formula;
    els.ecuFormula.addEventListener("change", () => {
      this.formula = els.ecuFormula.value;
      els.ecuFormulaCustomRow.style.display = this.formula === "custom" ? "" : "none";
      this._recompute();
      this._saveStateSoon();
    });
    els.ecuFormulaCustomRow.style.display = "none";
    els.ecuFormulaCustom.value = this.customExpr;
    els.ecuFormulaCustom.addEventListener("change", () => {
      this.customExpr = els.ecuFormulaCustom.value;
      this._recompute();
      this._saveStateSoon();
    });

    els.ecuScale.value = this.scale;
    els.ecuScale.addEventListener("change", () => {
      this.scale = Number(els.ecuScale.value) || 1;
      this._recompute();
      this._saveStateSoon();
    });
    els.ecuOffset.value = this.offset;
    els.ecuOffset.addEventListener("change", () => {
      this.offset = Number(els.ecuOffset.value) || 0;
      this._recompute();
      this._saveStateSoon();
    });

    els.ecuTxId.value = this.txId;
    els.ecuTxId.addEventListener("change", () => {
      this.txId = els.ecuTxId.value.trim() || "0x100";
      this._applyTxConfig();
      this._saveStateSoon();
    });
    els.ecuTxMode.value = this.txMode;
    els.ecuTxMode.addEventListener("change", () => {
      this.txMode = els.ecuTxMode.value;
      this._restartTxTimer();
      this._saveStateSoon();
    });
    els.ecuTxInterval.value = this.txIntervalMs;
    els.ecuTxInterval.addEventListener("change", () => {
      this.txIntervalMs = Math.max(50, Number(els.ecuTxInterval.value) || 500);
      this._restartTxTimer();
      this._saveStateSoon();
    });
    els.ecuTxNow.addEventListener("click", () => this._publishNow());

    els.busRxAdd.addEventListener("click", () => {
      this.rxFilters.push({ can_id: "0x100", label: "", encoding: "float32" });
      this._renderBusRxConfig();
      this._applyRxConfig();
      this._saveStateSoon();
    });
    els.busRxApply.addEventListener("click", () => {
      this._readRxFiltersFromDom();
      this._applyRxConfig();
      this._renderBusRxConfig();
      this._saveStateSoon();
      setStatus(this.els.stDot, this.els.stText, true, "RX фильтры применены");
    });

    if (els.ctrlResetBusOff) {
      els.ctrlResetBusOff.addEventListener("click", () => {
        this.client?.send({ type: "reset_bus_off" });
        setStatus(this.els.stDot, this.els.stText, true, "Запрошен сброс bus-off");
      });
    }
  }

  _restartTxTimer() {
    if (this.txTimer) {
      clearInterval(this.txTimer);
      this.txTimer = null;
    }
    this._applyTxConfig();
    this._stagePayloadSoon();
  }

  _applyTxConfig() {
    if (!this.client) return;
    this.client.setTxConfig({
      can_interface: "vcan0",
      can_id: this.txId,
      encoding: "float32",
      schedule: { mode: this.txMode, interval_ms: this.txIntervalMs },
      pre_send: ["timestamp"],
    });
  }

  _applyRxConfig() {
    if (!this.client) return;
    this.client.setRxConfig({
      filters: this.rxFilters.map((f) => ({
        can_id: f.can_id,
        label: f.label,
        encoding: f.encoding || "float32",
      })),
    });
  }

  _publishNow() {
    if (!this.client) return;
    this.client.publish({ type: "ecu", value: this.txValue });
  }

  _stagePayloadSoon() {
    if (!this.client || this.txMode !== "interval") return;
    clearTimeout(this.stageTimer);
    this.stageTimer = setTimeout(() => this._stagePayloadNow(), 10);
  }

  _stagePayloadNow() {
    if (!this.client || this.txMode !== "interval") return;
    this.client.stagePayload({ type: "ecu", value: this.txValue });
  }

  // ---------- Persistence (per ECU; no live RX/CAN logs) ----------

  _makeStorageKey() {
    const ecuId = this.cfg?.ecu_id ?? "unknown";
    return `linuxcan.ecu.${ecuId}.state.v${STORAGE_VERSION}`;
  }

  _serializeState() {
    return {
      version: STORAGE_VERSION,
      formula: this.formula,
      customExpr: this.customExpr,
      scale: this.scale,
      offset: this.offset,
      txId: this.txId,
      txMode: this.txMode,
      txIntervalMs: this.txIntervalMs,
      rxFilters: this.rxFilters,
      sensors: this.sensors.map((s) => ({
        typeId: s.type.id,
        alias: s.alias,
        settings: s.settings,
        tickMs: s.tickMs,
      })),
    };
  }

  _saveStateSoon() {
    if (!this.persistenceReady || !this.storageKey) return;
    clearTimeout(this.saveTimer);
    this.saveTimer = setTimeout(() => this._saveState(), 120);
  }

  _saveState() {
    if (!this.storageKey) return;
    try {
      localStorage.setItem(this.storageKey, JSON.stringify(this._serializeState()));
    } catch (e) {
      console.warn("Failed to save ECU UI state", e);
    }
  }

  _restoreState() {
    if (!this.storageKey) return;
    const raw = localStorage.getItem(this.storageKey);
    if (!raw) return;

    let data;
    try {
      data = JSON.parse(raw);
    } catch {
      return;
    }
    if (data?.version !== STORAGE_VERSION) return;

    this.formula = data.formula ?? this.formula;
    this.customExpr = data.customExpr ?? this.customExpr;
    this.scale = Number(data.scale ?? this.scale);
    this.offset = Number(data.offset ?? this.offset);
    this.txId = data.txId ?? this.txId;
    this.txMode = data.txMode ?? this.txMode;
    this.txIntervalMs = Number(data.txIntervalMs ?? this.txIntervalMs);
    if (Array.isArray(data.rxFilters)) {
      this.rxFilters = data.rxFilters.map((f) => ({
        can_id: f.can_id ?? "0x100",
        label: f.label ?? "",
        encoding: f.encoding ?? "float32",
      }));
    }

    for (const sensor of this.sensors) sensor.stop();
    this.sensors = [];
    for (const item of data.sensors ?? []) {
      if (!item?.typeId || !getSensorType(item.typeId)) continue;
      this.addSensor(
        item.typeId,
        {
          alias: item.alias,
          settings: item.settings,
          tickMs: item.tickMs,
        },
        false,
      );
    }
  }

  _syncControlsFromState() {
    this.els.ecuFormula.value = this.formula;
    this.els.ecuFormulaCustomRow.style.display = this.formula === "custom" ? "" : "none";
    this.els.ecuFormulaCustom.value = this.customExpr;
    this.els.ecuScale.value = this.scale;
    this.els.ecuOffset.value = this.offset;
    this.els.ecuTxId.value = this.txId;
    this.els.ecuTxMode.value = this.txMode;
    this.els.ecuTxInterval.value = this.txIntervalMs;
    this._renderBusRxConfig();
    this._recompute();
  }

  // ---------- Подключение к C++ ECU (CAN) ----------

  async _connectBackend() {
    this.client = new BrokerClient();
    this.client.on("error", (m) => setStatus(this.els.stDot, this.els.stText, false, m.message));
    this.client.on("close", () => setStatus(this.els.stDot, this.els.stText, false, "Нет связи"));
    this.client.on("sent", () => setStatus(this.els.stDot, this.els.stText, true, "TX OK"));
    this.client.on("message", (msg) => this._onBusRx(msg));
    this.client.on("controller_state", (msg) => this._onControllerState(msg));
    this.client.on("bus_delivered", (msg) => this._onBusDelivered(msg));

    const msg = await this.client.connect();
    setStatus(this.els.stDot, this.els.stText, true, "На шине");

    this.cfg = await fetch("/ecu-config.json").then((r) => r.json()).catch(() => msg);
    if (this.cfg?.ecu_id) this.els.titleEcu.textContent = this.cfg.ecu_id;
    this._renderControllerState(this.cfg?.controller, this.cfg?.can_mode);
    if (this.cfg?.tx?.can_id) {
      this.txId = this.cfg.tx.can_id;
      this.els.ecuTxId.value = this.txId;
    }
    this.rxFilters = this._normalizeRxFilters(this.cfg?.rx?.filters ?? []);
    this.els.ecuInfo.textContent = `${this.cfg?.ecu_id ?? msg.ecu_id} · ${this.cfg?.can_interface ?? "vcan0"}`;
    this._renderBusRxConfig();

    this.storageKey = this._makeStorageKey();
    this._restoreState();
    this._syncControlsFromState();
    this.persistenceReady = true;

    this._applyTxConfig();
    this._applyRxConfig();
    this._restartTxTimer();
  }

  _normalizeRxFilters(filters) {
    return filters.map((f) => ({
      can_id: f.can_id ?? "0x100",
      label: f.label ?? "",
      encoding: f.encoding ?? "float32",
    }));
  }

  _renderBusRxConfig() {
    const host = this.els.busRxFilters;
    host.innerHTML = "";
    if (!this.rxFilters.length) {
      const empty = document.createElement("p");
      empty.className = "hint";
      empty.textContent = "Фильтров нет. ECU не принимает кадры.";
      host.appendChild(empty);
      return;
    }
    this.rxFilters.forEach((filter, idx) => {
      const row = document.createElement("div");
      row.className = "rx-filter-row";
      row.dataset.index = String(idx);

      const canId = document.createElement("input");
      canId.className = "mono rx-can-id";
      canId.value = filter.can_id;
      canId.title = "CAN ID, например 0x100";

      const label = document.createElement("input");
      label.className = "rx-label";
      label.value = filter.label ?? "";
      label.placeholder = "label";

      const encoding = document.createElement("select");
      encoding.className = "rx-encoding";
      for (const value of ["float32", "json", "text"]) {
        const option = document.createElement("option");
        option.value = value;
        option.textContent = value;
        encoding.appendChild(option);
      }
      encoding.value = filter.encoding || "float32";

      const remove = document.createElement("button");
      remove.type = "button";
      remove.className = "rx-remove";
      remove.title = "Удалить фильтр";
      remove.textContent = "✕";

      row.append(canId, label, encoding, remove);

      remove.addEventListener("click", () => {
        this._readRxFiltersFromDom();
        this.rxFilters.splice(idx, 1);
        this._renderBusRxConfig();
        this._applyRxConfig();
        this._saveStateSoon();
      });
      row.querySelectorAll("input, select").forEach((el) => {
        el.addEventListener("change", () => {
          this._readRxFiltersFromDom();
          this._applyRxConfig();
          this._saveStateSoon();
        });
      });
      host.appendChild(row);
    });
  }

  _readRxFiltersFromDom() {
    this.rxFilters = Array.from(this.els.busRxFilters.querySelectorAll(".rx-filter-row"))
      .map((row) => ({
        can_id: row.querySelector(".rx-can-id").value.trim(),
        label: row.querySelector(".rx-label").value.trim(),
        encoding: row.querySelector(".rx-encoding").value,
      }))
      .filter((f) => f.can_id);
  }

  _onControllerState(msg) {
    this._renderControllerState(msg.data, this.cfg?.can_mode);
  }

  _onBusDelivered(_msg) {
    // ECU не показывает биты — это задача bus-monitor, но статус контроллера
    // приходит отдельным сообщением controller_state.
  }

  _renderControllerState(state, mode) {
    if (!state || typeof state !== "object") return;
    const m = (state.mode || "—").toLowerCase();
    if (this.els.ctrlMode) {
      this.els.ctrlMode.textContent = state.mode || "—";
      this.els.ctrlMode.dataset.mode = m;
    }
    if (this.els.ctrlTec) this.els.ctrlTec.textContent = state.tec ?? 0;
    if (this.els.ctrlRec) this.els.ctrlRec.textContent = state.rec ?? 0;
    if (this.els.ctrlTransport) {
      this.els.ctrlTransport.textContent = mode ? `${mode}` : "bussim";
    }
    const stats = state.stats || {};
    if (this.els.ctrlTxStats) this.els.ctrlTxStats.textContent = `${stats.tx_ok ?? 0} / ${stats.tx_errors ?? 0}`;
    if (this.els.ctrlRxStats) this.els.ctrlRxStats.textContent = `${stats.rx_ok ?? 0} / ${stats.rx_errors ?? 0}`;
    if (this.els.ctrlAppRxStats) this.els.ctrlAppRxStats.textContent = stats.app_rx_ok ?? 0;
    if (this.els.ctrlArbLost) this.els.ctrlArbLost.textContent = stats.arbitration_lost ?? 0;
    if (this.els.ctrlBusOff) this.els.ctrlBusOff.textContent = stats.bus_off_events ?? 0;
  }

  _onBusRx(msg) {
    const label = msg.filter_label ? `[${msg.filter_label}] ` : "";
    const value = msg.payload?.value ?? msg.payload?.text ?? JSON.stringify(msg.payload);
    const line = `${label}${msg.from} → ${fmt(value)}`;
    const prev = this.els.busRxLast.textContent === "—" ? "" : this.els.busRxLast.textContent;
    this.els.busRxLast.textContent = [line, ...prev.split("\n").filter(Boolean)].slice(0, 3).join("\n");
    setStatus(this.els.stDot, this.els.stText, true, "RX");
  }
}

export function bootEcuApp(root = document) {
  return new EcuApp(root);
}
