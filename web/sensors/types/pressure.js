import { registerSensorType } from "../registry.js";

registerSensorType({
  id: "pressure",
  name: "Датчик давления",
  unit: "bar",
  defaults: { mean: 2.0, sigma: 0.15, clampMin: 0, clampMax: 5 },
  initialSpread: { mean: 0.2, clampMax: 0.4 },
  settings: [
    { key: "mean", label: "Среднее", type: "number", step: 0.1 },
    { key: "sigma", label: "Сигма", type: "number", step: 0.05, min: 0 },
    { key: "clampMin", label: "Огранич. мин", type: "number", step: 0.1 },
    { key: "clampMax", label: "Огранич. макс", type: "number", step: 0.1 },
  ],
  tick(state, settings) {
    // Случайное блуждание вокруг mean
    const prev = state.value ?? settings.mean;
    const next = prev + (Math.random() - 0.5) * settings.sigma * 0.4 + (settings.mean - prev) * 0.05;
    state.value = Math.max(settings.clampMin, Math.min(settings.clampMax, next));
    return state.value;
  },
});
