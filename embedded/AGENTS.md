# AGENTS.md — Desky Embedded Systems

> **Critical constraint for subagents:** You have no access to prior session context. Treat every instruction as self-contained. Re-read this file before each task.

## Monorepo Layout
- `embedded/core/` — Primary brain (ESP32-S3)
- `embedded/head/` — Face-unit stub (ESP32-CAM)
- `embedded/shared/` — Cross-firmware shared pure functions and communication schemas.

## Core Rules & Architecture Layers
Every file in `core` and `head` must live strictly within one of these five layers:
1. `hal/` — Pins, registers, raw drivers. **Zero business logic or networking.**
2. `transport/` — Sockets, packet framing, and communication channels. **Dumb pipes only.**
3. `services/` — System plumbing (`config_store`, `power_manager`, `cli_manager`).
4. `abilities/` — Self-functional features. *The Feature Rule:* Must split complex abilities into `*_codec.h` (pure parsing), `*_manager.h` (state machine), and `*_task.h` (RTOS glue).
5. `behavior/` — Autonomous orchestration and personality engines.

## Agent Governance & Planning Protocol
- **Planning Check:** Your initial plan must state the target layer and (if an ability) its 3-part split.
- **Escalation Trigger:** If a task violates layer boundaries, **halt execution**, raise a warning, and propose the correct placement or a documented compromise.

## Architecture Guardrails (Self-Correcting Loop)
- Layer edges are enforced by `embedded/tools/arch_guardrails.yaml` (Phase 1, holds the fort) and `embedded/tools/arch_ideal.yaml` (Phase 2, ideal direction), both run in `make check`. Any change adding a layer edge must either satisfy the rulesets or adapt them in the same change with a one-line justification. Rule changes review like code.
- Any future compromise needs an allowlist entry naming the slice that will remove it. Every milestone reruns audit mode and reviews warnings for promotion to errors.

## Coding Conventions & Code Hygiene
- **No History Lessons:** Never comment on what code *used* to do or what spikes were tried.
- **No Task Breadcrumbs:** Never include task numbers or subagent notes in code.
- **Gotchas Only:** Comments are strictly for non-obvious hardware/timing constraints. Refactor unclear code instead of commenting.
- **No Dead Code Hoarding:** Completely delete replaced code; never leave commented-out blocks or unused compatibility wrappers.
- **Cross-Seam Tests:** Contracts must be pinned with host tests in `native-test` using shared pure functions.
- **No Blind Logging:** Sockets must check transmission return values. Never log a packet as "sent" based on code execution alone. Use periodic `LOG_I` accounting counters for field telemetry instead of relying on transient debug logs.

## Commands & Hardware Gotchas
- **Build / Test:** `cd embedded/core && make check` (Focused) | `cd embedded && make check` (Holistic). New machine: install `pipx`, then `cd embedded && make setup` for the pinned Semgrep + clang-format the gates shell out to.
- **Gate scope:** the holistic gate also covers `testbench/` plus the `desky-bench` env; the required builds stay `desky`, `desky-release`, `desky-head`.
- **USB Serial:** Opens at **9600 baud** and **resets the chip**. Scripted captures must use `baudrate=115200`.
- **Watchdog:** Call `esp_task_wdt_delete` before halting a task to prevent panic-reboot loops.
- **Flash Verification:** Proven only by `Hash of data verified` + `Hard resetting`.
