# Link investigation learnings (2026-10-18)

Durable lessons from the core↔head link diagnosis + QR render bug.
Kept here because run data without its conditions misleads; principles don't.
All firmware references are to `embedded/core`, `embedded/head`, `embedded/shared/link`.

## 1. The QR render bug (confirmed, fixed)

`UdpServer::qrBitmap()` returns the UNPACKED 441B module array; the 63B
QRBITMAP wire payload is the PACKED form (`qr::packBitmap`). Staging the
first 63 raw bytes lit only OLED columns 7 and 15 (two dotted bars —
see simulation in the 2026-10-18 session notes). Rule: **every
representation boundary gets a cross-seam test** (real encoder output fed
into the real consumer), never synthetic fixtures on both sides.
Regression: `test_qr_packed_matches_golden_rows`.

## 2. Passive serial capture discipline

- Open ports WITHOUT touching DTR/RTS (`pyserial ... dsrdtr=False`, no
  pulse). The old DTR/RTS "settle" dance once trapped a classic-ESP32 MB
  board in the ROM download bootloader (silent, 0 bytes for a whole run)
  and every port-open resets the target, destroying the uptime precondition
  of the test you are about to run.
- Append-only single-session logs, hash-verify (`shasum`), timestamped
  filenames so runs never overwrite each other.

## 3. Logs must not overclaim delivery

Every UDP `beginPacket/write/endPacket` site ignored return values while
logging `qr sent` / `hb_ack tx` and bumping TX counters — the logs proved
the send path executed, not that bytes left the radio. Pattern fix:
checked-send helper + TX-fail counters (see systemic plan).

## 4. Throttled DEBUG logs are not telemetry

`LOG_EVERY_N` expands to `LOG_D`, which vanishes in `desky-release`
(`LOG_LEVEL_INFO`) — leaving one first-of-boot line per class, making
100%-loss vs 1%-loss indistinguishable in the field. Periodic `LOG_I`
accounting lines (counters both sides) close the loop without scraping.

## 5. The bench must not inflict the disease it measures

`link_bench.py` reused `boot_id=0x12345678` with `link_seq` from 1 every
run. Against a quiet non-rebooted core, rerun #2's frames all drop as
stale — a self-inflicted wedge mimicking real link loss. Rule: random
`--boot-id` per run, record it in the results JSON, and teach the verdict
to hint the wedge (0 ack + 100% misses → fresh boot-id).

## 6. Two live senders defeat the epoch gate (benign, but know it)

The core keeps ONE `lastBootId_` slot. Bench peer + head interleaved means
every frame carries a different epoch than the last, so every frame
resync-accepts (~236 resyncs/150 s observed) and the strictly-greater gate
never engages. Harmless (idempotent HB_ACKs), but the QR unicast goes to
whoever HB'd last — run pairing/scan proofs with NO bench load on the AP.

## 7. Stale-wedge vs downlink-loss fingerprints

- Stale wedge: core logs `drop link stale HB`, sends NO ACKs.
- Downlink loss: core logs `hb` + `hb_ack tx`, head logs NO drops
  (nothing arrives to reject), head `ack` counter stalls → 3-miss
  re-associate (~9 s cycle, which is the designed response, not a bug).
- TCP handshake succeeding proves nothing about UDP downlink (different path).

## 8. Triage order that worked

Passive observation (simultaneous captures + third-peer bench) BEFORE any
firmware touch; photo of the artifact BEFORE theorizing; simulation of the
bug shape BEFORE the fix; regression test pinned to an independently
decoded golden BEFORE closing. No firmware was modified until the OLED
photo + simulation agreed.

## 9. Head OTA (HTTP pull, `ota <url>`)

Wireless flashing for the single ESP32-CAM head (4MB flash, Arduino,
board=esp32cam, NO SPIFFS): `partitions_ota_nospiffs.csv` tiles
0x10000..0x400000 exactly (nvs 20KB + otadata 8KB + ota_0/ota_1 ~1.94MB each
+ coredump 64KB), and `OtaUpdater` (`src/middleware/ota_client.h`) pulls a
plain-HTTP image into the inactive slot. No TLS by design (closed AP, cert
theater); integrity is the optional `X-Firmware-Sha256` manifest plus the
0xE9 magic + Content-Length-vs-slot checks. `fb_count=1` is load-bearing
(double-buffered frames + an OTA write pump is a crash combo), and no head
image ships without the OTA client (standing rule in the ota header).

- ONE-TIME USB ERASE SESSION (human runs this once, then USB retires): the
  partition move invalidates any existing flash, so erase first —
  `cd embedded/head && pio run -e desky-head -t erase && pio run -e desky-head -t upload`
  (prove with `Hash of data verified` + `Hard resetting`), then
  `pio device monitor --baud 115200` for the BOOT banner.
- TORN-DOWNLOAD SAFETY: the updater writes the inactive slot only; any
  short/stall/magic/manifest/commit failure calls `Update.abort()` and the
  next boot runs the old image. A partial stream can never commit.
- USB-RECOVERY BACKSTOP: if an OTA wedges (bad-but-bootable image with no
  rollback — Arduino default, no mark-valid yet), reflash over USB with the
  `upload` half of the sequence above (no erase needed unless the table
  itself changes again).
- NO-AUTO-ROLLBACK STATUS: `Update.end()` commits unconditionally; there is
  no valid-marking or self-test-then-confirm. A mark-valid stub (boot-count +
  confirm gate) is future work — do not mistake a successful `Update.end()`
  for a health-checked image.
