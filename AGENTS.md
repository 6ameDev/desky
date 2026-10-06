# AGENTS.md — desky v2

> **Critical constraint for subagents:** you have no access to the shared
> task list or prior session context. Treat every instruction as
> self-contained: if background is missing, ask for it — never assume it.
> Re-read this file before each new task; do not let earlier tasks fade it.

## Monorepo layout

- `embedded/` — two sibling firmwares + one gate: `core/` (desky-core,
  primary brain, ESP32-S3) + `head/` (desky-head, face-unit stub) +
  `shared/link/` (talk-wire codec placeholder). Focused work runs inside
  one sibling; `cd embedded && make check` gates the whole robot.
- `client/` — empty placeholder for the future KMP / Compose app.
- `hardware/` — hand-solder carriers + contract. Conditional: only when the
  task touches `hardware/` — read `hardware/INTERFACE.md` §0 first; it is
  normative over anything below.
- `shared/protocol/` — protocol contracts; normative source is
  `embedded/src/middleware/udp_codec.h`.
- `embedded/docs/` + `embedded/scripts/` — intentionally untracked
  (owner-managed); not in git, so their absence from `git status` is normal.

## Start here (reading order for any task)

1. `embedded/docs/architecture/v2.md` — the design decisions; everything else follows it.
2. This file — navigation below, then the short gotcha list at the bottom.
3. `embedded/core/platformio.ini` — envs and pins (it doubles as the lockfile).
4. `embedded/core/src/main.cpp` — `setup()` order is the system boot order.
5. The service under change (`embedded/core/src/` map below).

## Project map

- `embedded/core/src/hal/` — sensor/actuator interfaces (`ISensor`, `IActuator`) + drivers.
- `embedded/core/src/services/` — system services: logger, fault/diag/config/i2c/power managers.
- `embedded/core/src/middleware/` — sensor fusion, UDP codec/server (network-facing logic).
- `embedded/core/src/behavior/` — coordinator (state machine), motion controller.
- `embedded/core/include/mcu/` — board definitions; swap MCUs via `active_mcu.h` only.
- `embedded/core/include/config.h` — tunable constants; never hardcode pins elsewhere.
- `embedded/core/test/` — host Unity tests (Arduino-free headers only).
- `embedded/head/` — face-unit shell (boot banner only until task 2).

## Docs (which answers what)

- `embedded/docs/architecture/v2.md` — decisions.
- `embedded/docs/debugging.md` — serial/crash workflow, baselines, coredump spike note.

## Commands & envs

Prefer `make` targets over raw `pio` — the targets encode the lessons.
Focused: run inside one sibling (`cd embedded/core && make check`).
Holistic gate: `cd embedded && make check` (checks both siblings).
Core envs: `desky` (dev/debug), `desky-release` (field), `native-test`
(laptop Unity); head env: `desky-head` (+ stub `native-test`).

## Conventions (structural, keep them)

- Services are header-only; shared suffix is `*_manager`.
- HAL files carry the chip name when bound to one chip's registers/lib
  (`mpu6500_*`, `ssd1306_*`), the role name when written against an
  abstraction (`motor_*`, `camera_*`) — a part swap must never force a rename.
- Layer placement rule: peripheral lifecycle (pins, clocks, DMA,
  init/teardown) → `hal/` (or a `shared/` bus driver if cross-firmware);
  data transformation + protocols → `middleware/`; policy, state, sequencing
  → `services/`. Consumers reach hardware only through HAL. One port with
  shared clocks gets exactly one owner (`i2s_audio`); per-part format
  knowledge splits into role-named siblings (`speaker`, `microphone`).
- No firmware prefixes on filenames; the project path scopes them
  (`core|head/src/...`) — shared concepts live once in `shared/common/`.
- `setup()` order: logger → fault → config → banner (nothing that can fail
  runs before logging exists).
- Shared ESP32 settings live in `[common]` + explicit `extends`; never
  reintroduce a bare `[env]` (it auto-inherits into every env).
- Strictness is split by design (`-Wall` everywhere, `-Werror` src-only);
  library warnings are third-party noise, never chase them.

## Hardware notes (READ BEFORE touching USB)

- The port (`/dev/cu.usbserial-0001`) opens at **9600 baud** and **opening it
  resets the chip**. Scripted captures must use pyserial with explicit
  `baudrate=115200`, and every capture head holds ~200ms of settling noise
  plus a fresh boot — never conclude "it just booted" from that. If a
  capture comes back silent/stale, pulse DTR/RTS explicitly and flush the
  buffer inside a single capture session — reopen alone doesn't always
  reset the chip.
- Arduino pre-inits the task watchdog (`TWDT already initialized` is
  benign); effective timeout is Arduino's (~10s), not the 5s config.
- A halted task that stays WDT-subscribed becomes a panic-reboot loop
  (bisect-proven) — `esp_task_wdt_delete` before halting is load-bearing.
- V1 silkscreen lies (its "MPU6050" is an MPU6500); verify hardware claims
  against silicon, and assert chip IDs in future HAL `init()`.
- Exact pins in each `platformio.ini` ARE the lockfile (no `@ ^`, no bare `.git`
  URLs, no `stable/` platform — `check-pins` enforces).

## Process rules for hardware runs

- Flash/capture via subagents: explicit baud, redirect-never-truncate,
  hash-verified, reset-noise expected. Crash-test patches are temporary
  local edits — never commit; restore with `git checkout .` + rebuild.
