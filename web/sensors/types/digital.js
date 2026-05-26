import { registerSensorType } from "../registry.js";

registerSensorType({
  id: "digital",
  name: "Цифровой вход (0/1)",
  unit: "",
  defaults: { value: 0 },
  settings: [
    { key: "value", label: "Состояние", type: "select", options: [
      { value: 0, label: "0 — выкл" },
      { value: 1, label: "1 — вкл" },
    ] },
  ],
  tick(_state, settings) {
    return Number(settings.value) ? 1 : 0;
  },
});
