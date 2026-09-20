# AGENTS.md — desky v2

> **Critical constraint for subagents:** you have no access to the shared
> task list or prior session context. Treat every instruction as
> self-contained: if background is missing, ask for it — never assume it.
> Re-read this file before each new task; do not let earlier tasks fade it.

## Start here (reading order for any task)

1. `docs/architecture/v2.md` — the design decisions; everything else follows it.
2. This file — navigation below, then the short gotcha list at the bottom.
3. `platformio.ini` — envs and pins (it doubles as the lockfile).
4. `src/main.cpp` — `setup()` order is the system boot order.
5. The service under change (`src/` map below).

## Project map

- `src/hal/` — sensor/actuator interfaces (`ISensor`, `IActuator`) + drivers.
- `src/services/` — system services: logger, fault/diag/config/i2c/power managers.
- `src/middleware/` — sensor fusion, UDP codec/server (network-facing logic).
- `src/behavior/` — coordinator (state machine), motion controller.
- `include/mcu/` — board definitions; swap MCUs via `active_mcu.h` only.
- `include/config.h` — tunable constants; never hardcode pins elsewhere.
- `test/` — host Unity tests (Arduino-free headers only).

## Docs (which answers what)

- `docs/architecture/v2.md` — decisions. Read this, not the proposals.
- `docs/architecture/01|02|03.md` — superseded proposals; history only.
- `docs/debugging.md` — serial/crash workflow, baselines, coredump spike note.

## Commands & envs

Prefer `make` targets over raw `pio` — the targets encode the lessons.
`check` (format + pins + both firmware builds + host tests) is the gate;
`test`, `upload-monitor(*)`, `monitor-decode` for the rest.
Envs: `desky` (dev/debug artifact), `desky-release` (field artifact),
`native-test` (laptop Unity runs).

## Conventions (structural, keep them)

- Services are header-only; shared suffix is `*_manager`.
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
- Exact pins in `platformio.ini` ARE the lockfile (no `@ ^`, no bare `.git`
  URLs, no `stable/` platform — `check-pins` enforces).

## Process rules for hardware runs

- Flash/capture via subagents: explicit baud, redirect-never-truncate,
  hash-verified, reset-noise expected. Crash-test patches are temporary
  local edits — never commit; restore with `git checkout .` + rebuild.
