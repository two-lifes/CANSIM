/**
 * Реестр типов датчиков.
 *
 * Тип датчика — это объект:
 *   {
 *     id:       уникальный строковый идентификатор (например, "temperature"),
 *     name:     человекочитаемое имя для UI,
 *     unit:     единица измерения для вывода,
 *     defaults: дефолтные значения настроек,
 *     settings: [{ key, label, type, min?, max?, step? }, ...] — описания полей,
 *     tick(state, settings, dtMs) -> number   — возвращает текущее значение датчика
 *   }
 *
 * Чтобы добавить новый датчик: создай файл в web/sensors/types/<name>.js
 * и импортируй его в web/sensors/index.js (одна строчка). Больше ничего.
 */

const types = new Map();

export function registerSensorType(def) {
  if (!def?.id) throw new Error("Sensor type requires id");
  if (types.has(def.id)) {
    console.warn(`Sensor type "${def.id}" already registered, overriding`);
  }
  types.set(def.id, Object.freeze({
    settings: [],
    defaults: {},
    unit: "",
    ...def,
  }));
}

export function getSensorType(id) {
  return types.get(id) ?? null;
}

export function listSensorTypes() {
  return Array.from(types.values());
}
