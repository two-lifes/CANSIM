# Architecture

## One ECU = one process = one CAN controller

There is **no central application broker**. Each configured ECU runs as
its own process and owns one CAN controller. All cross-node traffic goes
through `linuxcan-bussim` — a single bus simulator that models the CAN
wire bit-by-bit (wired-AND arbitration, CRC-15, bit stuffing, ACK,
TEC/REC, active/passive/bus-off). Frames that pass the bus successfully
are mirrored to the kernel `vcan0` device, so external tools
(`candump`, `cansend`) keep working.

```
┌─────────────┐  WS  ┌────────────┐    UDS    ┌──────────────────┐
│  UI engine  │◄────►│  ECU proc  │◄────────►│                   │
└─────────────┘      │  CanBus    │           │                   │
                     └────────────┘           │                   │
┌─────────────┐  WS  ┌────────────┐    UDS    │ linuxcan-bussim   │
│  UI dash    │◄────►│  ECU proc  │◄────────►│  - arbitration    │
└─────────────┘      │  CanBus    │           │  - CRC, stuffing  │
                     └────────────┘           │  - ACK, errors    │
┌─────────────┐  WS  ┌────────────┐    UDS    │  - TEC/REC, off   │
│  UI bus mon │◄────►│  ECU proc  │◄────────►│                   │
└─────────────┘      │  CanBus    │           └────────┬──────────┘
                     └────────────┘                    │ mirror
                                                       ▼
                                                    vcan0
                                            (candump / cansend)
```

If `linuxcan-bussim` isn't running, `CanBus` falls back to raw `vcan0`
(legacy v0.2 behaviour) — useful for CI/headless tests but **without**
realistic arbitration/CRC/ACK.

## Launcher

`./linuxcan` builds the C++ targets, starts `linuxcan-bussim`, then
spawns one ECU process per `ecus/*/ecu.json`. Set
`LINUXCAN_ECUS=/other/path` for a different ECU set.

Important env vars:

- `LINUXCAN_BUS_SOCKET` — UDS path of bus simulator (default
  `/tmp/linuxcan-bus.sock`).
- `LINUXCAN_NO_BUSSIM=1` — force legacy raw-vcan mode in an ECU even
  when bussim is up.
- `LINUXCAN_CAN` — kernel CAN interface used by bussim for mirroring
  (default `vcan0`).

## WebSocket protocol (per ECU)

Client → ECU:

- `register`
- `tx_config` / `rx_config` — CAN ID, encoding, filters, schedule
- `publish` — application payload → CAN TX
- `reset_bus_off` — manually leave bus-off

ECU → client:

- `registered` — full status (ecu_id, can_interface, can_mode,
  controller snapshot, network)
- `controller_state` — TEC/REC/mode/stats of this ECU's controller
- `bus_delivered` — full delivered frame (bit stream, CRC, ACK, errors,
  arbitration steps); only for ECUs marked as bus monitors
- `message` — application-level RX (matched by filter)
- `sent` — application-level TX ack

## CAN encoding

Classic CAN 2.0A/B, up to 8 bytes per frame. Application-level encodings
in `src/encoding.cpp`: `float32`, `json`, `text`. The bit-level path
through bussim is independent of encoding.

## Bit-level details

See [docs/CAN_PROTOCOL.md](CAN_PROTOCOL.md) for the full description of
frame layout, CRC, stuffing, arbitration and error-state model.
