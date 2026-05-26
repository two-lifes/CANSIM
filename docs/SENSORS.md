# Датчики (sensors)

Датчик — самостоятельная сущность во фронтенде. Он генерирует значение, ничего не знает про CAN, подключается к ECU как «вход».

```
[датчик] → rawValue → [modifier] → inputValue → [ECU вход] → формула → computed → scale/offset → TX value → CAN
```

## Структура

```
web/sensors/
  registry.js        — registerSensorType / getSensorType / listSensorTypes
  index.js           — точка сборки: один импорт на каждый тип
  types/
    temperature.js
    pressure.js
    speed.js
    digital.js
    constant.js
```

## Как добавить новый датчик

1. Создай `web/sensors/types/myThing.js`:

```js
import { registerSensorType } from "../registry.js";

registerSensorType({
  id: "myThing",
  name: "Мой датчик",
  unit: "X",
  defaults: { gain: 1.0 },
  initialSpread: { gain: 0.1 },
  settings: [
    { key: "gain", label: "Усиление", type: "number", step: 0.1 },
  ],
  tick(state, settings, dtMs) {
    // state — твой внутренний state, сохраняется между тиками
    // settings — текущие значения настроек из UI
    // dtMs — время с прошлого тика
    return Math.random() * settings.gain;
  },
});
```

2. Добавь импорт в `web/sensors/index.js`:

```js
import "./types/myThing.js";
```

Всё — датчик появится в каталоге слева, его можно подключать, настраивать, удалять.

## Удалить датчик из проекта

Удали файл `types/<name>.js` и убери один `import` в `index.js`.

## API типа датчика

| Поле | Назначение |
|------|-----------|
| `id` | Уникальный идентификатор (строка) |
| `name` | Имя для UI |
| `unit` | Единица измерения (показывается рядом со значением) |
| `defaults` | Стартовые значения настроек |
| `initialSpread` | Необязательный разброс настроек при ручном добавлении датчика |
| `settings[]` | Описания полей: `{ key, label, type, min?, max?, step?, options? }` |
| `tick(state, settings, dtMs)` | Возвращает текущее значение |

`initialSpread` применяется только для новых датчиков. При восстановлении из сохранения параметры не рандомизируются повторно.

## Модификаторы сигнала (заложено под будущее)

`SensorInstance._applyModifier(value)` сейчас тождественный. Дальше можно подключить:
- помехи (`value + noise`)
- обрыв (`null` / последнее значение)
- залипание (фиксированное значение)
- линейный/нелинейный пересчёт

`rawValue` и `inputValue` намеренно разнесены, чтобы UI показывал и то, что выдал датчик, и то, что пришло на вход ECU после модификации.

## Сохранение настроек

Для каждого ECU состояние UI сохраняется в `localStorage` по `ecu_id`.

Сохраняется:
- подключённые датчики;
- имя датчика;
- настройки датчика;
- период обновления датчика;
- формула ECU;
- `scale`, `offset`;
- TX CAN ID;
- режим и период отправки.
- RX-фильтры ECU (`can_id`, `label`, `encoding`).

Не сохраняется:
- CAN traffic log;
- RX-журнал последних кадров;
- текущие live-значения датчиков.
