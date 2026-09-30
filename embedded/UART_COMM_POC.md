# UART Transport POC — desky-head ↔ desky-core (branch `poc-comm-link`)

Handoff doc for future agents (and humans). Read this + `AGENTS.md` before touching
anything here. POC scope: barebones bi-directional UART verification ONLY —
peripherals (IMU, ToF, OLED, Wi-Fi) stripped, only the physical UART link + clocks run.

## 1. Where things stand (2026-09-30)

- **Branch:** `poc-comm-link`. Base checkpoint `fcf14a4 "Comms test base"` (tracked);
  SELFTEST settle/warmup/retry fix + baud-table extension on top (see `git log`).
- **Stage 1 (Head standalone): DONE on hardware.** Flash + generator + USB CLI verified
  over MB USB, then over Nano breadboard bridge.
- **Option A (in-memory loopback): DONE.** 45/45 MEM combos clean (modes 0/1/2 ×
  chunks 16–1024 × pace 0/1000/5000) + mode-3 live-JPEG CRC match + arg validation.
- **Option B (physical loopback, Nano bridge + 30cm breadboard jumper): DONE,
  incl. HIGH tier.** `SELFTEST SWEEP FULL`: **135/166 clean**. Envelope locked
  (see §4); 230400 quarantined (see §5). HIGH tier (1M–5M, 42 combos) **all
  clean**, incl. mode-3 JPEG points (2026-09-30 run).
- **Stage 2 (S3 link) / Stage 3 (integration): NOT STARTED.** Needs S3 on USB +
  CAM-TX12→S3-RX18 / CAM-RX13←S3-TX17 / common GND (remove loopback jumper first).

## 2. Architecture (what was built)

- **Frame envelope** (`embedded/shared/link/uart_frame.h`, Arduino-free, **LE** —
  deliberate deviation from `udp_codec.h` BE): `DS` magic + MsgType
  (CMD 0x01 / RESP 0x02 / CHUNK 0x03 / HB 0x04) + flags (LAST_CHUNK, SYNTHETIC) +
  frameId/chunkIdx/payloadLen u16LE + CRC16-CCITT (poly 0x1021, init 0xFFFF) over
  header[0..9] + payload + CRC32-IEEE tail. `kMaxPayload` 1024. Streaming `Decoder`
  (magic-resync, overlong drop, remainder re-feed contract) + `Reassembler`
  (OOO/dup tolerant, STALE contract, 1KB LAST-first stash). No heap/Arduino/String.
- **Runtime config** (`embedded/shared/link/poc_config.h`): `chunk_bytes|chunk`
  16–1024 (clamped), `pace_us|pace` 0–50000 (clamped), `baud` in
  {9600, 57600, 115200, 230400, 460800, 921600, 1000000, 1500000, 2000000,
  3000000, 4000000, 5000000}, `mode` 0–3 (clamped), `fps` 1–30 (clamped).
  Out-of-range numerics ACK the clamped value; unknown keys / non-numerics /
  off-list bauds NACK-rejected.
- **Memory rule (enforced):** all RX/reassembly/TX-staging/verify buffers via
  `heap_caps_malloc(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)` + `esp_ptr_internal`
  asserts; camera fb slices memcpy'd PSRAM→INTERNAL per chunk. 64KB Head verify
  slot mirrors S3 `kSlotCap` so Head loopback predicts S3 1:1.
- **Head firmware** (`embedded/head/src/services/poc_manager.h` + `poc_synth.h`,
  `src/main.cpp` stripped to Serial+Serial2 only): generator task (Core 0, modes
  0 TEXT 512B / 1 RAMP 1024B / 2 SYNTH_JPEG 2048B xorshift + SOI/EOI markers only,
  NOT decodable / 3 HW_CAM lazy-init QVGA JPEG), 1Hz HB when held, CMD→RESP,
  deferred-baud protocol (RESP@old → Head +100ms / S3 +250ms switch → 2×HB at new
  rate, 3s rollback to 115200). Self-test verbs (single image serves MB +
  breadboard stages, USB console fixed 115200 — only Serial2 changes rate):
  `SELFTEST MEM <mode> <chunk> <pace> <nframes>` (zero wire traffic),
  `SELFTEST WIRE <baud> <mode> <chunk> <pace> <nframes>` (needs TX12–RX13 jumper),
  `SELFTEST SWEEP [QUICK|FULL]` (QUICK 24 combos; FULL base matrix + HIGH tier).
  WIRE path hardening (learned the hard way, see §6): settle → flush AFTER
  settling (never before) → throwaway-HB warmup through a junk decoder →
  counted frames, one per-frame retry, `txHold_` + `testActive_`-owned Serial2.
- **S3 firmware** (`embedded/core/src/middleware/poc_link.h`, `core/src/main.cpp`
  stripped): RX-only Serial1 until first valid Head frame (protects CAM GPIO12
  strapping), INTERNAL 64KB slot, counters
  `ok/chunks/herr/perr/drops/ooo/dups/hb/bytes/kbps/kbs`, `STATS/RESET/SET/GET/
  HEAD SET/HEAD GET/HELP`, deferred-baud protocol. **Built + host-tested,
  never flashed.**
- **Sweep tool** (`embedded/scripts/uart_poc.py`, UNTRACKED owner-managed, never
  `git add`): explicit-baud open, DTR/RTS pulse + settle-discard, `capture` + `sweep`.

## 3. Proven working (hardware-measured)

- Head streaming + USB CLI on MB and breadboard; `SET/GET/STATS/START/STOP/HELP`.
- MEM 45/45, zero herr/perr/drops/mismatch; `intfree` bit-stable (225576 w/ 64KB
  slot; 297568 pre-slot; 182788 w/ camera) — no leaks anywhere.
- WIRE loopback over **30cm breadboard dupont**: 57600 / 115200 / 460800 /
  921600 / **1M / 1.5M / 2M / 3M / 4M / 5M** clean across chunks {64,128,512},
  pace {0,1000}, modes 1 (+2 in base tier), incl. live JPEG ~4.7KB/frame mode-3
  points per baud. Exact rxbytes, zero errors, ~200 runs flake-free post-fix.
- Reflash paths proven: MB USB (`pio run -t upload`, hash-verified) AND Nano
  bridge (GPIO0→GND + power-cycle for download mode, flash, unjumper, power-cycle).
- Host gates: head 53/53, core 88/88 Unity; `make check` green (both firmwares).

## 4. Reliable envelope (carries into v2 task 3)

Baud {57600, 115200, 460800, 921600, 1M, 1.5M, **2M**} verified loopback-clean
(see HIGH note below) · chunk 16–1024 unconstrained · pace floor 0 (no minimum) ·
modes 0–3 · 30cm dupont worst case.
**Defaults stay 115200/128/1000** (huge margins); 460800 approved step-up
(mode-3 airtime ~0.5s/frame @115200 → ~4× at 460800); **2M approved ceiling**
for inter-board use (24fps+ QVGA headroom).
HIGH note: 3M/4M/5M also passed loopback, but loopback cancels clock error while
Head↔S3 are separate clock domains — divider accuracy up there is unvalidated
across two chips, so 3M+ stays investigational until Stage 2 retests it live.

## 5. Quarantined: 230400 (deterministic perr island, cause unknown)

All 230400 combos fail identically (`perr` = exactly 2× chunks/frame, `herr=0`,
exact byte counts, every mode/chunk/pace, standalone-reproducible). Ruled out:
harness bug, driver rot (fresh-boot fails), streaming interleave (fails streaming
provably idle), data-dependence (TEXT fails too), timing (pace-5000 fails too),
signal integrity (middle-baud island while 921600 shines over the same wires),
clock accuracy (same-peripheral loopback cancels it). Suspect: ESP32 UART
data-path quirk at that divider value. **Do not use 230400.** Stage 2 localizes
it free: S3-decodes-230400-clean ⇒ Head-RX-path quirk (harmless for the robot);
S3-chokes-too ⇒ Head-TX-path quirk.

## 6. Mechanism lessons (don't regress these)

- Opening USB resets the CAM; every session starts from defaults (expected).
- `Serial2.end()/begin()` can spit a framing glitch that eats frame 0's magic via
  the decoder's *silent* resync (no counter fires) → whole test stalls. Fix lives
  in `runSelfWireRes_`: settle → flush → warmup → retry. Any future rework of the
  WIRE path must keep all three.
- S3-side note: the deferred-baud switch crosses the same settle window; the
  protocol's HB handshake + 3s rollback absorbs it (HBs are cheap and retried).
- `pio device monitor` doesn't work in headless shells (no TTY) — use pyserial
  with explicit baud + DTR/RTS handling per `AGENTS.md`.

## 7. Bandwidth reference (measured + computed)

Usable ≈ baud × 0.0889 (8N1 + 16B framing @128B chunks). QVGA JPEG q12 ≈ 4.7KB
typical (4–12KB scene-dependent; matches observed ~2fps mode-3 @115200).
921600 ≈ 82KB/s ≈ 8–16fps QVGA. 24fps QVGA @8KB ⇒ ~2.2Mbaud; 30fps ⇒ ~2.7Mbaud.
VGA @24–30fps ⇒ 6.5–10Mbaud ⇒ infeasible on UART (TRM max 5M, practical ≤2M);
needs SPI or scope cut (QVGA / lower quality / lower fps). Robot need per v2 arch
is face-unit stills ≤10fps ⇒ 921600 sufficient with margin; 2M is headroom.

## 8. Open work

- **HIGH baud tier (1M/1.5M/2M/3M/4M/5M): SWEPT 2026-09-30, all 42 combos clean**
  (36 mode-1 + 6 mode-3 over 30cm jumpers). 2M approved for inter-board use;
  3M+ loopback-clean but held investigational (separate clock domains untested).
  VGA-rate video still needs SPI regardless (see §7).
- **Stage 2:** flash S3, wire link (remove loopback jumper), verify `STATS`,
  `HEAD SET` bridge, deferred-baud 115200→460800, and 230400 Head→S3 (localizes §5).
- **Stage 3:** `uart_poc.py sweep` via S3 CLI, lock final defaults for v2 task 3.
- **Power:** Nano-5V breadboard feed OK for tests; watch brownouts with camera+radio.

## 9. Recovery / pointers

- V2 shells: `git checkout v2 -- embedded/head/src/main.cpp embedded/core/src/main.cpp`.
- Self-test entry: `SELFTEST SWEEP QUICK` (24 combos) then `FULL` (124 + HIGH tier).
- No-jumper WIRE shows `timeouts=1 rxbytes=0` (honest); post-test streaming/baud
  self-restore.
- Breadboard console: Nano RST→GND bridge, D0→CAM-U0R, D1←CAM-U0T, common GND,
  115200. Download mode: GPIO0→GND + power-cycle, flash, unjumper, power-cycle.
