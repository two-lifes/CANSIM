/**
 * Логика ECU: агрегация входов с датчиков и расчёт значений.
 *
 *   inputs  → aggregate(formula) → computed → transform(scale,offset) → txValue
 */

export const FORMULAS = [
  { id: "avg", label: "Среднее" },
  { id: "sum", label: "Сумма" },
  { id: "min", label: "Минимум" },
  { id: "max", label: "Максимум" },
  { id: "first", label: "Первый вход" },
  { id: "custom", label: "Формула (a,b,c…)" },
];

export function aggregate(values, formulaId, customExpr = "") {
  if (!values.length) return 0;
  switch (formulaId) {
    case "sum":   return values.reduce((a, b) => a + b, 0);
    case "min":   return Math.min(...values);
    case "max":   return Math.max(...values);
    case "first": return values[0];
    case "custom": return evalCustom(values, customExpr);
    case "avg":
    default:      return values.reduce((a, b) => a + b, 0) / values.length;
  }
}

/** Безопасный калькулятор: a..z в качестве имён переменных, math-операции, Math.*. */
function evalCustom(values, expr) {
  if (!expr) return 0;
  const safe = expr.replace(/[^0-9a-zA-Z+\-*/().,%\s_]/g, "");
  const vars = values
    .slice(0, 26)
    .map((v, i) => `var ${String.fromCharCode(97 + i)} = ${Number(v)};`)
    .join("");
  try {
    // eslint-disable-next-line no-new-func
    const fn = new Function(`${vars} return (${safe});`);
    const r = Number(fn());
    return Number.isFinite(r) ? r : 0;
  } catch {
    return 0;
  }
}

export function transformTx(computed, scale = 1, offset = 0) {
  return Number(computed) * Number(scale || 1) + Number(offset || 0);
}
