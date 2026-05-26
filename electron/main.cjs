const { app, BrowserWindow } = require("electron");
const { spawn } = require("child_process");
const path = require("path");
const fs = require("fs");

const ROOT = path.join(__dirname, "..");
const BIN = path.join(ROOT, "build", "linuxcan");
const WEB = path.join(ROOT, "web");
const ECUS_DIR = process.env.LINUXCAN_ECUS || path.join(ROOT, "ecus");
const HOST = "127.0.0.1";
const CAN = "vcan0";
const BUS_PORT = Number(process.env.LINUXCAN_BUS_PORT || 8750);

const procs = [];
const windows = [];

function ensureBinary() {
  if (!fs.existsSync(BIN)) {
    console.error("Соберите брокер: ./linuxcan из корня проекта");
    process.exit(1);
  }
}

function loadEcus() {
  return fs.readdirSync(ECUS_DIR, { withFileTypes: true })
    .filter((d) => d.isDirectory())
    .map((d) => {
      const dir = path.join(ECUS_DIR, d.name);
      const cfgPath = path.join(dir, "ecu.json");
      if (!fs.existsSync(cfgPath)) return null;
      const cfg = JSON.parse(fs.readFileSync(cfgPath, "utf8"));
      return {
        id: cfg.id || d.name,
        port: Number(cfg.port || 8765),
        page: cfg.page || "ecu.html",
        can: cfg.can || CAN,
        tx: cfg.tx || {},
        rx: cfg.rx || {},
        title: cfg.id || d.name,
        dir,
        w: Number(cfg.window?.width || 760),
        h: Number(cfg.window?.height || 720),
      };
    })
    .filter(Boolean);
}

function networkJson(ecus) {
  return JSON.stringify(ecus.map((ecu) => ({
    id: ecu.id,
    port: ecu.port,
    page: ecu.page,
    can: ecu.can,
    tx: ecu.tx || {},
    rx: ecu.rx || {},
    url: `http://${HOST}:${ecu.port}/${ecu.page}`,
  })));
}

function startBusMonitor(ecus) {
  const p = spawn(
    BIN,
    [
      "--ecu-id", "bus-monitor",
      "--can", CAN,
      "--host", HOST,
      "--port", String(BUS_PORT),
      "--page", "bus.html",
      "--config-dir", ROOT,
      "--network-json", networkJson(ecus),
      "--bus-monitor",
      "--quiet",
    ],
    { cwd: ROOT, env: { ...process.env, LINUXCAN_WEB: WEB }, stdio: ["ignore", "pipe", "pipe"] }
  );
  p.stderr.on("data", (d) => process.stderr.write(d));
  procs.push(p);
  return p;
}

function startEcu({ id, port, page, can, dir }, networkPayload) {
  const p = spawn(
    BIN,
    [
      "--ecu-id", id,
      "--can", can,
      "--host", HOST,
      "--port", String(port),
      "--page", page,
      "--config-dir", dir,
      "--network-json", networkPayload,
      "--quiet",
    ],
    { cwd: ROOT, env: { ...process.env, LINUXCAN_WEB: WEB }, stdio: ["ignore", "pipe", "pipe"] }
  );
  p.stderr.on("data", (d) => process.stderr.write(d));
  procs.push(p);
  return p;
}

function openWindow({ port, page, title, w, h }) {
  const win = new BrowserWindow({
    width: w || 380,
    height: h || 400,
    minWidth: 320,
    minHeight: 280,
    resizable: true,
    title: `LinuxCAN — ${title}`,
    autoHideMenuBar: true,
  });
  win.loadURL(`http://${HOST}:${port}/${page}`);
  windows.push(win);
}

function openBusWindow() {
  const win = new BrowserWindow({
    width: 1100,
    height: 760,
    minWidth: 900,
    minHeight: 600,
    resizable: true,
    title: "LinuxCAN — CAN Bus",
    autoHideMenuBar: true,
  });
  win.loadURL(`http://${HOST}:${BUS_PORT}/bus.html`);
  windows.push(win);
}

function stopAll() {
  for (const p of procs) {
    if (p && !p.killed) p.kill("SIGTERM");
  }
  procs.length = 0;
}

app.whenReady().then(async () => {
  ensureBinary();
  const ECUS = loadEcus();
  if (!ECUS.length) {
    console.error(`Нет ECU-конфигов: ${ECUS_DIR}/*/ecu.json`);
    app.quit();
    return;
  }
  startBusMonitor(ECUS);
  const networkPayload = networkJson(ECUS);
  for (const ecu of ECUS) startEcu(ecu, networkPayload);
  await new Promise((r) => setTimeout(r, 500));
  openBusWindow();
  for (const ecu of ECUS) openWindow(ecu);
});

app.on("window-all-closed", () => {
  stopAll();
  if (process.platform !== "darwin") app.quit();
});

app.on("before-quit", stopAll);
