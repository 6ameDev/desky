# desky shared protocol

Placeholder for machine-readable packet schemas / byte maps shared by the
embedded firmware (`embedded/`) and the future cross-platform client
(`client/`).

Normative source of truth today: `embedded/src/middleware/udp_codec.h`.
Python peers: `scripts/udp_drive.py`, `scripts/udp_telemetry.py`.

## Conventions

- Multi-byte fields are big-endian.
- Each packet ends with a 1-byte XOR checksum over all preceding bytes.
- No heap, no Arduino headers in the codec — pure byte math so host
  Unity tests and future Kotlin code can mirror it exactly.

## Control packet: App -> Robot, 6 bytes @30-50Hz

```
[0xAA | mode | throttle | steering | flags | XOR-checksum]
```

| Byte | Field    | Meaning                                              |
| ---- | -------- | ---------------------------------------------------- |
| 0    | header   | `0xAA` (`kControlHeader`)                            |
| 1    | mode     | mode byte                                            |
| 2    | throttle | center-128 byte: 128 = stop, 255 = full +, 0 = full - |
| 3    | steering | center-128 byte: 128 = centered, + = right           |
| 4    | flags    | flags byte                                           |
| 5    | checksum | XOR of bytes 0..4                                    |

Decode rejects wrong header, wrong length, or bad checksum.

## Telemetry packet: Robot -> App, 9 bytes @10-20Hz

```
[0xBB | pitchBE i16 | rollBE i16 | distBE u16 | statusFlags | XOR-checksum]
```

| Bytes | Field       | Meaning                                    |
| ----- | ----------- | ------------------------------------------ |
| 0     | header      | `0xBB` (`kTelemetryHeader`)                 |
| 1..2  | pitch       | int16 big-endian, decidegrees (deg = raw/10) |
| 3..4  | roll        | int16 big-endian, decidegrees               |
| 5..6  | distanceMm  | uint16 big-endian, millimetres              |
| 7     | statusFlags | bit0 = cliff, bit1 = driving, bits2-3 = mode (0 MANUAL, 1 AUTONOMOUS, 2 LOW_POWER, 3 EMERGENCY), bits4-7 reserved 0 |
| 8     | checksum    | XOR of bytes 0..7                           |

Decode rejects wrong header, wrong length, or bad checksum.

## Transport

- UDP port `3333` (see scripts for the registration-hello / unicast convention).
- Robot failsafes to stop ~500ms after the last control datagram.
