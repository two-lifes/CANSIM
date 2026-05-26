import { registerSensorType } from "../registry.js";

registerSensorType({
  id: "speed",
  name: "Датчик скорости (об/мин)",
  unit: "rpm",
  defaults: {
    idle: 800,
    max: 6500,
    accelRpmPerSec: 600,
    decelRpmPerSec: 400,
    throttle: 0.5,
    wobbleRpm: 80,
    noiseRpm: 15,
  },
  initialSpread: { idle: 80, max: 350 },
  settings: [
    { key: "idle", label: "Холостой", type: "number", step: 50 },
    { key: "max", label: "Макс", type: "number", step: 100 },
    { key: "accelRpmPerSec", label: "Разгон rpm/с", type: "number", step: 50 },
    { key: "decelRpmPerSec", label: "Замедл. rpm/с", type: "number", step: 50 },
    { key: "throttle", label: "Газ (0..1)", type: "number", step: 0.05, min: 0, max: 1 },
    { key: "wobbleRpm", label: "Пульс.", type: "number", step: 10, min: 0 },
    { key: "noiseRpm", label: "Шум", type: "number", step: 5, min: 0 },
  ],
  tick(state, settings, dtMs) {
    const idle = Number(settings.idle) || 800;
    const max = Math.max(idle, Number(settings.max) || 6500);
    const throttle = Math.max(0, Math.min(1, Number(settings.throttle) || 0));
    const accel = Math.max(0, Number(settings.accelRpmPerSec) || 0);
    const decel = Math.max(0, Number(settings.decelRpmPerSec) || 0);
    const wobbleRpm = Math.max(0, Number(settings.wobbleRpm ?? 80));
    const noiseRpm = Math.max(0, Number(settings.noiseRpm ?? 15));
    const dt = Math.max(0.001, dtMs / 1000);

    const target = idle + (max - idle) * throttle;
    const cur = state.baseValue ?? idle;
    const nextBase = cur < target
      ? Math.min(target, cur + accel * dt)
      : Math.max(target, cur - decel * dt);

    state.baseValue = nextBase;
    state.phase = (state.phase ?? 0) + dt * (0.6 + throttle * 1.8);

    const wobble = Math.sin(state.phase * Math.PI * 2) * wobbleRpm * (0.25 + throttle * 0.75);
    const noise = (Math.random() - 0.5) * 2 * noiseRpm;
    return Math.max(idle, Math.min(max, nextBase + wobble + noise));
  },
});
