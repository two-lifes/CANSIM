import { getSensorType } from "./sensors/index.js";

let nextInstanceId = 1;

function hasExplicitSettings(overrides) {
  return Object.prototype.hasOwnProperty.call(overrides, "settings");
}

function roundSetting(value) {
  return Math.round(value * 100) / 100;
}

function applyInitialSpread(type, settings, overrides) {
  if (hasExplicitSettings(overrides) || !type.initialSpread) return settings;

  const next = { ...settings };
  for (const [key, spread] of Object.entries(type.initialSpread)) {
    const base = Number(next[key]);
    const amount = Number(spread);
    if (!Number.isFinite(base) || !Number.isFinite(amount) || amount <= 0) continue;
    next[key] = roundSetting(base + (Math.random() * 2 - 1) * amount);
  }

  if (Number(next.max) <= Number(next.min)) next.max = roundSetting(Number(next.min) + 1);
  if (Number(next.max) <= Number(next.idle)) next.max = roundSetting(Number(next.idle) + 100);
  if (Number(next.clampMax) <= Number(next.clampMin)) next.clampMax = roundSetting(Number(next.clampMin) + 0.1);

  return next;
}

/**
 * Экземпляр подключённого датчика на ECU.
 *
 * Имеет три значения в каждый момент времени:
 *   rawValue  — то, что генерирует сам датчик
 *   modifier  — преобразование (для будущих помех/прерываний)
 *   inputValue — то, что приходит на вход ECU (после modifier)
 *
 * Сейчас modifier = тождественный, заложен под расширение.
 */
export class SensorInstance {
  constructor(typeId, overrides = {}) {
    const type = getSensorType(typeId);
    if (!type) throw new Error(`Unknown sensor type: ${typeId}`);

    this.id = `s${nextInstanceId++}`;
    this.type = type;
    this.alias = overrides.alias ?? type.name;
    this.settings = applyInitialSpread(type, { ...type.defaults, ...(overrides.settings ?? {}) }, overrides);
    this.tickMs = overrides.tickMs ?? 500;
    this.state = {};
    this.rawValue = 0;
    this.inputValue = 0;
    this.lastTickAt = 0;
    this._timer = null;
    this._listeners = new Set();
  }

  start() {
    if (this._timer) return;
    this.lastTickAt = performance.now();
    this._tick();
    this._timer = setInterval(() => this._tick(), this.tickMs);
  }

  stop() {
    if (this._timer) {
      clearInterval(this._timer);
      this._timer = null;
    }
  }

  /** Меняет настройку и перезапускает таймер при смене tickMs. */
  updateSetting(key, value) {
    if (key === "tickMs") {
      this.tickMs = Number(value) || 500;
      if (this._timer) {
        this.stop();
        this.start();
      }
      return;
    }
    this.settings[key] = value;
  }

  setAlias(alias) {
    this.alias = alias;
  }

  onChange(fn) {
    this._listeners.add(fn);
    return () => this._listeners.delete(fn);
  }

  _tick() {
    const now = performance.now();
    const dt = now - this.lastTickAt;
    this.lastTickAt = now;
    this.rawValue = Number(this.type.tick(this.state, this.settings, dt) ?? 0);
    this.inputValue = this._applyModifier(this.rawValue);
    for (const fn of this._listeners) fn(this);
  }

  _applyModifier(value) {
    // Заглушка под будущие модификаторы сигнала (помехи, обрыв, фиксированное значение)
    return value;
  }
}
