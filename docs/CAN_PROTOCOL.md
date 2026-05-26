# CAN протокол в LinuxCAN

Этот документ описывает, что именно из CAN 2.0A/B реализовано в LinuxCAN
и где проходят границы упрощения.

## Архитектура

```
+----------+        +----------+        +----------+
|  ECU #1  |        |  ECU #2  |   ...  |  ECU #N  |
|  proc    |        |  proc    |        |  proc    |
|          |        |          |        |          |
|  CanBus  |        |  CanBus  |        |  CanBus  |
|  client  |        |  client  |        |  client  |
+----+-----+        +----+-----+        +----+-----+
     |  UDS /tmp/linuxcan-bus.sock                |
     +-------+----------+-------------+-----------+
             |          |             |
        +----v----------v-------------v----+
        |        linuxcan-bussim           |
        |   (центральный симулятор шины)   |
        |                                  |
        |  * tick_loop ~100 мкс            |
        |  * wired-AND арбитраж            |
        |  * CRC-15 + bit stuffing         |
        |  * ACK от приёмников             |
        |  * error detection (stuff/CRC/   |
        |    form/ack/bit)                 |
        |  * TEC/REC, active/passive/      |
        |    bus-off                       |
        |  * зеркало в vcan0 для candump   |
        +-----------------+----------------+
                          |
                       vcan0  (candump, cansend)
```

Если демона нет, `CanBus` падает в legacy-режим и пишет кадры прямо в
`vcan0` (как было до v0.3). Это удобно для отладки/CI, но без арбитража.

## Структура кадра

LinuxCAN кодирует и физически прокручивает по проводу полный набор полей
CAN 2.0:

| Поле | Длина (бит) | Заметки |
|------|-------------|---------|
| SOF | 1 | dominant |
| Identifier (SFF) | 11 | MSB первым |
| RTR | 1 | data frame = dominant |
| IDE | 1 | dominant = standard, recessive = extended |
| r0 / r1 | 1 / 1 | reserved (dominant) |
| ID-A + SRR + IDE + ID-B + RTR (EFF) | 11 + 1 + 1 + 18 + 1 | для 29-bit ID |
| DLC | 4 | байтовая длина 0..8 |
| Data | 0..64 | по DLC |
| CRC sequence | 15 | полином 0x4599 |
| CRC delimiter | 1 | recessive |
| ACK slot | 1 | TX гонит recessive, RX — dominant |
| ACK delimiter | 1 | recessive |
| EOF | 7 | recessive |
| IFS | 3 | recessive |

Все поля **реально присутствуют в потоке** (`stuffed` / `trailer` в
`DeliveredFrame`) и показываются в UI шины с подсветкой полей и
stuff-битов.

### Bit stuffing

Применяется к области SOF…конец CRC. После пяти одинаковых битов
вставляется один противоположный. Позиции этих stuff-битов
сохраняются в `stuff_positions` и подсвечиваются на странице шины
жёлтым.

Если в `stuffed`-потоке встречается 6 одинаковых битов подряд, это
**stuff error**: контроллер-приёмник его регистрирует.

### CRC

`crc15()` реализует CAN CRC-15 с полиномом `0x4599`. Считается по
**де-стаффленному** потоку SOF…конец data. Передаётся 15 битами после
data, затем CRC delimiter (recessive).

Приёмник в `Bus::simulate_transmission` повторно делает destuffing,
пересчитывает CRC и сравнивает. Несовпадение → **crc error**.

### Арбитраж

Реализован в `Bus::run_arbitration` как побитное wired-AND по всему
arbitration field. На каждом бите:

```
bus_bit = AND(контендеры → их бит арбитража)
любой, кто отдал 1 (recessive), пока bus = 0 (dominant) — теряет
```

Это даёт корректное поведение:

- Меньший ID побеждает (как в реальном CAN).
- SFF data frame с тем же base-ID побеждает EFF (потому что IDE
  у SFF = dominant, а у EFF = recessive).
- RTR-фрейм проигрывает data-фрейму с тем же ID.

Шаги арбитража и кто на каком бите выбыл — печатаются в UI шины в
блоке «Арбитраж».

### ACK

После CRC delimiter и до ACK delimiter — ACK slot. Передатчик гонит
recessive (1). Любой приёмник в режиме `active`/`passive`, у которого
включён `ack_enabled` (по умолчанию да), накладывает dominant (0). Если
ни один приёмник не подтвердил — у передатчика **ack error**, TEC += 8.

Список `acked_by` отдаётся вместе с кадром.

Важно: **ACK и PHY RX не равны application RX**. CAN-контроллер физически
видит валидный кадр, проверяет CRC и может дать ACK даже тогда, когда
приложение ECU не подписано на этот CAN ID. Поэтому в UI разделены:

- `PHY RX ok/err` — контроллер физически увидел кадр на шине;
- `APP RX` — кадр прошёл application RX-фильтр ECU (`rx.filters[]`).

В `DeliveredFrame` это тоже разнесено: `acked_by` показывает физический
ACK, а `app_receivers` — ECU, у которых кадр реально принят приложением.

## TEC/REC и переходы

Каждое ECU имеет два счётчика:

- **TEC** — Transmit Error Counter
- **REC** — Receive Error Counter

Состояния:

| Состояние | Условие |
|-----------|---------|
| `active`  | TEC < 128 и REC < 128 |
| `passive` | TEC ≥ 128 или REC ≥ 128 |
| `bus-off` | TEC ≥ 256 |

Изменения (приближённо к ISO 11898, без всех 8 ветвей):

- успешная TX: TEC −1
- успешная RX: REC −1 (или 120 при возврате с ≥128)
- ошибка TX (stuff/crc/form/ack/bit): TEC += 8
- ошибка RX: REC += 1

В bus-off контроллер не передаёт. Сброс — из UI кнопкой
**«Сброс bus-off»** (отправляется команда `reset_bus_off` через WS, далее
по UDS в bussim).

## Что моделируется упрощённо

- **Битовое время**. Тик шины ~100 мкс — это «условный» темп. В
  `DeliveredFrame.bus_time_us` хранится накопленное «время», пересчитанное
  как `bit_time_us × число_бит`. Реального real-time bit timing
  (синхронизация, resync, sample point) нет.
- **Error frame** не передаётся отдельным потоком 6 dominant + 8 recessive;
  обнаружение ошибок учитывается в счётчиках и помечается в
  `errors[]`, а кадр считается недоставленным и повторно отправляется
  на следующем тике (как в реальном CAN с автоматическим retransmit).
- **CAN-FD** не поддерживается (только классический CAN 2.0A/B,
  до 8 байт данных).
- **Bus-off recovery**: 128 × 11 recessive bit таймер не считается,
  возврат — по ручной команде из UI.

## Где смотреть в коде

- `src/cansim/frame.{hpp,cpp}` — слой кадра, CRC, stuffing
- `src/cansim/controller.{hpp,cpp}` — TEC/REC/state machine
- `src/cansim/bus.{hpp,cpp}` — арбитраж, ACK, error detection
- `src/cansim/bus_server.{hpp,cpp}` — UDS-сервер демона, тик-цикл,
  зеркало в vcan0
- `src/cansim/bus_client.{hpp,cpp}` — клиент шины внутри `CanBus`
- `src/cansim/main_bussim.cpp` — точка входа `linuxcan-bussim`

## Wire-protocol UDS

JSON, по одной команде на строку (`\n` — разделитель).

### ECU → демон

```json
{"op":"hello","node_id":"engine","ack":true,"listen_only":false}
{"op":"tx","id":256,"ide":false,"rtr":false,"dlc":4,"data":[1,2,3,4]}
{"op":"set_ack","enabled":true}
{"op":"reset_bus_off"}
{"op":"set_bit_error_rate","rate":0.0}
```

### Демон → ECU

```json
{"op":"hello_ok","data":{controller snapshot}}
{"op":"state","data":{controller snapshot}}
{"op":"frame","data":{DeliveredFrame: bits, crc, ack, errors, arbitration, …}}
{"op":"error","message":"…"}
```

`DeliveredFrame` включает поля: `unstuffed`, `stuffed`, `trailer`,
`stuff_positions`, `arbitration_begin/end`, `crc`, `ack`, `acked_by`,
`winner`, `losers`, `errors`, `arbitration` (по шагам), `states`
(снимок TEC/REC всех контроллеров после кадра).
