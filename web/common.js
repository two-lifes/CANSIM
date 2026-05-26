export class BrokerClient {
  constructor({ port = location.port || "8765", host = location.hostname || "127.0.0.1" } = {}) {
    this.url = `ws://${host}:${port}/ws`;
    this.ws = null;
    this.handlers = {};
    this.ecuId = null;
    this.closedByApp = false;
  }

  on(type, fn) {
    this.handlers[type] = fn;
  }

  connect() {
    return new Promise((resolve, reject) => {
      let settled = false;
      this.ws = new WebSocket(this.url);
      this.ws.onopen = () => this.send({ type: "register" });
      this.ws.onmessage = (ev) => {
        const msg = JSON.parse(ev.data);
        if (msg.type === "registered") {
          this.ecuId = msg.ecu_id;
          settled = true;
          resolve(msg);
          const fn = this.handlers.registered;
          if (fn) fn(msg);
          return;
        }
        const fn = this.handlers[msg.type];
        if (fn) fn(msg);
      };
      this.ws.onerror = () => {
        if (!settled) reject(new Error("Нет связи с ECU"));
      };
      this.ws.onclose = () => {
        if (!settled) reject(new Error("Нет связи с ECU"));
        if (this.handlers.close) this.handlers.close({ byApp: this.closedByApp });
      };
    });
  }

  close() {
    this.closedByApp = true;
    this.ws?.close();
  }

  isOpen() {
    return this.ws?.readyState === WebSocket.OPEN;
  }

  send(obj) {
    if (this.ws?.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify(obj));
    }
  }

  setSendConfig(profile) {
    this.send({ type: "tx_config", profile });
  }

  setReceiveConfig(profile) {
    this.send({ type: "rx_config", profile });
  }

  setTxConfig(profile) {
    this.setSendConfig(profile);
  }

  setRxConfig(profile) {
    this.setReceiveConfig(profile);
  }

  publish(payload) {
    this.send({ type: "publish", payload });
  }

  stagePayload(payload) {
    this.send({ type: "stage_payload", payload });
  }
}

/** Строка состояния внизу окна (Win7 status bar). */
export function setStatus(dotEl, textEl, ok, text) {
  if (dotEl) {
    dotEl.className = "status-dot " + (ok ? "ok" : "err");
  }
  if (textEl) {
    textEl.className = "status-text " + (ok ? "ok" : "err");
    textEl.textContent = text;
  }
}

export function setStatusLine(id, ok, text) {
  const dot = document.getElementById(id + "Dot");
  const line = document.getElementById(id + "Text");
  setStatus(dot, line, ok, text);
}

const AUTO_RECONNECT_KEY = "linuxcan.autoReconnect.enabled";

export function readAutoReconnect() {
  try {
    return localStorage.getItem(AUTO_RECONNECT_KEY) !== "0";
  } catch {
    return true;
  }
}

export function writeAutoReconnect(enabled) {
  try {
    localStorage.setItem(AUTO_RECONNECT_KEY, enabled ? "1" : "0");
  } catch {
    // localStorage может быть недоступен в приватном режиме; тогда оставляем значение в памяти.
  }
}
