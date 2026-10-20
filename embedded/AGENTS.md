# AGENTS.md — Desky Embedded Systems

> **Critical constraint for subagents:** You have no access to prior session context. Treat every instruction as self-contained: if background is missing, ask for it — never assume it. Re-read this file before each new task; do not let earlier tasks fade it.

## Monorepo Layout & Scope
- `embedded/` — Two sibling firmwares + shared contracts:
  - `core/` (desky-core, primary brain, ESP32-S3)
  - `head/` (desky-head, face-unit stub, classic ESP32)
  - `shared/` — Cross-firmware shared pure functions, communication schemas, and Synapse envelope definitions.
- Focused work runs inside one sibling (`cd embedded/core && make check` gates that target).
- Holistic gate: `cd embedded && make check` (runs tests across all siblings).

## Start Here (Reading Order for Any Task)
1. `embedded/docs/architecture/v2.md` — Design decisions and system blueprint.
2. This file — Architecture layers, governance rules, and hardware gotchas.
3. `embedded/core/platformio.ini` — Envs and hardware pin definitions (doubles as lockfile).
4. `embedded/core/src/main.cpp` — `setup()` initialization order.
5. The specific module under change.

---

## Architectural Layers & Project Map

Every file in `core` and `head` must live strictly within one of these five layers:

- **Hardware Abstraction Layer (`hal/`)**: Pins, registers, I2C/SPI transactions, raw sensor reads. **Zero business logic. Zero networking.**
- **The Wire & Communication Layer (`transport/`)**: Pure networking, socket management, packet framing, and codecs (`udp_server`, `link_server`, Synapse bus). **Dumb pipes only; no business states.**
- **System Infrastructure (`services/`)**: Low-level system plumbing and cross-cutting infrastructure (`config_store`, `power_manager`, `cli_manager`).
- **Self-Contained Functional Features (`abilities/`)**: End-to-end capabilities like `pairing`, `ota`, or `audio`.
  - *The Feature Rule:* Any complex ability MUST be encapsulated in its own subfolder and split into three distinct files:
    1. `*_codec.h` (Pure serialization/deserialization, 100% native-testable).
    2. `*_manager.h` (State machine and business logic, zero socket dependencies).
    3. `*_task.h` (RTOS task or event binder gluing it to transport).
- **Autonomous Orchestration & Personality (`behavior/`)**: Autonomous intent, multi-ability orchestration, and personality engines (`coordinator`, `mood_manager`).

---

## Agent Governance & Planning Protocol

### 1. Mandatory Planning Check
Before writing or modifying any code, your initial plan MUST explicitly state:
- Which layer the file belongs to.
- If it is an ability, how it splits into codec, manager, and task.

### 2. Escalation & Boundary Rules
- **No Layer Violations:** Sockets/transports must never live inside behavior files; business logic must never call sockets directly (use the Synapse `MessageBus`). 
- **Escalation Trigger:** If a requested task violates these layers (e.g., dumping a new feature into a generic file or mixing wire parsing with state machines), **you MUST halt execution during the planning phase**, raise an explicit warning to the user, and propose:
  1. The correct architectural placement.
  2. A documented, temporary compromise (to be tracked for later refactoring).
  3. A structural change to accommodate the pattern if existing layers don't fit.

---

## Commands & Envs
- Prefer `make` targets over raw `pio` (targets encode verified build steps).
- **Focused Check:** `cd embedded/core && make check`
- **Holistic Gate:** `cd embedded && make check`
- **Core Envs:** `desky` (dev/debug), `desky-release` (field), `native-test` (laptop Unity).
- **Head Envs:** `desky-head` (+ stub `native-test`).

---

## Coding Conventions
- Services and managers are header-only; shared suffix is `*_manager`.
- **HAL naming:** Chip name when bound to specific registers/libraries (`mpu6500_*`, `ssd1306_*`), role name when written against an abstraction (`motor_*`, `camera_*`). A part swap must never force a rename.
- No firmware prefixes on filenames; the project path scopes them (`core|head/src/...`). Shared concepts live once in `shared/`.
- **`setup()` order:** logger → fault → config → banner (nothing that can fail runs before logging exists).
- Shared ESP32 settings live in `[common]` + explicit `extends`; never reintroduce a bare `[env]` (it auto-inherits into every env).
- Strictness is split by design (`-Wall` everywhere, `-Werror` src-only); library warnings are third-party noise—never chase them.
- **Cross-Seam Tests:** Every new cross-seam contract (phone↔robot, core↔head, firmware↔bench-script) ships in the same change with host cross-seam tests pinning both ends: shared-codec golden vectors plus each peer's encode/decode/framing edge in `native-test`. Value vocabularies must live once as shared pure functions both sides call (never mirrored constants), backed by literal-byte tests.

---

## Hardware Notes (READ BEFORE Touching USB)
- The USB serial port (`/dev/cu.usbserial-0001`) opens at **9600 baud**, and **opening it resets the chip**. Scripted captures must use pyserial with explicit `baudrate=115200`, and capture heads hold ~200ms of settling noise plus a fresh boot. If a capture comes back silent, pulse DTR/RTS explicitly and flush the buffer inside a single capture session.
- Arduino pre-inits the task watchdog (`TWDT already initialized` is benign); effective timeout is Arduino's (~10s), not the 5s config.
- A halted task that stays WDT-subscribed becomes a panic-reboot loop — call `esp_task_wdt_delete` before halting.
- V1 silkscreen lies (its "MPU6050" is an MPU6500); verify hardware claims against silicon, and assert chip IDs in HAL `init()`.
- Exact pins in each `platformio.ini` ARE the lockfile (no `@ ^`, no bare `.git` URLs, no `stable/` platform — `check-pins` enforces).

## Process Rules for Hardware Runs
- Flash/capture via subagents: explicit baud, redirect-never-truncate, hash-verified, reset-noise expected. 
- Crash-test patches are temporary local edits — never commit; restore with `git checkout .` + rebuild.
