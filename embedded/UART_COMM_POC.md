# UART Transport POC — desky-head ↔ desky-core (branch `poc-comm-link`)

Normative record for v2 task 3 carry-over. History is condensed to a PASS
table — the code + host gates (head 58/58, core 99/99) + hardware transcripts
prove the claims. POC scope: barebones bi-directional UART verification ONLY.

## 0. Carry-over to v2 task 3 (READ FIRST)

- Port codec + config + deferred-baud + retry discipline.
- Defaults 115200/128/1000; 460800 approved step-up; 2M ceiling.
- **230400 BANNED** (`isValidBaud` NACKs it; sweep matrices exclude it; §5 —
  do not use, do not investigate).
- Wiring: loopback jumpers OFF both boards, CAM-TX12→S3-RX18 /
  CAM-RX13←S3-TX17 / common GND. NOTHING else may drive either console.
- Control/stream: separate channels or keep CMD retry. Never rewire mid-suite.

Stage history (condensed — all green):

| Stage | Result |
|---|---|
| 1 Head standalone (generator + USB CLI) | PASS |
| Option A in-memory loopback (MEM + mode-3 CRC) | PASS |
| Option B physical loopback (envelope lock, HIGH tier) | PASS |
| Stage-1 final suite (CAM regression) | PASS |
| 2 S3 loopback isolation (Path A) | PASS |
| 3 Integration (baseline, bridge ACK, deferred-baud, 2M cross-clock) | PASS |

## 1. Architecture

- **Frame envelope** (`embedded/shared/link/uart_frame.h`, Arduino-free, **LE** —
  deliberate deviation from `udp_codec.h` BE): `DS` magic + MsgType
  (CMD 0x01 / RESP 0x02 / CHUNK 0x03 / HB 0x04) + flags (LAST_CHUNK, SYNTHETIC) +
  frameId/chunkIdx/payloadLen u16LE + CRC16-CCITT (poly 0x1021, init 0xFFFF) over
  header[0..9] + payload + CRC32-IEEE tail. `kMaxPayload` 1024. Streaming `Decoder`
  (magic-resync, overlong drop, remainder re-feed contract) + `Reassembler`
  (OOO/dup tolerant, STALE contract, 1KB LAST-first stash). No heap/Arduino/String.
- **Runtime config** (`embedded/shared/link/poc_config.h`): `chunk_bytes|chunk`
  16–1024 (clamped), `pace_us|pace` 0–50000 (clamped), `baud` in
  {9600, 57600, 115200, 460800, 921600, 1000000, 1500000, 2000000,
  3000000, 4000000, 5000000} (230400 BANNED, §5), `mode` 0–3 (clamped),
  `fps` 1–30 (clamped), `framesize` qvga|vga|svga|xga|sxga|uxga|qxga
  (Head mode-3 camera re-init, default qvga). Out-of-range numerics ACK the
  clamped value; unknown keys / non-numerics / off-list bauds / unknown
  framesize names NACK-rejected (framesize NACK lists the valid names).
- **Memory rule (enforced):** all RX/reassembly/TX-staging/verify buffers via
  `heap_caps_malloc(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)` + `esp_ptr_internal`
  asserts; camera fb slices memcpy'd PSRAM→INTERNAL per chunk. 64KB Head verify
  slot mirrors S3 `kSlotCap` so Head loopback predicts S3 1:1. `intfree`
  bit-stable everywhere — no leaks.
- **Head sender** (`embedded/head/src/services/poc_manager.h`): generator task
  (Core 0, modes 0 TEXT 512B / 1 RAMP 1024B / 2 SYNTH_JPEG 2048B
  xorshift + SOI/EOI markers only, NOT decodable / 3 HW_CAM lazy-init QVGA JPEG,
  double-buffered `fb_count=2`, PSRAM→INTERNAL per-chunk staging), 1Hz HB when
  held, CMD→RESP, deferred-baud (RESP@old → Head +100ms / S3 +250ms switch →
  2×HB at new rate, 3s rollback to 115200). Self-test verbs (USB CLI, USB fixed
  115200 — only Serial2 changes rate): `SELFTEST MEM / WIRE / SWEEP`.
- **S3 receiver** (`embedded/core/src/middleware/poc_link.h`): RX-only Serial1
  (4KB ring) until first valid Head frame (protects CAM GPIO12 strapping),
  INTERNAL 64KB slot, stale-partial flush (100ms idle → drop), counters
  `ok/chunks/herr/perr/drops/big/ooo/dups/ovf/hb/bytes/kbps/kbs` (`big` = frames
  over the 64KB slot, subset of `drops`; `ovf` = RX overflow-pressure events),
  `STATS/RESET/SET/GET/HEAD SET/HEAD GET/HELP`, deferred-baud protocol,
  `SELFTEST WIRE/SWEEP` verbs (TX17–RX18 jumper, CAM off).

## 2. Reliable envelope (carries into v2 task 3)

Baud {57600, 115200, 460800, 921600, 1M, 1.5M, **2M**} loopback-clean **and
460800 / 2M live Head→S3 with bridge ACK** · chunk 16–1024 unconstrained ·
pace floor 0 · modes 0–3 · 30cm dupont worst case.
**Defaults stay 115200/128/1000** (huge margins); 460800 approved step-up;
**2M approved ceiling** for inter-board use (24fps+ QVGA headroom).
HIGH note: 3M/4M/5M loopback-clean, but loopback cancels clock error while
Head↔S3 are separate clock domains — 3M+ stays investigational.

## 3. BANNED: 230400 — DO NOT USE, DO NOT INVESTIGATE

Historical cause (kept so nobody re-pays the debugging hours): deterministic
`perr` island on Head loopback (`perr` = exactly 2× chunks/frame); S3 loopback
was fully clean ⇒ island lived in the Head's loopback-RX path
(classic-ESP32 UART silicon/driver at that divider); shared codec exonerated.

Verdict: **230400 is BANNED, full stop.** `isValidBaud()` rejects it
(`NACK bad_baud`), both sweep matrices exclude it. Future agents: **do not
sweep it, do not localize it, do not re-litigate it.** 460800 is the approved
step-up; every 230400 probe is wasted time.

## 4. Mechanism lessons (don't regress these)

- Opening USB resets the CAM; every session starts from defaults (expected).
- WIRE path: settle → flush AFTER settling (never before) → throwaway-HB
  warmup through a junk decoder → counted frames, one per-frame retry. The
  `Serial2.end()/begin()` framing glitch eats frame 0's magic via silent
  resync — all three hardenings must stay.
- **Head TX funnel:** ALL Serial2 TX through `txWriteRaw_()` under a FreeRTOS
  mutex (generator task vs loop task). S3 is single-threaded — no equivalent
  hazard.
- **Greedy-decoder frankenframes (fixed by retry):** a RESP arriving atop a
  partial streaming chunk is consumed as payload (chunk header CRC stays valid)
  → RESP destroyed with exactly one `perr` (~25% first-try loss streaming-on,
  0% stopped). Fixed at transport: S3 `sendCmdAndWait_()` retries idempotent
  CMDs up to 3× (≈70% → ≈99.7%). v2 task 3: separate control/stream channels
  or keep retry.
- **Baud-switch wedge (fixed):** `updateBaudRate()` mid-stream intermittently
  wedged UART TX silent. Fix: all switch paths (Head deferred/local/rollback,
  S3 protocol) drain-flush + `end()/begin()` + decoder reset. Never reintroduce
  `updateBaudRate` mid-stream.
- **S3 task-watchdog guard:** `pollLink()` drain bounded to 64 passes + WDT
  feeds in drain and CMD-wait loops — no UART pathology can panic the task.
- **Head TX degradation with uptime:** after long continuous paced streaming,
  Head TX can develop deterministic chunk-0 loss (S3 `drops` climbing with
  `herr=perr=0` is the signature; power-cycle cures). Mitigations live in v2
  task 3 (scheduled TX re-init, health self-check, or driver root-cause).
- **Switch discipline (mandatory):** (1) verify link flowing + TX attached
  BEFORE switching — never switch blind; (2) print full switch transcripts;
  (3) verify BOTH bauds explicitly after; (4) single process holding the port
  (S3 reboots on USB-open — always park via bridge `HEAD SET baud 115200` at
  session start).
- Nano-bridge flashing/console is SUSPECT (owner CAM flash failed via Nano;
  Nano↔CAM-UART0 path later went fully silent) — MB USB is normative until
  re-verified. Nano-5V rail as reboot suspect is unconfirmed (single occurrence).

## 5. Bandwidth reference (verdicts)

Usable ≈ baud × 0.0889 (8N1 + 16B framing @128B chunks). QVGA JPEG q12 ≈ 3–5KB.
Sparse-clean ≠ saturated-clean: baud-limited clean ≤921600 (best sustained
~20fps); BER storm ≥1M under saturation on 30cm dupont (`perr` + `drops`,
`herr≈0`; `ovf` pressure events only @1M). **Max stable: ~20fps @921600**;
**460800–921600 is the sweet band.** VGA @24–30fps ⇒ 6.5–10Mbaud ⇒ infeasible
on UART (TRM max 5M, practical ≤2M); needs SPI or scope cut. Robot need
(face-unit stills ≤10fps) ⇒ 921600 sufficient with margin; 2M is headroom.

## 6. Open work

- **Resolution switching** (S3-side verified live 2026-10-01: `HEAD SET
  framesize vga` → ACK, 10s streaming `big=0`; Head-side knob path — camera
  re-init, fallback, SKIP lines — pending owner Head flash via MB USB): Head
  `SET framesize <qvga|…|qxga>` re-inits the camera at runtime (`fb_count=2` +
  `jpeg_quality 12` kept; failure keeps old size + NACKs, never bricks). Bridges over
  `HEAD SET`. 64KB-slot rule: oversize frames are explicit `OVERSIZE` drops
  (S3 `big ⊆ drops`; Head `SKIP_BIGFRAME`, counted as neither ok nor bad).
  Measured: QVGA fits, VGA fits (`big=0`); SVGA/XGA still to settle via the
  `big` counter; SXGA+ expected `big` drops. Head reflash (MB USB) required
  once for the knob image; a reboot alone does not add it.
- * DEFERRED — Head-TX soak bracketing (onset bounds + re-init/health-check
  root-cause). Parked; do not raise until owner asks.
- * DEFERRED — 10cm vs 30cm wire A/B at saturated 1M/2M. Parked; do not raise
  until owner asks.

## 7. Recovery / pointers

- V2 shells: `git checkout v2 -- embedded/head/src/main.cpp embedded/core/src/main.cpp`.
- Self-test entry: `SELFTEST SWEEP QUICK` (18 combos) then `FULL` (93 base + HIGH tier).
- V2 consoles must document the S3-reboot-on-open trap and the bridge-park rule.
