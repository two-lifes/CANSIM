import { registerSensorType } from "../registry.js";

registerSensorType({
  id: "temperature",
  name: "Датчик температуры",
  unit: "°C",
  defaults: { min: 20, max: 80, periodMs: 8000, noise: 0.5 },
  initialSpread: { min: 3, max: 4 },
  settings: [
    { key: "min", label: "Мин", type: "number", step: 1 },
    { key: "max", label: "Макс", type: "number", step: 1 },
    { key: "periodMs", label: "Период (мс)", type: "number", min: 200, step: 100 },
    { key: "noise", label: "Шум (±)", type: "number", step: 0.1 },
  ],
  tick(_state, settings) {
    const { min, max, periodMs, noise } = settings;
    const t = Date.now() / periodMs;
    const base = (min + max) / 2 + ((max - min) / 2) * Math.sin(t * Math.PI * 2);
    return base + (Math.random() - 0.5) * 2 * noise;
  },
});
