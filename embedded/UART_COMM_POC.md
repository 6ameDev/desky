# UART Transport POC — desky-head ↔ desky-core (branch `poc-comm-link`)

Normative record for v2 task 3 carry-over. History is condensed to a PASS
table — the code + host gates (head 97/97, core 142/142) + hardware transcripts
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
  double-buffered `fb_count=2`, stage-and-release: whole-frame PSRAM→INTERNAL
  copy + immediate fb return, pumpStaged_ ships the copy with zero DMA refs),
  1Hz HB when held, CMD→RESP, deferred-baud (RESP@old → Head +100ms / S3
  +250ms switch → 2×HB at new rate, 3s rollback to 115200). Self-test verbs
  (USB CLI, USB fixed 115200 — only Serial2 changes rate): `SELFTEST MEM /
  WIRE / SWEEP / CAMBENCH`. Camera knobs: `exposure` (live AEC+gain12,
  0=auto) + `jpeg_quality` (re-init) + `framesize` (re-init).
- **S3 receiver** (`embedded/core/src/middleware/poc_link.h`): RX-only Serial1
  (16KB ring) until first valid Head frame (protects CAM GPIO12 strapping),
  INTERNAL 64KB slot, stale-partial flush (100ms idle → drop), counters
  `ok/chunks/herr/perr/drops/big/ooo/dups/ovf/hb/bytes/kbps/kbs` (`big` = frames
  over the 64KB slot, subset of `drops`; `ovf` = RX overflow-pressure events),
  `STATS/RESET/SET/GET/HEAD SET/HEAD GET/HEAD STATS/HELP`, deferred-baud
  protocol, `SELFTEST WIRE/SWEEP` verbs (TX17–RX18 jumper, CAM off).

## 2. Reliable envelope (carries into v2 task 3)

Baud {57600, 115200, 460800, 921600, 1M, 1.5M, **2M**} loopback-clean **and
460800 / 2M live Head→S3 with bridge ACK** · chunk 16–1024 unconstrained ·
pace floor 0 · modes 0–3 · 30cm dupont worst case.
**Defaults stay 115200/128/1000** (huge margins); 460800 approved step-up;
**2M approved ceiling** for sparse inter-board use (pre-24FPS-campaign;
saturated-2M beyond-K per §5b — no longer headroom for streaming).
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
   session start); (5) **near-silent switching ONLY** (`HEAD SET mode 0` +
   `HEAD SET fps 1` ≈ 4 chunks/s — chunk-128 continuous streams eat switch
   RESPs; proven 3/3 clean near-silent vs 0/2 saturated); (6) **single
   session from switch-up through switch-back** — never reopen USB while Head
   is off-115200 (reopen reboots S3 to tristated-115200 = split-brain with no
   S3-side recovery; owner power-cycle is the only cure); (7) ACK matcher
   must accept `baud now <N>` / `already at <N> baud` (S3 never sends
   `ACK baud`); (8) hold host baudrate constant — S3 console is native-USB
   CDC, host baud is meaningless on the wire.
- Nano-bridge flashing/console is SUSPECT (owner CAM flash failed via Nano;
  Nano↔CAM-UART0 path later went fully silent) — MB USB is normative until
  re-verified. Proven 2026-10-01 (twice): flashing the CAM with the S3 link
  attached fails (`flash-chip comms` / `serial stream stopped` — GPIO12
  strapping pulled high by the attached link); detached-link flashing is clean
  (`Hash of data verified`). Flash with the link detached, then reattach.
  Nano-5V rail as reboot suspect is unconfirmed (single occurrence).

## 5. Bandwidth reference (verdicts)

Usable ≈ baud × 0.0889 (8N1 + 16B framing @128B chunks). QVGA JPEG q12 ≈ 3–5KB.
Sparse-clean ≠ saturated-clean: baud-limited clean ≤921600 (best sustained
~20fps); BER storm ≥1M under saturation on 30cm dupont (`perr` + `drops`,
`herr≈0`; `ovf` pressure events only @1M). **Max stable: ~20fps @921600**;
**460800–921600 is the sweet band.** VGA @24–30fps ⇒ 6.5–10Mbaud ⇒ infeasible
 on UART (TRM max 5M, practical ≤2M); needs SPI or scope cut. Robot need
(face-unit stills ≤10fps) ⇒ 921600 sufficient with margin; 2M is headroom.
(pre-campaign baseline; §5b refines: 20fps re-demonstrated @1.5M K=0,
24fps infeasible, knee clean ≤50–51% / storm ≥63%.)

## 5b. 24FPS campaign results (2026-10-04, branch `poc-comm-link`)

 Chain accounting closed end-to-end at 1.5M, QVGA, q18, short-exposure+gain-12:
 sensor 27fps (CAMBENCH ceiling, drops=0) → generator 100% (txdrop=0 post-
 decouple) → wire decides. Firmware: Head `fw-head-006-decouple`
 (stage-and-release: grab→memcpy fecBuf_→immediate fb_return; pumpStaged_
 from INTERNAL; `pumpMs`/`grabMs` STATS fields) + S3 `fw-s3-006-ring16+split`
 (`HEAD STATS` bridge, 16KB RX ring, split-print, 288B log display).

 | Leg @1.5M | Offered | Delivered | Wire % | perr/drops | txdrop |
 |---|---|---|---|---|---|
 | K=0 fps15 | 56 KB/s | 15.12 (100%) | 44.6% | 0/0 | 0 |
 | K=0 fps20 | 66.9 KB/s | 19.76 (625/625) | 50.2% | 0/0 | 0 |
 | K=0 fps24 | ~83–89 KB/s | 0.00 (0/832) | 63–67% | 860/778 | 0 |
 | K=2 fps15 | ~89 KB/s | 13.22 (88%) | 67% | 1135/87 | 0 |
 | K=2 fps15 retry | ~84 KB/s | 14.9 | 28.8%* | 1026/2 | 0 |

 *wire% denominator differs per session log (received vs offered); treat as
 order-of-magnitude. Frames are scene-dependent (3.1–7KB at q18) — size the
 operating point for LARGE frames, not the 2–4KB ones.

 Verdicts:
 - **Decouple CONFIRMED:** txdrop/frame 1.00 → 0.00 in all legs; grabMs/txf
   ≈ 0.06–0.21ms (grab never blocks); pumpMs/txf ≈ 22–40ms (UART time to
   the ms: 4 chunks ≈ 25ms, 6 ≈ 42ms). Generator bottleneck eliminated.
 - **Max clean demonstrated: 20fps** (19.76 delivered, 625/625, zero loss).
   **24fps infeasible on UART** at these frame sizes: fps24 storms from the
   first 845ms (ovf=4 already) — the knee is a cliff, not gradual.
 - **Knee: clean ≤50–51%, storm ≥63%** of 1.5M usable (~133KB/s). E2
   discriminator (128B chunks at matched ~56KB/s: 675/675 clean) proved
   storms track burst/control structure, not wire BER.
 - **K=2 shelved:** +58% parity bytes self-impose the storm at 1.5M, and
   recovery never engages inside sustained storms (par=0 has two
   compounding causes: post-completion parity is silent by design, storm
   victims' parity is corrupted like everything else). One genuine rec=1
   observed @115200 — machinery works on sparse losses only.
 - **Storm mechanism (best candidate):** S3 128B HW UART FIFO overflow
   holes on >0.8ms ISR/scheduling stalls during dense bursts (software ring
   size irrelevant past the FIFO). Fits perr≫0 + herr≈0 (magic-resync lands
   on real headers) + ovf pressure + tail-position parity annihilation.
   Future work: RX FIFO thresholds / UART ISR pinning (not attempted).

## 6. Open work

- **Resolution switching** (S3-side verified live 2026-10-01: `HEAD SET
  framesize vga` → ACK, 10s streaming `big=0`; Head-side knob path — camera
  re-init, fallback, SKIP lines — pending owner Head flash via MB USB): Head
  `SET framesize <qvga|…|qxga>` re-inits the camera at runtime (`fb_count=2` +
  `jpeg_quality 12` kept; failure keeps old size + NACKs, never bricks). Bridges over
  `HEAD SET`. 64KB-slot rule: oversize frames are explicit `OVERSIZE` drops
  (S3 `big ⊆ drops`; Head `SKIP_BIGFRAME`, counted as neither ok nor bad).
  Measured: QVGA fits, VGA fits (`big=0`); SVGA/XGA still to settle via the
  `big` counter; SXGA+ expected `big` drops. Head image (ban + knob) flashed
  2026-10-01 via MB USB with the S3 link detached (link-attached flashing fails
  on GPIO12 strapping — see §4; `SET baud 230400` → `NACK bad_baud` verified
  live on Head); a reboot alone does not add it.
- **Reliability batch (task list, in order — source of truth):**
  - [x] RS-style FEC shared core (Arduino-free, no-heap) + Unity recovery vectors.
    DONE: `shared/link/uart_fec.h` (GF(2^8)/0x11D, kMaxParity=4, never-corrupt
    recover) + mirrored `test_uart_fec.cpp` (12 tests); gates 70/70 + 111/111.
  - [x] Head parity emit (N+K shape, IS_PARITY flag, stride padding). DONE:
    `fec_k` knob (default 0 = wire-identical passthrough), K parity chunks
    (idx N..N+K−1, flags 0x04, LAST stays on N−1), 16KB skip cap, `txp=`
    counter; gates 74/74 + 115/115. S3 decode DONE (`rec`/`par` shipped);
    K=0 stays the operating point per §5b (K=2 shelved — self-imposed storm).
  - [x] S3 recover-before-reset + recovered-vs-clean counters. DONE: parity
    intercept (4x1KB INTERNAL slots, never in reassembly accounting),
    recover-before-reset through normal push (COMPLETE honesty), `rec` counter
    (ok stays clean-only); gates 125/125 + 84/84.
  - [x] Sparse-2M then saturated-1.5M/2M frame-completion measures. DONE
    2026-10-01: sparse K=2 @460800/@2M clean; Head mode-3 emit PROVEN direct
    (txp=56 over ~28 K=2 frames); HISTORICAL 2026-10-01, superseded by §5b
    (run-variable — do not cite as operating point): saturated K=2
    @1.5M/pace-0 delivered 98 drops=0 one run, stormed (5.66fps, drops 55)
    the next; saturated @2M (pace 0 and pace 500) beyond-K.
    SUPERSEDED 2026-10-04: 20fps demonstrated (19.76, 625/625, §5b).
    Rules: quiet-switch-then-saturate (never switch saturated);
    near-silent switching for control plane (mode 0 + fps 1); single session
    from switch-up through switch-back; power-cycle Head on
    ok=0/drops-climbing/herr=perr=0. Payload-integrity beyond counters
    unevaluated. (Raw session transcripts retired post-campaign; all
    measured numbers above are the record.)
   - [ ] Auto-pace (fps-derived, manual `pace_us` override kept).
   - [ ] Head progressive-drop (overrun check + mid-frame abort).
   - [ ] AQC: quality knob + S3 window + hysteresis/dwell + re-init guard.
   - [ ] Zero-copy A/B experiment (parked until 1–7 measured).
 - **Post-24FPS-campaign queue (2026-10-04, in priority order):**
   - [ ] Operating-point lock: K=0 @20fps (19.76 demonstrated) with
     frame-size discipline (cap JPEG bytes so offered stays ≤50% usable);
     robot need (≤10fps stills) already covered with large margin.
   - [ ] UART RX FIFO thresholds / ISR pinning experiment (best storm
     mechanism); success criterion: K=2 fps15 clean (perr≈0) + rec>0 on a
     lossy leg. Parked until someone owns the driver work.
   - [ ] K=2 revisit ONLY after the FIFO experiment rehabilitates it —
     gated like fps20/24 (perr≈0/drops≈0/avg≤4500B to proceed).
   - [ ] 24fps on UART: closed infeasible at QVGA/q18 frame sizes (0/832
     storm) — do not re-run without a smaller-frame codec point or SPI.
   - [ ] S3 `SET baud --force-local` escape hatch + Head persistent
     HB-timeout rollback (split-brain recovery without owner hands).
   - [ ] `HEAD SELFTEST` bridge passthrough (would remove the
     detach-flash-attach cycle for bench runs; weighed before, deferred).
- * DEFERRED — Head-TX soak bracketing (onset bounds + re-init/health-check
  root-cause). Parked; do not raise until owner asks.
- * DEFERRED — 10cm vs 30cm wire A/B at saturated 1M/2M. Parked; do not raise
  until owner asks.

## 7. Recovery / pointers

- V2 shells: `git checkout v2 -- embedded/head/src/main.cpp embedded/core/src/main.cpp`.
- Self-test entry: `SELFTEST SWEEP QUICK` (18 combos) then `FULL` (93 base + HIGH tier).
- V2 consoles must document the S3-reboot-on-open trap and the bridge-park rule.
 - FLASH RULE (standing): flashing Head OR Core requires the owner's explicit
   go-ahead each time — never flash on assumed permission, and every subagent
   brief must carry this rule. Head flashes need the link detached (GPIO12
   strapping); S3 flashes need explicit `--upload-port` (auto-detect picks the
   wrong node).
 - **Firmware identity (standing, learned 2026-10-04):** hash verifies
   bytes-written, NOT source-identity — every flashed image carries a manual
   `fw=<board>-<rev>-<content>` banner marker (pinned in host tests), and
   every hardware session re-verifies the marker + behavior fingerprint
   (CAMBENCH ceiling, STATS field set) BEFORE measured legs. A `pumpMs`-class
   counter anomaly once masqueraded as a firmware bug across sessions; the
   marker + fingerprint gates exist so that never recurs.
 - **Logger display budget:** `Logger::kBufSize` 288B (was 192B — truncated
   trailing STATS fields at display time while the wire carried them intact;
   worst case 216 + 12 + 36 = 264 < 288; pinned in host tests).
 - **Bridge limits (known):** S3 bridge forwards `SET`/`GET`/`STATS` only
   (no `HEAD SELFTEST`, no Head-side verbs); `HEAD GET all` / long RESPs go
   through the greedy decoder and TIMEOUT while streaming — use per-key GETs
   + SET ACKs, never depend on `GET all` mid-stream. S3-half-only is the
   standing observation ceiling while linked.
 - Minor watch items (no action): post-park `HEAD CMD` TIMEOUT clusters on
   proven-good links; `gpio_install_isr_service: already installed` on
   camera re-init paths; first camera touch after boot can NACK once
   (transient SCCB probe timeout, retry succeeds); Head USB enumeration
   floats (`-120`/`-2120`, never `-0001`); S3 `up` is ms-since-first-frame,
   not boot; compare Head/S3 counter deltas only over identical windows.
