# Telemetry Phase-2 Spec (DRAFT for owner refinement)

Adaptive telemetry: app-negotiated steady rate + urgent edge channel.
Status: **research draft** — Phase-1 (app-side ~5Hz coalescing, no wire change) is the accepted near-term path. This doc scopes Phase-2 (wire change) for later refinement.

Normative v1 wire today: `embedded/core/src/middleware/udp_codec.h` + `shared/protocol/README.md` (pairing/auth v1 frozen 2026-10-15). Nothing here changes v1 until this spec is accepted.

## 0. Baseline (what exists today)

- Robot -> App telemetry: 9-byte `0xBB` (`pitchBE i16 | rollBE i16 | distBE u16 | statusFlags | XOR`), `CFG_TELEMETRY_HZ 15` (`config.h:110`), `kTelemetryMs = 1000/15 ≈ 66ms` (`udp_server.h:273`), unicast-to-last-peer (`udp_server.h:803-848`).
- App -> Robot drive: 16-byte `DRIVE 0xAC` @50Hz (`tickMs=20`, `RobotControlViewModel.kt:105`), `HMAC-SHA256(psk, counter||mode||thr||str||flags)[0:6]`, counter from `DriveCounter` (persist every 128), wrap-aware `isFreshCounter`, ~333µs verify.
- Failsafes: robot stops ~500ms after last good drive (`CFG_COORDINATOR_STALE_MS 500`); app demotes `Connected -> Disconnected` after 2s silence (`TELEMETRY_TIMEOUT_MS 2000`).
- Problem with pure app-side filtering: robot still spends TX CPU + Wi-Fi airtime + phone radio wake + decode for packets the UI throws away.

## 1. Goals / non-goals

Goals: cut phone CPU + Wi-Fi airtime without touching safety; keep 500ms robot failsafe and 2s app timeout independent of negotiated rate; stay backward compatible.

Non-goals: changing `0xBB` payload shape in v1; touching drive path timing; encrypting telemetry (auth-only, like MAVLink signing).

## 2. What mature products do (sources)

### 2.1 MAVLink (drones — primary model to copy)

- Steady: `MAV_CMD_SET_MESSAGE_INTERVAL` (cmd 511) in `COMMAND_LONG`: `param1=msgid, param2=interval_us (100000=10Hz, 1000000=1Hz, -1/0=stop)`. Acked by `COMMAND_ACK` (`ACCEPTED/DENIED/UNSUPPORTED`). Per-message, per-link.
  Refs: https://mavlink.io/en/mavgen_python/howto_requestmessages.html, https://ardupilot.org/dev/docs/mavlink-requesting-data.html
- Legacy: `REQUEST_DATA_STREAM` (groups 0-12) **superseded since 2015-08** by `SET_MESSAGE_INTERVAL`; ArduPilot keeps `SRx_` boot params + `REQUEST_MESSAGE` single-shot poll. Conflict rule on shared links: set aggressor to `-1`.
  Refs: https://mavlink.io/en/messages/common, https://ardupilot.org/plane/docs/common-mavlink-configuration.html
- Urgent: `STATUSTEXT (#253)`: `severity (RFC-5424) + text[50] + id + chunk_seq`. "Use only for important status, ~10Hz max". ArduPilot: `GCS_MAVLINK::send_text()` + manual throttle. INAV de-spam (best anti-flap ref): changed text immediate; repeats at **30s NOTICE / 10s WARNING / 5s CRITICAL**; arming-disable re-sent on change + 1/10s/port cap; mode transitions always NOTICE.
  Refs: https://github.com/mavlink/c_library_v2/blob/master/common/mavlink_msg_statustext.h, https://ardupilot.org/dev/docs/debug-with-send-text.html, https://inavflight.github.io/docs/next/advanced-features/mavlink
- Reliability: only commands are reliable (`COMMAND_* -> COMMAND_ACK`, auto-resend, `IN_PROGRESS`, `COMMAND_CANCEL`). Streams incl. `STATUSTEXT` are unacked; loss via 8-bit per-channel seq gap.
  Refs: https://mavlink.io/en/services/command.html, https://mavlink.io/en/guide/packet_loss.html
- Security: MAVLink2 signing appends 13B (`linkID + 48-bit timestamp + 48-bit sig = first48(SHA256(packet+timestamp+key))`), monotonic per link, never update clock on bad sig. Auth-only, not encryption; unsigned links spoofable; jam/drop always possible.
  Refs: https://mavlink.io/en/guide/message_signing.html
- Lesson for desky: keep heartbeat/failsafe independent of negotiated rate; slow streams 4x+ during transfers.

### 2.2 ROS topics (throttle / latch / event split)

- Steady: `topic_tools throttle` (`throttle messages <in> <hz>`, `throttle bytes <in> <B/s> <window>`): forward-on-arrival, drop if `< 1/rate` since last forward; `lazy`, `use_wall_clock`. Canonical wireless example: laser `base_scan -> 1Hz`.
  Refs: http://wiki.ros.org/topic_tools/throttle, https://github.com/ros-tooling/topic_tools
- Urgent + latch: ROS1 `latch=true`; ROS2 durability `transient_local` (both ends must agree). Convention: fast sensors `VOLATILE/BEST_EFFORT` + rare state as separate latched topics republished slowly (e.g. `Bool transient_local @1Hz`). Diagnostics: drivers `/diagnostics @1Hz` default (REP-107), aggregator `@1Hz`.
  Refs: https://docs.ros.org/en/humble/Concepts/Intermediate/About-Quality-of-Service-Settings.html, https://github.com/ros/diagnostics, https://github.com/ros-infrastructure/rep/blob/master/rep-0107.rst
- Reliability: QoS `RELIABLE` (DDS retries) vs `BEST_EFFORT`; `deadline/lifespan/liveliness` drop stale. No per-message ACK.
- Lesson: desky `SET_RATE` must be authenticated (bond PSK) — ROS has none at topic layer (needs DDS-Security).

### 2.3 DJI SDK / gamepads / BLE (negotiated intervals + notify vs indicate)

- DJI Onboard: **Broadcast** (0/1/10/50/100Hz per component, packet at max rate, slow fields stale) vs **Subscription** (preferred, 5 packages x 300B, each own freq; accel 400Hz, quaternion 200Hz, GPS/RC/battery 50Hz). Control uplink 50Hz. Custom data via transparent channel, not telemetry.
  Refs: https://developer.dji.com/onboard-sdk/documentation/guides/component-guide-telemetry.html
- HID/gamepad: interrupt IN `bInterval` is the negotiated poll (FS 10ms, HS ~1ms). Host silences unchanged reports with `SET_IDLE` (4ms units): device sends on-change + idle refresh. BT 125-250Hz, wired 500-1000Hz.
- BLE GATT: interval **7.5ms-4s negotiated** (+ latency, timeout, MTU/DLE). **Notifications = unacked steady** (~780kbps-1.25Mbps); **Indications = acked critical** (~66kbps, one per interval, 30s timeout). CCCD gates both; `CoAP-over-GATT` maps unreliable->notify, reliable->indicate + 1-bit dedup.
  Refs: https://docs.silabs.com/bluetooth/7.4.1/bluetooth-gatt/gatt-server-client-roles, https://www.ietf.org/archive/id/draft-amsuess-core-coap-over-gatt-04.html
- Lesson: steady=notify/unacked/coalesced; critical=indicate/acked-or-repeated; interval server-clamped.

### 2.4 Other small-Wi-Fi-robot sources

- **CoAP Observe (RFC 7641)** — closest academic fit: client registers `GET + Observe`, **server decides when to notify** (may skip under congestion), per-notification CON/NON choice, ≥1 CON per 24h liveness, `Max-Age` freshness (client must not use past Max-Age). Conditional observe ("only when cliff=true") is the intended pattern. Threat note: stale-state operation — same reason desky keeps 500ms failsafe independent.
  Ref: https://datatracker.ietf.org/doc/html/rfc7641
- **MQTT-SN**: gateway + sleeping clients, `QoS -1/0/1/2`, retained (= latch), short topic IDs. Pattern: steady QoS0, alarms retained QoS1.
- **ESP-NOW**: 250B v1 / 1470B v2, ≤20 peers (≤7-17 encrypted CCMP), 1Mbps PHY ~214kbps open / ~555kbps shielded; per-peer rate; **no guaranteed delivery — app ACKs**. ESP32 UDP ceiling ~30Mbps air — desky `9B@15Hz (~1kbps)` is noise; Phase-2 saves phone CPU/airtime contention, not link capacity.
  Refs: https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32/api-reference/network/esp_now.html
- **FIRST/FRC NetworkTables 4**: **retained** (server holds last while running) vs **persistent** (also saved across restarts) — analog of NVS rate + latched cliff; ~100ms dashboards, on-robot logs for high-rate truth; field cap ~4Mbps/team forces throttling; persistent Alerts API separates dashboards from sticky faults.
  Refs: https://docs.wpilib.org/en/stable/docs/software/networktables/networktables-intro.html, https://github.com/wpilibsuite/allwpilib/blob/main/ntcore/doc/networktables4.adoc

## 3. Proposed Phase-2 wire (to refine)

Frozen: `0xBB` payload bytes, `DRIVE` layout, XOR + HMAC construction. Rate changes cadence only.

- `SET_RATE_REQ` (new header/len, never `0xAA/0xAC/0xBB`): `[hdr | rate_hz u8 | counterBE u32 | tag6=HMAC(psk,"RATE"||counter||rate) | XOR]`. Verify order mirrors DRIVE (len -> header -> XOR -> paired -> fresh-counter -> HMAC-ctEqual). Fail = silent drop.
- Allowlist e.g. `{1,5,10,15}`Hz; unknown clamps + NAKs nearest. Robot boots to NVS rate, factory default 15Hz. `kTelemetryMs = 1000/negotiatedRate`.
- `RATE_ACK(rate, status)` with app retry 3x backoff; robot re-apply idempotent.
- Urgent edges ride same socket as immediate out-of-cadence frames + repeat while latched: cliff assert/clear immediate, mode-change x3 burst 100ms apart, e-stop state immediate; suggest cliff `1Hz` repeat while asserted. Steady never delayed by edges; edges never blocked by low rate.
- Old app ignores `RATE_ACK`; old robot drops `SET_RATE` (unknown len/header) and stays 15Hz — interop matrix must prove both directions.

## 4. Anti-flap + rate limits

- TCRT hysteresis stays the assert/clear authority; add min re-announce spacing (INAV shape scaled down) + 10Hz burst cap + per-second TX ceiling so flap cannot exceed 15Hz budget by more than bounded burst.
- App keeps 2s `TELEMETRY_TIMEOUT_MS` demote + Max-Age-style staleness label; last packet stays visible.

## 5. Reliability split

- Steady unacked; edges repeated-not-acked (keeps UDP stateless, matches MAVLink/NT/BLE-notify); `SET_RATE` acked. Loss detected via counter gap + staleness. No app-ACK for telemetry in v1 of Phase-2 (revisit if field shows gap).

## 6. Security

- `SET_RATE` requires bond PSK + monotonic counter (replay = drop); unsigned pre-bond senders pin peer only (existing rule). Attacker without PSK cannot change rate or forge edges but CAN jam/drop — degrades to failsafes (robot 500ms stop, app 2s demote). Log rate-change + first-drop per class at INFO, then throttled.

## 7. Failsafe interaction

- 500ms stale-command stop independent of telemetry rate; rate never touches control RX; pre-bond unicast-only rule unchanged.

## 8. Power / airtime budget (to measure)

| Rate | pkts/s | bytes/s | airtime vs 15Hz | phone wakes/s |
| ---- | ------ | ------- | --------------- | ------------- |
| 15Hz | 15 | 135 | 1x | 15 + 50 drive |
| 10Hz | 10 | 90 | 0.67x | 10 + 50 |
| 5Hz | 5 | 45 | 0.33x | 5 + ~10* |
| 1Hz | 1 | 9 | 0.07x | 1 + ~10* |

*with idle-heartbeat + 5Hz UI coalescing (Phase-1). Bench gate proposal: 5Hz ≤ 1/3 airtime of 15Hz, urgent latency p99 < 100ms on bench AP. Fill in measured ESP32 TX mA + app CPU% per rate before accepting.

## 9. App contract (KMP)

- `ObserveTelemetryUseCase` coalescing (Phase-1) stays; add rate-request UI + `SetTelemetryRateUseCase`; `RobotControlViewModel` 20ms loop untouched; timeout/latch rules unchanged; downgrade to local coalescing when robot NAKs or is old (no `RATE_ACK`).

## 10. Test + rollout

- Unity host tests (rate clamp, edge repeat, wrap, interop old/new both ways), bench airtime captures, NVS migration (remembered rate), `dropFirst` + throttled logging + `droppedIncoming`-style diagnostics counters, old/new interop matrix.

## 11. Recommended Phase-2 requirements (for owner refinement)

1. Authenticated `SET_RATE`; default 15Hz; NVS-remembered; clamped allowlist.
2. Frozen `0xBB` payload; rate changes cadence only.
3. Urgent edge channel: immediate + severity-graded repeat while latched.
4. Anti-flap per signal + burst ceiling.
5. Reliability split: steady unacked, edges repeated, `SET_RATE` acked with retry.
6. Attacker without PSK cannot silence urgent or downgrade rate; jam degrades to failsafes.
7. Budget + observability acceptance (measured table + bench gate + throttled logs).

## 12. Open questions for owner

- Ack urgent edges or repeat-only? (repeat keeps stateless; ack costs round trips)
- Exact rate steps? Persist per-bond or global?
- Expose rate in UI or auto (foreground 10Hz / background 1Hz)?
- Battery-threshold urgent values?

## 13. Appendix: counter durability + exhaustion + persist-interval (shipped state, owner-refinable)

Status note: this appendix records what is already shipped on `feat/pairing-auth`
vs what remains spec-only, so a later Phase-2 change does not re-litigate it.
Layer-A flush, Tier-2 fail-closed, Tier-1 pair-refresh frames + key schedule, and the
persist-interval byte are implemented; Layer-B resync is spec-only; the Tier-1
background trigger is an inert stub. Normative wire for all shipped items stays
in `shared/protocol/README.md` — this section is rationale + ranking only.

### 13.1 Layer-B resync (spec-only — not implemented)

Problem: Layer-A (flush `DriveCounter` to disk every N sends + on graceful
stop) already covers graceful kills — app backgrounded, clean close, robot
reboot with NVS intact. What it does not cover is the ungraceful kill: OS
jetsam / crash / power loss between two persist points leaves the in-memory
counter up to N ahead of disk, so the next launch replays a stale counter and
the robot drops the first frames until the counter climbs past `lastSeen`.
Bounded (≤ N drops, self-healing), but visible as a ~seconds stick lag after a
crash. Layer-B closes that gap. Three options, ranked:

- **B1 — `lastCounter` hint in telemetry (forward-only, capped). Recommended
  first.** Robot piggybacks its `lastSeen` counter on the steady `0xBB`
  telemetry (new field or trailing byte; shape TBD — must not disturb the
  frozen 9-byte layout until Phase-2 accepts a wire change). On launch / after
  TX-gap, the app reads the hint and fast-forwards its counter to
  `max(diskCounter, hint)` — forward-only, capped at e.g. `hint + 1` step so a
  spoofed/jammed hint can never rewind or leap the counter. Tradeoffs: cheapest
  (no new round trips, no new headers, rides existing unicast telemetry);
  self-limiting (hint is advisory, `isFreshCounter` stays authoritative);
  stale-hint safe (forward-only + cap). Backward compat: old robot sends no
  hint → app falls back to disk counter (today's behaviour); old app ignores
  the extra byte (length-gated decode, same rule as `PAIR_OK` 38B→39B).
  Open: hint cadence (every frame vs every Kth), exact cap, where the byte
  lives post-Phase-2.
- **B2 — write-ahead N+128 window.** App persists `counter + 128` ahead of the
  wire (i.e. disk claims headroom the wire has not spent yet), so a crash
  replays from inside already-authorised headroom rather than behind
  `lastSeen`. Tradeoffs: zero wire change, zero round trips; but widens the
  replay window the robot must accept (a stolen DB snapshot replays up to 128
  frames — same bound as today's persist cadence, just shifted); interacts
  with the exhaustion gate (headroom arithmetic must not cross
  `0xFFFFFF00` silently). Backward compat: pure app-side, no interop matrix.
  Open: whether the +128 headroom needs clamping near the gate.
- **B3 — `RESYNC_REQ` / `RESYNC_ACK` handshake.** App asks the robot for its
  `lastSeen`; robot answers authenticated under the PSK; app fast-forwards.
  Tradeoffs: exact (no guessing, no headroom math); but costs new headers
  (must avoid `0xAA/0xAC/0xBB/0x50/0x51/0x52/0x53/0x54/0xAD`), new verify
  order, new retry/backoff + lost-ACK idempotency — a second reliable exchange
  next to `SET_RATE` / `PAIR_REFRESH` for a seconds-long cosmetic lag. Backward
  compat: old robot drops the request (unknown header) → app falls back to B0
  (today's self-heal); old app never sends it. Open: ack auth (under PSK like
  `PAIR_REFRESH_ACK`?), rate-limiting the request so it cannot become a peer-oracle.

Ranking: **B1 > B2 > B3.** B1 keeps the UDP-stateless shape of §5 (advisory,
no ACKs, robot stays authoritative); B2 is the zero-wire fallback if B1's
byte cannot be placed; B3 only if field data shows crash-replay lag is a real
complaint — do not pay handshake complexity for a self-healing cosmetic.

### 13.2 Counter exhaustion: Tier-2 fail-closed + Tier-1 silent auto-pair-refresh (shipped)

- **Tier-2 (no bond / legacy path): fail-closed at `0xFFFFFF00` + explicit
  re-pair (shipped).** When the counter reaches `EXHAUSTION_GATE`
  (`0xFFFFFF00`, ~256 frames of headroom before u32 wrap), the side refuses to
  send further and surfaces the Tier-2 repair copy ("session expired — please
  re-pair"). No wrap, no silent reuse, no auto-recovery — the user re-pairs.
  Rationale: without a shared key there is nothing to rotate *under*, so
  fail-closed is the only safe shape.
- **Tier-1 (paired): silent auto-pair-refresh era rotation (shipped frames +
  schedule; trigger stub inert).** Before the gate, both sides rotate to a new
  era + PSK under the old PSK, reset the counter to the bond baseline (0
  persisted, first post-pair-refresh drive uses 1 — same as a fresh pair), and resume
  gapless. Wire (claimed headers, never reuse): `PAIR_REFRESH_REQ 0x53 [eraBE2 |
  counterBE4 | tag6]` with
  `tag = first6(HMAC-SHA256(oldPsk, "PAIRREFRESH"||eraBE2||counterBE4))`;
  `PAIR_REFRESH_ACK 0x54 [eraBE2 | tag6]` with
  `tag = first6(HMAC-SHA256(newPsk, "PAIRREFRESHACK"||eraBE2))` so the ACK proves
  the switch. Key schedule (no key transport — both sides derive):
  `PSK_next = HKDF-SHA256(salt="desky-pair-refresh-v1", IKM=oldPsk16,
  info=eraBE2(new), L=16)`. Verify mirrors drive/unpair (len → header → XOR →
  paired? → era == stored+1, duplicate era == stored re-ACKs idempotently
  without re-verifying → counter fresh → HMAC constant-time under OLD key);
  fail = silent drop with 3x byte-identical app retry, then Tier-2 repair
  copy. Pass = persist (`era+1`/`psk_next`/counter 0) BEFORE the ACK, switch
  atomically, old PSK dropped immediately (no reorder window — stale in-flight
  old-key drives drop via HMAC). Proactive margin: app fires at
  `lastSent >= GATE - 1000` (`PAIR_REFRESH_MARGIN`, ~20 s at 50 Hz) so rotation
  completes before the gate.
- **Follow-up (not in this branch): background-launch the trigger.** The
  `maybeRekey` check at send time is intentionally inert (returns false) so
  the shipped path cannot half-fire. The follow-up wires it to a background
  launcher with: a `pairRefreshInFlight` guard (one exchange at a time), a vault/DB
  refresh after persist (so the new era + PSK survive process death), and
  bounded retry with backoff (3x byte-identical, then Tier-2 copy — no tight
  loop at 50 Hz). Until then exhaustion is exercised by tests only, and field
  behaviour at the gate is fail-closed on both tiers.

### 13.3 Persist-interval `persist_u8` (shipped, v1.1 `PAIR_OK` 39B)

The counter-persist cadence N travels once, at pair, inside the `PAIR_OK`
ciphertext — never as a bare field. Plaintext is `psk16||persist_u8` (17B);
`ct||tag = AES-128-GCM(KEK, nonce12=server_nonce||client_nonce||00 00 00 00,
AAD=0x51||nonces, psk16||persist_u8)` — the byte is authenticated by the GCM
tag + AAD exactly like the PSK. Guard is minimal (not an allowlist): N in
8..1024 else 128 (wire holds the u8 subset 8..255; 256..1024 reserved for a
future u16 field and map to 128 on the wire) — kills 0/div-zero + absurd
values. Both sides default 128 (`COUNTER_PERSIST_EVERY` / `DriveCounter`
default); a v1 robot (38B, no byte) → app defaults to 128 and stays
compatible. Once-at-pair by design: no re-negotiation, no `SET_RATE`-style
exchange — changing N mid-bond would fork the crash-replay bound between the
two sides. Shared test vector pins `persist_every=128 (0x80)` with
`ct=f47f282e8b8020ec2acf8bf1eaf2ee2590`.

Open questions for owner: accept B1-first ranking (or prefer B2's zero-wire
shape)? Hint byte placement + cap for B1? `PAIR_REFRESH_MARGIN` 1000 the right
proactive distance? Background pair-refresh backoff shape (fixed vs exponential)?
Persist-interval ever per-bond renegotiable, or once-at-pair forever?
