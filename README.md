# LinuxCAN — команды

Симулятор CAN-сети с битовым уровнем: своя шина с арбитражем (wired-AND),
CRC-15, bit stuffing, ACK, TEC/REC и переходами active → passive → bus-off.
Каждое ECU — отдельный процесс со своим CAN-контроллером, общаются через
демон-симулятор шины (`linuxcan-bussim`). Доставленные кадры зеркалятся
в `vcan0`, чтобы работали `candump` / `cansend`.

## Сборка

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

`./linuxcan` сам проверяет сборку перед запуском.

## Запуск

```bash
chmod +x linuxcan scripts/setup-vcan.sh

# виртуальная шина (один раз, sudo)
./scripts/setup-vcan.sh

# все ECU из папки ecus/*/ecu.json
./linuxcan

# проверить/остановить запущенную сеть
./linuxcan status
./linuxcan stop
```

Открыть UI в браузере:

- адреса печатаются в терминале после `./linuxcan`
- страница шины: http://127.0.0.1:8750/bus.html
- пример из репозитория:
  - http://127.0.0.1:8765/ecu.html — engine
  - http://127.0.0.1:8766/ecu.html — dashboard
  - http://127.0.0.1:8767/ecu.html — gateway

Один ECU:

```bash
./linuxcan --ecu-id engine --port 8765 --page ecu.html --config-dir ecus/engine --open
```

Electron (нативные окна):

```bash
./linuxcan
cd electron && npm install && npm start
```

Остановка сети: **Ctrl+C** в терминале, где запущен `./linuxcan`,
или `./linuxcan stop` из другого терминала.

## Тесты

Смотреть шину (в отдельном терминале):

```bash
candump vcan0
```

Ручная отправка кадра:

```bash
cansend vcan0 100#DEADBEEF12345678
```

Проверка интерфейса:

```bash
ip link show vcan0
```

Сценарий UI (ECU + датчики, формула, TX на CAN):

1. `./linuxcan` + в другом терминале `candump vcan0`
2. Открыть любой ECU из списка в терминале
3. В каталоге слева выбрать датчик (двойной клик или «Добавить»)
4. Подключённый датчик появляется как карточка, его выход уходит линией на вход ECU (a, b, c…)
5. Сверху ECU видит: «Вычислено» (по формуле) и «На CAN (TX value)»
6. В candump — кадры на CAN ID, указанный в TX блоке ECU

Добавление нового ECU: создать папку `ecus/my_ecu/ecu.json`.
Подробнее — [docs/ECUS.md](docs/ECUS.md)

Добавление нового типа датчика: создать файл в `web/sensors/types/<name>.js`
и добавить один `import` в `web/sensors/index.js`. Подробнее — [docs/SENSORS.md](docs/SENSORS.md)
Подробнее про шину — [docs/REAL_BUS.md](docs/REAL_BUS.md)

Битовый уровень CAN (арбитраж, CRC, bit stuffing, ACK, TEC/REC, bus-off):
[docs/CAN_PROTOCOL.md](docs/CAN_PROTOCOL.md)

Подробнее: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
