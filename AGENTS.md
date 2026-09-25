# AGENTS.md — desky v2

> **Critical constraint for subagents:** you have no access to the shared
> task list or prior session context. Treat every instruction as
> self-contained: if background is missing, ask for it — never assume it.
> Re-read this file before each new task; do not let earlier tasks fade it.

## Monorepo layout

- `embedded/` — C++ PlatformIO microcontroller firmware (all builds run
  here, e.g. `cd embedded && pio run` / `cd embedded && make check`).
- `client/` — empty placeholder for the future KMP / Compose app.
- `shared/protocol/` — protocol contracts; normative source is
  `embedded/src/middleware/udp_codec.h`.
- `embedded/docs/` + `embedded/scripts/` — intentionally untracked
  (owner-managed); not in git, so their absence from `git status` is normal.

## Start here (reading order for any task)

1. `embedded/docs/architecture/v2.md` — the design decisions; everything else follows it.
2. This file — navigation below, then the short gotcha list at the bottom.
3. `embedded/platformio.ini` — envs and pins (it doubles as the lockfile).
4. `embedded/src/main.cpp` — `setup()` order is the system boot order.
5. The service under change (`embedded/src/` map below).

## Project map

- `embedded/src/hal/` — sensor/actuator interfaces (`ISensor`, `IActuator`) + drivers.
- `embedded/src/services/` — system services: logger, fault/diag/config/i2c/power managers.
- `embedded/src/middleware/` — sensor fusion, UDP codec/server (network-facing logic).
- `embedded/src/behavior/` — coordinator (state machine), motion controller.
- `embedded/include/mcu/` — board definitions; swap MCUs via `active_mcu.h` only.
- `embedded/include/config.h` — tunable constants; never hardcode pins elsewhere.
- `embedded/test/` — host Unity tests (Arduino-free headers only).

## Docs (which answers what)

- `embedded/docs/architecture/v2.md` — decisions.
- `embedded/docs/debugging.md` — serial/crash workflow, baselines, coredump spike note.

## Commands & envs

Prefer `make` targets over raw `pio` — the targets encode the lessons.
Run them from `embedded/` (e.g. `cd embedded && make check`).
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
- Exact pins in `embedded/platformio.ini` ARE the lockfile (no `@ ^`, no bare `.git`
  URLs, no `stable/` platform — `check-pins` enforces).

## Process rules for hardware runs

- Flash/capture via subagents: explicit baud, redirect-never-truncate,
  hash-verified, reset-noise expected. Crash-test patches are temporary
  local edits — never commit; restore with `git checkout .` + rebuild.
