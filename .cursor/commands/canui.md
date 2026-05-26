# LinuxCAN — профессиональный код

Проект **LinuxCAN**: сеть **отдельных ECU** (C++ + SocketCAN + Web UI + Electron). **Нет центрального брокера** — только CAN между процессами.

## Архитектура

- **Один процесс = один ECU** = один CAN-сокет на `vcan0`.
- UI подключается по WebSocket **только к своему** ECU (порт = номер ECU).
- Между ECU — **только кадры CAN**, не direct-routing.
- Не возвращать архитектуру с общим `Broker` и transport `direct`/`both`.

## C++17

- RAII, явные ошибки CAN/сокетов, минимальный diff.
- `src/ecu.cpp` — логика узла; `src/can_bus.cpp` — SocketCAN; `src/http_server.cpp` — HTTP/WS для локального UI.

## UI

- ES-модули, `common.js`: WebSocket на `location.port`.
- `register` без hub; профили: `can_id`, `encoding`, schedule.

## CAN

- 8 байт на кадр; кодирование в `encoding.cpp`.
- RX: `from` = hex CAN ID отправителя на шине.

## Сборка

- `cmake --build build`; запуск `./linuxcan` или три процесса с `--ecu-id`.

## Ответ

- Русский, если пользователь на русском; ссылки на код: `startLine:endLine:path`.
