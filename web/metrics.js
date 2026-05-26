export const TX_METRICS = [
  { id: "percent", label: "Проценты", unit: "%", min: 0, max: 100 },
  { id: "temperature_c", label: "Температура", unit: "°C", min: -40, max: 140 },
  { id: "pressure_bar", label: "Давление", unit: "bar", min: 0, max: 10 },
  { id: "rpm", label: "Обороты", unit: "rpm", min: 0, max: 8000 },
  { id: "voltage_v", label: "Напряжение", unit: "V", min: 0, max: 16 },
  { id: "speed_kmh", label: "Скорость", unit: "km/h", min: 0, max: 260 },
  { id: "raw", label: "Raw value", unit: "", min: 0, max: 100 },
];

export function metricById(id) {
  return TX_METRICS.find((m) => m.id === id) || TX_METRICS[0];
}

export function formatMetricValue(value, metricId) {
  const metric = metricById(metricId);
  if (!Number.isFinite(value)) return "—";
  const rounded = Math.abs(value) >= 100 ? value.toFixed(0) : value.toFixed(1);
  return metric.unit ? `${rounded} ${metric.unit}` : rounded;
}

export function normalizeMetricValue(value, metricId) {
  const metric = metricById(metricId);
  const span = metric.max - metric.min || 1;
  const pct = ((value - metric.min) / span) * 100;
  return Math.max(0, Math.min(100, pct));
}

export function ecuPastelColor(ecuId) {
  let hash = 0;
  for (const ch of String(ecuId || "ecu")) {
    hash = (hash * 31 + ch.charCodeAt(0)) >>> 0;
  }
  const hue = hash % 360;
  return `hsl(${hue} 70% 72%)`;
}

export function ecuStrokeColor(ecuId) {
  let hash = 0;
  for (const ch of String(ecuId || "ecu")) {
    hash = (hash * 31 + ch.charCodeAt(0)) >>> 0;
  }
  const hue = hash % 360;
  return `hsl(${hue} 68% 38%)`;
}
