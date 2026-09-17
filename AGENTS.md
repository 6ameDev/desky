# AGENTS.md — Desky (ESP32 robot, Arduino + PlatformIO)

Read this before changing anything. It records durable project knowledge:
directions and pointers, not specifics — pin numbers, thresholds, intervals,
and versions live in `include/Config.h` and `platformio.ini`; read them there.

## Build / flash / monitor

- Targets live in `Makefile` (`build`, `upload-monitor`). Source layout:
  `src/main.cpp`, `src/core/` (shared state), `src/drivers/` (hardware),
  `src/web/` (server + embedded page).
- If the serial port is busy, a stale monitor process is holding it —
  find it via the OS port listing and kill it before retrying.
- A flash that dies partway ("chip stopped responding") is a cable/power
  problem, never a code problem. Retry first, then change cable/port and
  isolate the board's power. The board is not bricked; reflashing recovers it.

## Runtime architecture

- 100 Hz hardware task on Core 1 (motors + sensors) and network upkeep on
  Core 0; see `HardwareTask` in `src/main.cpp`.
- `RobotStateStore` (`src/core/`) is the only cross-core bridge. New shared
  state goes through it (mutex + getter/setter), never globals. Async
  UI-to-firmware requests use the take/request flag idiom there.
- Never block the 100 Hz loop: bound every I2C transaction with a timeout
  and poll slow sensors on decimated ticks, never every tick.
- `HardwareTask` (Core 1, 10 ms) runs motors + I2C sensors sequentially. I2C
  components (`ToF 0x29`/`MPU 0x68/69`/`OLED 0x3C/3D` on `Wire 21/22`) may wait
  on the shared `i2cMutex` (5 ms acquire, ~23 ms OLED hold). Non-I2C actors
  (motors on `25/26/18/19` PWM) do not use the bridge — keep sequential for
  now; if jitter matters, split motor control to never wait on the I2C bridge.

## Web UI lives in a C++ string

- The whole page is a raw string in `src/web/`; the compiler cannot check
  its JS/CSS. Before every build, verify the `<script>` block has balanced
  braces/parens — an imbalance kills the entire page silently at runtime.
- After uploading, hard-refresh the browser. No cache headers are served,
  so a normal refresh may test stale code.

## I2C — shared Wire 21/22, now stable at 400 kHz

- Wiring is now stable at `400 kHz` (`Wire` on `21/22` shared by ToF `0x29`, MPU `0x68/0x69`, OLED `0x3C/0x3D`). Revisit only if harness changes.
- Keep proven patterns (reuse, don't re-derive): validity-gate every reading, hold-last-good, distinct fault status with fail-safe posture, manual recovery triggers; automatic recovery only for proven scenarios. On loose wiring expect `259`/`INVALID_STATE` storms.
- Cautionary precedent: a sensor-error sentinel was once consumed as a real distance and caused a permanent false cliff block. Return codes alone are not validation — plausibility-check sensor data.

## Verified hardware truths (do not re-derive)

- The IMU breakout is an MPU6500 (`WHO_AM_I 0x70`) despite its silkscreen;
  the driver must accept that ID.
- The sensor-to-car axis mapping was derived empirically from capture data,
  not from the datasheet or board printing. If mounting changes, re-derive
  it with a tilt test; the orientation-cycle setting exists for yaw fixes.
- AD0 floats: keep dual-address probing. Boot I2C scan and `WHO_AM_I`
  diagnostics exist for exactly these ambiguities — extend them, don't bypass.

## Telemetry and protocol

- Binary command IDs and JSON telemetry/`config` shapes are defined in
  `src/web/` — extend the payload, don't redesign the cadence. Push-gating
  (slow base + idle backoff + instant event pushes) exists to spare client
  batteries; keep it.
- The server pushes `config` on every WebSocket connect; clients self-sync.

## Eyes / display

- OLED `128×64 SSD1306` on shared `Wire` (`0x3C`/`0x3D`), rendered by a low-prio Core 0 `DisplayTask` with an `I2C` mutex.
- [FluxGarage/RoboEyes](https://github.com/FluxGarage/RoboEyes) (GPL-3.0, GFX) — directly vendored smooth tween `cur=(cur+next)/2`, autoblinker/idle drift and eyelid moods.
- [playfultechnology/esp32-eyes](https://github.com/playfultechnology/esp32-eyes) (GPL-3.0, `u8g2`) — inspiration/ideas from 18-emotion presets (slopes/radii), not vendored.

## Constraints

- Flash sits near 80%: check the size line after any dependency change.
  Headroom comes from the partition layout, not code dieting.
- `Preferences`/NVS must initialize in `setup()`, never in a global
  constructor (NVS isn't up yet — boot-error precedent).
- Git-URL dependencies must be tag-pinned; registry deps use semver.
- New tunables go in `include/Config.h`. No magic numbers elsewhere.
