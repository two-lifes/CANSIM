import { registerSensorType } from "../registry.js";

registerSensorType({
  id: "constant",
  name: "Константа",
  unit: "",
  defaults: { value: 1 },
  settings: [
    { key: "value", label: "Значение", type: "number", step: 0.1 },
  ],
  tick(_state, settings) {
    return Number(settings.value);
  },
});
