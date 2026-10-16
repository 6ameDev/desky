# desky shared protocol

Placeholder for machine-readable packet schemas / byte maps shared by the
embedded firmware (`embedded/`) and the future cross-platform client
(`client/`).

Normative source of truth today: `embedded/core/src/middleware/udp_codec.h`
(pairing/auth v1 below is authoritative alongside it; firmware
`middleware/pairing.h` + client `domain/Codec.kt` mirror it exactly).
Python peers: `embedded/scripts/udp_drive.py`, `embedded/scripts/udp_telemetry.py`.

## Conventions

- Multi-byte fields are big-endian.
- Each packet ends with a 1-byte XOR checksum over all preceding bytes.
- No heap, no Arduino headers in the codec — pure byte math so host
  Unity tests and future Kotlin code can mirror it exactly.

## Control packet: App -> Robot, 6 bytes @30-50Hz

```
[0xAA | mode | throttle | steering | flags | XOR-checksum]
```

| Byte | Field    | Meaning                                              |
| ---- | -------- | ---------------------------------------------------- |
| 0    | header   | `0xAA` (`kControlHeader`)                            |
| 1    | mode     | mode byte                                            |
| 2    | throttle | center-128 byte: 128 = stop, 255 = full +, 0 = full - |
| 3    | steering | center-128 byte: 128 = centered, + = right           |
| 4    | flags    | flags byte                                           |
| 5    | checksum | XOR of bytes 0..4                                    |

Decode rejects wrong header, wrong length, or bad checksum.

## Telemetry packet: Robot -> App, 9 bytes @10-20Hz

```
[0xBB | pitchBE i16 | rollBE i16 | distBE u16 | statusFlags | XOR-checksum]
```

| Bytes | Field       | Meaning                                    |
| ----- | ----------- | ------------------------------------------ |
| 0     | header      | `0xBB` (`kTelemetryHeader`)                 |
| 1..2  | pitch       | int16 big-endian, decidegrees (deg = raw/10) |
| 3..4  | roll        | int16 big-endian, decidegrees               |
| 5..6  | distanceMm  | uint16 big-endian, millimetres              |
| 7     | statusFlags | bit0 = cliff, bit1 = driving, bits2-3 = mode (0 MANUAL, 1 AUTONOMOUS, 2 LOW_POWER, 3 EMERGENCY), bits4-7 reserved 0 |
| 8     | checksum    | XOR of bytes 0..7                           |

Decode rejects wrong header, wrong length, or bad checksum.

## Transport

- UDP port `3333` (see scripts for the registration-hello / unicast convention).
- Robot AP `desky-<last4 MAC>` + common pass = transport only (zero security).
  App uses fixed `192.168.4.1:3333`; no IP input.
- Robot failsafes to stop ~500ms after the last control datagram.
- Telemetry Robot -> App is unicast-only while bonded and alive (`kAliveMs` 5s), silent otherwise — subnet broadcast deleted, pre-bond silent.

## Pairing + authenticated drive v1 (frozen by review 2026-10-15)

Two tiers, decoupled: Tier 1 ephemeral pairing code proves physical
presence; Tier 2 persistent PSK proves ownership on every drive packet.
Rotation of Tier 1 never evicts an existing bond. Single owner: a new
bond evicts the old; re-pair only via 10s button hold (clears bond).

Constants: code = 6 ASCII digits (`esp_random`, CSPRNG); single-use,
5-min window, 5 bad tries -> 30s lockout; PSK = 16 bytes (`esp_random`);
counter = 32-bit BE, first drive packet uses 1 (0 is reserved as the bond
baseline and never valid on the wire), strictly increasing, persisted
periodically;
`KEK = HKDF-SHA256(salt=client_nonce||server_nonce, IKM=code ASCII,
info="desky-pair-v1", L=16)`.

- `PAIR_REQ` App -> Robot, 12 bytes:
  `[0x50 | code 6B ASCII | client_nonce 4B | XOR]`
- `PAIR_OK` Robot -> App (unicast), 39 bytes (v1.1; v1 robots send 38):
  `[0x51 | server_nonce 4B | ct 17B | gcm_tag 16B | XOR]`
  with `ct||tag = AES-128-GCM(KEK, nonce12=server_nonce||client_nonce||
  00 00 00 00, AAD=0x51||nonces, psk16||persist_u8)`. Code burned on send.
  v1.1 persist-interval addendum: the 17th plaintext byte is the counter-
  persist cadence (default 128 = `COUNTER_PERSIST_EVERY` both sides),
  authenticated by the GCM tag+AAD exactly like the PSK — never a bare
  field. Accepted once at pair only; guard is minimal (not an allowlist):
  N in 8..1024 else 128 (wire holds the u8 subset 8..255; 256..1024
  reserved for a future u16 field and map to 128 on the wire) — kills
  0/div-zero + absurd values. A v1 (38B) robot has no byte: the app
  defaults to 128 and stays compatible.
- `PAIR_ERR` Robot -> App, 4 bytes:
  `[0x52 | reason 1B | retry_s 1B | XOR]`
  reason: 0=bad_code, 1=locked, 2=not_in_window, 3=already_bonded.
- `DRIVE_V2` App -> Robot, 16 bytes @30-50Hz:
  `[0xAC | mode | thr | str | flags | counter 4B BE | tag 6B | XOR]`
  with `tag = first6(HMAC-SHA256(psk, counter||mode||thr||str||flags))`.
- `UNBOND` App -> Robot, 12 bytes (fire-and-forget, no reply):
  `[0xAD | counterBE 4B | tag 6B | XOR]`
  with `tag = first6(HMAC-SHA256(psk, "UNBOND"||counterBE4))` — the 10B
  message is the ASCII prefix `UNBOND` + the 4B big-endian counter, domain-
  separating the unbond tag from drive tags. Verify order mirrors drive:
  len -> header -> XOR -> bonded? -> counter fresh (`isFreshCounter`) ->
  HMAC constant-time. Fail = silent drop. Pass = symmetric forget: the
  robot clears the NVS bond + in-memory key + PAIR_OK redelivery cache and
  opens a fresh pairing window, so a new phone can pair immediately.
- `REKEY_REQ` App -> Robot, 14 bytes (Tier-1 silent auto-rekey):
  `[0x53 | eraBE2 | counterBE4 | tag 6B | XOR]`
  with `tag = first6(HMAC-SHA256(oldPsk, "REKEY"||eraBE2||counterBE4))` —
  the 11B message is the 5 ASCII bytes of `"REKEY"` + the 2B big-endian
  requested era (stored era + 1, u16 wrap mod 65536) + the 4B big-endian
  counter (fresh per `isFreshCounter`, allocated from the shared holder).
- `REKEY_ACK` Robot -> App, 10 bytes:
  `[0x54 | eraBE2 | tag 6B | XOR]`
  with `tag = first6(HMAC-SHA256(newPsk, "REKEYACK"||eraBE2))` — the 10B
  message is the 8 ASCII bytes of `"REKEYACK"` + the new era, authenticated
  under the NEW key so the ACK proves the switch.
- `ALIVE` App -> Robot, 14 bytes (keepalive under the bonded PSK):
  `[0x55 | eraBE2 | counterBE4 | tag 6B | XOR]`
  with `tag = first6(HMAC-SHA256(psk, "ALIVE"||eraBE2||counterBE4))` —
  the 11B message is the 5 ASCII bytes of `"ALIVE"` + the 2B big-endian
  CURRENT era (stored era; a rekey moves it) + the 4B big-endian counter
  (fresh per `isFreshCounter`, allocated from the shared holder — a valid
  ALIVE consumes `lastCounter_` exactly like drive, so a replayed ALIVE
  can never refresh twice).
- `DISCONNECT` App -> Robot, 14 bytes (graceful goodbye, fire-and-forget,
  no reply):
  `[0x56 | eraBE2 | counterBE4 | tag 6B | XOR]`
  with `tag = first6(HMAC-SHA256(psk, "DISCONNECT"||eraBE2||counterBE4))`
  — the 16B message is the 10 ASCII bytes of `"DISCONNECT"` + eraBE2 +
  counterBE4. Verify order mirrors ALIVE. Pass runs refresh-then-clear
  like UNBOND (alive timestamp refreshed, net peer dropped) but the bond
  is KEPT: the next launch rejoins instantly. Never drives.
  Headers `0x55`/`0x56` are claimed here — never reuse
  `0xAA`/`0xAC`/`0xBB`/`0x50`/`0x51`/`0x52`/`0x53`/`0x54`/`0xAD`.

Robot alive rule (firmware `UdpServer::markAlive` funnel): every VALID
bonded packet — ALIVE, DRIVE_V2, REKEY_REQ — pins the telemetry unicast
peer AND refreshes the alive timestamp BEFORE any opcode-specific effect
(event publish, counter persist, ACK). No opcode holds a privilege: a 1Hz
ALIVE keeps the robot alive exactly like 50Hz drive, and ALIVE never
touches the drive effect (`lastV_`/`lastOmega_`/`lastRxMs_` stay
drive-only). Fresh while within `kAliveMs` (5s). DISCONNECT/UNBOND run
refresh-then-clear (net gone: `havePeer_ = false`); UNBOND additionally
clears the NVS bond + key + redelivery cache and opens a fresh pairing
window. Pre-bond ALIVE/DISCONNECT drop like pre-bond drive; old-key
frames die via HMAC with no layout change. App-side ALIVE send is a
separate task — the firmware handler ships first so the wire is stable.

Platform split (structure-only — wire unchanged): connection peer + alive
funnel core is `embedded/core/src/middleware/connection_manager.h`
(`connection::ConnectionManager`, u32 peer; `UdpServer` adapts
`IPAddress`<->u32 and keeps the WiFiUDP fd + gate logging + send);
telemetry cadence/encode is `embedded/core/src/services/telemetry_manager.h`
(aggregate `Snapshot` in, bytes out via a `UdpServer`-provided send fn);
bond/era lifecycle is `middleware/pairing.h` `Core` (`UdpServer` keeps the
PSK/counter/NVS + redelivery cache).

Tier-1 silent auto-rekey (era + PSK rotation, fully silent, no user
action): before the app counter hits `EXHAUSTION_GATE` (`0xFFFFFF00`) both
sides rotate to a new era + PSK under the old PSK, reset the counter to the
bond baseline (0 persisted, first post-rekey drive uses 1 — same as a fresh
pair), and resume gapless. Mirrors TLS `KeyUpdate` / IPsec rekey / LoRaWAN
rejoin. Key schedule (no key transport — both sides derive):
`PSK_next = HKDF-SHA256(salt="desky-rekey-v1", IKM=oldPsk16,
info=eraBE2(new), L=16)`. The era lives in NVS (`bera`, u16, additive key
— pre-rekey bonds read back 0; fresh pair resets to 0) + the Room v5 column
+ the `DriveCounter` holder; drive frames carry no era (post-switch old-key
frames drop via HMAC — no drive layout change). Verify order mirrors
drive/unbond: len -> header -> XOR -> bonded? -> era is exactly stored+1
(a duplicate with era == stored re-ACKs idempotently WITHOUT re-verifying
— the lost-ACK retry reuses the same era + counter bytes; the ACK leaks no
secret: era + HMAC tag under the new key) -> counter fresh -> HMAC
constant-time under the OLD key. Fail = silent drop (no error frame; the
app retries 3x byte-identical, then falls back to the Tier-2 repair copy
"session expired — please re-pair"). Pass = persist (`era+1`/`psk_next`/
counter 0) BEFORE the ACK, switch atomically, reply ACK. The old PSK is
dropped immediately on switch (no reorder window): stale in-flight drive
frames under the old key drop via HMAC. Proactive margin: the app fires the
exchange when `lastSent >= GATE - 1000` (`REKEY_MARGIN`, ~20 s at full
50 Hz drive) so rotation completes before the fail-closed gate. Headers
`0x53`/`0x54` are claimed here — never reuse
`0xAA`/`0xAC`/`0xBB`/`0x50`/`0x51`/`0x52`/`0xAD` (a Phase-2 `SET_RATE`
must pick other values).

Verify order (Core 0, pre-publish): len -> header -> XOR -> bonded? ->
counter > lastSeen -> HMAC constant-time (`mbedtls_ct_memcmp`). Fail =
silent drop (500ms failsafe stops safely). Pre-bond: legacy `0xAA`
frames pin the telemetry peer only, never drive. Post-bond: legacy
drive datagrams are dropped.

Libraries only, no custom crypto: firmware `mbedtls_md_hmac` +
`mbedtls_hkdf` + `mbedtls_gcm` + `mbedtls_ct_memcmp` + `esp_random`;
client `dev.whyoleg.cryptography:0.6.0` (JCA/CryptoKit backends).

## Design decisions (bench-proven 2026-10-15, `pair_bench.py`)

- Two tiers, decoupled: the pairing code proves presence (single-use,
  time-boxed), the PSK proves ownership (persistent). Rotation never
  evicts; single owner; re-pair only via bond wipe.
- Counter, not timestamps, for freshness: the S3 has no trustworthy clock
  (no RTC, no NTP on the AP), and timestamps without a seen-cache don't
  stop in-window replay anyway. One `uint32`, strictly greater, wrap-aware;
  first drive uses 1, 0 is never valid. No sync protocol to desync.
- HMAC covers counter + sticks: the tag authenticates `counter||mode||thr`
  `||str||flags`, so bumping the counter (or the sticks) without the PSK
  is a drop. Nothing is encrypted — motion is publicly observable, so
  there is no confidentiality requirement, only authorship + integrity.
- Timing budget: sustained worst-case HMAC verify measured 333µs on-device
  (software SHA256); budget is 1000µs — ~20% of one 5ms Core 0 tick, Core 1
  control untouched. Throttled `drive verify dt/max` line proves it live.
- Drops log first-of-boot at INFO, then throttled: `LOG_EVERY_N` alone hid
  the first N-1 occurrences and blinded the first bench run. Same for the
  audio meter (now DEBUG) — INFO transcripts stay readable.
- Dev-stage pragmatism (revisit before real customers): destructive Room
  migration, no NVS/PSK migration code, client discovery scan deleted
  (fixed `192.168.4.1` needs none in AP mode).
- Known-tolerated smell: refusal logging is per-site duplicated
  (`dropFirst` + `LOG_EVERY_N` at each site). Graduate to a central
  `refuse()` funnel if a second consumer of drop data ever appears
  (telemetry counters, diagnostics) — not before.
Shared test vector (fixed code/nonces/psk -> KEK/ct/tag) must pass both
sides (`embedded/scripts/gen_pair_vectors.py` regenerates it;
client `domain/PairingCryptoTest.kt` pins it):
fixed inputs code=`123456`, client_nonce=`01020304`,
server_nonce=`05060708`, psk=`00112233445566778899aabbccddeeff`,
persist_every=`128` (`0x80`; v1.1 plaintext is `psk16||persist_u8`, 17B)
(drive sample: counter=1, mode=`0x00`, thr=`0x80`, str=`0x80`,
flags=`0x00`); KEK=`0544036bb09f5c5a7a41759d414e2f46`,
ct=`f47f282e8b8020ec2acf8bf1eaf2ee2590` (17B),
gcm_tag=`e211690852a899b6d4062fbf1058876c`,
drive_tag=`3ec8b625c9c1`.
Rekey test vector (same psk as above, nextEra=`1`, counter=`100`):
rekey_msg=`52454b4559000100000064` (`"REKEY"||eraBE2(0001)||ctrBE4(00000064)`),
rekey_tag=`fdfe277d1623`,
next_psk=`df7441d0d71778b2dd10493962d45a5b`
(`HKDF-SHA256(salt="desky-rekey-v1", IKM=oldPsk, info=0001, L=16)`),
ack_msg=`52454b455941434b0001` (`"REKEYACK"||eraBE2(0001)`),
ack_tag=`ed5f52335855`,
`REKEY_REQ`=`53000100000064fdfe277d16235a` (14B),
`REKEY_ACK`=`540001ed5f523358558b` (10B).
Alive/disconnect test vector (same psk as above, era=`1`, counter=`100`):
alive_msg=`414c495645000100000064`
(`"ALIVE"||eraBE2(0001)||ctrBE4(00000064)`),
alive_tag=`dd152baa54f7`,
`ALIVE`=`55000100000064dd152baa54f7da` (14B),
disc_msg=`444953434f4e4e454354000100000064`
(`"DISCONNECT"||eraBE2(0001)||ctrBE4(00000064)`),
disc_tag=`ca90ca039040`,
`DISCONNECT`=`56000100000064ca90ca03904070` (14B).
