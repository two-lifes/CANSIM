# ECU folders

Каждый блок ECU живёт в отдельной папке:

```text
ecus/
  engine/
    ecu.json
  dashboard/
    ecu.json
  gateway/
    ecu.json
```

`./linuxcan` ищет все файлы `ecus/*/ecu.json` и поднимает по одному процессу на каждый конфиг.

## Минимальный конфиг

```json
{
  "id": "my_ecu",
  "port": 8770,
  "page": "ecu.html",
  "can": "vcan0",
  "tx": { "can_id": "0x400", "encoding": "float32" },
  "rx": {
    "filters": [
      { "can_id": "0x100", "encoding": "float32", "label": "engine_value" }
    ]
  }
}
```

## Добавить новый ECU

```bash
mkdir -p ecus/my_ecu
cp ecus/engine/ecu.json ecus/my_ecu/ecu.json
```

Поменяй в `ecu.json`:

- `id` — имя блока
- `port` — свободный порт
- `tx.can_id` — ID, на который ECU отправляет
- `rx.filters[]` — ID, которые ECU слушает

После этого:

```bash
./linuxcan
```

Новый ECU появится в выводе терминала.

## Запустить сеть из другой папки

```bash
LINUXCAN_ECUS=/path/to/my/ecus ./linuxcan
```

## Запустить один ECU

```bash
./linuxcan --ecu-id my_ecu --port 8770 --page ecu.html --config-dir ecus/my_ecu --open
```

## Правило портов

Порт должен быть уникальный для каждого ECU. Если два ECU используют один порт, второй не стартует.
